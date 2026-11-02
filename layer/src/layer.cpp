// VK_LAYER_RP6_framegen: implicit Vulkan layer that inserts generated frames
// between the application's presents.
//
// CPU budget in the game thread is the design constraint:
//  - every command buffer is recorded once, when the swapchain is created;
//  - a present costs two vkQueueSubmit calls with one VkSubmitInfo each (Mesa
//    turns every VkSubmitInfo into its own kernel submit), up to M-1
//    vkAcquireNextImageKHR and up to M vkQueuePresentKHR;
//  - no fences and no CPU waits: ordering is GPU-side through semaphores on the
//    single graphics queue. The only blocking call is the acquire, which blocks
//    exactly when FIFO is full, i.e. the vsync wait the game would do anyway.
//
// Per real frame:
//   submit A (before the acquire, so the GPU works while the CPU waits for vsync):
//     [copy app image -> colour[p], analyze[p]: luma pyramid + motion estimation]
//     signal realDone[src]
//   acquire M-1 swapchain images
//   submit B: [generate[tgt][p][phase]] for each, signal genDone[tgt]
//   present generated images, then the app's image.
#include "engine.hpp"
#include "live.hpp"

#include <vulkan/vk_layer.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#define RP6FG_EXPORT extern "C" __attribute__((visibility("default")))

namespace {

void logf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void logf(const char* fmt, ...) {
    static FILE* f = [] {
        const char* path = getenv("RP6FG_LOG");
        FILE* out = path ? fopen(path, "a") : nullptr;
        return out ? out : stderr;
    }();
    va_list ap;
    va_start(ap, fmt);
    fprintf(f, "[rp6fg] ");
    vfprintf(f, fmt, ap);
    fprintf(f, "\n");
    fflush(f);
    va_end(ap);
}

// First few occurrences of non-success results, to explain swapchain re-creation.
void logResult(const char* what, VkResult r) {
    static int count = 0;
    if (count++ < 20) logf("%s returned %d", what, r);
}

int envInt(const char* name, int def, int lo, int hi) {
    const char* v = getenv(name);
    if (!v) return def;
    int x = atoi(v);
    return x < lo ? lo : x > hi ? hi : x;
}

uint64_t nowNs() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
}

// ---- live control ---------------------------------------------------------------

rp6fg::LiveControl* g_live = nullptr;

// Maps the shared control block, creating it with defaults on first use.
rp6fg::LiveControl* live() {
    static std::once_flag once;
    std::call_once(once, [] {
        // /dev/shm, not $XDG_RUNTIME_DIR: Steam Runtime containers get a private
        // /run/user/<uid> but share /dev/shm with the host (and the plugin).
        char path[64];
        snprintf(path, sizeof(path), "/dev/shm/rp6fg-live-%u", static_cast<unsigned>(getuid()));
        int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0644);
        if (fd < 0) return;
        struct stat st;
        bool fresh = fstat(fd, &st) == 0 && st.st_size < static_cast<off_t>(sizeof(rp6fg::LiveControl));
        // Allocate, not just truncate: a sparse tmpfs page would be allocated on
        // first write and could SIGBUS the game if /dev/shm is full.
        if (fresh && posix_fallocate(fd, 0, 4096) != 0) {
            close(fd);
            return;
        }
        void* m = mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        close(fd);
        if (m == MAP_FAILED) return;
        auto* l = static_cast<rp6fg::LiveControl*>(m);
        if (l->magic != rp6fg::kLiveMagic || l->version != rp6fg::kLiveVersion) {
            l->enabled = 1;
            l->mult = static_cast<uint32_t>(envInt("RP6FG_MULT", 2, 2, 4));
            l->adaptive = 1;
            l->refreshHz = 120;
            l->version = rp6fg::kLiveVersion;
            l->magic = rp6fg::kLiveMagic;
        }
        g_live = l;
    });
    return g_live;
}

uint32_t liveLoad(const uint32_t& v) { return __atomic_load_n(&v, __ATOMIC_RELAXED); }

bool wantedOn() {
    rp6fg::LiveControl* l = live();
    return !l || liveLoad(l->enabled) != 0;
}

int wantedMult() {
    rp6fg::LiveControl* l = live();
    if (!l) return envInt("RP6FG_MULT", 2, 2, 4);
    uint32_t m = liveLoad(l->mult);
    return m < 2 ? 2 : m > 4 ? 4 : static_cast<int>(m);
}

template <typename T>
void* key(T handle) { return *reinterpret_cast<void**>(handle); }

VkBaseOutStructure* findInChain(const void* chain, VkStructureType type) {
    for (auto* s = reinterpret_cast<VkBaseOutStructure*>(const_cast<void*>(chain)); s; s = s->pNext)
        if (s->sType == type) return s;
    return nullptr;
}

// Temporarily unlinks structures of the given types from a pNext chain whose
// head pointer we own; the destructor relinks them exactly.
class ChainStrip {
public:
    ChainStrip(const void** head, std::initializer_list<VkStructureType> types) {
        auto** link = reinterpret_cast<VkBaseOutStructure**>(const_cast<void**>(head));
        while (*link) {
            VkBaseOutStructure* s = *link;
            if (std::find(types.begin(), types.end(), s->sType) != types.end() && m_count < kMax) {
                m_removed[m_count++] = {link, s};
                *link = s->pNext;
            } else {
                link = &s->pNext;
            }
        }
    }
    ~ChainStrip() {
        for (int i = m_count - 1; i >= 0; i--) *m_removed[i].first = m_removed[i].second;
    }
private:
    static constexpr int kMax = 8;  // fixed: no allocation on the present path
    std::pair<VkBaseOutStructure**, VkBaseOutStructure*> m_removed[kMax];
    int m_count = 0;
};

struct InstanceData {
    VkInstance instance;
    PFN_vkGetInstanceProcAddr gipa;
    PFN_vkDestroyInstance DestroyInstance;
    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties;
    PFN_vkGetPhysicalDeviceFeatures2 GetPhysicalDeviceFeatures2;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties GetPhysicalDeviceQueueFamilyProperties;
    PFN_vkEnumerateDeviceExtensionProperties EnumerateDeviceExtensionProperties;
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR GetPhysicalDeviceSurfaceCapabilitiesKHR;
};

struct DeviceData {
    VkDevice device;
    VkPhysicalDevice physical;
    InstanceData* inst;
    PFN_vkGetDeviceProcAddr gdpa;
    PFN_vkSetDeviceLoaderData setLoaderData;
    DeviceFns vk;
    PFN_vkCreateSwapchainKHR CreateSwapchainKHR;
    PFN_vkDestroySwapchainKHR DestroySwapchainKHR;
    PFN_vkGetSwapchainImagesKHR GetSwapchainImagesKHR;
    PFN_vkAcquireNextImageKHR AcquireNextImageKHR;
    PFN_vkAcquireNextImage2KHR AcquireNextImage2KHR;
    PFN_vkQueuePresentKHR QueuePresentKHR;
    PFN_vkWaitSemaphores WaitSemaphores;
    VkPhysicalDeviceMemoryProperties memProps;
    uint32_t queueFamily = 0;  // graphics family
    bool active = false;       // block matching + timeline semaphores enabled
};

