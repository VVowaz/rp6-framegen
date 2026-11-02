# RP6 FrameGen

Frame generation for the **Retroid Pocket 6** running the Armada Linux image (Fedora, KDE Plasma, Steam, Decky
Loader). An original Vulkan layer inserts interpolated frames between a game's own frames, x2 to x4, with
**per-game profiles** managed from a Decky plugin. It is written for the RP6's GPU (Adreno 740, Mesa Turnip):
motion is estimated by the texture unit's **hardware block matcher** (`VK_QCOM_image_processing`), not by shader
code, and the layer costs the game thread a few microseconds per frame.

* x2 / x3 / x4 or **adaptive**: as many generated frames as the game needs to fill every refresh
  (60 fps -> x2, 40 -> x3, 30 -> x4 at 120 Hz)
* per-game **target refresh 60 or 120 Hz**: the panel switches while the game runs and switches back after
* on/off and multiplier changes **live**, without restarting the game
* no Lossless Scaling or other proprietary parts needed

> Status: 0.1.0. Developed and tested on one device (Armada image, kernel 7.2.6, Mesa 26.2, Steam ARM, Decky
> Loader) with vkcube and Lucid Blocks (Godot, Proton ARM64). See [Known limitations](#known-limitations).

## Quick install

```sh
git clone https://github.com/VVowaz/rp6-framegen && cd rp6-framegen && sudo ./install.sh
```

Then open the Decky menu -> **RP6 FrameGen**, pick a game, turn on **Frame generation** and start the game.
AI agents: see [AGENTS.md](AGENTS.md).

## How it works

```
game frame N --copy--> luma pyramid --hardware block-match SAD--> forward/backward motion fields
                                                                    |
              per-pixel vector choice (hardware SAD, half res) <----+
                                  |
                    compose frame N-1/2 straight into the swapchain --> present, then present frame N
```

* The layer (`VK_LAYER_RP6_framegen`, implicit, enabled by `RP6FG=1` in a game's launch options) records all its
  command buffers once per swapchain. Per real frame the game thread does two queue submits, up to three
  acquires and up to four presents: no fences, no waits except the vsync acquire.
* GPU cost at 1920x1080 and x2: about 2.1 ms per real frame at the default 475 MHz GPU clock.
* The Decky plugin toggles `RP6FG=1` per game (it coexists with Armada's own launch wrapper), keeps the per-game
  profiles and drives the running game through a small shared-memory block (`/dev/shm/rp6fg-live-<uid>`).

More detail in [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Requirements

* Retroid Pocket 6 with the Armada image (`aarch64`), Mesa Turnip with `VK_QCOM_image_processing`
* Decky Loader, `podman` (the layer is built in a Fedora container), internet access for the build

## Install

```sh
git clone https://github.com/VVowaz/rp6-framegen && cd rp6-framegen
sudo ./install.sh              # options: --user NAME, --skip-build
./tools/doctor.sh              # every line should say "ok"
```

`sudo ./uninstall.sh` removes the plugin and the layer; profiles in `~/.config/rp6fg` are kept.

## Using it

In **RP6 FrameGen**:

* **Game**: the game to configure (the running one is preselected; "Default" holds the settings every game
  without its own profile inherits)
* **Frame generation**: adds or removes `RP6FG=1` in the game's launch options; applies on the next start
* **Target refresh**: 60 or 120 Hz while that game runs
* **Adaptive** and **(Max) multiplier**: each multiplier step adds a swapchain image, i.e. one frame of queue
  latency
* **Generate frames** (live): off passes the game's frames straight through

Environment variables for testing: `RP6FG_LOG=<file>`, `RP6FG_MULT=2..4`, `RP6FG_DUMP=<dir>` with
`RP6FG_DUMP_AT=<frame>` (or create `/dev/shm/rp6fg-dump-now`) to save a real frame triplet with the generated
middle frame and the vector view, `DISABLE_RP6FG=1`.

## Known limitations

* Tested on one device and two applications; games through DXVK/vkd3d-proton and x86 games through FEX are
  expected to work (the layer is a host aarch64 layer, which FEX's Vulkan thunks load) but are not verified.
* Vulkan only; OpenGL games are untouched.
* Generated frames add about one frame of latency, more with higher multipliers.
* Semi-transparent HUD elements over moving backgrounds can smear; games with per-frame dither or grain look
  softer in generated frames.
* The GPU clock limit of the active Armada power profile applies (475 MHz in "balanced").

## License

GPL-3.0, see [LICENSE](LICENSE).
