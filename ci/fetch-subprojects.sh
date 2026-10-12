#!/bin/bash
# Fetch the pinned build dependencies (wayland + libdrm + wlroots + scenefx)
# into subprojects/. Meson is configured with wrap_mode=nodownload, so these
# must be present. Distros with recent system libraries (e.g. Arch) simply
# ignore the subprojects whose system dependency already satisfies the
# version (set SKIP_WAYLAND/SKIP_LIBDRM=1 to skip cloning entirely).
set -eu
cd "$(dirname "$0")/.."

: "${WAYLAND_TAG:=1.24.0}"
: "${WAYLAND_PROTOCOLS_TAG:=1.47}"
: "${LIBDRM_TAG:=libdrm-2.4.134}"
: "${LIBXKBCOMMON_TAG:=xkbcommon-1.9.2}"
: "${WLR_TAG:=0.20.0}"
: "${SCENEFX_TAG:=0.5}"

if [ -z "${SKIP_WAYLAND:-}" ] && [ ! -d subprojects/wayland ]; then
	echo "Cloning wayland $WAYLAND_TAG (Debian stable ships < 1.24)"
	git clone --depth 1 -b "$WAYLAND_TAG" \
		https://gitlab.freedesktop.org/wayland/wayland.git subprojects/wayland
fi

if [ -z "${SKIP_WAYLAND_PROTOCOLS:-}" ] && [ ! -d subprojects/wayland-protocols ]; then
	echo "Cloning wayland-protocols $WAYLAND_PROTOCOLS_TAG (Debian stable ships < 1.47)"
	git clone --depth 1 -b "$WAYLAND_PROTOCOLS_TAG" \
		https://gitlab.freedesktop.org/wayland/wayland-protocols.git subprojects/wayland-protocols
fi

if [ -z "${SKIP_LIBDRM:-}" ] && [ ! -d subprojects/libdrm ]; then
	echo "Cloning libdrm $LIBDRM_TAG (Debian stable ships < 2.4.129)"
	git clone --depth 1 -b "$LIBDRM_TAG" \
		https://gitlab.freedesktop.org/mesa/drm.git subprojects/libdrm
fi

if [ -z "${SKIP_LIBXKBCOMMON:-}" ] && [ ! -d subprojects/libxkbcommon ]; then
	echo "Cloning libxkbcommon $LIBXKBCOMMON_TAG (Debian stable ships < 1.8)"
	git clone --depth 1 -b "$LIBXKBCOMMON_TAG" \
		https://github.com/xkbcommon/libxkbcommon.git subprojects/libxkbcommon
fi

if [ ! -d subprojects/wlroots ]; then
	echo "Cloning wlroots $WLR_TAG"
	git clone --depth 1 -b "$WLR_TAG" \
		https://gitlab.freedesktop.org/wlroots/wlroots.git subprojects/wlroots
fi

if [ ! -d subprojects/scenefx ]; then
	echo "Cloning scenefx $SCENEFX_TAG"
	git clone --depth 1 -b "$SCENEFX_TAG" \
		https://github.com/wlrfx/scenefx subprojects/scenefx
fi

echo "subprojects ready: $(ls subprojects/ | tr '\n' ' ')"
