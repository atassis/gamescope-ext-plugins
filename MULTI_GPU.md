# Upscaling on a second GPU

**Status: not yet run on a machine with two GPUs.** Everything below follows from how the pieces
work, and each step says how to check it, but I have only tested the plugin on the same GPU that
gamescope uses. If you try it, the log lines in step 4 are what I would like to see.

## The idea

Three things run on a GPU when you play through gamescope:

1. **the game**, rendering at a low resolution,
2. **the upscale** from that resolution to your screen's,
3. **gamescope's compositing and display**.

Normally 2 and 3 run on the same GPU as each other. With the `vulkan_fsr1` plugin, the upscale can run
on a different GPU: for example the game and the display on a discrete GPU, and FSR1 on the integrated
GPU that would otherwise sit idle. Each frame crosses between the GPUs twice (in to the upscaler, and
back out), so whether this is faster than upscaling on the game's GPU depends on your hardware.
FSR1 itself is cheap: expect a small gain at best, and measure it (step 5).

## 1. Find your GPUs

    vulkaninfo --summary

For each GPU, note `deviceName`, `deviceUUID` (for the plugin) and `vendorID:deviceID` (for gamescope,
written like `1002:1586`). Both GPUs need `VK_EXT_external_memory_dma_buf`; the plugin installer
checks this and lists which ones have it.

## 2. Install, choosing the upscaling GPU

    ./install.sh --device <deviceUUID of the GPU that should upscale>

Without `--device`, the installer picks the first GPU that is not GPU0, since GPU0 is usually the one
rendering the game. You can also override it per game:
`-F external:vulkan_fsr1:uuid:<deviceUUID>`.

## 3. Launch options in Steam

    gamescope-ext --prefer-vk-device <vendorID:deviceID> -w 1280 -h 720 -W 2560 -H 1440 -f -F external:vulkan_fsr1 -- env DRI_PRIME=<n> %command%

- `--prefer-vk-device` picks the GPU gamescope composites and displays on. Use the GPU your monitor is
  plugged into.
- `DRI_PRIME` picks the GPU the game renders on (Mesa drivers: `DRI_PRIME=1`, or the PCI address as
  `DRI_PRIME=pci-0000_03_00_0`). It goes after `--`, so it applies to the game and not to gamescope.
  On NVIDIA's driver use `__NV_PRIME_RENDER_OFFLOAD=1 __GLX_VENDOR_LIBRARY_NAME=nvidia` instead.
- `-w`/`-h` is the game's resolution, `-W`/`-H` your screen's.

## 4. Check it is doing what you asked

Steam does not keep gamescope's output, so run the same line from a terminal first, with
`GAMESCOPE_EXTERNAL_LOG=1` in front and a game (or `vkcube --wsi xcb`) in place of `%command%`.
You should see:

    external_upscaler: loaded .../vulkan_fsr1/libgamescope_vulkan_fsr1.so: device "vulkan:<the GPU you chose>" kind 2
    vulkan: external frame 120 (early): submit() blocked caller 0.4 ms, submit -> shown 6.1 ms, commit done -> shown 6.1 ms

The device name must be the GPU you meant to upscale on. If the plugin cannot run, gamescope stops
instead of upscaling some other way, and its last lines say why (`external upscaler "vulkan_fsr1":
...; stopping`).

## 5. Is it worth it?

Compare the game's frame rate with `-F external:vulkan_fsr1` against `-F fsr` (gamescope's own FSR1 on
its own GPU), at the same sizes. `submit -> shown` in the log is how long the upscale on the other GPU
takes, including both crossings.