struct SwapchainData {
    DeviceData* dev;
    VkSwapchainKHR swapchain;
    rp6fg::Engine engine;
    std::vector<VkImage> images;
    std::vector<VkImageView> views;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    int mult = 2;
    VkCommandPool pool = VK_NULL_HANDLE;
    // Pre-recorded command buffers.
    VkCommandBuffer init = VK_NULL_HANDLE;   // first frame only
    std::vector<VkCommandBuffer> copy;       // [src][parity]
    VkCommandBuffer analyze[2]{};            // [parity]
    std::vector<VkCommandBuffer> generate;   // [tgt][parity][phase]
    std::vector<VkSemaphore> genDone;        // per image: generated frame rendered
    std::vector<VkSemaphore> realDone;       // per image: app image copied, ready to present
    std::vector<VkSemaphore> acquired;       // ring for our own acquires
    VkSemaphore timeline = VK_NULL_HANDLE;   // counts our submits, for teardown only
    uint64_t timelineValue = 0;
    uint32_t acquireIdx = 0;
    int parity = 0;
    bool hasPrev = false;
    bool initDone = false;
    uint64_t frame = 0;
    uint64_t cpuNs = 0, cpuMaxNs = 0, acquireNs = 0, submitNs = 0, presentNs = 0;
    // Multiplier change: SUBOPTIMAL from present first; engines that ignore it
    // (Godot only looks at acquire) get OUT_OF_DATE from their next acquires
    // once the swapchain is still alive 30 frames later.
    uint32_t recreateFrames = 0;
    std::atomic<bool> forceOutOfDate{false};  // read by the app's acquire thread
    // Paused swapchain: created exactly as the app asked (its present mode, no
    // extra images), presents pass straight through until generation resumes.
    bool passive = false;
    // Once-a-second statistics for the live block.
    uint64_t statsT0 = 0, statsFrames = 0, statsPresents = 0, statsCpuNs = 0, statsSubmitNs = 0;

    size_t n() const { return images.size(); }
    VkCommandBuffer& cp(size_t src, int p) { return copy[src * 2 + p]; }
    // Generated frames per real frame: k = 1..mult-1, frame j of k sits at
    // t = j / (k + 1). One pre-recorded buffer per (k, j) slot.
    int maxGen() const { return mult - 1; }
    int slots() const { return maxGen() * (maxGen() + 1) / 2; }
    static int slot(int k, int j) { return (k - 1) * k / 2 + (j - 1); }
    VkCommandBuffer& gen(size_t tgt, int p, int slotIdx) {
        return generate[(tgt * 2 + p) * static_cast<size_t>(slots()) + slotIdx];
    }
    // Adaptive pacing state.
    int k = 1;                 // generated frames per real frame in use (0 = none)
    int votesUp = 0, votesDown = 0;
    int loggedK = 1;
    uint64_t lastReturnNs = 0; // when our hook returned to the app last time
    // Time the app spent blocked in its own acquires since then; the app may
    // acquire and present from different threads (DXVK), hence atomic.
    std::atomic<uint64_t> appAcquireNs{0};
    double appNsAvg = 0;
};

std::mutex g_lock;
std::unordered_map<void*, std::unique_ptr<InstanceData>> g_instances;
std::unordered_map<void*, std::unique_ptr<DeviceData>> g_devices;
std::unordered_map<VkSwapchainKHR, std::unique_ptr<SwapchainData>> g_swapchains;

InstanceData* instanceOf(void* k) {
    std::lock_guard<std::mutex> l(g_lock);
    auto it = g_instances.find(k);
    return it == g_instances.end() ? nullptr : it->second.get();
}
DeviceData* deviceOf(void* k) {
    std::lock_guard<std::mutex> l(g_lock);
    auto it = g_devices.find(k);
    return it == g_devices.end() ? nullptr : it->second.get();
}
SwapchainData* swapchainOf(VkSwapchainKHR s) {
    std::lock_guard<std::mutex> l(g_lock);
    auto it = g_swapchains.find(s);
    return it == g_swapchains.end() ? nullptr : it->second.get();
}

bool isSrgb(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
        return true;
    default:
        return false;
    }
}

// ---- instance -------------------------------------------------------------------

VKAPI_ATTR VkResult VKAPI_CALL CreateInstance(const VkInstanceCreateInfo* ci, const VkAllocationCallbacks* alloc,
                                              VkInstance* out) {
    auto* link = reinterpret_cast<VkLayerInstanceCreateInfo*>(const_cast<void*>(ci->pNext));
    while (link && !(link->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO && link->function == VK_LAYER_LINK_INFO))
        link = reinterpret_cast<VkLayerInstanceCreateInfo*>(const_cast<void*>(link->pNext));
    if (!link) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetInstanceProcAddr gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;
    auto create = reinterpret_cast<PFN_vkCreateInstance>(gipa(VK_NULL_HANDLE, "vkCreateInstance"));
    VkResult r = create(ci, alloc, out);
    if (r != VK_SUCCESS) return r;

    auto d = std::make_unique<InstanceData>();
    d->instance = *out;
    d->gipa = gipa;
#define GET(name) d->name = reinterpret_cast<PFN_vk##name>(gipa(*out, "vk" #name))
    GET(DestroyInstance);
    GET(GetPhysicalDeviceMemoryProperties);
    GET(GetPhysicalDeviceFeatures2);
    GET(GetPhysicalDeviceQueueFamilyProperties);
    GET(EnumerateDeviceExtensionProperties);
    GET(GetPhysicalDeviceSurfaceCapabilitiesKHR);
#undef GET
    if (!d->GetPhysicalDeviceFeatures2)
        d->GetPhysicalDeviceFeatures2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
            gipa(*out, "vkGetPhysicalDeviceFeatures2KHR"));
    std::lock_guard<std::mutex> l(g_lock);
    g_instances[key(*out)] = std::move(d);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL DestroyInstance(VkInstance instance, const VkAllocationCallbacks* alloc) {
    InstanceData* d = instanceOf(key(instance));
    if (!d) return;
    PFN_vkDestroyInstance destroy = d->DestroyInstance;
    {
        std::lock_guard<std::mutex> l(g_lock);
        g_instances.erase(key(instance));
    }
    destroy(instance, alloc);
}

// ---- device ---------------------------------------------------------------------

struct DeviceSupport {
    bool blockMatch = false;
    bool timeline = false;
    bool timelineExt = false;      // VK_KHR_timeline_semaphore listed
    bool formatFlags2Ext = false;  // VK_KHR_format_feature_flags2 listed (needed below 1.3)
};

