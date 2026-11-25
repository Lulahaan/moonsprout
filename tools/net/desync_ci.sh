#!/usr/bin/env bash
# desync_ci.sh - the M5.1b determinism matrix and golden-log check (docs/online/M5_1A_HARNESS.md,
# "Determinism matrix"). Every cell replays the same synthetic 2P Battle log in another
# configuration; its checksum log must be byte-identical (cmp) to the reference run of that log.
#
# Usage: tools/net/desync_ci.sh [-b BUILD_DIR] [-d DISPLAY] [-o OUT_DIR] [-c CELL[,CELL...]]
#                               [--quick] [--update-golden] [--format 1|2]
#   -b  build directory holding mppm (and p2libm_selftest)  (default ./build-pc)
#   -d  Xvfb display number for xvfb-run -n                  (default 97)
#   -o  output directory                                     (default /var/tmp/mppm-desync)
#   -c  run only these cells (the refs they need always run); the extra names
#       neg (negative control) and libm (vendored libm self-test) select those checks
#   --quick          the headless cells of L10, the refs of Ls and Le, the negative
#                    control and the golden check (for every commit)
#   --update-golden  copy the reference checksum logs to tools/net/golden/
#   --format 1       test mode against builds before M5.1b: version 1 logs, no Le,
#                    no draw-first cell, no RNG line, libm or golden check
#   -l  list the cells and exit
#
# Run from the repo root on the host. The disc comes from MPPM_TEST_DISC (required). Each cell
# runs under Xvfb with its own home directory inside OUT_DIR. Environment: MPPM_GOLDEN=strict
# (default: a golden difference fails), warn (report only) or skip; MPPM_DESYNC_TIMEOUT (seconds
# per run, default 1500). Exit 0 only if every check passed; one PASS or FAIL line per check.

set -u

BUILD_DIR="./build-pc"
DISPLAY_NUM=97
OUT_DIR="/var/tmp/mppm-desync"
ONLY=""
QUICK=0
UPDATE_GOLDEN=0
FORMAT=2
LIST=0
DISC="${MPPM_TEST_DISC:-}" # path to your own disc image (required)
RUN_TIMEOUT="${MPPM_DESYNC_TIMEOUT:-1500}"
GOLDEN_MODE="${MPPM_GOLDEN:-strict}"
NEG_TICK=9000 # negative control: port 0 changed at this record of L10 ...
NEG_AFTER=600 # ... and the log cut this many ticks later
NO_FMA="glibc.cpu.hwcaps=-AVX2,-FMA"

usage() {
	sed -n '2,25p' "$0" | sed 's/^# \{0,1\}//'
	exit 2
}

while [ $# -gt 0 ]; do
	case "$1" in
	-b) BUILD_DIR="${2:?-b needs a value}"; shift 2 ;;
	-d) DISPLAY_NUM="${2:?-d needs a value}"; shift 2 ;;
	-o) OUT_DIR="${2:?-o needs a value}"; shift 2 ;;
	-c) ONLY="${2:?-c needs a value}"; shift 2 ;;
	--quick) QUICK=1; shift ;;
	--update-golden) UPDATE_GOLDEN=1; shift ;;
	--format) FORMAT="${2:?--format needs a value}"; shift 2 ;;
	--format=*) FORMAT="${1#--format=}"; shift ;;
	-l) LIST=1; shift ;;
	-h | --help) usage ;;
	*) echo "unknown argument: $1" >&2; usage ;;
	esac
done

case "$DISPLAY_NUM" in '' | *[!0-9]*) echo "bad -d: $DISPLAY_NUM" >&2; exit 2 ;; esac
case "$FORMAT" in 1 | 2) ;; *) echo "bad --format: $FORMAT (1 or 2)" >&2; exit 2 ;; esac
case "$GOLDEN_MODE" in strict | warn | skip) ;; *) echo "bad MPPM_GOLDEN: $GOLDEN_MODE (strict, warn or skip)" >&2; exit 2 ;; esac
case "$RUN_TIMEOUT" in '' | *[!0-9]*) echo "bad MPPM_DESYNC_TIMEOUT: $RUN_TIMEOUT" >&2; exit 2 ;; esac
if [ "$FORMAT" = 1 ] && [ "$UPDATE_GOLDEN" = 1 ]; then
	echo "--update-golden needs format 2" >&2
	exit 2
