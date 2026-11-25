#!/usr/bin/env bash
# latency_probe.sh - software press-to-present measurement with --latency-probe
# (docs/online/M5_1A_HARNESS.md, "Latency probe" and "Latency procedure").
#
# Usage: tools/net/latency_probe.sh [-b BUILD_DIR] [-d DISPLAY] [-o OUT_DIR] [-m vs|cave]
#                                   [-k KEY] [-n COUNT] [-- NAME=VALUE ...]
#   -b  build directory holding mppm               (default ./build-pc)
#   -d  Xvfb display number for xvfb-run -n        (default 98)
#   -o  output directory (log, screenshots, home)  (default /var/tmp/mppm-latency)
#   -m  vs:   2P Battle stage 0 (--vs-stage 0), 1280x960
#       cave: the route of the M5.1a fix-round probe from the title into the save's game
#             (with the reference save: the Day 3 field), 640x480, 60 Hz cap; needs
#             MPPM_TEST_CARD, a save card file (.gci) of your own, outside the repo
#   -k  key pressed (xdotool name; default space = A). d turns right, l is the C-stick.
#   -n  number of held presses (default 30)
#   --  extra environment assignments for the game, e.g. -- MPPM_LATE_SAMPLE=0
#
# Passed through from the caller's environment: MPPM_SMOOTH_MOTION, MPPM_TICK_ORDER,
# MPPM_LATE_SAMPLE, MPPM_LATE_SAMPLE_US. The disc comes from MPPM_TEST_DISC (required).
# Prints the last 'mppm: probe: n=' line of the run (all samples, the route's presses included in
# cave mode) and the median, minimum and maximum of the last COUNT samples (the measured presses).
# Exit 0 when a sample ended.

set -u

BUILD_DIR="./build-pc"
DISPLAY_NUM=98
OUT_DIR="/var/tmp/mppm-latency"
MODE="vs"
KEY="space"
COUNT=30
DISC="${MPPM_TEST_DISC:-}"
CARD="${MPPM_TEST_CARD:-}"
EXTRA_ENV=()

usage() {
	sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'
	exit 2
}

while [ $# -gt 0 ]; do
	case "$1" in
	-b) BUILD_DIR="${2:?-b needs a value}"; shift 2 ;;
	-d) DISPLAY_NUM="${2:?-d needs a value}"; shift 2 ;;
	-o) OUT_DIR="${2:?-o needs a value}"; shift 2 ;;
	-m) MODE="${2:?-m needs a value}"; shift 2 ;;
	-k) KEY="${2:?-k needs a value}"; shift 2 ;;
	-n) COUNT="${2:?-n needs a value}"; shift 2 ;;
	-h | --help) usage ;;
	--)
		shift
		EXTRA_ENV=("$@")
		break
		;;
	*) echo "unknown argument: $1" >&2; usage ;;
	esac
done

case "$DISPLAY_NUM" in '' | *[!0-9]*) echo "bad -d: $DISPLAY_NUM" >&2; exit 2 ;; esac
case "$COUNT" in '' | *[!0-9]*) echo "bad -n: $COUNT" >&2; exit 2 ;; esac
case "$MODE" in vs | cave) ;; *) echo "bad -m: $MODE (vs or cave)" >&2; exit 2 ;; esac
for e in "${EXTRA_ENV[@]}"; do
	case "$e" in [A-Za-z_]*=*) ;; *) echo "bad environment assignment after --: $e" >&2; exit 2 ;; esac
done
if [ ! -x "$BUILD_DIR/mppm" ]; then
	echo "no executable at $BUILD_DIR/mppm" >&2
	exit 2
fi
if [ -z "$DISC" ] || [ ! -f "$DISC" ]; then
	echo "disc not found (set MPPM_TEST_DISC to your own disc image)" >&2
	exit 2
fi
if [ "$MODE" = cave ] && { [ -z "$CARD" ] || [ ! -f "$CARD" ]; }; then
	echo "-m cave needs MPPM_TEST_CARD: a save card file (.gci) of your own" >&2
	exit 2
fi
for tool in xdotool xvfb-run; do
	command -v "$tool" >/dev/null || { echo "$tool not installed" >&2; exit 2; }
done
if [ -e "/tmp/.X$DISPLAY_NUM-lock" ]; then
	echo "display :$DISPLAY_NUM is in use (pick another with -d)" >&2
	exit 2
fi

NAME="${MODE}_${KEY}"
HOME_DIR="$OUT_DIR/home_$NAME"
LOG="$OUT_DIR/$NAME.log"
rm -rf -- "$HOME_DIR"
mkdir -p "$HOME_DIR/.local/share" "$HOME_DIR/.config" || exit 2

PASS_ENV=()
for v in MPPM_SMOOTH_MOTION MPPM_TICK_ORDER MPPM_LATE_SAMPLE MPPM_LATE_SAMPLE_US; do
	[ -n "${!v+x}" ] && PASS_ENV+=("$v=${!v}")
done

