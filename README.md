# gamescope-ext plugins

Upscaler plugins for [gamescope-ext](https://github.com/atassis/gamescope-ext), a gamescope that can
hand its upscale pass to a separately installed plugin with `-F external:<name>`. The plugin ABI and
how plugins are found are described in the host's
[EXTERNAL_UPSCALER.md](https://github.com/atassis/gamescope-ext/blob/ext/EXTERNAL_UPSCALER.md).

Install the host first; each plugin builds against the ABI header and pkg-config file it installs.

## vulkan_fsr1

FSR1 (EASU + RCAS) on a Vulkan device the plugin chooses, not necessarily the one gamescope renders
with. Frames come in and go out as dma-bufs, and `submit()` returns a sync_file fence, so gamescope
does not wait for it. It runs the same FSR1 math as gamescope's built-in pass, which makes it the
reference plugin for the ABI.

    vulkan_fsr1/install.sh                     # checks, builds, installs to ~/.local
    vulkan_fsr1/install.sh --check             # checks only
    vulkan_fsr1/install.sh --device <uuid>     # pick the device (uuid as vulkaninfo prints it)
    vulkan_fsr1/install.sh --uninstall

`--prefix DIR` must match the host's prefix. Building needs meson, ninja, glslang, the Vulkan headers
and `vulkaninfo` (vulkan-tools). With one Vulkan device the installer uses it; with several it picks
the first one that is not GPU0, since GPU0 is usually rendering the game. The chosen device is
written into the plugin's manifest as its default config.

    gamescope-ext -w 1280 -h 800 -W 2560 -H 1600 -F external:vulkan_fsr1 -- %command%
    gamescope-ext ... -F external:vulkan_fsr1:uuid:<deviceUUID> -- %command%    # or index:<n>

For testing: `GAMESCOPE_VULKAN_FSR1_LIST=1` prints the devices and fails to load,
`GAMESCOPE_VULKAN_FSR1_SYNC=1` waits for the GPU inside `submit()`, and
`GAMESCOPE_VULKAN_FSR1_DECLINE=1` declines every size so gamescope falls back to its own FSR1.

Known gap: I have only run it on a machine with one GPU, so the cross-device case (import from one
GPU, upscale on another) is untested.

## Licence

BSD 2-Clause (`LICENSE`), the same as gamescope. `vulkan_fsr1/shaders/ffx_*.h` are AMD's FidelityFX
headers under their own MIT notice.