fi

NETLOG="tools/net/netlog.py"
GOLDEN_DIR="tools/net/golden"
LIBM_EXPECTED="port/libm/selftest_expected.txt"
if [ ! -f "$NETLOG" ]; then
	echo "run from the repo root ($NETLOG not found)" >&2
	exit 2
fi

# ---- the matrix -------------------------------------------------------------------------------
# One row per cell: name|logs|screen|kind|environment|arguments
#   kind: h = headless (--headless-ticks), p = paced (real time)
#   environment and arguments are split on spaces (no value holds one)
CELLS=(
	"ref|L10 Ls Le|1280x960|h||"
	"repeat|L10 Le|1280x960|h||"
	"paced|Ls|1280x960|p||"
	"fr-original|Ls|1280x960|p|MPPM_FRAME_RATE=original MPPM_VSYNC=0|"
	"fr-120|Ls|1280x960|p|MPPM_FRAME_RATE=120 MPPM_VSYNC=0|"
	"fr-unlimited|Ls|1280x960|p|MPPM_FRAME_RATE=unlimited MPPM_VSYNC=0|"
	"audio-off|L10 Le|1280x960|h|MPPM_AUDIO=0|"
	"win-1080|L10|1920x1080|h|MPPM_WINDOW=1920x1080|"
	"win-1080-original|L10|1920x1080|h|MPPM_WINDOW=1920x1080|--cvar video.aspectRatio=original"
	"win-1080-stretch|L10|1920x1080|h|MPPM_WINDOW=1920x1080|--cvar video.aspectRatio=stretch"
	"win-ultrawide|L10|2560x1080|h|MPPM_WINDOW=2560x1080|--cvar video.aspectRatio=21:9"
	"win-640|L10|1280x960|h|MPPM_WINDOW=640x480|"
	"draw-first|L10 Le|1280x960|h||--tick-order draw"
	"no-fma|L10 Le|1280x960|h|GLIBC_TUNABLES=$NO_FMA|"
	"settings|L10|1280x960|h|MPPM_TIME_SCALE=2 MPPM_SMOOTH_MOTION=1 MPPM_ALLOW_SUNSET=1|--cvar cheat.invincibleCaptains=true --cvar cheat.infiniteSprays=true --cvar game.pauseInMenu=true"
	"rng-perturb|L10 Le|1280x960|h|MPPM_RNG_PERTURB=pres=305419896,other=2596069104|"
	"show-every|Ls|1280x960|h|MPPM_HEADLESS_SHOW_EVERY=1|"
)
# The settings cell also writes this config.json into its home (the user layer under the lock).
SETTINGS_CONFIG='{
    "$version": 0,
    "cheat.invincibleCaptains": true,
    "cheat.infiniteSprays": true,
    "cheat.daySpeed": 200,
    "game.pauseInMenu": true,
    "video.smoothMotion": true
}'

if [ "$LIST" = 1 ]; then
	printf '%-18s %-10s %-10s %-6s %s\n' cell logs screen kind "environment / arguments"
	for row in "${CELLS[@]}"; do
		IFS='|' read -r c l s k e a <<<"$row"
		printf '%-18s %-10s %-10s %-6s %s %s\n' "$c" "$l" "$s" "$k" "$e" "$a"
	done
	echo "extra checks: neg libm (and the golden check of every ref)"
	exit 0
fi

# ---- selection ----------------------------------------------------------------------------------
LOGS_ALL=(L10 Ls Le)
[ "$FORMAT" = 1 ] && LOGS_ALL=(L10 Ls)

has_word() { # has_word WORD LIST...
	local w="$1" x
	shift
	for x in "$@"; do [ "$x" = "$w" ] && return 0; done
	return 1
}

