#!/bin/bash
# Fetch the pinned build dependencies (wlroots + scenefx) into subprojects/.
# Meson is configured with wrap_mode=nodownload, so these must be present.
set -eu
cd "$(dirname "$0")/.."

: "${WLR_TAG:=0.20.0}"
: "${SCENEFX_TAG:=0.5}"

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
