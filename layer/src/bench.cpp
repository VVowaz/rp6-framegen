// Offline test bench for the engine: synthetic frames with known motion (or raw
// RGBA dumps), quality against the true middle frame, per-pass GPU timings and
// the driver's compiled shader statistics / ir3 disassembly.
#include "engine.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include <zlib.h>

#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { fprintf(stderr, "%s failed: %d (line %d)\n", #x, r_, __LINE__); exit(1); } } while (0)

namespace {

uint32_t W = 1920, H = 1080;

// ---- synthetic content ------------------------------------------------------

float hash2(int x, int y) {
    uint32_t h = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(y) * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return static_cast<float>((h ^ (h >> 16)) & 0xFFFF) / 65535.0f;
}

float noise(float x, float y) {
    int ix = static_cast<int>(std::floor(x)), iy = static_cast<int>(std::floor(y));
    float fx = x - ix, fy = y - iy;
    fx = fx * fx * (3 - 2 * fx);
    fy = fy * fy * (3 - 2 * fy);
    float a = hash2(ix, iy), b = hash2(ix + 1, iy), c = hash2(ix, iy + 1), d = hash2(ix + 1, iy + 1);
    return (a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy;
}

void background(float x, float y, float* rgb) {
    float n1 = noise(x / 40.0f, y / 40.0f), n2 = noise(x / 9.0f + 100, y / 9.0f), n3 = noise(x / 3.0f, y / 3.0f + 50);
    float stripes = 0.5f + 0.5f * std::sin(x * 0.07f + 3.0f * n1);
    rgb[0] = 0.15f + 0.45f * n1 + 0.25f * n2 + 0.1f * n3;
    rgb[1] = 0.2f + 0.3f * stripes + 0.3f * n2;
    rgb[2] = 0.25f + 0.4f * (1 - n1) + 0.2f * n3;
}

void foreground(float x, float y, float* rgb) {
    int cx = static_cast<int>(std::floor(x / 24.0f)), cy = static_cast<int>(std::floor(y / 24.0f));
    float chk = ((cx + cy) & 1) ? 1.0f : 0.0f;
    float n = noise(x / 5.0f + 300, y / 5.0f);
    rgb[0] = 0.85f * chk + 0.1f * n;
    rgb[1] = 0.1f + 0.2f * n;
    rgb[2] = 0.9f - 0.6f * chk;
}

const float kBg[2] = {14.0f, 6.0f};    // background motion, px/frame
const float kFg[2] = {-36.0f, 18.0f};  // foreground motion, px/frame
const float kFg0[2] = {900.0f, 400.0f};
const float kFgSize[2] = {320.0f, 220.0f};

std::vector<uint8_t> synth(float tau) {
    std::vector<uint8_t> img(W * H * 4);
    float fx = kFg0[0] + tau * kFg[0], fy = kFg0[1] + tau * kFg[1];
    for (uint32_t y = 0; y < H; y++)
        for (uint32_t x = 0; x < W; x++) {
            float px = x + 0.5f, py = y + 0.5f, c[3];
            if (px >= fx && px < fx + kFgSize[0] && py >= fy && py < fy + kFgSize[1])
                foreground(px - fx, py - fy, c);
            else
                background(px - tau * kBg[0], py - tau * kBg[1], c);
            uint8_t* o = &img[(y * W + x) * 4];
            for (int i = 0; i < 3; i++) o[i] = static_cast<uint8_t>(std::fmin(std::fmax(c[i], 0.0f), 1.0f) * 255.0f + 0.5f);
            o[3] = 255;
        }
    return img;
}

// ---- PNG output (zlib) ------------------------------------------------------

void writePng(const char* path, const std::vector<uint8_t>& rgba, uint32_t w, uint32_t h) {
    std::vector<uint8_t> raw;
    raw.reserve((w * 3 + 1) * h);
    for (uint32_t y = 0; y < h; y++) {
        raw.push_back(0);
        for (uint32_t x = 0; x < w; x++)
            for (int i = 0; i < 3; i++) raw.push_back(rgba[(y * w + x) * 4 + i]);
    }
    uLongf zlen = compressBound(raw.size());
    std::vector<uint8_t> z(zlen);
    compress2(z.data(), &zlen, raw.data(), raw.size(), 6);
    z.resize(zlen);
    FILE* f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    auto be32 = [&](uint32_t v) { uint8_t b[4] = {uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)}; fwrite(b, 1, 4, f); };
    auto chunk = [&](const char* type, const uint8_t* data, uint32_t len) {
        be32(len);
        uint32_t crc = crc32(0, reinterpret_cast<const Bytef*>(type), 4);
        fwrite(type, 1, 4, f);
        if (len) { fwrite(data, 1, len, f); crc = crc32(crc, data, len); }
        be32(crc);
    };
    static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    fwrite(sig, 1, 8, f);
    uint8_t ihdr[13] = {uint8_t(w >> 24), uint8_t(w >> 16), uint8_t(w >> 8), uint8_t(w),
                        uint8_t(h >> 24), uint8_t(h >> 16), uint8_t(h >> 8), uint8_t(h), 8, 2, 0, 0, 0};
    chunk("IHDR", ihdr, 13);
    chunk("IDAT", z.data(), static_cast<uint32_t>(z.size()));
    chunk("IEND", nullptr, 0);
    fclose(f);
}

double psnr(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, int border) {
    double se = 0;
    size_t n = 0;
    for (uint32_t y = border; y < H - border; y++)
        for (uint32_t x = border; x < W - border; x++)
            for (int i = 0; i < 3; i++) {
                double d = double(a[(y * W + x) * 4 + i]) - double(b[(y * W + x) * 4 + i]);
                se += d * d;
                n++;
            }
    double mse = se / n;
    return mse <= 0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}

// ---- Vulkan plumbing ----------------------------------------------------------

struct Ctx {
    VkInstance inst;
    VkPhysicalDevice pd;
    VkDevice dev;
    VkQueue queue;
    VkPhysicalDeviceMemoryProperties mp;
    DeviceFns vk;
    VkCommandPool pool;
    float tsPeriod;
};

struct Buffer { VkBuffer buf; VkDeviceMemory mem; void* map; };

Buffer makeBuffer(Ctx& c, VkDeviceSize size) {
    Buffer b{};
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = size;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    CHECK(c.vk.CreateBuffer(c.dev, &bci, nullptr, &b.buf));
    VkMemoryRequirements req;
    c.vk.GetBufferMemoryRequirements(c.dev, b.buf, &req);
    uint32_t type = 0;
    for (uint32_t i = 0; i < c.mp.memoryTypeCount; i++)
        if ((req.memoryTypeBits & (1u << i)) &&
            (c.mp.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) { type = i; break; }
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type;
    CHECK(c.vk.AllocateMemory(c.dev, &mai, nullptr, &b.mem));
    CHECK(c.vk.BindBufferMemory(c.dev, b.buf, b.mem, 0));
    CHECK(c.vk.MapMemory(c.dev, b.mem, 0, size, 0, &b.map));
    return b;
}

struct Tex { VkImage img; VkDeviceMemory mem; VkImageView view; };

Tex makeTex(Ctx& c, VkFormat fmt) {
    Tex t{};
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = fmt;
    ici.extent = {W, H, 1};
    ici.mipLevels = ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    CHECK(c.vk.CreateImage(c.dev, &ici, nullptr, &t.img));
    VkMemoryRequirements req;
    c.vk.GetImageMemoryRequirements(c.dev, t.img, &req);
    uint32_t type = 0;
    for (uint32_t i = 0; i < c.mp.memoryTypeCount; i++)
        if (req.memoryTypeBits & (1u << i)) { type = i; break; }
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type;
    CHECK(c.vk.AllocateMemory(c.dev, &mai, nullptr, &t.mem));
    CHECK(c.vk.BindImageMemory(c.dev, t.img, t.mem, 0));
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = t.img;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = fmt;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    CHECK(c.vk.CreateImageView(c.dev, &vci, nullptr, &t.view));
    return t;
}

VkCommandBuffer beginCmd(Ctx& c) {
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = c.pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd;
    CHECK(c.vk.AllocateCommandBuffers(c.dev, &ai, &cmd));
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    CHECK(c.vk.BeginCommandBuffer(cmd, &bi));
    return cmd;
}

void submitWait(Ctx& c, VkCommandBuffer cmd) {
    CHECK(c.vk.EndCommandBuffer(cmd));
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    CHECK(c.vk.QueueSubmit(c.queue, 1, &si, VK_NULL_HANDLE));
    CHECK(c.vk.QueueWaitIdle(c.queue));
    c.vk.FreeCommandBuffers(c.dev, c.pool, 1, &cmd);
}

void toGeneral(Ctx& c, VkCommandBuffer cmd, VkImage img) {
    VkImageMemoryBarrier ib{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    ib.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    ib.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ib.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    ib.srcQueueFamilyIndex = ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ib.image = img;
    ib.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    c.vk.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                            nullptr, 1, &ib);
}

void upload(Ctx& c, Tex& t, const std::vector<uint8_t>& px) {
    Buffer b = makeBuffer(c, px.size());
    memcpy(b.map, px.data(), px.size());
    VkCommandBuffer cmd = beginCmd(c);
    toGeneral(c, cmd, t.img);
    VkBufferImageCopy r{};
    r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.imageExtent = {W, H, 1};
    c.vk.CmdCopyBufferToImage(cmd, b.buf, t.img, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
    submitWait(c, cmd);
    c.vk.DestroyBuffer(c.dev, b.buf, nullptr);
    c.vk.FreeMemory(c.dev, b.mem, nullptr);
}

std::vector<uint8_t> download(Ctx& c, Tex& t) {
    Buffer b = makeBuffer(c, W * H * 4);
    VkCommandBuffer cmd = beginCmd(c);
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    c.vk.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0,
                            nullptr, 0, nullptr);
    VkBufferImageCopy r{};
    r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.imageExtent = {W, H, 1};
    c.vk.CmdCopyImageToBuffer(cmd, t.img, VK_IMAGE_LAYOUT_GENERAL, b.buf, 1, &r);
    submitWait(c, cmd);
    std::vector<uint8_t> out(W * H * 4);
    memcpy(out.data(), b.map, out.size());
    c.vk.DestroyBuffer(c.dev, b.buf, nullptr);
    c.vk.FreeMemory(c.dev, b.mem, nullptr);
    return out;
}

Ctx createCtx(bool stats) {
    Ctx c{};
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    CHECK(vkCreateInstance(&ici, nullptr, &c.inst));
    uint32_t n = 1;
    vkEnumeratePhysicalDevices(c.inst, &n, &c.pd);
    vkGetPhysicalDeviceMemoryProperties(c.pd, &c.mp);
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(c.pd, &props);
    c.tsPeriod = props.limits.timestampPeriod;

    VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR pexe{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR};
    pexe.pipelineExecutableInfo = VK_TRUE;
    VkPhysicalDeviceImageProcessingFeaturesQCOM ipf{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_PROCESSING_FEATURES_QCOM};
    ipf.textureBlockMatch = VK_TRUE;
    ipf.pNext = stats ? &pexe : nullptr;
    const char* exts[] = {VK_QCOM_IMAGE_PROCESSING_EXTENSION_NAME, VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME};
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &ipf;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = stats ? 2 : 1;
    dci.ppEnabledExtensionNames = exts;
    CHECK(vkCreateDevice(c.pd, &dci, nullptr, &c.dev));
    if (!c.vk.load(vkGetDeviceProcAddr, c.dev)) { fprintf(stderr, "missing device functions\n"); exit(1); }
    c.vk.GetDeviceQueue(c.dev, 0, 0, &c.queue);
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    CHECK(c.vk.CreateCommandPool(c.dev, &pci, nullptr, &c.pool));
    return c;
}

void dumpStats(Ctx& c, rp6fg::Engine& eng, const std::string& dir) {
    auto getProps = reinterpret_cast<PFN_vkGetPipelineExecutablePropertiesKHR>(vkGetDeviceProcAddr(c.dev, "vkGetPipelineExecutablePropertiesKHR"));
    auto getStats = reinterpret_cast<PFN_vkGetPipelineExecutableStatisticsKHR>(vkGetDeviceProcAddr(c.dev, "vkGetPipelineExecutableStatisticsKHR"));
    auto getIR = reinterpret_cast<PFN_vkGetPipelineExecutableInternalRepresentationsKHR>(vkGetDeviceProcAddr(c.dev, "vkGetPipelineExecutableInternalRepresentationsKHR"));
    for (auto [name, pipe] : eng.pipelines()) {
        VkPipelineInfoKHR pi{VK_STRUCTURE_TYPE_PIPELINE_INFO_KHR};
        pi.pipeline = pipe;
        uint32_t ne = 0;
        getProps(c.dev, &pi, &ne, nullptr);
        std::vector<VkPipelineExecutablePropertiesKHR> ep(ne, {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_PROPERTIES_KHR});
        getProps(c.dev, &pi, &ne, ep.data());
        for (uint32_t e = 0; e < ne; e++) {
            if (!(ep[e].stages & VK_SHADER_STAGE_FRAGMENT_BIT)) continue;
            VkPipelineExecutableInfoKHR ei{VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR};
            ei.pipeline = pipe;
            ei.executableIndex = e;
            uint32_t ns = 0;
            getStats(c.dev, &ei, &ns, nullptr);
            std::vector<VkPipelineExecutableStatisticKHR> st(ns, {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_STATISTIC_KHR});
            getStats(c.dev, &ei, &ns, st.data());
            printf("[%s]", name);
            for (auto& s : st) {
                long long v = s.format == VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_FLOAT64_KHR ? (long long)s.value.f64
                            : s.format == VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_INT64_KHR ? s.value.i64
                            : s.format == VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR ? (long long)s.value.u64 : s.value.b32;
                printf(" %s=%lld;", s.name, v);
            }
            printf("\n");
            uint32_t nr = 0;
            getIR(c.dev, &ei, &nr, nullptr);
            std::vector<VkPipelineExecutableInternalRepresentationKHR> ir(nr, {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INTERNAL_REPRESENTATION_KHR});
            getIR(c.dev, &ei, &nr, ir.data());
            std::vector<std::vector<char>> bufs(nr);
            for (uint32_t i = 0; i < nr; i++) {
                bufs[i].resize(ir[i].dataSize);
                ir[i].pData = bufs[i].data();
            }
            getIR(c.dev, &ei, &nr, ir.data());
            std::string path = dir + "/" + name + ".ir.txt";
            FILE* f = fopen(path.c_str(), "w");
            for (uint32_t i = 0; i < nr; i++)
                fprintf(f, "==== %s (%s)\n%s\n", ir[i].name, ir[i].description, bufs[i].data());
            fclose(f);
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    std::string out = ".";
    bool stats = false;
    int iters = 60;
    float fbLo = 1e9f, fbHi = 2e9f;
    std::string pairPrev, pairCur, pairTruth;  // real frames: raw BGRA dumps from the layer (RP6FG_DUMP)
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "--stats")) stats = true;
        else if (!strcmp(argv[i], "--iters") && i + 1 < argc) iters = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--size") && i + 1 < argc) sscanf(argv[++i], "%ux%u", &W, &H);
        else if (!strcmp(argv[i], "--fallback") && i + 1 < argc) sscanf(argv[++i], "%f,%f", &fbLo, &fbHi);
        else if (!strcmp(argv[i], "--pair") && i + 2 < argc) { pairPrev = argv[++i]; pairCur = argv[++i]; }
        // Real triplet: interpolate prev -> next, score against the real middle frame.
        else if (!strcmp(argv[i], "--triplet") && i + 3 < argc) {
            pairPrev = argv[++i]; pairTruth = argv[++i]; pairCur = argv[++i];
        }
    }
    const bool pair = !pairPrev.empty();
    auto loadRaw = [&](const std::string& path) {
        std::vector<uint8_t> px(W * H * 4);
        FILE* f = fopen(path.c_str(), "rb");
        if (!f || fread(px.data(), 1, px.size(), f) != px.size()) { fprintf(stderr, "cannot read %s (%ux%u)\n", path.c_str(), W, H); exit(1); }
        fclose(f);
        for (size_t i = 0; i < px.size(); i += 4) std::swap(px[i], px[i + 2]);  // BGRA -> RGBA
        return px;
    };

    Ctx c = createCtx(stats);
    const VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;
    Tex A = makeTex(c, fmt), B = makeTex(c, fmt), O = makeTex(c, fmt);

    // With --pair there is no ground truth and no history: only the cold run is meaningful.
    std::vector<uint8_t> fm2, fm1, f0, f1, truth;
    if (pair) {
        f0 = loadRaw(pairPrev);
        f1 = loadRaw(pairCur);
        fm2 = fm1 = f0;
        if (!pairTruth.empty()) truth = loadRaw(pairTruth);
    } else {
        fm2 = synth(-2.0f), fm1 = synth(-1.0f), f0 = synth(0.0f), f1 = synth(1.0f), truth = synth(0.5f);
    }
    Tex Am2 = makeTex(c, fmt), Am1 = makeTex(c, fmt);
    upload(c, Am2, fm2);
    upload(c, Am1, fm1);
    upload(c, A, f0);
    upload(c, B, f1);
    {
        VkCommandBuffer cmd = beginCmd(c);
        toGeneral(c, cmd, O.img);
        submitWait(c, cmd);
    }

    rp6fg::EngineConfig cfg;
    cfg.width = W;
    cfg.height = H;
    cfg.colorFormat = fmt;
    cfg.captureStats = stats;
    cfg.fallbackLo = fbLo;
    cfg.fallbackHi = fbHi;
    rp6fg::Engine eng;
    if (!eng.init(c.dev, &c.vk, c.mp, cfg)) { fprintf(stderr, "engine init failed\n"); return 1; }
    int tO = eng.addTarget(O.view);
    // One real frame: grab the image, analyse against the previous one.
    auto frame = [&](VkCommandBuffer cmd, VkImage img, int p) {
        eng.recordCopy(cmd, img, VK_IMAGE_LAYOUT_GENERAL, p);
        eng.recordAnalyze(cmd, p);
    };

    {
        VkCommandBuffer cmd = beginCmd(c);
        eng.recordInit(cmd);
        submitWait(c, cmd);
    }

    // Quality: a short sequence so temporal candidates exist (as in a game),
    // then generate the middle of A -> B in three modes. "cold" skips history.
    const char* modeName[] = {"interp", "vectors", "blend", "cold"};
    for (int mode : {0, 1, 2, 3}) {
        VkCommandBuffer cmd = beginCmd(c);
        eng.recordInit(cmd);  // forget history between runs
        int p = 0;
        if (mode != 3) {
            frame(cmd, Am2.img, p);
            frame(cmd, Am1.img, p ^= 1);
            p ^= 1;
        }
        frame(cmd, A.img, p);
        frame(cmd, B.img, p ^= 1);
        eng.recordGenerate(cmd, tO, p, 0.5f, mode == 3 ? 0 : mode);
        submitWait(c, cmd);
        auto img = download(c, O);
        if (mode != 1 && !truth.empty()) printf("%-7s PSNR %.2f dB (inner %.2f dB)\n", modeName[mode], psnr(img, truth, 0), psnr(img, truth, 48));
        writePng((out + "/" + modeName[mode] + ".png").c_str(), img, W, H);
    }
    if (!truth.empty()) writePng((out + "/truth.png").c_str(), truth, W, H);

    // Timing: ping-pong A/B with timestamps after every pass.
    VkQueryPoolCreateInfo qpci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qpci.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qpci.queryCount = 64;
    VkQueryPool qp;
    CHECK(c.vk.CreateQueryPool(c.dev, &qpci, nullptr, &qp));
    std::vector<const char*> labels;
    eng.onPass = [&](VkCommandBuffer cmd, const char* name) {
        c.vk.CmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qp, static_cast<uint32_t>(labels.size()) + 1);
        labels.push_back(name);
    };
    std::map<std::string, double> total;
    double frameTotal = 0;
    for (int it = 0; it < iters + 2; it++) {
        labels.clear();
        VkCommandBuffer cmd = beginCmd(c);
        c.vk.CmdResetQueryPool(cmd, qp, 0, 64);
        c.vk.CmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, qp, 0);
        bool odd = it & 1;
        frame(cmd, odd ? B.img : A.img, it & 1);
        eng.recordGenerate(cmd, tO, it & 1, 0.5f, 0);
        submitWait(c, cmd);
        if (it < 2) continue;  // warm-up, and the first frame has no motion estimation
        std::vector<uint64_t> ts(labels.size() + 1);
        CHECK(c.vk.GetQueryPoolResults(c.dev, qp, 0, static_cast<uint32_t>(ts.size()), ts.size() * 8, ts.data(), 8,
                                       VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
        for (size_t i = 0; i < labels.size(); i++) total[labels[i]] += (ts[i + 1] - ts[i]) * c.tsPeriod * 1e-6;
        frameTotal += (ts.back() - ts[0]) * c.tsPeriod * 1e-6;
    }
    printf("GPU time per real frame (%ux%u, x2), avg of %d:\n", W, H, iters);
    for (auto& [k, v] : total) printf("  %-8s %.3f ms\n", k.c_str(), v / iters);
    printf("  %-8s %.3f ms\n", "TOTAL", frameTotal / iters);

    if (stats) dumpStats(c, eng, out);
    eng.destroy();
    return 0;
}
