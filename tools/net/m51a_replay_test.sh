#!/usr/bin/env bash
# m51a_replay_test.sh - the M5.1a done test (docs/ONLINE_MULTIPLAYER.md, section 8):
# "a 10-minute 2P Battle log replays on one machine to identical checksums,
# twice in a row, headless".
#
# Usage: tools/net/m51a_replay_test.sh [-b BUILD_DIR] [-d DISPLAY_NUMBER] [-o OUT_DIR]
#                                      [-t TICKS] [-s VS_STAGE] [-a on|off] [-f 1|2] [-c]
#   -b  build directory holding mppm            (default ./build-pc)
#   -d  Xvfb display number for xvfb-run -n     (default 96)
#   -o  output directory                         (default /var/tmp/mppm-online/verify)
#   -t  ticks in the synthetic log (30 per s)   (default 18000 = 10 minutes)
#   -s  2P Battle stage index                    (default 0)
#   -a  audio on|off; off sets MPPM_AUDIO=0      (default on)
#   -f  input log format version: 2 for M5.1b builds, 1 for M5.1a builds (default 2)
#   -c  check only: do not launch the game, check the files already in OUT_DIR
#       (r1/r2/neg .cksum and .mpnl, exit codes from rN.exit when present)
#
# Run from the repo root on the host. The game runs under Xvfb with an isolated
# HOME inside OUT_DIR; the disc path comes from MPPM_TEST_DISC (required).
# Exit 0 only if every check passed. The M5.1b determinism matrix is tools/net/desync_ci.sh.

set -u

BUILD_DIR="./build-pc"
DISPLAY_NUM=96
OUT_DIR="/var/tmp/mppm-online/verify"
TICKS=18000
STAGE=0
AUDIO="on"
FORMAT=2
CHECK_ONLY=0
DISC="${MPPM_TEST_DISC:-}" # path to your own disc image (required: set MPPM_TEST_DISC)
RUN_TIMEOUT=1500
NEG_AFTER=600 # ticks the negative control runs past its flipped tick

usage() {
	sed -n '2,21p' "$0" | sed 's/^# \{0,1\}//'
	exit 2
}

while getopts ":b:d:o:t:s:a:f:ch" opt; do
	case "$opt" in
	b) BUILD_DIR="$OPTARG" ;;
	d) DISPLAY_NUM="$OPTARG" ;;
	o) OUT_DIR="$OPTARG" ;;
	t) TICKS="$OPTARG" ;;
	s) STAGE="$OPTARG" ;;
	a) AUDIO="$OPTARG" ;;
	f) FORMAT="$OPTARG" ;;
	c) CHECK_ONLY=1 ;;
	h) usage ;;
	*) usage ;;
	esac
done

case "$TICKS" in '' | *[!0-9]*) echo "bad -t: $TICKS" >&2; exit 2 ;; esac
case "$STAGE" in '' | *[!0-9]*) echo "bad -s: $STAGE" >&2; exit 2 ;; esac
case "$DISPLAY_NUM" in '' | *[!0-9]*) echo "bad -d: $DISPLAY_NUM" >&2; exit 2 ;; esac
case "$AUDIO" in on | off) ;; *) echo "bad -a: $AUDIO (on or off)" >&2; exit 2 ;; esac
case "$FORMAT" in 1 | 2) ;; *) echo "bad -f: $FORMAT (1 or 2)" >&2; exit 2 ;; esac

NETLOG="tools/net/netlog.py"
if [ ! -f "$NETLOG" ]; then
	echo "run from the repo root ($NETLOG not found)" >&2
	exit 2
fi

HOME_DIR="$OUT_DIR/home"
FAILS=0
CHILD_PID=""
CACHE_EXISTED=0

pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; FAILS=$((FAILS + 1)); }
info() { echo "      $*"; }

mkdir -p "$OUT_DIR" "$HOME_DIR/.local/share" "$HOME_DIR/.config" || exit 2
[ -d "$HOME_DIR/.cache" ] && CACHE_EXISTED=1
MARKER="$OUT_DIR/.started"
: >"$MARKER"

