#!/bin/bash
# Headless smoke test for sway(fx) under ASan/UBSan.
# Usage: ci/asan-smoke.sh <build-dir> [timeout-seconds]
#
# Starts a nested headless sway with two outputs and exercises the paths that
# have historically crashed the animation code (workspace switches, workspace
# moves between outputs, output disable/enable cycles, IPC storm). Any
# AddressSanitizer/UBSan report or crash fails the job.
set -u

BUILD="${1:?usage: asan-smoke.sh <build-dir> [timeout]}"
WAIT="${2:-20}"
SWAY="$BUILD/sway/sway"
MSG="$BUILD/swaymsg/swaymsg"
[ -x "$SWAY" ] || { echo "FAIL: $SWAY not found"; exit 1; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
RT="$TMP/rt"; mkdir -p "$RT"; chmod 700 "$RT"
CONF="$TMP/config"
cat > "$CONF" <<'EOF'
animation_duration_ms 250
default_border none
gaps inner 0
gaps outer 0
output HEADLESS-1 pos 0 0 res 800x600
output HEADLESS-2 pos 800 0 res 800x600
EOF

unset SWAYSOCK
export XDG_RUNTIME_DIR="$RT"
export WLR_BACKENDS=headless
export WLR_HEADLESS_OUTPUTS=2
export WLR_LIBINPUT_NO_DEVICES=1
export LIBGL_ALWAYS_SOFTWARE=1
export ASAN_OPTIONS=detect_leaks=0:abort_on_error=1
export UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1

LOG="$TMP/sway.log"
"$SWAY" -c "$CONF" -d >"$LOG" 2>&1 &
PID=$!

# Wait for the IPC socket.
SOCK=""
for _ in $(seq 1 "$WAIT"); do
	SOCK=$(ls "$RT"/sway-ipc.*.sock 2>/dev/null | head -1)
	[ -n "$SOCK" ] && break
	sleep 1
done
if [ -z "$SOCK" ]; then
	echo "FAIL: sway did not start"
	tail -20 "$LOG"
	kill "$PID" 2>/dev/null
	exit 1
fi
export SWAYSOCK="$SOCK"

run() { "$MSG" "$@" >/dev/null 2>&1; }

run workspace 1; run workspace 2; run workspace 3

for i in 1 2 3; do
	echo "-- cycle $i: switches/moves/disable-enable --"
	run workspace 1; run workspace 2; run workspace 3
	run move workspace to output HEADLESS-2
	run workspace 1
	run move workspace to output HEADLESS-1
	"$MSG" output HEADLESS-2 disable >/dev/null 2>&1; sleep 0.5
	"$MSG" output HEADLESS-2 enable  >/dev/null 2>&1; sleep 0.5
	run workspace 1; run workspace 2
	kill -0 "$PID" 2>/dev/null || { echo "FAIL: sway died in cycle $i"; tail -40 "$LOG"; exit 1; }
done

echo "-- IPC storm --"
for _ in $(seq 1 30); do "$MSG" -t get_tree >/dev/null 2>&1; done

echo "-- rename --"
run rename workspace 1 to ci-renamed
run rename workspace ci-renamed to 1

if ! kill -0 "$PID" 2>/dev/null; then
	echo "FAIL: sway died at the end"
	tail -40 "$LOG"
	exit 1
fi
kill "$PID" 2>/dev/null
wait "$PID" 2>/dev/null

if grep -qiE 'AddressSanitizer|UndefinedBehaviorSanitizer|runtime error|double free|corruption' "$LOG"; then
	echo "FAIL: sanitizer report detected"
	grep -iE -A15 'AddressSanitizer|UndefinedBehaviorSanitizer|runtime error|double free|corruption' "$LOG" | head -60
	exit 1
fi

echo "PASS: no sanitizer/error report"
