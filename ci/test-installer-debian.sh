#!/bin/bash
# Local end-to-end test of the Debian installer path (no CI minutes spent).
# Runs packaging/aur/install.sh in a bare debian:stable-slim container,
# then verifies the installed compositor.
#
# If the host has /dev/dri, it is passed through so the test does a full
# headless start; otherwise only version/libs/installed-files are checked
# (scenefx cannot start without a DRM node — same rule as ci/render-smoke.sh;
# even `sway -C` needs the renderer, server.c inits it first).
#
# Usage: ci/test-installer-debian.sh [repo-root]
# Needs a local docker daemon and the changes committed (the installer
# under test builds them via SWAYFS_SRC_DIR + git archive).
set -eu
ROOT="${1:-$(cd "$(dirname "$0")/.." && pwd)}"

DRI=""
if [ -e /dev/dri/renderD128 ] || [ -e /dev/dri/card0 ] || [ -e /dev/dri/card1 ]; then
	DRI="--device /dev/dri:/dev/dri"
fi

# shellcheck disable=SC2086
docker run --rm -v "$ROOT:/test/repo:ro" $DRI \
	-e SWAYFS_BRANCH=main -e SWAYFS_SRC_DIR=/test/repo \
	debian:stable-slim bash -c '
set -e
bash /test/repo/packaging/aur/install.sh
echo "=== binary ==="
/usr/local/bin/sway --version
if ldd /usr/local/bin/sway | grep -q "not found"; then
	echo "MISSING LIBRARIES"; ldd /usr/local/bin/sway | grep "not found"; exit 1
fi
echo "=== start check ==="
export XDG_RUNTIME_DIR=/tmp/rt && mkdir -p "$XDG_RUNTIME_DIR" && chmod 700 "$XDG_RUNTIME_DIR"
export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1 \
	WLR_RENDERER=gles2
if [ -e /dev/dri/card0 ] || [ -e /dev/dri/card1 ] || [ -e /dev/dri/renderD128 ]; then
	echo "(DRM node present: full headless run)"
	code=0
	timeout 15 /usr/local/bin/sway -c /dev/null -d > /tmp/sway-smoke.log 2>&1 || code=$?
	echo "sway exit: $code (124 = stayed alive 15s)"
	tail -3 /tmp/sway-smoke.log
	test "$code" -eq 124
else
	echo "(no DRM node: install manifest check)"
	test -x /usr/local/bin/sway
	test -f /usr/local/etc/sway/config
	test -f /usr/local/share/man/man1/sway.1
	echo "manifest OK"
fi
echo INSTALLER-DEBIAN-OK
'