DeviceSupport querySupport(InstanceData* inst, VkPhysicalDevice pd) {
    DeviceSupport s;
    if (!inst || !inst->GetPhysicalDeviceFeatures2) return s;
    uint32_t n = 0;
    inst->EnumerateDeviceExtensionProperties(pd, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> ext(n);
    inst->EnumerateDeviceExtensionProperties(pd, nullptr, &n, ext.data());
    bool qcom = false;
    for (auto& e : ext) {
        qcom |= !strcmp(e.extensionName, VK_QCOM_IMAGE_PROCESSING_EXTENSION_NAME);
        s.timelineExt |= !strcmp(e.extensionName, VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);
        s.formatFlags2Ext |= !strcmp(e.extensionName, VK_KHR_FORMAT_FEATURE_FLAGS_2_EXTENSION_NAME);
    }
    VkPhysicalDeviceTimelineSemaphoreFeatures tl{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
    VkPhysicalDeviceImageProcessingFeaturesQCOM ipf{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_PROCESSING_FEATURES_QCOM, &tl};
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, qcom ? static_cast<void*>(&ipf) : &tl};
    inst->GetPhysicalDeviceFeatures2(pd, &f2);
    s.blockMatch = qcom && ipf.textureBlockMatch;
    s.timeline = tl.timelineSemaphore;
    return s;
}

uint32_t graphicsFamily(InstanceData* inst, VkPhysicalDevice pd, const VkDeviceCreateInfo* ci, bool* found) {
    *found = false;
    if (!inst || !inst->GetPhysicalDeviceQueueFamilyProperties) return 0;
    uint32_t n = 0;
    inst->GetPhysicalDeviceQueueFamilyProperties(pd, &n, nullptr);
    std::vector<VkQueueFamilyProperties> fam(n);
    inst->GetPhysicalDeviceQueueFamilyProperties(pd, &n, fam.data());
    for (uint32_t i = 0; i < ci->queueCreateInfoCount; i++) {
        uint32_t f = ci->pQueueCreateInfos[i].queueFamilyIndex;
        if (f < n && (fam[f].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
            *found = true;
            return f;
        }
    }
    return 0;
}

VKAPI_ATTR VkResult VKAPI_CALL CreateDevice(VkPhysicalDevice pd, const VkDeviceCreateInfo* ci,
                                            const VkAllocationCallbacks* alloc, VkDevice* out) {
    auto* link = reinterpret_cast<VkLayerDeviceCreateInfo*>(const_cast<void*>(ci->pNext));
    while (link && !(link->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO && link->function == VK_LAYER_LINK_INFO))
        link = reinterpret_cast<VkLayerDeviceCreateInfo*>(const_cast<void*>(link->pNext));
    auto* cb = reinterpret_cast<VkLayerDeviceCreateInfo*>(const_cast<void*>(ci->pNext));
    while (cb && !(cb->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO && cb->function == VK_LOADER_DATA_CALLBACK))
        cb = reinterpret_cast<VkLayerDeviceCreateInfo*>(const_cast<void*>(cb->pNext));
    if (!link) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetInstanceProcAddr gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr gdpa = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;

    InstanceData* inst = instanceOf(key(pd));
    DeviceSupport sup = querySupport(inst, pd);
    bool hasGraphics = false;
    uint32_t family = graphicsFamily(inst, pd, ci, &hasGraphics);
    bool active = sup.blockMatch && sup.timeline && hasGraphics && cb;
    if (!active) logf("device lacks block matching / timeline semaphores / graphics queue, layer passive");

    // Enable what we need on top of the app's request. App-owned feature
    // structs are patched in place and restored after the call.
    std::vector<const char*> exts(ci->ppEnabledExtensionNames, ci->ppEnabledExtensionNames + ci->enabledExtensionCount);
    auto addExt = [&](const char* name) {
        for (auto* e : exts)
            if (!strcmp(e, name)) return;
        exts.push_back(name);
    };
    VkDeviceCreateInfo dci = *ci;
    VkPhysicalDeviceImageProcessingFeaturesQCOM ipf{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_PROCESSING_FEATURES_QCOM};
    VkPhysicalDeviceTimelineSemaphoreFeatures tlf{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
    std::vector<std::pair<VkBool32*, VkBool32>> restore;
    auto setFeature = [&](VkBool32* f) {
        restore.push_back({f, *f});
        *f = VK_TRUE;
    };
    if (active) {
        addExt(VK_QCOM_IMAGE_PROCESSING_EXTENSION_NAME);
        if (sup.timelineExt) addExt(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);
        if (sup.formatFlags2Ext) addExt(VK_KHR_FORMAT_FEATURE_FLAGS_2_EXTENSION_NAME);
        dci.enabledExtensionCount = static_cast<uint32_t>(exts.size());
        dci.ppEnabledExtensionNames = exts.data();

        if (auto* s = findInChain(ci->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_PROCESSING_FEATURES_QCOM)) {
            setFeature(&reinterpret_cast<VkPhysicalDeviceImageProcessingFeaturesQCOM*>(s)->textureBlockMatch);
        } else {
            ipf.textureBlockMatch = VK_TRUE;
            ipf.pNext = const_cast<void*>(dci.pNext);
            dci.pNext = &ipf;
        }
        // Timeline semaphores live either in the 1.2 aggregate or their own struct, never both.
        if (auto* s = findInChain(ci->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES)) {
            setFeature(&reinterpret_cast<VkPhysicalDeviceVulkan12Features*>(s)->timelineSemaphore);
        } else if (auto* t = findInChain(ci->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES)) {
            setFeature(&reinterpret_cast<VkPhysicalDeviceTimelineSemaphoreFeatures*>(t)->timelineSemaphore);
        } else {
            tlf.timelineSemaphore = VK_TRUE;
            tlf.pNext = const_cast<void*>(dci.pNext);
            dci.pNext = &tlf;
        }
    }
    auto create = reinterpret_cast<PFN_vkCreateDevice>(gipa(inst ? inst->instance : VK_NULL_HANDLE, "vkCreateDevice"));
    VkResult r = create(pd, &dci, alloc, out);
    for (auto& [ptr, old] : restore) *ptr = old;
    if (r != VK_SUCCESS) return r;

    auto d = std::make_unique<DeviceData>();
    d->device = *out;
    d->physical = pd;
    d->inst = inst;
    d->gdpa = gdpa;
    d->setLoaderData = cb ? cb->u.pfnSetDeviceLoaderData : nullptr;
    d->queueFamily = family;
    // Load unconditionally: passive devices still need DestroyDevice and friends.
    const bool loaded = d->vk.load(gdpa, *out);
    if (!d->vk.DestroyDevice)
        d->vk.DestroyDevice = reinterpret_cast<PFN_vkDestroyDevice>(gdpa(*out, "vkDestroyDevice"));
#define GET(name) d->name = reinterpret_cast<PFN_vk##name>(gdpa(*out, "vk" #name))
    GET(CreateSwapchainKHR);
    GET(DestroySwapchainKHR);
    GET(GetSwapchainImagesKHR);
    GET(AcquireNextImageKHR);
    GET(AcquireNextImage2KHR);
    GET(QueuePresentKHR);
    GET(WaitSemaphores);
#undef GET
    if (!d->WaitSemaphores)
        d->WaitSemaphores = reinterpret_cast<PFN_vkWaitSemaphores>(gdpa(*out, "vkWaitSemaphoresKHR"));
    d->active = active && loaded && d->WaitSemaphores && d->CreateSwapchainKHR;
    if (inst) inst->GetPhysicalDeviceMemoryProperties(pd, &d->memProps);
    logf("device created, frame generation %s", d->active ? "enabled" : "disabled");
    std::lock_guard<std::mutex> l(g_lock);
    g_devices[key(*out)] = std::move(d);
    return VK_SUCCESS;
}

void destroySwapchainData(SwapchainData* s);

VKAPI_ATTR void VKAPI_CALL DestroyDevice(VkDevice device, const VkAllocationCallbacks* alloc) {
    DeviceData* d = deviceOf(key(device));
    if (!d) return;
    std::vector<SwapchainData*> mine;
    {
        std::lock_guard<std::mutex> l(g_lock);
        for (auto& [h, s] : g_swapchains)
            if (s->dev == d) mine.push_back(s.get());
    }
    for (auto* s : mine) destroySwapchainData(s);
    PFN_vkDestroyDevice destroy = d->vk.DestroyDevice;
    {
        std::lock_guard<std::mutex> l(g_lock);
        g_devices.erase(key(device));
    }
    destroy(device, alloc);
}

// ---- swapchain ----------------------------------------------------------------------

void destroySwapchainData(SwapchainData* s) {
    DeviceData* d = s->dev;
    VkDevice dev = d->device;
    // Wait for our own work only (never the app's queue): the timeline counts our submits.
    if (s->timeline && s->timelineValue) {
        VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
        wi.semaphoreCount = 1;
        wi.pSemaphores = &s->timeline;
        wi.pValues = &s->timelineValue;
        d->WaitSemaphores(dev, &wi, UINT64_MAX);
    }
    s->engine.destroy();
    for (auto v : s->views) d->vk.DestroyImageView(dev, v, nullptr);
    for (auto* list : {&s->genDone, &s->realDone, &s->acquired})
        for (auto sem : *list)
            if (sem) d->vk.DestroySemaphore(dev, sem, nullptr);
    if (s->timeline) d->vk.DestroySemaphore(dev, s->timeline, nullptr);
    if (s->pool) d->vk.DestroyCommandPool(dev, s->pool, nullptr);
    std::lock_guard<std::mutex> l(g_lock);
    g_swapchains.erase(s->swapchain);
}

void imageBarrier(const DeviceFns& vk, VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to,
                  VkPipelineStageFlags srcStage, VkAccessFlags srcAccess,
                  VkPipelineStageFlags dstStage, VkAccessFlags dstAccess) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = srcAccess;
    b.dstAccessMask = dstAccess;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vk.CmdPipelineBarrier(cb, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

bool recordAll(SwapchainData* s) {
    DeviceData* d = s->dev;
    const DeviceFns& vk = d->vk;
    VkDevice dev = d->device;
    const size_t n = s->n();
    const int slots = s->slots();

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.queueFamilyIndex = d->queueFamily;
    if (vk.CreateCommandPool(dev, &pci, nullptr, &s->pool)) return false;

    s->copy.resize(n * 2);
    s->generate.resize(n * 2 * slots);
    std::vector<VkCommandBuffer> all(3 + s->copy.size() + s->generate.size());
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = s->pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = static_cast<uint32_t>(all.size());
    if (vk.AllocateCommandBuffers(dev, &ai, all.data())) return false;
    for (auto cb : all) d->setLoaderData(dev, cb);  // our own dispatchable handles
    s->init = all[0];
    s->analyze[0] = all[1];
    s->analyze[1] = all[2];
    std::copy(all.begin() + 3, all.begin() + 3 + s->copy.size(), s->copy.begin());
    std::copy(all.begin() + 3 + s->copy.size(), all.end(), s->generate.begin());

    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;

    vk.BeginCommandBuffer(s->init, &bi);
    s->engine.recordInit(s->init);
    vk.EndCommandBuffer(s->init);

    for (int p = 0; p < 2; p++) {
        vk.BeginCommandBuffer(s->analyze[p], &bi);
        s->engine.recordAnalyze(s->analyze[p], p);
        vk.EndCommandBuffer(s->analyze[p]);
        for (size_t src = 0; src < n; src++) {
            // The app left the image in PRESENT_SRC; waiting its semaphores at
            // ALL_COMMANDS orders us after its writes. Copy, then hand it back.
            VkCommandBuffer cb = s->cp(src, p);
            vk.BeginCommandBuffer(cb, &bi);
            imageBarrier(vk, cb, s->images[src], VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
            s->engine.recordCopy(cb, s->images[src], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, p);
            imageBarrier(vk, cb, s->images[src], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0);
            vk.EndCommandBuffer(cb);
        }
        for (size_t tgt = 0; tgt < n; tgt++) {
            for (int k = 1; k <= s->maxGen(); k++) {
                for (int j = 1; j <= k; j++) {
                    VkCommandBuffer g = s->gen(tgt, p, SwapchainData::slot(k, j));
                    vk.BeginCommandBuffer(g, &bi);
                    float t = static_cast<float>(j) / static_cast<float>(k + 1);
                    s->engine.recordGenerate(g, static_cast<int>(tgt), p, t);
                    vk.EndCommandBuffer(g);
                }
            }
        }
    }
    return true;
}

VKAPI_ATTR VkResult VKAPI_CALL CreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR* ci,
                                                  const VkAllocationCallbacks* alloc, VkSwapchainKHR* out) {
    DeviceData* d = deviceOf(key(device));
    if (!d) return VK_ERROR_INITIALIZATION_FAILED;
    if (!d->active) return d->CreateSwapchainKHR(device, ci, alloc, out);

    const int mult = wantedMult();
    if (!wantedOn()) {
        VkResult pr = d->CreateSwapchainKHR(device, ci, alloc, out);
        if (pr == VK_SUCCESS) {
            auto s = std::make_unique<SwapchainData>();
            s->dev = d;
            s->swapchain = *out;
            s->mult = mult;
            s->passive = true;
            s->format = ci->imageFormat;
            s->extent = ci->imageExtent;
            std::lock_guard<std::mutex> l(g_lock);
            g_swapchains[*out] = std::move(s);
            logf("swapchain %ux%u created untouched (generation paused)", ci->imageExtent.width, ci->imageExtent.height);
        }
        return pr;
    }
    VkSurfaceCapabilitiesKHR caps{};
    if (d->inst && d->inst->GetPhysicalDeviceSurfaceCapabilitiesKHR)
        d->inst->GetPhysicalDeviceSurfaceCapabilitiesKHR(d->physical, ci->surface, &caps);
    VkSwapchainCreateInfoKHR sci = *ci;
    sci.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    sci.flags &= ~VK_SWAPCHAIN_CREATE_DEFERRED_MEMORY_ALLOCATION_BIT_EXT;
    sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;  // generated frames are paced by vsync
    // The images beyond the surface minimum are what the app may hold; M-1 more
    // cover our generated frames. Every extra image is a frame of queue latency.
    sci.minImageCount = std::max(ci->minImageCount, caps.minImageCount) + static_cast<uint32_t>(mult - 1);
    if (caps.maxImageCount) sci.minImageCount = std::min(sci.minImageCount, caps.maxImageCount);
    const bool usageOk = !caps.supportedUsageFlags ||
                         (caps.supportedUsageFlags & sci.imageUsage) == sci.imageUsage;
    if (!usageOk) {
        logf("surface lacks transfer/attachment usage, passthrough");
        return d->CreateSwapchainKHR(device, ci, alloc, out);
    }

    // swapchain_maintenance1 lets apps switch present modes per present; only FIFO is allowed here.
    VkResult r;
    {
        auto* modes = reinterpret_cast<VkSwapchainPresentModesCreateInfoEXT*>(
            findInChain(ci->pNext, VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODES_CREATE_INFO_EXT));
        static const VkPresentModeKHR fifo = VK_PRESENT_MODE_FIFO_KHR;
        const VkPresentModeKHR* oldModes = modes ? modes->pPresentModes : nullptr;
        uint32_t oldCount = modes ? modes->presentModeCount : 0;
        if (modes) {
            modes->pPresentModes = &fifo;
            modes->presentModeCount = 1;
        }
        r = d->CreateSwapchainKHR(device, &sci, alloc, out);
        if (modes) {
            modes->pPresentModes = oldModes;
            modes->presentModeCount = oldCount;
        }
    }
    if (r != VK_SUCCESS) {
        logf("modified swapchain failed (%d)", r);
        // A failed call already retired oldSwapchain: do not retry with it.
        if (ci->oldSwapchain) return r;
        return d->CreateSwapchainKHR(device, ci, alloc, out);
    }

    auto s = std::make_unique<SwapchainData>();
    s->dev = d;
    s->swapchain = *out;
    s->mult = mult;
    s->format = ci->imageFormat;
    s->extent = ci->imageExtent;
    // Re-created for a new multiplier, pause or resize: keep the adaptive estimate.
    if (SwapchainData* old = ci->oldSwapchain ? swapchainOf(ci->oldSwapchain) : nullptr) {
        s->appNsAvg = old->appNsAvg;
        s->k = std::min(old->k, s->maxGen());
    }
    uint32_t n = 0;
    d->GetSwapchainImagesKHR(device, *out, &n, nullptr);
    s->images.resize(n);
    d->GetSwapchainImagesKHR(device, *out, &n, s->images.data());

    bool ok = n >= caps.minImageCount + static_cast<uint32_t>(mult - 1);
    if (!ok) logf("only %u images (surface min %u), cannot hold %d generated frames", n, caps.minImageCount, mult - 1);

    rp6fg::EngineConfig cfg;
    cfg.width = ci->imageExtent.width;
    cfg.height = ci->imageExtent.height;
    cfg.colorFormat = ci->imageFormat;
    cfg.linearIn = isSrgb(ci->imageFormat);
    cfg.targetFinalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    ok = ok && s->engine.init(device, &d->vk, d->memProps, cfg);
    for (uint32_t i = 0; ok && i < n; i++) {
        // The view is only ever a render target; do not inherit e.g. STORAGE from the app.
        VkImageViewUsageCreateInfo usage{VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};
        usage.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, &usage};
        vci.image = s->images[i];
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = ci->imageFormat;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView v;
        ok = d->vk.CreateImageView(device, &vci, nullptr, &v) == VK_SUCCESS;
        if (!ok) break;
        s->views.push_back(v);
        s->engine.addTarget(v);
    }
    VkSemaphoreCreateInfo semci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    auto sems = [&](std::vector<VkSemaphore>& v, size_t count) {
        v.resize(count, VK_NULL_HANDLE);
        for (auto& sem : v) ok = ok && d->vk.CreateSemaphore(device, &semci, nullptr, &sem) == VK_SUCCESS;
    };
    sems(s->genDone, n);
    sems(s->realDone, n);
    sems(s->acquired, n + 1);
    VkSemaphoreTypeCreateInfo tci{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    tci.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo tsci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &tci};
    ok = ok && d->vk.CreateSemaphore(device, &tsci, nullptr, &s->timeline) == VK_SUCCESS;

    SwapchainData* raw = s.get();
    {
        std::lock_guard<std::mutex> l(g_lock);
        g_swapchains[*out] = std::move(s);
    }
    ok = ok && recordAll(raw);
    if (!ok) {
        // The swapchain itself is fine (FIFO, more images): run it untouched.
        logf("engine setup failed, swapchain passthrough");
        destroySwapchainData(raw);
        return VK_SUCCESS;
    }
    logf("swapchain %ux%u fmt %d, %u images, x%d", cfg.width, cfg.height, cfg.colorFormat, n, mult);
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL AcquireNextImageKHR(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout,
                                                   VkSemaphore semaphore, VkFence fence, uint32_t* index) {
    DeviceData* d = deviceOf(key(device));
    if (!d) return VK_ERROR_DEVICE_LOST;
    SwapchainData* s = swapchainOf(swapchain);
    if (!s) return d->AcquireNextImageKHR(device, swapchain, timeout, semaphore, fence, index);
    if (s->forceOutOfDate.load(std::memory_order_relaxed)) return VK_ERROR_OUT_OF_DATE_KHR;
    // Blocking here is vsync back-pressure, not the game's work: adaptive pacing subtracts it.
    const uint64_t a0 = nowNs();
    VkResult r = d->AcquireNextImageKHR(device, swapchain, timeout, semaphore, fence, index);
    s->appAcquireNs.fetch_add(nowNs() - a0, std::memory_order_relaxed);
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL AcquireNextImage2KHR(VkDevice device, const VkAcquireNextImageInfoKHR* info,
                                                    uint32_t* index) {
    DeviceData* d = deviceOf(key(device));
    if (!d) return VK_ERROR_DEVICE_LOST;
    SwapchainData* s = swapchainOf(info->swapchain);
    if (!d->AcquireNextImage2KHR) return VK_ERROR_EXTENSION_NOT_PRESENT;
    if (!s) return d->AcquireNextImage2KHR(device, info, index);
    if (s->forceOutOfDate.load(std::memory_order_relaxed)) return VK_ERROR_OUT_OF_DATE_KHR;
    const uint64_t a0 = nowNs();
    VkResult r = d->AcquireNextImage2KHR(device, info, index);
    s->appAcquireNs.fetch_add(nowNs() - a0, std::memory_order_relaxed);
    return r;
}

VKAPI_ATTR void VKAPI_CALL DestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks* alloc) {
    DeviceData* d = deviceOf(key(device));
    if (!d) return;
    if (SwapchainData* s = swapchainOf(swapchain)) destroySwapchainData(s);
    d->DestroySwapchainKHR(device, swapchain, alloc);
}

// ---- debug dump (RP6FG_DUMP=<dir>, RP6FG_DUMP_AT=<frame>) ------------------------
// Off the hot path: only on the selected frame, and it waits for the queue.

bool writePpm(const char* path, const uint8_t* px, VkFormat fmt, uint32_t w, uint32_t h) {
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    fprintf(f, "P6\n%u %u\n255\n", w, h);
    std::vector<uint8_t> row(w * 3);
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            const uint8_t* p = px + (static_cast<size_t>(y) * w + x) * 4;
            uint32_t v;
            memcpy(&v, p, 4);
            uint8_t r, g, b;
            switch (fmt) {
            case VK_FORMAT_B8G8R8A8_UNORM:
            case VK_FORMAT_B8G8R8A8_SRGB:
                r = p[2]; g = p[1]; b = p[0];
                break;
            case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
                r = (v >> 22) & 0xFF; g = (v >> 12) & 0xFF; b = (v >> 2) & 0xFF;
                break;
            case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
                r = (v >> 2) & 0xFF; g = (v >> 12) & 0xFF; b = (v >> 22) & 0xFF;
                break;
            default:
                r = p[0]; g = p[1]; b = p[2];
            }
            row[x * 3] = r; row[x * 3 + 1] = g; row[x * 3 + 2] = b;
        }
        fwrite(row.data(), 1, row.size(), f);
    }
    fclose(f);
    return true;
}

// Debug dump of a real triplet: frame N-1 (prev), N (cur), the generated
// middle and the vector view, then frame N+1 (next). Nothing waits between N
// and N+1 (a stall there would make N+1 a much later frame): copies are only
// submitted at N; the wait and the file writes happen after N+1 is captured.
// Resources are leaked on purpose: debug only, and the engine keeps
// framebuffers for the views it was given.
struct DumpState {
    SwapchainData* swapchain = nullptr;  // N+1 must come from the same swapchain
    VkExtent2D extent{};
    VkBuffer buf = VK_NULL_HANDLE;
    void* map = nullptr;
    VkDeviceSize frame = 0;
};
DumpState g_dump;

uint32_t hostMemType(DeviceData* d, uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < d->memProps.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (d->memProps.memoryTypes[i].propertyFlags & want) == want) return i;
    return 0u;
}

VkCommandBuffer dumpCmd(SwapchainData* s) {
    DeviceData* d = s->dev;
    VkCommandBuffer cb;
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = s->pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    if (d->vk.AllocateCommandBuffers(d->device, &ai, &cb)) return VK_NULL_HANDLE;
    d->setLoaderData(d->device, cb);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    d->vk.BeginCommandBuffer(cb, &bi);
    return cb;
}

void dumpSubmit(SwapchainData* s, VkQueue queue, VkCommandBuffer cb) {
    s->dev->vk.EndCommandBuffer(cb);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    s->dev->vk.QueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
}

void copyToDump(SwapchainData* s, VkCommandBuffer cb, VkImage img, int slot) {
    VkBufferImageCopy r{};
    r.bufferOffset = g_dump.frame * slot;
    r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.imageExtent = {s->extent.width, s->extent.height, 1};
    s->dev->vk.CmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_GENERAL, g_dump.buf, 1, &r);
}

