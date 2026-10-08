#!/bin/bash
# Headless rendering regression test for sway(fx).
#
# Usage: ci/render-smoke.sh <build-dir>
#
# Renders a real client (foot) on a nested headless sway and inspects the
# framebuffer with grim. This catches the class of bugs that IPC-only tests
# cannot see, in particular the workspace-fade regression where switching
# empty -> populated left the new workspace fully transparent (present,
# arranged, input working, but invisible).
#
# Requires a DRM render node (scenefx has no pixman fallback): without one the
# test is skipped, matching ci/asan-smoke.sh.
set -u

BUILD="${1:?usage: render-smoke.sh <build-dir>}"
SWAY="$BUILD/sway/sway"
MSG="$(command -v swaymsg || echo /usr/sbin/swaymsg)"
GRIM="$(command -v grim || echo /usr/sbin/grim)"
FOOT="$(command -v foot || echo /usr/sbin/foot)"
PPMSTAT="$(dirname "$0")/ppm-stats.py"

fail() { echo "FAIL: $*"; [ -n "${TMP:-}" ] && { echo "-- sway log --"; tail -30 "$TMP/sway.log"; }; exit 1; }

[ -x "$SWAY" ] || fail "$SWAY not found"
for t in "$MSG" "$GRIM" "$FOOT"; do [ -x "$t" ] || { echo "SKIP: missing $t"; exit 0; }; done
command -v jq >/dev/null || { echo "SKIP: jq not found"; exit 0; }
command -v python3 >/dev/null || { echo "SKIP: python3 not found"; exit 0; }
if ! compgen -G "/dev/dri/renderD*" >/dev/null 2>&1; then
	echo "SKIP: no DRM render node; scenefx needs one for the headless test"
	exit 0
fi

TMP=""; RT=""; PID=""; FPID=""

start_sway() { # $1 = animation_duration_ms
	TMP="$(mktemp -d)"; RT="$TMP/rt"; mkdir -p "$RT"; chmod 700 "$RT"
	CONF="$TMP/config"
	cat > "$CONF" <<EOF
default_border normal
gaps inner 0
gaps outer 0
font monospace 12
animation_duration_ms $1
output HEADLESS-1 pos 0 0 res 800x600
client.focused #ff0000 #ff0000 #ffffff #ff0000 #ff0000
client.focused_inactive #ff0000 #ff0000 #ffffff #ff0000 #ff0000
client.unfocused #ff0000 #ff0000 #ffffff #ff0000 #ff0000
client.urgent #ff0000 #ff0000 #ffffff #ff0000 #ff0000
EOF
	unset SWAYSOCK
	export XDG_RUNTIME_DIR="$RT" WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 \
		WLR_LIBINPUT_NO_DEVICES=1 LIBGL_ALWAYS_SOFTWARE=1 WLR_RENDERER=gles2
	"$SWAY" -c "$CONF" -d >"$TMP/sway.log" 2>&1 &
	PID=$!
	export SWAYSOCK=""
	for _ in $(seq 1 40); do
		SWAYSOCK=$(ls "$RT"/sway-ipc.*.sock 2>/dev/null | head -1)
		[ -n "$SWAYSOCK" ] && break
		kill -0 "$PID" 2>/dev/null || fail "sway died on startup"
		sleep 0.25
	done
	[ -n "$SWAYSOCK" ] || fail "no IPC socket"
	export WAYLAND_DISPLAY=""
	for _ in $(seq 1 40); do
		WAYLAND_DISPLAY=$(ls "$RT"/wayland-* 2>/dev/null | head -1 | sed 's#.*/##')
		[ -n "$WAYLAND_DISPLAY" ] && break
		sleep 0.25
	done
	[ -n "$WAYLAND_DISPLAY" ] || fail "no wayland display for clients"
	export WAYLAND_DISPLAY
	# A config error makes sway pop up a swaynag panel (which would pollute the
	# framebuffer analysis), so treat it as a hard failure.
	if grep -q "Error(s) loading config" "$TMP/sway.log"; then
		fail "sway config error"
	fi
}

