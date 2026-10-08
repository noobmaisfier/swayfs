#!/bin/sh
# swayfs one-shot installer for Arch Linux.
#
# Builds swayfs from the packaging PKGBUILD (which fetches the release tag
# archive from the public repository) and installs the resulting package with
# makepkg. Nothing is downloaded or executed from an untrusted source: the
# PKGBUILD and its checksum live in this repository.
#
# Usage:
#   curl -fsSL https://gitlab.com/alaminedion/swayfs/-/raw/stable/packaging/aur/install.sh | sh
#
# Environment:
#   SWAYFS_BRANCH   branch to take the PKGBUILD from (default: stable)
#   MAKEPKG         makepkg command to run (default: makepkg)
set -eu

BRANCH="${SWAYFS_BRANCH:-stable}"
BASE="https://gitlab.com/alaminedion/swayfs/-/raw/$BRANCH/packaging/aur/swayfs"
MAKEPKG="${MAKEPKG:-makepkg}"

if [ ! -f /etc/arch-release ]; then
	echo "swayfs: this installer targets Arch Linux (/etc/arch-release not found)." >&2
	exit 1
fi

for c in curl "$MAKEPKG"; do
	if ! command -v "$c" >/dev/null 2>&1; then
		echo "swayfs: required command not found: $c" >&2
		exit 1
	fi
done

missing=""
for c in gcc make fakeroot; do
	command -v "$c" >/dev/null 2>&1 || missing="$missing $c"
done
if [ -n "$missing" ]; then
	echo "swayfs: missing build tools:$missing. Install them first:" >&2
	echo "  sudo pacman -S --needed base-devel" >&2
	exit 1
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
cd "$tmp"

echo "swayfs: fetching PKGBUILD from branch '$BRANCH'..."
curl -fsSLO "$BASE/PKGBUILD"
curl -fsSLO "$BASE/.SRCINFO"

echo "swayfs: building and installing (makepkg will ask for your sudo password)..."
"$MAKEPKG" -si --noconfirm

echo "swayfs: done. Log out and pick 'sway' at your display manager, or run 'sway' from a TTY."