// Frame N: generate the middle frame and the vector view into private images
// and queue copies of prev, cur and those two. No wait.
void dumpFrame(SwapchainData* s, VkQueue queue, int p) {
    DeviceData* d = s->dev;
    const DeviceFns& vk = d->vk;
    VkDevice dev = d->device;
    const uint32_t w = s->extent.width, h = s->extent.height;
    VkImage img[2];
    int targets[2];
    for (int i = 0; i < 2; i++) {
        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = s->format;
        ici.extent = {w, h, 1};
        ici.mipLevels = ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        if (vk.CreateImage(dev, &ici, nullptr, &img[i])) return;
        VkMemoryRequirements req;
        vk.GetImageMemoryRequirements(dev, img[i], &req);
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = hostMemType(d, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VkDeviceMemory mem;
        if (vk.AllocateMemory(dev, &mai, nullptr, &mem) || vk.BindImageMemory(dev, img[i], mem, 0)) return;
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = img[i];
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = s->format;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView view;
        if (vk.CreateImageView(dev, &vci, nullptr, &view)) return;
        targets[i] = s->engine.addTarget(view);
    }
    g_dump.frame = static_cast<VkDeviceSize>(w) * h * 4;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = g_dump.frame * 5;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (vk.CreateBuffer(dev, &bci, nullptr, &g_dump.buf)) return;
    VkMemoryRequirements req;
    vk.GetBufferMemoryRequirements(dev, g_dump.buf, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = hostMemType(d, req.memoryTypeBits,
                                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkDeviceMemory bufMem;
    if (vk.AllocateMemory(dev, &mai, nullptr, &bufMem) || vk.BindBufferMemory(dev, g_dump.buf, bufMem, 0)) return;
    if (vk.MapMemory(dev, bufMem, 0, VK_WHOLE_SIZE, 0, &g_dump.map)) return;
    g_dump.swapchain = s;
    g_dump.extent = s->extent;

    VkCommandBuffer cb = dumpCmd(s);
    if (!cb) return;
    s->engine.recordGenerate(cb, targets[0], p, 0.5f, 0);
    s->engine.recordGenerate(cb, targets[1], p, 0.5f, 1);
    for (int i = 0; i < 2; i++)
        imageBarrier(vk, cb, img[i], VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_GENERAL,
                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    copyToDump(s, cb, s->engine.colorImage(1 - p), 0);
    copyToDump(s, cb, s->engine.colorImage(p), 1);
    copyToDump(s, cb, img[0], 2);
    copyToDump(s, cb, img[1], 3);
    dumpSubmit(s, queue, cb);
}

// Frame N+1: queue its copy, then wait and write all five files.
void dumpNext(SwapchainData* s, VkQueue queue, int p) {
    if (!g_dump.map) return;
    if (g_dump.swapchain != s || g_dump.extent.width != s->extent.width || g_dump.extent.height != s->extent.height) {
        logf("dump cancelled: swapchain changed between N and N+1");
        g_dump = DumpState{};
        return;
    }
    VkCommandBuffer cb = dumpCmd(s);
    if (!cb) return;
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    s->dev->vk.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0,
                                  nullptr, 0, nullptr);
    copyToDump(s, cb, s->engine.colorImage(p), 4);
    dumpSubmit(s, queue, cb);
    s->dev->vk.QueueWaitIdle(queue);

    const char* dir = getenv("RP6FG_DUMP");
    const char* names[5] = {"prev", "cur", "gen", "vectors", "next"};
    for (int i = 0; i < 5; i++) {
        const uint8_t* px = static_cast<const uint8_t*>(g_dump.map) + g_dump.frame * i;
        char path[512];
        snprintf(path, sizeof(path), "%s/%s.ppm", dir, names[i]);
        writePpm(path, px, s->format, s->extent.width, s->extent.height);
        snprintf(path, sizeof(path), "%s/%s.raw", dir, names[i]);
        if (FILE* f = fopen(path, "wb")) {
            fwrite(px, 1, g_dump.frame, f);
            fclose(f);
        }
    }
    logf("dumped triplet ending at frame %llu (%ux%u fmt %d) to %s", static_cast<unsigned long long>(s->frame),
         s->extent.width, s->extent.height, s->format, dir);
    g_dump = DumpState{};
}

// ---- present ----------------------------------------------------------------------

void publishStats(SwapchainData* s, rp6fg::LiveControl* lc, uint64_t presents, uint64_t cpuNs, uint64_t submitNs,
                  int mult) {
    if (!lc) return;
    s->statsFrames++;
    s->statsPresents += presents;
    s->statsCpuNs += cpuNs;
    s->statsSubmitNs += submitNs;
    const uint64_t now = nowNs();
    if (!s->statsT0) s->statsT0 = now;
    const uint64_t dt = now - s->statsT0;
    if (dt < 1000000000ull) return;
    static const uint32_t appid = [] {
        const char* v = getenv("SteamAppId");
        if (!v || !*v) v = getenv("SteamGameId");
        return v ? static_cast<uint32_t>(strtoul(v, nullptr, 10)) : 0u;
    }();
    const float sec = static_cast<float>(dt) * 1e-9f;
    const float frames = static_cast<float>(s->statsFrames);
    lc->appid = appid;
    lc->width = s->extent.width;
    lc->height = s->extent.height;
    lc->activeMult = static_cast<uint32_t>(mult);
    lc->realFps = frames / sec;
    lc->outFps = static_cast<float>(s->statsPresents) / sec;
    lc->cpuUs = static_cast<float>(s->statsCpuNs) / frames / 1000.0f;
    lc->submitUs = static_cast<float>(s->statsSubmitNs) / frames / 1000.0f;
    __atomic_store_n(&lc->heartbeat, now, __ATOMIC_RELEASE);
    s->statsT0 = now;
    s->statsFrames = s->statsPresents = s->statsCpuNs = s->statsSubmitNs = 0;
}

// Asks the app to re-create its swapchain (new multiplier, pause/resume):
// SUBOPTIMAL from this present first; engines that ignore present results
// (Godot only reacts to acquire) get OUT_OF_DATE from their acquires once the
// swapchain is still alive 30 frames later.
VkResult requestRecreate(SwapchainData* s, const VkPresentInfoKHR* pi, VkResult r) {
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) return r;
    const uint32_t n = s->recreateFrames++;
    if (n == 0) {
        logf("swapchain re-creation requested (x%d%s -> x%d%s)", s->mult, s->passive ? " paused" : "",
             wantedMult(), wantedOn() ? "" : " paused");
        if (pi->pResults) pi->pResults[0] = VK_SUBOPTIMAL_KHR;
        return VK_SUBOPTIMAL_KHR;
    }
    if (n == 30) {
        logf("swapchain not re-created, failing the app's acquires with OUT_OF_DATE");
        s->forceOutOfDate.store(true, std::memory_order_relaxed);
    }
    return r;
}

void recreateSettled(SwapchainData* s) {
    s->recreateFrames = 0;
    s->forceOutOfDate.store(false, std::memory_order_relaxed);
}

VKAPI_ATTR VkResult VKAPI_CALL QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* pi) {
    // Test knob: pretend the app needs this much more time per frame.
    static const int testSleepUs = envInt("RP6FG_TEST_SLEEP_US", envInt("RP6FG_TEST_SLEEP_MS", 0, 0, 200) * 1000, 0, 200000);
    if (testSleepUs) {
        timespec ts{testSleepUs / 1000000, (testSleepUs % 1000000) * 1000L};
        nanosleep(&ts, nullptr);
    }
    const uint64_t t0 = nowNs();
    DeviceData* d = deviceOf(key(queue));
    if (!d) return VK_ERROR_DEVICE_LOST;
    SwapchainData* s = pi->swapchainCount == 1 ? swapchainOf(pi->pSwapchains[0]) : nullptr;
    if (!s) {
        // Multi-swapchain presents bypass us; history of our swapchains goes stale.
        for (uint32_t i = 0; i < pi->swapchainCount; i++)
            if (SwapchainData* o = swapchainOf(pi->pSwapchains[i])) o->hasPrev = false;
        return d->QueuePresentKHR(queue, pi);
    }

    rp6fg::LiveControl* lc = live();
    const bool on = wantedOn();
    if (s->passive || !on) {
        // Paused: frames pass through. A passive swapchain is the app's own and
        // gets its pNext untouched; an active one keeps FIFO until re-created.
        s->hasPrev = false;
        VkResult pr;
        if (s->passive) {
            pr = d->QueuePresentKHR(queue, pi);
        } else {
            VkPresentInfoKHR rp = *pi;
            ChainStrip strip(&rp.pNext, {VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_EXT});
            pr = d->QueuePresentKHR(queue, &rp);
        }
        publishStats(s, lc, 1, 0, 0, 0);
        if (s->passive == !on) {
            recreateSettled(s);
            return pr;
        }
        return requestRecreate(s, pi, pr);
    }

    const DeviceFns& vk = d->vk;
    const uint32_t src = pi->pImageIndices[0];
    const int p = s->parity;

    // Submit A: copy the app's frame (then release it), analyse. Issued before
    // the acquire so the GPU works while this thread waits for vsync.
    VkPipelineStageFlags stackStages[16];
    std::vector<VkPipelineStageFlags> heapStages;
    VkPipelineStageFlags* appStages = stackStages;
    if (pi->waitSemaphoreCount > 16) {
        heapStages.resize(pi->waitSemaphoreCount);
        appStages = heapStages.data();
    }
    for (uint32_t i = 0; i < pi->waitSemaphoreCount; i++) appStages[i] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkCommandBuffer copyCbs[3];
    uint32_t nCopy = 0;
    if (!s->initDone) copyCbs[nCopy++] = s->init;
    copyCbs[nCopy++] = s->cp(src, p);
    copyCbs[nCopy++] = s->analyze[p];
    // One VkSubmitInfo = one kernel submit. realDone fires after analyze too,
    // which costs nothing: the real frame queues behind the generated ones,
    // and those need analyze anyway.
    const uint64_t valueA = s->timelineValue + 1;
    const uint64_t valuesA[2] = {0, valueA};
    const VkSemaphore signalsA[2] = {s->realDone[src], s->timeline};
    VkTimelineSemaphoreSubmitInfo tsA{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    tsA.signalSemaphoreValueCount = 2;
    tsA.pSignalSemaphoreValues = valuesA;
    VkSubmitInfo a{VK_STRUCTURE_TYPE_SUBMIT_INFO, &tsA};
    a.waitSemaphoreCount = pi->waitSemaphoreCount;
    a.pWaitSemaphores = pi->pWaitSemaphores;
    a.pWaitDstStageMask = appStages;
    a.commandBufferCount = nCopy;
    a.pCommandBuffers = copyCbs;
    a.signalSemaphoreCount = 2;
    a.pSignalSemaphores = signalsA;
    const uint64_t sa0 = nowNs();
    VkResult r = vk.QueueSubmit(queue, 1, &a, VK_NULL_HANDLE);
    uint64_t submitNs = nowNs() - sa0;
    if (r != VK_SUCCESS) return r;
    s->timelineValue = valueA;  // only counted once really submitted, or teardown would wait forever
    s->initDone = true;

    // Acquire targets for the generated frames: this is the vsync wait.
    // How many frames to generate. Adaptive: the game's own time per frame
    // (from our return to its next present, minus the time it sat blocked in
    // its own acquires, i.e. vsync back-pressure) decides how many refreshes a
    // real frame must cover. Schmitt trigger: go up as soon as a frame no
    // longer fits its cadence, go down only when it fits one refresh fewer
    // with a 0.35-refresh margin; each needs 5 consecutive votes.
    const uint64_t blocked = s->appAcquireNs.exchange(0, std::memory_order_relaxed);
    const uint32_t hz = lc ? liveLoad(lc->refreshHz) : 120;
    const double vblankNs = 1e9 / static_cast<double>(hz >= 30 && hz <= 240 ? hz : 120);
    if (s->lastReturnNs && s->hasPrev) {
        const uint64_t span = t0 - s->lastReturnNs;
        // Clamp outliers (a hitch or loading stall must not throw k to max).
        const double appNs = std::min(static_cast<double>(span > blocked ? span - blocked : 0),
                                      vblankNs * (s->maxGen() + 2));
        s->appNsAvg = s->appNsAvg > 0 ? s->appNsAvg * 0.8 + appNs * 0.2 : appNs;
    }
    int k = s->maxGen();
    if (lc && liveLoad(lc->adaptive)) {
        const double frames = s->appNsAvg / vblankNs;  // refreshes of pure game work
        const int cadence = s->k + 1;
        if (frames + 0.1 > cadence && s->k < s->maxGen()) {
            s->votesDown = 0;
            if (++s->votesUp >= 5) {
                s->k = std::clamp(static_cast<int>(std::ceil(frames + 0.1)) - 1, s->k + 1, s->maxGen());
                s->votesUp = 0;
            }
        } else if (frames + 0.35 <= cadence - 1 && s->k > 0) {
            s->votesUp = 0;
            if (++s->votesDown >= 5) {
                s->k--;
                s->votesDown = 0;
            }
        } else {
            s->votesUp = s->votesDown = 0;
        }
        if (s->k != s->loggedK) {
            logf("adaptive: x%d (game %.1f ms/frame)", s->k + 1, s->appNsAvg / 1e6);
            s->loggedK = s->k;
        }
        k = s->k;
    }
    const int phases = s->hasPrev ? k : 0;
    uint32_t tgt[3];
    VkSemaphore acq[3];
    int got = 0;
    const uint64_t a0 = nowNs();
    for (int ph = 0; ph < phases; ph++) {
        VkSemaphore sem = s->acquired[s->acquireIdx];
        VkResult ar = d->AcquireNextImageKHR(d->device, s->swapchain, UINT64_MAX, sem, VK_NULL_HANDLE, &tgt[got]);
        if (ar != VK_SUCCESS) logResult("acquire", ar);
        if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR) break;
        s->acquireIdx = (s->acquireIdx + 1) % static_cast<uint32_t>(s->acquired.size());
        acq[got++] = sem;
    }
    const uint64_t acqNs = nowNs() - a0;

    // Submit B: every generated frame in one VkSubmitInfo.
    if (got) {
        VkCommandBuffer cbs[3];
        VkSemaphore signals[4];
        const VkPipelineStageFlags acqStages[3] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
        for (int i = 0; i < got; i++) {
            cbs[i] = s->gen(tgt[i], p, SwapchainData::slot(k, i + 1));
            signals[i] = s->genDone[tgt[i]];
        }
        signals[got] = s->timeline;
        const uint64_t valueB = s->timelineValue + 1;
        const uint64_t waitValues[3] = {0, 0, 0};
        uint64_t values[4] = {0, 0, 0, 0};
        values[got] = valueB;
        VkTimelineSemaphoreSubmitInfo tsB{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
        tsB.waitSemaphoreValueCount = static_cast<uint32_t>(got);
        tsB.pWaitSemaphoreValues = waitValues;
        tsB.signalSemaphoreValueCount = static_cast<uint32_t>(got + 1);
        tsB.pSignalSemaphoreValues = values;
        VkSubmitInfo b{VK_STRUCTURE_TYPE_SUBMIT_INFO, &tsB};
        b.waitSemaphoreCount = static_cast<uint32_t>(got);
        b.pWaitSemaphores = acq;
        b.pWaitDstStageMask = acqStages;
        b.commandBufferCount = static_cast<uint32_t>(got);
        b.pCommandBuffers = cbs;
        b.signalSemaphoreCount = static_cast<uint32_t>(got + 1);
        b.pSignalSemaphores = signals;
        const uint64_t sb0 = nowNs();
        r = vk.QueueSubmit(queue, 1, &b, VK_NULL_HANDLE);
        submitNs += nowNs() - sb0;
        if (r != VK_SUCCESS) return r;
        s->timelineValue = valueB;
    }

    static const char* dumpDir = getenv("RP6FG_DUMP");
    static uint64_t dumpAt = static_cast<uint64_t>(envInt("RP6FG_DUMP_AT", 300, 2, 1 << 30));
    static uint64_t globalFrame = 0;  // across swapchain re-creations
    if (dumpDir && s->hasPrev) {
        ++globalFrame;
        // Debug only: creating /dev/shm/rp6fg-dump-now triggers a dump on the next frame.
        if (unlink("/dev/shm/rp6fg-dump-now") == 0) dumpAt = globalFrame + 1;
        if (globalFrame == dumpAt) dumpFrame(s, queue, p);
        if (globalFrame == dumpAt + 1) dumpNext(s, queue, p);
    }

    const uint64_t p0 = nowNs();
    for (int i = 0; i < got; i++) {
        VkPresentInfoKHR gp{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        gp.waitSemaphoreCount = 1;
        gp.pWaitSemaphores = &s->genDone[tgt[i]];
        gp.swapchainCount = 1;
        gp.pSwapchains = &s->swapchain;
        gp.pImageIndices = &tgt[i];
        VkResult gr = d->QueuePresentKHR(queue, &gp);
        if (gr != VK_SUCCESS) logResult("present(generated)", gr);
    }
    {
        // Keep the app's pNext (present ids, present fences) but drop what would
        // break pacing or describe damage relative to the wrong frame.
        VkPresentInfoKHR rp = *pi;
        ChainStrip strip(&rp.pNext, {VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_EXT,
                                     VK_STRUCTURE_TYPE_PRESENT_REGIONS_KHR});
        rp.waitSemaphoreCount = 1;
        rp.pWaitSemaphores = &s->realDone[src];
        r = d->QueuePresentKHR(queue, &rp);
    }
    if (r != VK_SUCCESS) logResult("present(real)", r);
    const uint64_t p1 = nowNs();

    s->acquireNs += acqNs;
    s->submitNs += submitNs;
    s->presentNs += p1 - p0;
    const uint64_t own = (p1 - t0) - acqNs - submitNs - (p1 - p0);
    s->cpuNs += own;
    if (own > s->cpuMaxNs) s->cpuMaxNs = own;
    if (s->frame % 600 == 599) {
        logf("frames %llu: layer own cpu avg %.1f us (max %.1f us); driver: submit %.1f us, "
             "present %.2f ms, acquire %.2f ms",
             static_cast<unsigned long long>(s->frame + 1), s->cpuNs / 600.0 / 1000.0, s->cpuMaxNs / 1000.0,
             s->submitNs / 600.0 / 1000.0, s->presentNs / 600.0 / 1e6, s->acquireNs / 600.0 / 1e6);
        s->cpuNs = s->cpuMaxNs = s->acquireNs = s->submitNs = s->presentNs = 0;
    }
    s->parity ^= 1;
    s->hasPrev = true;
    s->frame++;
    s->lastReturnNs = nowNs();

    publishStats(s, lc, 1 + static_cast<uint64_t>(got), own, submitNs, got + 1);
    // A new multiplier needs a different image count: ask the app to re-create.
    if (wantedMult() != s->mult) return requestRecreate(s, pi, r);
    recreateSettled(s);
    return r;
}

// ---- dispatch ---------------------------------------------------------------------

PFN_vkVoidFunction intercept(const char* name) {
#define HOOK(fn) if (!strcmp(name, "vk" #fn)) return reinterpret_cast<PFN_vkVoidFunction>(fn)
    HOOK(CreateInstance);
    HOOK(DestroyInstance);
    HOOK(CreateDevice);
    HOOK(DestroyDevice);
    HOOK(CreateSwapchainKHR);
    HOOK(DestroySwapchainKHR);
    HOOK(AcquireNextImageKHR);
    HOOK(AcquireNextImage2KHR);
    HOOK(QueuePresentKHR);
#undef HOOK
    return nullptr;
}

} // namespace

RP6FG_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL rp6fg_GetDeviceProcAddr(VkDevice device, const char* name);

RP6FG_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL rp6fg_GetInstanceProcAddr(VkInstance instance, const char* name) {
    if (!strcmp(name, "vkGetInstanceProcAddr")) return reinterpret_cast<PFN_vkVoidFunction>(rp6fg_GetInstanceProcAddr);
    if (!strcmp(name, "vkGetDeviceProcAddr")) return reinterpret_cast<PFN_vkVoidFunction>(rp6fg_GetDeviceProcAddr);
    if (auto f = intercept(name)) return f;
    if (!instance) return nullptr;
    InstanceData* d = instanceOf(key(instance));
    return d ? d->gipa(instance, name) : nullptr;
}

RP6FG_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL rp6fg_GetDeviceProcAddr(VkDevice device, const char* name) {
    if (!strcmp(name, "vkGetDeviceProcAddr")) return reinterpret_cast<PFN_vkVoidFunction>(rp6fg_GetDeviceProcAddr);
    DeviceData* d = deviceOf(key(device));
    if (d && d->active) {
        // Never advertise a hook for something the driver below does not have.
        if (!strcmp(name, "vkAcquireNextImage2KHR") && !d->AcquireNextImage2KHR) return nullptr;
        if (auto f = intercept(name)) return f;
    } else if (!strcmp(name, "vkDestroyDevice")) {
        return reinterpret_cast<PFN_vkVoidFunction>(DestroyDevice);
    }
    return d ? d->gdpa(device, name) : nullptr;
}

RP6FG_EXPORT VKAPI_ATTR VkResult VKAPI_CALL rp6fg_NegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* v) {
    if (v->loaderLayerInterfaceVersion < 2) return VK_ERROR_INITIALIZATION_FAILED;
    v->loaderLayerInterfaceVersion = 2;
    v->pfnGetInstanceProcAddr = rp6fg_GetInstanceProcAddr;
    v->pfnGetDeviceProcAddr = rp6fg_GetDeviceProcAddr;
    v->pfnGetPhysicalDeviceProcAddr = nullptr;
    return VK_SUCCESS;
}
