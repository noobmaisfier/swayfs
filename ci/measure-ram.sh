#!/bin/bash
# Headless RAM measurement for a sway(fx) binary.
#
# Usage: ci/measure-ram.sh <sway-binary> <label> <n-clients> [effects: off|on]
#
# Starts a nested headless sway with a fixed config, opens <n-clients> foot
# terminals, lets it settle, and prints the compositor's PSS/RSS/anonymous
# memory (from /proc/<pid>/smaps_rollup), the number of mappings, whether pango
# is mapped, and the view count.
#
# This is a developer tool, not a CI gate. To compare two builds, run it once
# per binary and diff the RESULT lines; for a fair A/B both must link the same
# scenefx/wlroots and open the same number of clients. Example:
#
#   ci/measure-ram.sh build-upstream/sway/sway upstream 6 off
#   ci/measure-ram.sh build/sway/sway          fork     6 off
#
# Requires a DRM render node (scenefx has no pixman fallback), plus foot,
# swaymsg and jq.
set -u
SWAY="${1:?usage: measure-ram.sh <sway-binary> <label> <n-clients> [off|on]}"
LABEL="${2:?missing label}"
N="${3:-6}"
FX="${4:-off}"
MSG="$(command -v swaymsg || echo /usr/sbin/swaymsg)"
FOOT="$(command -v foot || echo /usr/sbin/foot)"

[ -x "$SWAY" ] || { echo "ERROR: $SWAY not found"; exit 1; }
command -v jq >/dev/null || { echo "ERROR: jq not found"; exit 1; }
if ! compgen -G "/dev/dri/renderD*" >/dev/null 2>&1; then
	echo "SKIP: no DRM render node; scenefx needs one for the headless test"
	exit 0
fi

TMP="$(mktemp -d)"; RT="$TMP/rt"; mkdir -p "$RT"; chmod 700 "$RT"
CONF="$TMP/config"
{
	echo "default_border pixel 2"
	echo "gaps inner 0"
	echo "gaps outer 0"
	echo "animation_duration_ms 0"
	echo "output HEADLESS-1 pos 0 0 res 1200x800"
	if [ "$FX" = on ]; then
		echo "blur enable"
		echo "blur_radius 8"
		echo "blur_passes 2"
		echo "shadows enable"
		echo "shadow_blur_radius 20"
		echo "corner_radius 8"
	fi
} > "$CONF"

FPIDS=()
cleanup() {
	local p
	for p in "${FPIDS[@]:-}"; do [ -n "$p" ] && kill "$p" 2>/dev/null; done
	[ -n "${PID:-}" ] && kill "$PID" 2>/dev/null
	for p in "${FPIDS[@]:-}"; do [ -n "$p" ] && wait "$p" 2>/dev/null; done
	[ -n "${PID:-}" ] && wait "$PID" 2>/dev/null
	rm -rf "$TMP"
}
trap cleanup EXIT

unset SWAYSOCK
export XDG_RUNTIME_DIR="$RT" WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 \
	WLR_LIBINPUT_NO_DEVICES=1 LIBGL_ALWAYS_SOFTWARE=1 WLR_RENDERER=gles2
"$SWAY" -c "$CONF" -d >"$TMP/sway.log" 2>&1 &
PID=$!
SOCK=""
for _ in $(seq 1 40); do
	SOCK=$(ls "$RT"/sway-ipc.*.sock 2>/dev/null | head -1)
	[ -n "$SOCK" ] && break
	kill -0 "$PID" 2>/dev/null || { echo "RESULT label=$LABEL ERROR=startup"; exit 1; }
	sleep 0.25
done
export SWAYSOCK="$SOCK"
WD=""
for _ in $(seq 1 40); do
	WD=$(ls "$RT"/wayland-* 2>/dev/null | head -1 | sed 's#.*/##')
	[ -n "$WD" ] && break
	sleep 0.25
done
export WAYLAND_DISPLAY="$WD"

for i in $(seq 1 "$N"); do
	"$FOOT" --title "m$i" >/dev/null 2>&1 &
	FPIDS+=("$!")
done
for _ in $(seq 1 60); do
	[ "$("$MSG" -t get_tree | jq '[..|objects|select(.pid?)]|length')" -ge "$N" ] && break
	sleep 0.25
done
sleep 4

pss=$(awk '/^Pss:/{print $2}' /proc/$PID/smaps_rollup)
rss=$(awk '/^Rss:/{print $2}' /proc/$PID/smaps_rollup)
anon=$(awk '/^Anonymous:/{print $2}' /proc/$PID/smaps_rollup)
heap=$(awk '/\[heap\]/{split($1,a,"-"); s+=strtonum("0x"a[2])-strtonum("0x"a[1])} END{print s/1024}' /proc/$PID/maps 2>/dev/null)
maps=$(wc -l < /proc/$PID/maps)
pango=$(grep -ci 'libpango' /proc/$PID/maps || true)
views=$("$MSG" -t get_tree | jq '[..|objects|select(.pid?)]|length')
echo "RESULT label=$LABEL fx=$FX n=$N pss=$pss rss=$rss anon=$anon heap=$heap maps=$maps pango=$pango views=$views"