cleanup() {
	if [ -n "$CHILD_PID" ] && kill -0 "$CHILD_PID" 2>/dev/null; then
		kill -TERM "$CHILD_PID" 2>/dev/null
		wait "$CHILD_PID" 2>/dev/null
	fi
	CHILD_PID=""
	# core files this run left behind (newer than the start marker)
	find "$OUT_DIR" . -maxdepth 2 -type f \( -name core -o -name 'core.[0-9]*' \) \
		-newer "$MARKER" -delete 2>/dev/null
	if [ "$CACHE_EXISTED" = 0 ] && [ -d "$HOME_DIR/.cache" ]; then
		rm -rf -- "$HOME_DIR/.cache"
	fi
	rm -f -- "$MARKER"
}
trap 'cleanup; exit 130' INT TERM
trap cleanup EXIT

# run_game NAME INPUT [extra args...]: replay INPUT, record OUT_DIR/NAME.mpnl,
# checksum log OUT_DIR/NAME.cksum, console OUT_DIR/NAME.log, exit code
# OUT_DIR/NAME.exit. Sets RC.
run_game() {
	local name="$1" input="$2"
	shift 2
	local audio_env=()
	[ "$AUDIO" = off ] && audio_env=(MPPM_AUDIO=0)
	rm -f -- "$OUT_DIR/$name.mpnl" "$OUT_DIR/$name.cksum" "$OUT_DIR/$name.exit"
	# the previous run's Xvfb may still hold the display; SDL then fails
	# with "x11 not available", so wait for its lock file to go away
	local w=0
	while [ -e "/tmp/.X$DISPLAY_NUM-lock" ] && [ "$w" -lt 50 ]; do
		sleep 0.2
		w=$((w + 1))
	done
	sleep 1
	info "running $name (timeout ${RUN_TIMEOUT}s, display :$DISPLAY_NUM, audio $AUDIO)"
	local t0=$SECONDS
	local attempt
	for attempt in 1 2; do
		env -u WAYLAND_DISPLAY -u XDG_RUNTIME_DIR -u MPPM_MODS_DIR -u MPPM_NO_MODS GDK_BACKEND=x11 \
			DBUS_SESSION_BUS_ADDRESS=unix:path=/nonexistent SDL_VIDEODRIVER=x11 SDL_AUDIODRIVER=dummy \
			HOME="$HOME_DIR" XDG_DATA_HOME="$HOME_DIR/.local/share" \
			XDG_CONFIG_HOME="$HOME_DIR/.config" XDG_CACHE_HOME="$HOME_DIR/.cache" \
			MPPM_SKIP_LAUNCHER=1 "${audio_env[@]}" \
			timeout "$RUN_TIMEOUT" xvfb-run -n "$DISPLAY_NUM" -s "-screen 0 1280x960x24" \
			"$BUILD_DIR/mppm" "$DISC" --replay "$input" --record "$OUT_DIR/$name.mpnl" \
			--checksum-log "$OUT_DIR/$name.cksum" --headless-ticks "$@" \
			>"$OUT_DIR/$name.log" 2>&1 </dev/null &
		CHILD_PID=$!
		wait "$CHILD_PID"
		RC=$?
		CHILD_PID=""
		# an Xvfb start-up race aborts SDL before the game runs; retry once
		if [ "$RC" = 134 ] && [ "$attempt" = 1 ] && grep -q "x11 not available" "$OUT_DIR/$name.log"; then
			info "$name: Xvfb not ready (x11 not available), retrying"
			sleep 2
			continue
		fi
		break
	done
	echo "$RC" >"$OUT_DIR/$name.exit"
	info "$name exit $RC after $((SECONDS - t0)) s (log $OUT_DIR/$name.log)"
}

