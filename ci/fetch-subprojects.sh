#!/bin/bash
# Fetch the pinned build dependencies (wayland + wlroots + scenefx) into
# subprojects/. Meson is configured with wrap_mode=nodownload, so these must
# be present. Distros with recent system libraries (e.g. Arch) simply ignore
# the subprojects whose system dependency already satisfies the version.
set -eu
cd "$(dirname "$0")/.."

: "${WAYLAND_TAG:=1.24.0}"
: "${WLR_TAG:=0.20.0}"
: "${SCENEFX_TAG:=0.5}"

if [ -z "${SKIP_WAYLAND:-}" ] && [ ! -d subprojects/wayland ]; then
	echo "Cloning wayland $WAYLAND_TAG (Debian stable ships < 1.24)"
	git clone --depth 1 -b "$WAYLAND_TAG" \
		https://gitlab.freedesktop.org/wayland/wayland.git subprojects/wayland
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