ONLY_LIST=()
if [ -n "$ONLY" ]; then
	IFS=',' read -r -a ONLY_LIST <<<"$ONLY"
	for c in "${ONLY_LIST[@]}"; do
		known=0
		for row in "${CELLS[@]}"; do [ "${row%%|*}" = "$c" ] && known=1; done
		case "$c" in neg | libm) known=1 ;; esac
		[ "$known" = 1 ] || { echo "unknown cell: $c (-l lists them)" >&2; exit 2; }
	done
fi

# want_run CELL LOG: 0 when this cell runs on this log
want_run() {
	local cell="$1" log="$2"
	has_word "$log" "${LOGS_ALL[@]}" || return 1
	if [ "$FORMAT" = 1 ] && [ "$cell" = draw-first ]; then return 1; fi
	if [ "$QUICK" = 1 ]; then
		[ "$cell" = ref ] && return 0
		[ "$log" = L10 ] || return 1
	fi
	if [ "${#ONLY_LIST[@]}" -gt 0 ] && [ "$cell" != ref ]; then
		has_word "$cell" "${ONLY_LIST[@]}" || return 1
	fi
	return 0
}
want_check() { # want_check neg|libm
	[ "${#ONLY_LIST[@]}" -eq 0 ] || has_word "$1" "${ONLY_LIST[@]}"
}

# ---- state and helpers ------------------------------------------------------------------------
FAILS=0
MATRIX_FAILS=()
GOLDEN_FAILS=()
CHILD_PID=""
RESULT_ROWS=()

pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; FAILS=$((FAILS + 1)); MATRIX_FAILS+=("$*"); }
gfail() { echo "FAIL  $*"; FAILS=$((FAILS + 1)); GOLDEN_FAILS+=("$*"); }
info() { echo "      $*"; }

mkdir -p "$OUT_DIR/home" || exit 2
MARKER="$OUT_DIR/.started"
: >"$MARKER"

cleanup() {
	if [ -n "$CHILD_PID" ] && kill -0 "$CHILD_PID" 2>/dev/null; then
		kill -TERM "$CHILD_PID" 2>/dev/null
		wait "$CHILD_PID" 2>/dev/null
	fi
	CHILD_PID=""
	find "$OUT_DIR" . -maxdepth 3 -type f \( -name core -o -name 'core.[0-9]*' \) \
		-newer "$MARKER" -delete 2>/dev/null
	rm -f -- "$MARKER"
}
trap 'cleanup; exit 130' INT TERM
trap cleanup EXIT

