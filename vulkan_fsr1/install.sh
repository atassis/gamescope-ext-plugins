#!/usr/bin/env bash
# Checks for the gamescope-ext host's ABI header and a Vulkan device that can import dma-bufs, then
# builds and installs the vulkan_fsr1 upscaler plugin to <prefix>/share/gamescope-upscalers/vulkan_fsr1/.
#
#   install.sh [--prefix DIR] [--build-dir DIR] [--device UUID] [--check]
#   install.sh --uninstall [--prefix DIR]
#
# With one device it is used; with several, the first one that is not GPU0 (the loader's default,
# usually the one rendering the game) is picked unless --device names one. The choice is written
# to the manifest as the plugin's default config (uuid:<deviceUUID>).
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
prefix=$HOME/.local
build=${XDG_CACHE_HOME:-$HOME/.cache}/gamescope-upscalers/build-vulkan_fsr1
want=
check_only=0
uninstall=0

while [ $# -gt 0 ]; do
	case "$1" in
		--prefix) prefix=$2; shift 2 ;;
		--build-dir) build=$2; shift 2 ;;
		--device) want=$2; shift 2 ;;
		--check) check_only=1; shift ;;
		--uninstall) uninstall=1; shift ;;
		*) echo "unknown argument: $1" >&2; exit 2 ;;
	esac
done

dest=$prefix/share/gamescope-upscalers/vulkan_fsr1

if [ $uninstall = 1 ]; then
	rm -rf "$dest"
	rmdir "$(dirname "$dest")" 2>/dev/null || true
	echo "removed $dest"
	exit 0
fi

fail() { echo "vulkan_fsr1: $*" >&2; exit 1; }

command -v vulkaninfo >/dev/null || fail "vulkaninfo not found (vulkan-tools)"
export PKG_CONFIG_PATH=$prefix/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}
pkg-config --exists gamescope-external-upscaler ||
	fail "gamescope-external-upscaler.pc not found: install the gamescope-ext host into $prefix first, or set PKG_CONFIG_PATH"

# One line per device: index, uuid, dma-buf support (0/1), type, name.
devices=$(vulkaninfo 2>/dev/null | awk '
	/^GPU[0-9]+:$/ { if (gpu != "") print gpu "\t" uuid "\t" dmabuf "\t" type "\t" name;
	                 gpu = substr($1, 4, length($1) - 4); uuid = ""; dmabuf = 0; type = ""; name = "" }
	gpu != "" && /deviceUUID/ && uuid == "" { uuid = $NF }
	gpu != "" && /deviceType/ && type == "" { type = $NF }
	gpu != "" && /deviceName/ && name == "" { sub(/.*= /, ""); name = $0 }
	gpu != "" && /VK_EXT_external_memory_dma_buf/ { dmabuf = 1 }
	END { if (gpu != "") print gpu "\t" uuid "\t" dmabuf "\t" type "\t" name }')

[ -n "$devices" ] || fail "no Vulkan devices"
echo "vulkan_fsr1: Vulkan devices (index, uuid, dma-buf import, type, name):"
sed 's/^/  /' <<<"$devices"

usable=$(awk -F'\t' '$3 == 1' <<<"$devices")
[ -n "$usable" ] || fail "no Vulkan device supports VK_EXT_external_memory_dma_buf"

if [ -n "$want" ]; then
	pick=$(awk -F'\t' -v u="$want" '$2 == u' <<<"$usable")
	[ -n "$pick" ] || fail "--device $want is not a listed device with dma-buf import"
elif [ "$(wc -l <<<"$usable")" -gt 1 ]; then
	pick=$(awk -F'\t' '$1 != 0' <<<"$usable" | head -1)
else
	pick=$usable
fi
uuid=$(cut -f2 <<<"$pick")
echo "vulkan_fsr1: using GPU$(cut -f1 <<<"$pick") $(cut -f5 <<<"$pick") (uuid:$uuid)"

[ $check_only = 1 ] && exit 0

reconf=; [ -d "$build/meson-private" ] && reconf=--reconfigure
meson setup $reconf "$build" "$here" --prefix "$prefix" -Ddefault_config="uuid:$uuid" >/dev/null
ninja -C "$build" >/dev/null
meson install -C "$build" --no-rebuild >/dev/null
[ -f "$dest/manifest.json" ] || fail "install did not produce $dest/manifest.json"
echo "installed to $dest; use -F external:vulkan_fsr1"
