#!/bin/bash
# Build and install a pinned pixman into $PREFIX (default /usr/local).
#
# wlroots 0.20 unconditionally builds its pixman renderer, which requires
# pixman >= 0.46.0 *without* a meson fallback, so a subproject cannot satisfy
# it. Debian stable ships 0.44: install a newer pixman to the prefix instead.
# pkg-config searches the prefix first, so the newer release shadows the
# system one. Run ldconfig afterwards (done here when root-owned).
set -eu

: "${PIXMAN_TAG:=pixman-0.46.4}"
PREFIX="${PREFIX:-/usr/local}"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

echo "Cloning pixman $PIXMAN_TAG"
git clone --depth 1 -b "$PIXMAN_TAG" \
	https://gitlab.freedesktop.org/pixman/pixman.git "$tmp/pixman"

echo "Building + installing pixman $PIXMAN_TAG to $PREFIX"
# --libdir=lib: pkg-config and ldconfig both search $PREFIX/lib by default,
# while meson's Debian default ($PREFIX/lib64) is in neither search path.
meson setup "$tmp/pixman/build" "$tmp/pixman" --prefix="$PREFIX" --libdir=lib -Dtests=disabled
meson compile -C "$tmp/pixman/build"
meson install -C "$tmp/pixman/build"

if [ -w /etc/ld.so.cache ] || [ "$(id -u)" -eq 0 ]; then
	ldconfig || true
fi
ver="$(pkg-config --modversion pixman-1)"
echo "pixman-1 via pkg-config: $ver"
minor="$(echo "$ver" | cut -d. -f2)"
if [ "${minor:-0}" -lt 46 ]; then
	echo "error: expected pixman >= 0.46 via pkg-config, got $ver" >&2
	exit 1
fi