# run_game NAME CELL INPUT SCREEN KIND "ENV" "ARGS" [extra args...]: replay INPUT; checksum log
# OUT_DIR/NAME.cksum, console OUT_DIR/NAME.log, exit code OUT_DIR/NAME.exit. Sets RC.
run_game() {
	local name="$1" cell="$2" input="$3" screen="$4" kind="$5" envs="$6" args="$7"
	shift 7
	local home="$OUT_DIR/home/$cell"
	local -a env_arr=() arg_arr=() mode_arr=()
	[ -n "$envs" ] && read -r -a env_arr <<<"$envs"
	[ -n "$args" ] && read -r -a arg_arr <<<"$args"
	[ "$kind" = h ] && mode_arr=(--headless-ticks)
	mkdir -p "$home/.local/share/mppm" "$home/.config" || return 2
	local cache_existed=0
	[ -d "$home/.cache" ] && cache_existed=1
	if [ "$cell" = settings ]; then
		printf '%s\n' "$SETTINGS_CONFIG" >"$home/.local/share/mppm/config.json"
	fi
	rm -f -- "$OUT_DIR/$name.cksum" "$OUT_DIR/$name.exit" "$OUT_DIR/$name.mpnl"
	# the previous run's Xvfb may still hold the display; SDL then fails with
	# "x11 not available", so wait for its lock file to go away
	local w=0
	while [ -e "/tmp/.X$DISPLAY_NUM-lock" ] && [ "$w" -lt 50 ]; do
		sleep 0.2
		w=$((w + 1))
	done
	sleep 1
	info "running $name ($screen, $([ "$kind" = h ] && echo headless || echo paced)${envs:+, $envs}${args:+, $args})"
	local t0=$SECONDS attempt
	for attempt in 1 2; do
		env -u WAYLAND_DISPLAY -u XDG_RUNTIME_DIR -u MPPM_MODS_DIR -u MPPM_NO_MODS GDK_BACKEND=x11 \
			DBUS_SESSION_BUS_ADDRESS=unix:path=/nonexistent SDL_VIDEODRIVER=x11 SDL_AUDIODRIVER=dummy \
			HOME="$home" XDG_DATA_HOME="$home/.local/share" \
			XDG_CONFIG_HOME="$home/.config" XDG_CACHE_HOME="$home/.cache" \
			MPPM_SKIP_LAUNCHER=1 "${env_arr[@]}" \
			timeout "$RUN_TIMEOUT" xvfb-run -n "$DISPLAY_NUM" -s "-screen 0 ${screen}x24" \
			"$BUILD_DIR/mppm" "$DISC" --replay "$input" --checksum-log "$OUT_DIR/$name.cksum" \
			"${mode_arr[@]}" "${arg_arr[@]}" "$@" \
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
	if [ "$cache_existed" = 0 ] && [ -d "$home/.cache" ]; then
		rm -rf -- "$home/.cache"
	fi
	info "$name exit $RC after $((SECONDS - t0)) s"
}

# first difference of two checksum logs, as one line
diff_summary() {
	python3 "$NETLOG" cmp-cksum "$1" "$2" 2>&1 | awk '
		/^differ/ { d = $0 } /^  fields:/ { f = $0; sub(/^  /, "", f) }
		END { if (d != "") print d (f != "" ? "; " f : ""); else print "no difference found by cmp-cksum" }'
}

# check_rng NAME: the run's "mppm: net: rng" line must say other-in-session=0
check_rng() {
	local name="$1" line n
	[ "$FORMAT" = 1 ] && return 0
	line=$(grep 'mppm: net: rng ' "$OUT_DIR/$name.log" 2>/dev/null | grep -v selftest | tail -n 1)
	n=$(printf '%s\n' "$line" | sed -n 's/.*other-in-session=\([0-9]*\).*/\1/p')
	if [ -z "$line" ]; then
		fail "$name: no 'mppm: net: rng' line in $OUT_DIR/$name.log"
	elif [ "$n" = 0 ]; then
		pass "$name: rng other-in-session=0 (${line#*mppm: net: rng })"
	else
		fail "$name: rng other-in-session=${n:-?} (${line#*mppm: net: })"
	fi
}

# ---- preconditions ------------------------------------------------------------------------------
T_START=$SECONDS
echo "desync_ci: format $FORMAT, build $BUILD_DIR, display :$DISPLAY_NUM, out $OUT_DIR$([ "$QUICK" = 1 ] && echo ', quick')"
if [ ! -x "$BUILD_DIR/mppm" ]; then
	echo "FAIL  no executable at $BUILD_DIR/mppm"
	exit 1
fi
if [ -z "$DISC" ] || [ ! -f "$DISC" ]; then
	echo "FAIL  disc not found (set MPPM_TEST_DISC to your own disc image)"
	exit 1
fi
if [ -e "/tmp/.X$DISPLAY_NUM-lock" ]; then
	echo "FAIL  display :$DISPLAY_NUM is in use (pick another with -d)"
	exit 1
fi