# exit code of an earlier run (check-only mode), or 0 if unknown
saved_rc() {
	if [ -f "$OUT_DIR/$1.exit" ]; then
		cat "$OUT_DIR/$1.exit"
	else
		echo 0
	fi
}

IN="$OUT_DIR/in.mpnl"
NEG_IN="$OUT_DIR/neg_in.mpnl" # not neg.mpnl: run_game records into that path

# ---- step 2: the synthetic two-player log ---------------------------------
if python3 "$NETLOG" synth --seed 20261122 --synth-seed 7 --ticks "$TICKS" \
	--vs-stage "$STAGE" --format-version "$FORMAT" -o "$IN" >/dev/null; then
	pass "synth $TICKS ticks, stage $STAGE, format $FORMAT -> $IN"
else
	fail "synth input log"
	exit 1
fi

# ---- step 3: two replays ---------------------------------------------------
if [ "$CHECK_ONLY" = 0 ]; then
	if [ ! -x "$BUILD_DIR/mppm" ]; then
		fail "no executable at $BUILD_DIR/mppm"
		exit 1
	fi
	if [ ! -f "$DISC" ]; then
		fail "disc not found: $DISC (set MPPM_TEST_DISC)"
		exit 1
	fi
	if [ -e "/tmp/.X$DISPLAY_NUM-lock" ]; then
		fail "display :$DISPLAY_NUM is in use (pick another with -d)"
		exit 1
	fi
	run_game r1 "$IN"
	RC1=$RC
	run_game r2 "$IN" --checksum-verify "$OUT_DIR/r1.cksum"
	RC2=$RC
else
	info "check only: using the files in $OUT_DIR"
	RC1=$(saved_rc r1)
	RC2=$(saved_rc r2)
fi

# ---- step 4: checks ----------------------------------------------------------
R1C="$OUT_DIR/r1.cksum"
R2C="$OUT_DIR/r2.cksum"

if [ "$RC1" = 0 ] && [ "$RC2" = 0 ]; then
	pass "exit codes r1=$RC1 r2=$RC2"
else
	fail "exit codes r1=$RC1 r2=$RC2 (3 = checksum mismatch, 124 = timeout)"
fi

CMP_OK=0
if [ -s "$R1C" ] && [ -f "$R2C" ] && cmp_out=$(cmp -- "$R1C" "$R2C" 2>&1) && [ -z "$cmp_out" ]; then
	pass "cmp r1.cksum r2.cksum is silent"
	CMP_OK=1
else
	fail "cmp r1.cksum r2.cksum: ${cmp_out:-missing or empty file}"
fi

E_TICK=""
[ -f "$R1C" ] && E_TICK=$(awk '$1 == "E" { t = $2 } END { print t }' "$R1C")
if [ "$E_TICK" = "$TICKS" ]; then
	pass "r1.cksum E line at tick $E_TICK"
else
	fail "r1.cksum E line at tick '${E_TICK:-none}', expected $TICKS"
fi

C_LINES=0
[ -f "$R1C" ] && C_LINES=$(grep -c '^C ' "$R1C")
if [ $((C_LINES * 45)) -ge "$TICKS" ]; then
	pass "r1.cksum has $C_LINES C lines (need $(((TICKS + 44) / 45)))"
else
	fail "r1.cksum has $C_LINES C lines (need $(((TICKS + 44) / 45)))"
fi

if [ "$TICKS" -ge 6000 ]; then
	STATS=""
	[ -f "$R1C" ] && STATS=$(python3 "$NETLOG" stats "$R1C")
	NAVI_DISTINCT=$(printf '%s\n' "$STATS" | sed -n 's/^distinct:.* navi=\([0-9]*\).*/\1/p')
	PIKI_MAX=$(printf '%s\n' "$STATS" | sed -n 's/^max counts:.* piki=\([0-9]*\).*/\1/p')
	if [ "${NAVI_DISTINCT:-0}" -gt 100 ]; then
		pass "distinct navi hashes: $NAVI_DISTINCT"
	else
		fail "distinct navi hashes: ${NAVI_DISTINCT:-none} (need more than 100)"
	fi
	if [ "${PIKI_MAX:-0}" -gt 0 ]; then
		pass "largest Pikmin count: $PIKI_MAX"
	else
		fail "largest Pikmin count: ${PIKI_MAX:-none} (need above 0)"
	fi
