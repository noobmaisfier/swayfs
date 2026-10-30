#!/bin/bash
# Local end-to-end test of the Arch installer path (no CI minutes spent).
# Runs packaging/aur/install.sh as an unprivileged user in a bare
# archlinux container (only sudo preinstalled): the script must bootstrap
# curl + base-devel itself, then build and install the package.
#
# Usage: ci/test-installer-arch.sh [repo-root]
# The Arch path downloads the PKGBUILD from the pushed branch, so push
# first (or point SWAYFS_BRANCH at a branch containing it).
set -eu
ROOT="${1:-$(cd "$(dirname "$0")/.." && pwd)}"
BRANCH="${SWAYFS_BRANCH:-main}"

docker run --rm -v "$ROOT:/test/repo:ro" \
	-e SWAYFS_BRANCH="$BRANCH" \
	archlinux:latest bash -c '
set -e
pacman -Sy --noconfirm sudo >/dev/null 2>&1
useradd -m builder
echo "builder ALL=(ALL) NOPASSWD: ALL" >> /etc/sudoers
su builder -c "bash /test/repo/packaging/aur/install.sh"
echo "=== package ==="
pacman -Q swayfs
/usr/bin/sway --version
echo INSTALLER-ARCH-OK
'