# ---- logs ----------------------------------------------------------------------------------------
echo
echo "== Logs"
SYNTH=(python3 "$NETLOG" synth --seed 20261122 --synth-seed 7 --vs-stage 0 --format-version "$FORMAT")
declare -A LOG_FILE LOG_TICKS
LOG_TICKS[L10]=18000
LOG_TICKS[Ls]=3600
LOG_TICKS[Le]=9000
for log in "${LOGS_ALL[@]}"; do
	LOG_FILE[$log]="$OUT_DIR/$log.mpnl"
	extra=()
	[ "$log" = Le ] && extra=(--event 2400:2)
	if "${SYNTH[@]}" --ticks "${LOG_TICKS[$log]}" "${extra[@]}" -o "${LOG_FILE[$log]}" >/dev/null; then
		pass "synth $log (${LOG_TICKS[$log]} ticks${extra[*]:+, ${extra[*]}}, format $FORMAT)"
	else
		fail "synth $log"
		exit 1
	fi
done

# ---- matrix --------------------------------------------------------------------------------------
echo
echo "== Matrix"
for row in "${CELLS[@]}"; do
	IFS='|' read -r cell logs screen kind envs args <<<"$row"
	for log in $logs; do
		want_run "$cell" "$log" || continue
		name="${cell}_$log"
		input="${LOG_FILE[$log]}"
		if [ "$cell" = ref ]; then
			run_game "$name" "$cell" "$input" "$screen" "$kind" "$envs" "$args" --record "$OUT_DIR/$name.mpnl"
		else
			run_game "$name" "$cell" "$input" "$screen" "$kind" "$envs" "$args"
		fi
		ref="$OUT_DIR/ref_$log.cksum"
		status="PASS"
		if [ "$RC" != 0 ]; then
			fail "$name: exit code $RC (3 = checksum mismatch, 6 = RNG strict, 124 = timeout; see $OUT_DIR/$name.log)"
			status="FAIL"
		fi
		if [ "$cell" = ref ]; then
			if [ "$RC" = 0 ] && [ -s "$ref" ]; then
				pass "$name: exit 0, $(grep -c '^C ' "$ref") C lines"
			elif [ "$RC" = 0 ]; then
				fail "$name: no checksum log"
				status="FAIL"
			fi
		elif [ -s "$ref" ] && [ -f "$OUT_DIR/$name.cksum" ] && cmp -s -- "$ref" "$OUT_DIR/$name.cksum"; then
			[ "$RC" = 0 ] && pass "$name: cmp against ref_$log is silent"
		else
			fail "$name: differs from ref_$log: $(diff_summary "$ref" "$OUT_DIR/$name.cksum")"
			status="FAIL"
		fi
		check_rng "$name" || true
		RESULT_ROWS+=("$(printf '%-18s %-4s %-5s exit %-3s %s' "$cell" "$log" "$status" "$RC" \
			"$( [ "$cell" = ref ] || [ "$status" = PASS ] || diff_summary "$ref" "$OUT_DIR/$name.cksum")")")
	done
done

# ---- extra checks on the refs -----------------------------------------------------------------
echo
echo "== Reference checks"
for log in "${LOGS_ALL[@]}"; do
	ref="$OUT_DIR/ref_$log.cksum"
	[ -s "$ref" ] || continue
	ticks=${LOG_TICKS[$log]}
	e_tick=$(awk '$1 == "E" { t = $2 } END { print t }' "$ref")
	if [ "$e_tick" = "$ticks" ]; then
		pass "ref_$log: E line at tick $e_tick"
	else
		fail "ref_$log: E line at tick '${e_tick:-none}', expected $ticks"
	fi
	head1=$(head -n 1 "$ref")
	case "$head1" in
	"# mppm-cksum v$FORMAT "*) pass "ref_$log: header '$head1'" ;;
	*) fail "ref_$log: header '$head1', expected '# mppm-cksum v$FORMAT ...'" ;;
	esac
	if [ -f "$OUT_DIR/ref_$log.mpnl" ] && ci=$(python3 "$NETLOG" cmp-input "${LOG_FILE[$log]}" "$OUT_DIR/ref_$log.mpnl" 2>&1); then
		pass "ref_$log: the record path reproduced the input ($(printf '%s' "$ci" | tr '\n' ' '))"
	else
		fail "ref_$log: cmp-input of the input and the recording: ${ci:-no recording}"
	fi