else
	info "skip navi/Pikmin thresholds (TICKS $TICKS below 6000)"
fi

if [ -f "$OUT_DIR/r1.mpnl" ] && ci_out=$(python3 "$NETLOG" cmp-input "$IN" "$OUT_DIR/r1.mpnl" 2>&1); then
	pass "cmp-input in.mpnl r1.mpnl: $ci_out"
else
	fail "cmp-input in.mpnl r1.mpnl: ${ci_out:-r1.mpnl missing}"
fi

# ---- step 6: where the two runs part -----------------------------------------
if [ "$CMP_OK" = 0 ] && [ -f "$R1C" ] && [ -f "$R2C" ]; then
	info "netlog.py cmp-cksum r1.cksum r2.cksum:"
	python3 "$NETLOG" cmp-cksum "$R1C" "$R2C" | sed 's/^/      /'
fi

# ---- step 5: negative control --------------------------------------------------
# The same input with port 0 changed at one tick mid-match (A flipped, main stick mirrored),
# cut NEG_AFTER ticks later. Its
# checksum log must match r1 line for line up to that tick and then differ in a field, so the
# check sees a real simulation divergence, not a load screen of another length.
MUT_TICK=$((TICKS / 2))
NEG_END=$((MUT_TICK + NEG_AFTER))
[ "$NEG_END" -le "$TICKS" ] || NEG_END=$TICKS
if [ "$MUT_TICK" -lt 1 ] || [ "$NEG_END" -le "$MUT_TICK" ]; then
	fail "negative control: -t $TICKS is too short"
elif python3 "$NETLOG" mutate "$IN" --tick "$MUT_TICK" --port 0 --ticks "$NEG_END" \
	-o "$NEG_IN" >/dev/null; then
	if [ "$CHECK_ONLY" = 0 ]; then
		run_game neg "$NEG_IN"
		[ "$RC" = 0 ] || info "negative control run exited $RC"
	fi
	NEG_OUT=""
	NEG_RC=0
	if [ -f "$R1C" ] && [ -f "$OUT_DIR/neg.cksum" ]; then
		NEG_OUT=$(python3 "$NETLOG" cmp-cksum "$R1C" "$OUT_DIR/neg.cksum")
		NEG_RC=$?
	fi
	NEG_TICK=$(printf '%s\n' "$NEG_OUT" | sed -n '1s/^differ at line [0-9]*, tick \([0-9]*\).*/\1/p')
	# A real difference is between two C lines; a log that merely ends early (C vs E) does not
	# count, and neither does a difference before the changed tick (a replay that is not repeatable).
	if [ "$NEG_RC" = 1 ] && printf '%s\n' "$NEG_OUT" | grep -q '^  A: C ' &&
		printf '%s\n' "$NEG_OUT" | grep -q '^  B: C ' &&
		[ -n "$NEG_TICK" ] && [ "$NEG_TICK" -gt "$MUT_TICK" ]; then
		pass "negative control (port 0 changed at tick $MUT_TICK) matches before it, then differs: $(printf '%s\n' "$NEG_OUT" | head -n 1)"
		printf '%s\n' "$NEG_OUT" | tail -n 1 | sed 's/^ */      /'
	else
		fail "negative control (port 0 changed at tick $MUT_TICK): ${NEG_OUT:-no neg.cksum}"
	fi
else
	fail "negative control input log"
fi

echo
if [ "$FAILS" = 0 ]; then
	echo "M5.1a done test: PASS ($TICKS ticks, stage $STAGE, audio $AUDIO)"
	exit 0
fi
echo "M5.1a done test: FAIL ($FAILS checks failed; outputs in $OUT_DIR)"
exit 1