stop_sway() {
	[ -n "$FPID" ] && kill "$FPID" 2>/dev/null
	[ -n "$PID" ] && kill "$PID" 2>/dev/null
	[ -n "$FPID" ] && wait "$FPID" 2>/dev/null
	[ -n "$PID" ] && wait "$PID" 2>/dev/null
	[ -n "$TMP" ] && rm -rf "$TMP"
	TMP=""; PID=""; FPID=""
}

open_foot() {
	"$FOOT" --title RENDER_TEST >/dev/null 2>&1 &
	FPID=$!
	for _ in $(seq 1 40); do
		[ "$("$MSG" -t get_tree | jq '[..|objects|select(.pid?)]|length')" -ge 1 ] && break
		sleep 0.25
	done
	sleep 1
}

shot() { "$GRIM" -o HEADLESS-1 -t ppm "$1" 2>/dev/null || fail "grim failed"; }
count() {
	if [ -n "${3:-}" ]; then
		python3 "$PPMSTAT" "$1" "$2" "$3" 2>/dev/null
	else
		python3 "$PPMSTAT" "$1" "$2" 2>/dev/null
	fi
}

# The red titlebar/border is the visibility canary: with the workspace at
# alpha 0 the whole thing (including decorations) disappears.
test_visible() { # $1 ppm $2 label
	local red; red=$(count "$1" red)
	echo "  $2: red=$red"
	[ "$red" -gt 1000 ] || fail "$2: window not visible (red=$red)"
}

check_scenario() { # $1 anim_ms  $2 label
	echo "-- scenario: $2 (animation_duration_ms=$1) --"
	start_sway "$1"
	open_foot
	"$MSG" workspace 2 >/dev/null; sleep 0.8
	shot "$TMP/empty.ppm"
	local empty_red; empty_red=$(count "$TMP/empty.ppm" red)
	echo "  empty workspace: red=$empty_red"
	# Populated workspace must be hidden while empty one is focused.
	[ "$empty_red" -lt 1000 ] || fail "$2: previous workspace still visible on empty workspace"

	"$MSG" workspace 1 >/dev/null; sleep 0.8
	shot "$TMP/back.ppm"
	# Regression: leaving an empty workspace destroys it, which used to leave
	# the returning workspace stuck at alpha 0.
	test_visible "$TMP/back.ppm" "$2: after empty -> populated switch"

	# Round-trip a few more times.
	for i in 1 2 3; do
		"$MSG" workspace 2 >/dev/null; sleep 0.5
		"$MSG" workspace 1 >/dev/null; sleep 0.6
	done
	shot "$TMP/back2.ppm"
	test_visible "$TMP/back2.ppm" "$2: after repeated round-trips"
	stop_sway
}

check_hidpi() {
	echo "-- scenario: HiDPI (scale 2) --"
	start_sway 250
	open_foot
	"$MSG" output HEADLESS-1 scale 2 >/dev/null; sleep 1
	shot "$TMP/hidpi.ppm"
	local red; red=$(count "$TMP/hidpi.ppm" red)
	echo "  scale2: red=$red"
	[ "$red" -gt 1000 ] || fail "HiDPI: window not visible (red=$red)"

	# Title text must be rasterized: look for near-white pixels in the titlebar
	# strip (rows 0..titlebar_height*scale). Get the titlebar height from the
	# view rect (logical) and scale it.
	local th
	th=$("$MSG" -t get_tree | jq -r '[..|objects|select(.pid?)][0].rect.y // 0')
	local strip=$(( th * 2 ))
	[ "$strip" -gt 0 ] || strip=140
	local text; text=$(count "$TMP/hidpi.ppm" white "$strip")
	echo "  scale2 title text pixels (y<$strip): $text"
	[ "$text" -gt 20 ] || fail "HiDPI: title text missing (white=$text)"
	stop_sway
}

trap 'stop_sway' EXIT

check_scenario 250 "animated"
check_scenario 0 "instant"
check_hidpi

echo "PASS: rendering regression checks passed"