done
if [ -s "$OUT_DIR/ref_L10.cksum" ]; then
	nc=$(grep -c '^C ' "$OUT_DIR/ref_L10.cksum")
	if [ "$nc" -ge 400 ]; then pass "ref_L10: $nc C lines (need 400)"; else fail "ref_L10: $nc C lines (need 400)"; fi
	stats=$(python3 "$NETLOG" stats "$OUT_DIR/ref_L10.cksum")
	navi=$(printf '%s\n' "$stats" | sed -n 's/^distinct:.* navi=\([0-9]*\).*/\1/p')
	if [ "${navi:-0}" -gt 100 ]; then
		pass "ref_L10: $navi distinct navi hashes"
	else
		fail "ref_L10: ${navi:-no} distinct navi hashes (need more than 100)"
	fi
fi
if [ -s "$OUT_DIR/ref_Le.cksum" ]; then
	if grep -q 'mppm: net: floor 1 reseed' "$OUT_DIR/ref_Le.log"; then
		pass "ref_Le: a second match started ('floor 1 reseed')"
	else
		fail "ref_Le: no 'mppm: net: floor 1 reseed' line: the match did not end and restart within 9000 ticks; move the event tick (Le is synth --event 2400:2) or lengthen the log"
	fi
fi

# ---- negative control ---------------------------------------------------------------------------
if want_check neg && [ -s "$OUT_DIR/ref_L10.cksum" ]; then
	echo
	echo "== Negative control"
	neg_in="$OUT_DIR/neg_in.mpnl"
	neg_end=$((NEG_TICK + NEG_AFTER))
	if python3 "$NETLOG" mutate "${LOG_FILE[L10]}" --tick "$NEG_TICK" --port 0 --ticks "$neg_end" -o "$neg_in" >/dev/null; then
		run_game neg_L10 neg "$neg_in" 1280x960 h "" ""
		[ "$RC" = 0 ] || info "negative control run exited $RC"
		neg_out=$(python3 "$NETLOG" cmp-cksum "$OUT_DIR/ref_L10.cksum" "$OUT_DIR/neg_L10.cksum" 2>&1)
		neg_rc=$?
		neg_t=$(printf '%s\n' "$neg_out" | sed -n 's/^differ at line [0-9]*, tick \([0-9]*\).*/\1/p' | head -n 1)
		if [ "$neg_rc" = 1 ] && printf '%s\n' "$neg_out" | grep -q '^  A: C ' &&
			printf '%s\n' "$neg_out" | grep -q '^  B: C ' && [ -n "$neg_t" ] && [ "$neg_t" -gt "$NEG_TICK" ]; then
			pass "negative control (L10, port 0 changed at record $NEG_TICK, cut at $neg_end): matches before, then differs at tick $neg_t; $(printf '%s\n' "$neg_out" | sed -n 's/^  fields: //p')"
		else
			fail "negative control: $(printf '%s' "$neg_out" | head -n 4 | tr '\n' ' ')"
		fi
	else
		fail "negative control input log"
	fi
fi

