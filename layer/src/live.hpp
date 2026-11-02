// Live control block shared with the Decky plugin through a small tmpfs file,
// /dev/shm/rp6fg-live-<uid>, mapped into memory: reading it on the present
// path is a plain memory load, no system call.
// Layout is little-endian and mirrored in the plugin's main.py.
#pragma once
#include <cstdint>

namespace rp6fg {

constexpr uint32_t kLiveMagic = 0x46365052;  // "RP6F"
constexpr uint32_t kLiveVersion = 2;

struct LiveControl {
    uint32_t magic;
    uint32_t version;
    // Written by the plugin.
    uint32_t enabled;     // 0 = pass frames through untouched, 1 = generate
    uint32_t mult;        // 2..4, the maximum in adaptive mode; a change makes the app re-create its swapchain
    // Written by the layer about once a second.
    uint32_t appid;       // SteamAppId of the presenting game, 0 if unknown
    uint32_t width;
    uint32_t height;
    uint32_t activeMult;  // multiplier of the live swapchain, 0 = passthrough
    float realFps;        // app frames per second
    float outFps;         // frames presented per second (real + generated)
    float cpuUs;          // layer's own CPU per app frame
    float submitUs;       // driver submit CPU per app frame
    uint64_t heartbeat;   // CLOCK_MONOTONIC ns of the last stats write
    // Written by the plugin (version 2).
    uint32_t adaptive;    // 1 = fit the generated frames to the game's frame time, 0 = always mult
    uint32_t refreshHz;   // display refresh rate used by adaptive mode
};
static_assert(sizeof(LiveControl) == 64, "layout shared with main.py");

} // namespace rp6fg