GAME_ARGS=(--latency-probe)
MODE_ENV=()
SCREEN="1280x960"
if [ "$MODE" = vs ]; then
	GAME_ARGS+=(--vs-stage 0)
else
	card_dir="$HOME_DIR/.local/share/mppm/USA/Card A"
	mkdir -p "$card_dir" || exit 2
	cp -- "$CARD" "$card_dir/" || exit 2
	MODE_ENV=(MPPM_UNLOCK_CHALLENGE=1 MPPM_FRAME_RATE=60 MPPM_VSYNC=0 MPPM_WINDOW=640x480)
fi

GAME_PID=""
cleanup() {
	if [ -n "$GAME_PID" ] && kill -0 "$GAME_PID" 2>/dev/null; then
		kill -TERM "$GAME_PID" 2>/dev/null
		wait "$GAME_PID" 2>/dev/null
	fi
	GAME_PID=""
	rm -rf -- "$HOME_DIR/.cache"
	find "$OUT_DIR" -maxdepth 2 -type f \( -name core -o -name 'core.[0-9]*' \) -delete 2>/dev/null
}
trap 'cleanup; exit 130' INT TERM
trap cleanup EXIT

echo "latency_probe: mode $MODE, key $KEY, $COUNT presses, display :$DISPLAY_NUM${PASS_ENV[*]:+, ${PASS_ENV[*]}}${EXTRA_ENV[*]:+, ${EXTRA_ENV[*]}}"
env -u WAYLAND_DISPLAY -u XDG_RUNTIME_DIR GDK_BACKEND=x11 \
	DBUS_SESSION_BUS_ADDRESS=unix:path=/nonexistent SDL_VIDEODRIVER=x11 SDL_AUDIODRIVER=dummy \
	HOME="$HOME_DIR" XDG_DATA_HOME="$HOME_DIR/.local/share" \
	XDG_CONFIG_HOME="$HOME_DIR/.config" XDG_CACHE_HOME="$HOME_DIR/.cache" \
	MPPM_SKIP_LAUNCHER=1 "${MODE_ENV[@]}" "${PASS_ENV[@]}" "${EXTRA_ENV[@]}" \
	timeout 900 xvfb-run -n "$DISPLAY_NUM" -s "-screen 0 ${SCREEN}x24" \
	"$BUILD_DIR/mppm" "$DISC" "${GAME_ARGS[@]}" >"$LOG" 2>&1 </dev/null &
GAME_PID=$!

export DISPLAY=":$DISPLAY_NUM"
WIN=""
for _ in $(seq 1 60); do
	sleep 1
	kill -0 "$GAME_PID" 2>/dev/null || break
	WIN=$(xdotool search --name '^(mppm|Moonsprout)$' 2>/dev/null | head -n 1)
	[ -n "$WIN" ] && break
done
if [ -z "$WIN" ]; then
	echo "no game window appeared (log: $LOG)" >&2
	exit 1
fi

press() { xdotool keydown --window "$WIN" "$1"; sleep 0.1; xdotool keyup --window "$WIN" "$1"; }
shot() { command -v magick >/dev/null && magick import -display "$DISPLAY" -window root "$OUT_DIR/${NAME}_$1.png" 2>/dev/null; }

if [ "$MODE" = vs ]; then
	sleep 25
else
	# The route of the M5.1a fix-round probe: Return on the title and its menu, s twice, A to
	# enter, then A through the file select and the intro until the game runs.
	sleep 8; press Return; sleep 22; shot title; press Return; sleep 4
	press s; sleep 1; press s; sleep 1; shot menu; press space; sleep 8
	press space; sleep 5; press space; sleep 25
	for _ in $(seq 1 16); do press space; sleep 2; done
	sleep 3
fi
shot start
for _ in $(seq 1 "$COUNT"); do
	press "$KEY"
	sleep 0.7
done
sleep 1
shot end
kill -TERM "$GAME_PID" 2>/dev/null
wait "$GAME_PID" 2>/dev/null
GAME_PID=""

echo "log: $LOG ($(grep -c 'mppm: probe: sim' "$LOG") samples)"
grep 'mppm: probe: on' "$LOG" | head -n 1
LINE=$(grep 'mppm: probe: n=' "$LOG" | tail -n 1)
if [ -z "$LINE" ]; then
	echo "no 'mppm: probe: n=' line (no sample ended; see $LOG)" >&2
	exit 1
fi
echo "$LINE"
grep 'mppm: probe: sim press->present' "$LOG" | tail -n "$COUNT" |
	sed -n 's/.*press->present \([0-9.]*\) ms.*/\1/p' | sort -n | awk -v want="$COUNT" '
		{ v[NR] = $1 }
		END {
			if (NR == 0) exit
			m = (NR % 2) ? v[(NR + 1) / 2] : (v[NR / 2] + v[NR / 2 + 1]) / 2
			printf "latency_probe: last %d samples (of %d presses): median %.2f ms min %.2f max %.2f\n", NR, want, m, v[1], v[NR]
		}'