# ---- vendored libm --------------------------------------------------------------------------------
if [ "$FORMAT" = 2 ] && want_check libm; then
	echo
	echo "== libm"
	if [ -x "$BUILD_DIR/p2libm_selftest" ] && [ -f "$LIBM_EXPECTED" ]; then
		"$BUILD_DIR/p2libm_selftest" "$LIBM_EXPECTED" >"$OUT_DIR/libm_plain.txt" 2>&1
		la=$?
		GLIBC_TUNABLES="$NO_FMA" "$BUILD_DIR/p2libm_selftest" "$LIBM_EXPECTED" >"$OUT_DIR/libm_nofma.txt" 2>&1
		lb=$?
		if [ "$la" = 0 ] && [ "$lb" = 0 ]; then
			pass "p2libm_selftest exits 0 plain and with GLIBC_TUNABLES=$NO_FMA"
		else
			fail "p2libm_selftest exit codes: plain $la, no-fma $lb (see $OUT_DIR/libm_*.txt)"
		fi
		if [ "$(grep '^p2m' "$OUT_DIR/libm_plain.txt")" = "$(grep '^p2m' "$OUT_DIR/libm_nofma.txt")" ] &&
			grep -q '^p2m' "$OUT_DIR/libm_plain.txt"; then
			pass "vendored libm hashes equal with and without the tunable"
		else
			fail "vendored libm hashes change with the tunable (or are missing)"
		fi
		if [ "$(grep '^host sinf' "$OUT_DIR/libm_plain.txt")" != "$(grep '^host sinf' "$OUT_DIR/libm_nofma.txt")" ]; then
			pass "host sinf changes with the tunable (the no-fma cells exercise another libm path)"
		else
			fail "tunable has no effect on this glibc ('host sinf' lines equal); find the right name with ld.so --list-tunables"
		fi
	else
		fail "no $BUILD_DIR/p2libm_selftest or $LIBM_EXPECTED (build it: ninja -C $BUILD_DIR p2libm_selftest)"
	fi
fi

# ---- golden logs ----------------------------------------------------------------------------------
if [ "$FORMAT" = 2 ]; then
	echo
	echo "== Golden"
	for log in "${LOGS_ALL[@]}"; do
		ref="$OUT_DIR/ref_$log.cksum"
		gold="$GOLDEN_DIR/$log.cksum"
		[ -s "$ref" ] || continue
		if [ "$UPDATE_GOLDEN" = 1 ]; then
			if [ "$(cat "$OUT_DIR/ref_$log.exit" 2>/dev/null)" = 0 ]; then
				mkdir -p "$GOLDEN_DIR" && cp -- "$ref" "$gold" && pass "golden $gold updated from ref_$log"
			else
				gfail "golden $gold not updated: ref_$log did not exit 0"
			fi
			continue
		fi
		[ "$GOLDEN_MODE" = skip ] && { info "golden $log: skipped (MPPM_GOLDEN=skip)"; continue; }
		if [ ! -f "$gold" ]; then
			if [ "$GOLDEN_MODE" = strict ]; then
				gfail "golden $gold missing (make it with --update-golden)"
			else
				info "golden $gold missing (MPPM_GOLDEN=warn)"
			fi
			continue
		fi
		g_out=$(python3 "$NETLOG" cmp-cksum "$gold" "$ref" 2>&1)
		if [ $? = 0 ]; then
			pass "golden $log: ref_$log matches $gold ($(printf '%s\n' "$g_out" | tail -n 1))"
		elif [ "$GOLDEN_MODE" = strict ]; then
			gfail "golden $log: $(diff_summary "$gold" "$ref")"
		else
			echo "WARN  golden $log: $(diff_summary "$gold" "$ref")"
		fi
	done
fi

# ---- summary -------------------------------------------------------------------------------------
echo
echo "== Cells"
for r in "${RESULT_ROWS[@]}"; do echo "  $r"; done
echo
if [ "${#MATRIX_FAILS[@]}" -gt 0 ]; then
	echo "Matrix failures (${#MATRIX_FAILS[@]}):"
	for f in "${MATRIX_FAILS[@]}"; do echo "  $f"; done
fi
if [ "${#GOLDEN_FAILS[@]}" -gt 0 ]; then
	echo "Golden failures (${#GOLDEN_FAILS[@]}):"
	for f in "${GOLDEN_FAILS[@]}"; do echo "  $f"; done
	if [ "${#MATRIX_FAILS[@]}" -eq 0 ]; then
		echo "  The matrix is clean: a golden difference alone usually means another compiler or"
		echo "  toolchain made this build. Rebuild with the toolchain of the golden logs, or refresh"
		echo "  them with --update-golden after checking the change."
	fi
fi
echo "desync_ci: $([ "$FAILS" = 0 ] && echo PASS || echo "FAIL ($FAILS checks failed)") in $((SECONDS - T_START)) s; outputs in $OUT_DIR"
[ "$FAILS" = 0 ]
