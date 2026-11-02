# Instructions for AI agents

This repository installs **frame generation for the Retroid Pocket 6 (RP6)** running the Armada Linux image
(Fedora aarch64, KDE Plasma, Steam, Decky Loader): an original Vulkan layer plus a Decky plugin. Use it when a user
asks for frame generation / more fps / "Lossless Scaling"-style interpolation on the RP6.

## Preconditions (check before installing)
- `uname -m` is `aarch64`; `vulkaninfo | grep VK_QCOM_image_processing` finds the extension (Adreno + Turnip).
- `podman` available; Decky Loader installed (`~/homebrew/plugins` exists).

## Install
```sh
git clone https://github.com/VVowaz/rp6-framegen && cd rp6-framegen
sudo ./install.sh                  # optional: --user <desktop user>, --skip-build
./tools/doctor.sh                  # every line should say "ok"
```

## Rules
- Enable it per game from the plugin (adds `RP6FG=1` to the launch options). Do not remove Armada's
  `/usr/libexec/armada/armada-game-launch %command%` wrapper from launch options.
- Settings: `~/.config/rp6fg/profiles.json` (per game, appid `0` = defaults). The running game is controlled live
  through `/dev/shm/rp6fg-live-<uid>` (`tools/live.py` reads/writes it).
- Do not compare fps across launches of games that generate a new world each time; use the plugin's live
  on/off toggle in one session, or the offline bench (`out/build/rp6fg-bench`).
- `sudo ./uninstall.sh` removes the plugin and the layer.
- Read `docs/ARCHITECTURE.md` before changing the layer, the shaders or the plugin.
