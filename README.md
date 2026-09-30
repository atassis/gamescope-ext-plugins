# gamescope-ext plugins

Upscaler plugins for [gamescope-ext](https://github.com/atassis/gamescope-ext), a gamescope that can
hand its upscale pass to a separately installed plugin with `-F external:<name>`. The plugin ABI and
how plugins are found are described in the host's
[EXTERNAL_UPSCALER.md](https://github.com/atassis/gamescope-ext/blob/ext/EXTERNAL_UPSCALER.md).

## Install

    ./install.sh              # the host and vulkan_fsr1, into ~/.local
    ./install.sh --check      # only check for a usable Vulkan device
    ./install.sh --uninstall

`install.sh` clones the host into `~/.cache/gamescope-ext/src`, builds and installs it, then builds
the plugin against it. It takes `--prefix DIR`, `--device UUID` (see below) and `--ref REF` (the host
branch or tag, default `ext`). The host installs as `gamescope-ext`, beside and not over a system
gamescope.

Building needs git, meson, ninja, glslang, pkg-config, vulkan-tools and gamescope's own build
dependencies. On Arch and its derivatives the quickest way to get those is to install the distro's
gamescope, which pulls in every library it needs:

    sudo pacman -S --needed gamescope git meson ninja glslang vulkan-headers vulkan-tools wayland-protocols cmake

On Debian/Ubuntu `sudo apt build-dep gamescope` and on Fedora `sudo dnf builddep gamescope` should do
the same; I have only built it on Arch.

## vulkan_fsr1

FSR1 (EASU + RCAS) on a Vulkan device the plugin chooses, not necessarily the one gamescope renders
with. Frames come in and go out as dma-bufs, and `submit()` returns a sync_file fence, so gamescope
does not wait for it. It runs the same FSR1 math as gamescope's built-in pass, which makes it the
reference plugin for the ABI.

    gamescope-ext -w 1280 -h 800 -W 2560 -H 1600 -F external:vulkan_fsr1 -- %command%
    gamescope-ext ... -F external:vulkan_fsr1:uuid:<deviceUUID> -- %command%    # or index:<n>

With one Vulkan device the installer uses it; with several it picks the first one that is not GPU0,
since GPU0 is usually rendering the game, unless `--device <deviceUUID>` names one. The choice is
written into the plugin's manifest as its default config. To upscale on a different GPU from the one
running the game, see [MULTI_GPU.md](MULTI_GPU.md).

`vulkan_fsr1/install.sh` installs the plugin alone, against a host already installed in the same
`--prefix`.

`gamescope-ext-run --list` shows the plugin's config syntax and these switches. For testing:
`GAMESCOPE_VULKAN_FSR1_LIST=1` prints the devices and fails to load,
`GAMESCOPE_VULKAN_FSR1_SYNC=1` waits for the GPU inside `submit()`, and
`GAMESCOPE_VULKAN_FSR1_DECLINE=1` declines every size so gamescope falls back to its own FSR1.

Known gap: I have only run it on a machine with one GPU, so the cross-device case (import from one
GPU, upscale on another) is untested.

## Licence

BSD 2-Clause (`LICENSE`), the same as gamescope. `vulkan_fsr1/shaders/ffx_*.h` are AMD's FidelityFX
headers under their own MIT notice.
