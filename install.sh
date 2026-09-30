#!/usr/bin/env bash
# Installs the gamescope-ext host and the vulkan_fsr1 plugin in one go, into ~/.local by default.
#
#   install.sh [--prefix DIR] [--device UUID] [--ref REF] [--src DIR] [--check]
#   install.sh --uninstall [--prefix DIR] [--src DIR]
#
# The host is cloned with its submodules from $repo at --ref (default ext) into --src (default
# ~/.cache/gamescope-ext/src) and installed by its own install.sh; vulkan_fsr1/install.sh then
# builds the plugin against it. --device and --check are passed to the plugin installer.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
repo=https://github.com/atassis/gamescope-ext.git
prefix=$HOME/.local
ref=ext
src=${XDG_CACHE_HOME:-$HOME/.cache}/gamescope-ext/src
plugin_args=()
check_only=0
uninstall=0

while [ $# -gt 0 ]; do
	case "$1" in
		--prefix) prefix=$2; shift 2 ;;
		--device) plugin_args+=(--device "$2"); shift 2 ;;
		--ref) ref=$2; shift 2 ;;
		--src) src=$2; shift 2 ;;
		--check) check_only=1; shift ;;
		--uninstall) uninstall=1; shift ;;
		*) echo "unknown argument: $1" >&2; exit 2 ;;
	esac
done
plugin_args+=(--prefix "$prefix")

fail() { echo "install: $*" >&2; exit 1; }

if [ $uninstall = 1 ]; then
	"$here/vulkan_fsr1/install.sh" --uninstall --prefix "$prefix"
	[ -x "$src/uninstall.sh" ] || fail "no host checkout at $src to uninstall from (pass --src)"
	"$src/uninstall.sh" --prefix "$prefix"
	exit 0
fi

missing=()
for tool in git meson ninja glslang pkg-config vulkaninfo; do
	command -v "$tool" >/dev/null || missing+=("$tool")
done
[ ${#missing[@]} = 0 ] || fail "missing tools: ${missing[*]} (see README.md, Install)"

if [ $check_only = 1 ]; then
	exec "$here/vulkan_fsr1/install.sh" --check "${plugin_args[@]}"
fi

if [ -d "$src/.git" ]; then
	git -C "$src" fetch --quiet origin "$ref"
	git -C "$src" checkout --quiet --detach FETCH_HEAD
else
	mkdir -p "$(dirname "$src")"
	git clone --quiet --branch "$ref" "$repo" "$src"
fi
git -C "$src" submodule update --init --recursive --quiet

"$src/install.sh" --prefix "$prefix"
"$here/vulkan_fsr1/install.sh" "${plugin_args[@]}"
echo "done: gamescope-ext -F external:vulkan_fsr1 -- %command%"
