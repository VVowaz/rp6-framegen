# Architecture

## Layout

```
layer/        Vulkan layer + engine + offline bench (C++20, GLSL -> SPIR-V at build time)
  src/layer.cpp    VK_LAYER_RP6_framegen: hooks, swapchain management, present path
  src/engine.*     GPU engine: records passes into caller-provided command buffers
  src/live.hpp     shared live control block (mirrored in decky/.../main.py and tools/live.py)
  src/bench.cpp    offline bench: synthetic or dumped frames, PSNR, per-pass GPU timestamps, ir3 stats
  shaders/         luma, down, me (motion estimation), select, compose
build/        Containerfile (Fedora 44 toolchain) and build.sh
decky/        Decky plugin: main.py (backend), dist/index.js (hand-written frontend, no build step)
tools/        doctor.sh, live.py, cef.py (JS in Steam's SharedJSContext), panel_text.sh, stick_dump.py, ppm2png.py
```

## Engine (per real frame)

1. **copy**: the game's swapchain image -> `colour[p]` (p alternates; `colour[1-p]` is the previous frame).
   The game's image is handed back right away.
2. **luma**: `colour[p]` -> half-resolution R8 luma (one bilinear tap per 2x2 quad; sqrt stretches dark tones),
   then three 2x downsamples: a 4-level pyramid.
3. **motion estimation** (`me.frag`), coarse to fine, forward and backward at once (two render targets), one
   fragment per block. The cost is `textureBlockMatchSADQCOM`, the Adreno texture unit's block-match SAD.
   Candidates follow the 3DRS scheme of TV motion-compensation chips (parent-level vectors, last frame's vector,
   zero), then a full search window; finest level gets a sub-pixel parabola fit from that window. Coarsest level:
   4x4 blocks, radius 4; others: 8x8 blocks, radius 1. Loops are fully unrolled so SAD requests are issued in
   batches and their latency overlaps; large windows use a running arg-min (arrays with dynamic indexing spill
   to private memory on ir3).
4. **generate**, once per generated frame at t = j/(k+1):
   - `select.frag` at half resolution: candidate vectors from the forward and backward fields around the pixel,
     projected to time t; cost = one 4x4 hardware SAD between the two ends; duplicates skipped, early out on a
     near-perfect match;
   - `compose.frag` at full resolution, written straight into an acquired swapchain image (render pass, so UBWC
     compression and the tiler stay in use). An optional fallback to a plain blend on low match confidence exists
     (`EngineConfig::fallbackLo/Hi`) and is off: on real content it brings back ghosting.

Measured on the device (1920x1080, x2, GPU at 475 MHz): copy 0.25-0.4 ms, luma+down 0.2 ms, motion estimation
0.65 ms, select 0.3-0.6 ms, compose 0.45-0.65 ms; about 2.1 ms in total. UBWC matters: without it the same work
takes about 4 ms.

## Layer

* Implicit layer, enabled by `RP6FG=1`. On devices without block matching or timeline semaphores it stays
  passive. It enables `VK_QCOM_image_processing` (textureBlockMatch) and timeline semaphores on top of the app's
  device, patching and restoring the app's feature structs.
* Swapchain: FIFO forced (also in `VkSwapchainPresentModesCreateInfoEXT`), `minImageCount + mult - 1` images,
  TRANSFER_SRC + COLOR_ATTACHMENT usage. Every command buffer is recorded at creation: copy per (image, parity),
  analyze per parity, generate per (target image, parity, (k, j) slot).
* Present: submit A (wait the app's semaphores; copy + analyze; signal realDone + timeline) before acquiring, so
  the GPU works while the thread waits for vsync; acquire k images; submit B (generate, one VkSubmitInfo); present
  the generated images, then the app's image with its pNext minus present-mode and damage structs.
* Adaptive: the game's own time per frame (hook return -> next present, minus time blocked in its own acquires)
  decides how many refreshes a real frame needs; Schmitt trigger (up when a frame no longer fits, down only with
  a 0.35-refresh margin, 5 votes each).
* Live control: `/dev/shm/rp6fg-live-<uid>` (`/run/user` is not shared with Steam Runtime containers), mapped
  and read with plain loads. Multiplier change or pause/resume re-creates the swapchain: SUBOPTIMAL from present,
  then OUT_OF_DATE from the app's acquires 30 frames later (Godot only reacts to acquire). A paused swapchain is
  created exactly as the app asked.
* Teardown waits on the layer's own timeline semaphore, never on the app's queue.

## Plugin

* Backend: installs the layer into `~/.local/lib/rp6fg` and the manifest into
  `~/.local/share/vulkan/implicit_layer.d`; follows the running game (the `AppId=` argument of Steam's
  `reaper SteamLaunch` process, any library); applies its profile (generator settings first, then the panel
  refresh through `kscreen-doctor`, called only when the rate changes: about 0.6 s CPU per call); restores the
  previous refresh after the game unless the user changed it, with the pending restore persisted in
  `~/.config/rp6fg/restore.json`.
* Frontend: launch-option editing only touches `RP6FG=` before `%command%` and re-reads the options before
  writing (Armada's armada-control plugin rewrites them too).

## Testing

```sh
out/build/rp6fg-bench [--stats]                       # synthetic frames, PSNR vs the true middle frame
out/build/rp6fg-bench --size WxH --triplet prev.raw cur.raw next.raw   # real frames from RP6FG_DUMP
```
Games with per-frame dither or grain favour a plain blend in PSNR although it ghosts; judge those visually.
