#!/usr/bin/env bash
# m53a_test.sh - the done test of M5.3a part 1: a headless 4-captain 2v2 Battle (docs/online/M5_3A_NPLAYER.md,
# "Done test").
#
# Usage: tools/net/m53a_test.sh [-b BUILD_DIR] [-d DISPLAY] [-o OUT_DIR] [-B BASE] [-q] [-c CHECK[,CHECK...]] [-l]
#   -b  build directory holding mppm                     (default ./build-pc)
#   -d  Xvfb display number for xvfb-run -n              (default 86)
#   -o  output directory                                 (default /var/tmp/mppm-online/m53a)
#   -B  base commit of the lines check                   (default 61aebcb69)
#   -q  the golden check runs desync_ci.sh --quick
#   -c  run only these checks (the runs they need always run)
#   -l  list the checks and exit
#
# Run from the repo root on the host with MPPM_TEST_DISC set (every check but lines needs it).
# Each run gets its own home directory inside OUT_DIR and runs under Xvfb. Environment:
# MPPM_M53A_TIMEOUT (seconds per run, default 1500), MPPM_GOLDEN (passed on to desync_ci.sh,
# default strict). One PASS or FAIL line per check; exit 0 only if every selected check passed.

set -u

BUILD_DIR="./build-pc"
DISPLAY_NUM=86
OUT_DIR="/var/tmp/mppm-online/m53a"
BASE="61aebcb69"
QUICK=0
ONLY=""
LIST=0
DISC="${MPPM_TEST_DISC:-}"
RUN_TIMEOUT="${MPPM_M53A_TIMEOUT:-1500}"
export MPPM_GOLDEN="${MPPM_GOLDEN:-strict}"
SEED=20261122
SYNTH_SEED=7
STAGE=0
SHOT_TICK=1500
NEG_TICK=9000  # neg: port 2 of V4 changed at this record ...
NEG_END=9600   # ... and the log cut here
PERF_MAX_MS=8.0
PERF_MIN_PIKI=90

usage() {
	sed -n '2,19p' "$0" | sed 's/^# \{0,1\}//'
	exit 2
}

while [ $# -gt 0 ]; do
	case "$1" in
	-b) BUILD_DIR="${2:?-b needs a value}"; shift 2 ;;
	-d) DISPLAY_NUM="${2:?-d needs a value}"; shift 2 ;;
	-o) OUT_DIR="${2:?-o needs a value}"; shift 2 ;;
	-B) BASE="${2:?-B needs a value}"; shift 2 ;;
	-q) QUICK=1; shift ;;
	-c) ONLY="${2:?-c needs a value}"; shift 2 ;;
	-l) LIST=1; shift ;;
	-h | --help) usage ;;
	*) echo "unknown argument: $1" >&2; usage ;;
	esac
done

CHECKS=(lines selftest golden v4 activity deaths view neg shots perf)
declare -A CHECK_DESC=(
	[lines]="src/ and include/ changed only by same-line PC_ONLY(/PC_SWITCH( edits since -B; tools/net/golden unchanged"
	[selftest]="netlog.py selftest, MPPM_RNG_SELFTEST=1, MPPM_NPLAYER_SELFTEST=core|piki|vs|view print PASS and exit 0"
	[golden]="desync_ci.sh (--quick with -q) exits 0 with MPPM_GOLDEN=strict: 1-2 captains unchanged"
	[v4]="V4 (18000 ticks, 4 captains) replayed twice: identical checksum logs, recording equals the input, n=4/"
	[activity]="v4_r1's tick-18000 stats: captains 0-3 each walked >= 300 and squadmax >= 1; a captains=4 heap line"
	[deaths]="Le4 (captain 2 down at 2400, captain 0 at 4800): match goes on, then Red loses, next match has 4 captains"
	[view]="Vs4 under the stock split, --view-captain 0 and --view-captain 3: identical checksum logs"
	[neg]="V4 with port 2 changed at record 9000: matches v4_r1 up to tick 9000, then differs between two C lines"
	[shots]="paced Vs4 held at tick 1500 (split, --view-captain 2, 3): screenshots not black and different"
	[perf]="sim/pass times of l2_perf, v4_r1, v4_h10; heap free for 2 and 4 captains; v4_h10 sim avg <= 8.0 ms with at least 90 Pikmin"
)

if [ "$LIST" = 1 ]; then
	for c in "${CHECKS[@]}"; do printf '%-9s %s\n' "$c" "${CHECK_DESC[$c]}"; done
	echo "logs: V4, Vs4, Le4, L2 from netlog.py synth --seed $SEED --synth-seed $SYNTH_SEED --vs-stage $STAGE"
	exit 0
fi

case "$DISPLAY_NUM" in '' | *[!0-9]*) echo "bad -d: $DISPLAY_NUM" >&2; exit 2 ;; esac
case "$RUN_TIMEOUT" in '' | *[!0-9]*) echo "bad MPPM_M53A_TIMEOUT: $RUN_TIMEOUT" >&2; exit 2 ;; esac

NETLOG="tools/net/netlog.py"
DESYNC="tools/net/desync_ci.sh"
if [ ! -f "$NETLOG" ] || [ ! -f "$DESYNC" ]; then
	echo "run from the repo root ($NETLOG not found)" >&2
	exit 2
fi

SEL=()
if [ -n "$ONLY" ]; then
	IFS=',' read -r -a SEL <<<"$ONLY"
	for c in "${SEL[@]}"; do
		[ -n "${CHECK_DESC[$c]:-}" ] || { echo "unknown check: $c (-l lists them)" >&2; exit 2; }
	done
else
	SEL=("${CHECKS[@]}")
fi
want() { local x; for x in "${SEL[@]}"; do [ "$x" = "$1" ] && return 0; done; return 1; }

# ---- state and helpers ------------------------------------------------------------------------
FAILS=0
FAIL_LIST=()
CHILD_PID=""
declare -A RAN

pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; FAILS=$((FAILS + 1)); FAIL_LIST+=("$*"); }
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

wait_display_free() {
	local w=0
	while [ -e "/tmp/.X$DISPLAY_NUM-lock" ] && [ "$w" -lt 50 ]; do
		sleep 0.2
		w=$((w + 1))
	done
	sleep 1
}

# launch NAME ENVS ARGS...: start mppm in the background under Xvfb (CHILD_PID), console
# OUT_DIR/NAME.log. ENVS is a space-separated list of VAR=value (no value holds a space).
launch() {
	local name="$1" envs="$2"
	shift 2
	local home="$OUT_DIR/home/$name"
	local -a env_arr=()
	[ -n "$envs" ] && read -r -a env_arr <<<"$envs"
	rm -rf -- "$home"
	mkdir -p "$home/.local/share/mppm" "$home/.config" || return 2
	env -u WAYLAND_DISPLAY -u XDG_RUNTIME_DIR -u MPPM_MODS_DIR -u MPPM_NO_MODS GDK_BACKEND=x11 \
		DBUS_SESSION_BUS_ADDRESS=unix:path=/nonexistent SDL_VIDEODRIVER=x11 SDL_AUDIODRIVER=dummy \
		HOME="$home" XDG_DATA_HOME="$home/.local/share" \
		XDG_CONFIG_HOME="$home/.config" XDG_CACHE_HOME="$home/.cache" \
		MPPM_SKIP_LAUNCHER=1 "${env_arr[@]}" \
		timeout "$RUN_TIMEOUT" xvfb-run -n "$DISPLAY_NUM" -s "-screen 0 1280x960x24" \
		"$BUILD_DIR/mppm" "$@" >"$OUT_DIR/$name.log" 2>&1 </dev/null &
	CHILD_PID=$!
}

# finish NAME: wait for the run; RC and OUT_DIR/NAME.exit get its exit code
finish() {
	local name="$1"
	wait "$CHILD_PID"
	RC=$?
	CHILD_PID=""
	echo "$RC" >"$OUT_DIR/$name.exit"
	rm -rf -- "$OUT_DIR/home/$name/.cache"
}

# run_mppm NAME ENVS ARGS...: one run (once per name; later calls only set RC), one retry when
# Xvfb was not ready
run_mppm() {
	local name="$1" envs="$2"
	shift 2
	if [ -n "${RAN[$name]:-}" ]; then
		RC=$(cat "$OUT_DIR/$name.exit" 2>/dev/null || echo 255)
		return
	fi
	RAN[$name]=1
	rm -f -- "$OUT_DIR/$name.exit"
	local t0=$SECONDS attempt
	info "running $name${envs:+ ($envs)}"
	for attempt in 1 2; do
		wait_display_free
		launch "$name" "$envs" "$@"
		finish "$name"
		if [ "$RC" = 134 ] && [ "$attempt" = 1 ] && grep -q "x11 not available" "$OUT_DIR/$name.log"; then
			info "$name: Xvfb not ready (x11 not available), retrying"
			sleep 2
			continue
		fi
		break
	done
	info "$name exit $RC after $((SECONDS - t0)) s"
}

# replay NAME ENVS INPUT [args...]: headless replay with checksum log OUT_DIR/NAME.cksum
replay() {
	local name="$1" envs="$2" input="$3"
	shift 3
	[ -n "${RAN[$name]:-}" ] || rm -f -- "$OUT_DIR/$name.cksum"
	run_mppm "$name" "$envs" "$DISC" --replay "$input" --checksum-log "$OUT_DIR/$name.cksum" --headless-ticks "$@"
}

# shot NAME ARGS...: paced replay of Vs4 held at SHOT_TICK, screenshot OUT_DIR/NAME.png
shot() {
	local name="$1"
	shift
	[ -n "${RAN[$name]:-}" ] && return
	RAN[$name]=1
	rm -f -- "$OUT_DIR/$name.png"
	local t0=$SECONDS w attempt
	info "running $name (paced, screenshot at tick $SHOT_TICK${*:+, $*})"
	for attempt in 1 2; do
		wait_display_free
		launch "$name" "MPPM_HOLD_AT_TICK=$SHOT_TICK" "$DISC" --replay "$LOG_VS4" --exit-after-ticks $((SHOT_TICK + 30)) "$@"
		w=0
		while [ "$w" -lt 2400 ] && kill -0 "$CHILD_PID" 2>/dev/null; do
			if grep -q "holding tick $SHOT_TICK" "$OUT_DIR/$name.log" 2>/dev/null; then
				sleep 0.7 # the hold lasts 3 s
				magick import -display ":$DISPLAY_NUM" -window root "$OUT_DIR/$name.png" 2>/dev/null
				break
			fi
			sleep 0.25
			w=$((w + 1))
		done
		finish "$name"
		if [ "$RC" = 134 ] && [ "$attempt" = 1 ] && grep -q "x11 not available" "$OUT_DIR/$name.log"; then
			info "$name: Xvfb not ready, retrying"
			sleep 2
			continue
		fi
		break
	done
	info "$name exit $RC after $((SECONDS - t0)) s$([ -s "$OUT_DIR/$name.png" ] || echo ', no screenshot')"
}

# ae A.png B.png FUZZ: number of differing pixels (empty on error)
ae() { magick compare -fuzz "${3:-0}%" -metric AE "$1" "$2" null: 2>&1 >/dev/null | awk '{ printf "%d", $1 }'; }
mean_of() { magick "$1" -format '%[fx:mean]' info: 2>/dev/null; }
pixels_of() { magick "$1" -format '%[fx:w*h]' info: 2>/dev/null; }

# first difference of two checksum logs, as one line
diff_summary() {
	python3 "$NETLOG" cmp-cksum "$1" "$2" 2>&1 | awk '
		/^differ/ { d = $0 } /^  fields:/ { f = $0; sub(/^  /, "", f) }
		END { if (d != "") print d (f != "" ? "; " f : ""); else print "no difference found by cmp-cksum" }'
}

# rng_other NAME: other-in-session of the run's last 'mppm: net: rng' line (empty when none)
rng_other() {
	grep 'mppm: net: rng ' "$OUT_DIR/$1.log" 2>/dev/null | grep -v selftest | tail -n 1 |
		sed -n 's/.*other-in-session=\([0-9]*\).*/\1/p'
}

# bad_lines NAME...: lines with PANIC, a failed assertion or the crash screen in the runs' logs
bad_lines() {
	local n
	for n in "$@"; do grep -H -m 3 -E 'PANIC|Failed assertion|crash screen' "$OUT_DIR/$n.log" 2>/dev/null; done
}

# stats_line NAME TICK CAPTAIN: the run's 'mppm: nplayer: tick=TICK captain=CAPTAIN ...' line
stats_line() { grep "mppm: nplayer: tick=$2 captain=$3 " "$OUT_DIR/$1.log" 2>/dev/null | tail -n 1; }
field() { sed -n "s/.* $1=\([-0-9]*\).*/\1/p" <<<" $2"; } # field KEY LINE

perf_line() { grep 'mppm: net: perf ticks=' "$OUT_DIR/$1.log" 2>/dev/null | tail -n 1; }
perf_val() { sed -n "s/.* $1=\([0-9.]*\).*/\1/p" <<<" $2"; } # perf_val avg|p50|p99|max LINE (first match)
max_piki() { python3 "$NETLOG" stats "$1" 2>/dev/null | sed -n 's/^max counts:.* piki=\([0-9]*\).*/\1/p'; }

# ---- preconditions ------------------------------------------------------------------------------
T_START=$SECONDS
echo "m53a_test: build $BUILD_DIR, display :$DISPLAY_NUM, out $OUT_DIR, base $BASE, checks ${SEL[*]}"
NEED_GAME=0
for c in "${SEL[@]}"; do [ "$c" != lines ] && NEED_GAME=1; done
if [ "$NEED_GAME" = 1 ]; then
	if [ ! -x "$BUILD_DIR/mppm" ]; then
		echo "FAIL  no executable at $BUILD_DIR/mppm"
		exit 1
	fi
	if [ -e "/tmp/.X$DISPLAY_NUM-lock" ]; then
		echo "FAIL  display :$DISPLAY_NUM is in use (pick another with -d)"
		exit 1
	fi
	for tool in xvfb-run timeout python3; do
		command -v "$tool" >/dev/null || { echo "FAIL  $tool not found"; exit 1; }
	done
fi
NEED_DISC=0
for c in "${SEL[@]}"; do case "$c" in lines | selftest) ;; *) NEED_DISC=1 ;; esac; done
if [ "$NEED_DISC" = 1 ] && { [ -z "$DISC" ] || [ ! -f "$DISC" ]; }; then
	echo "FAIL  disc not found (set MPPM_TEST_DISC to your own disc image)"
	exit 1
fi
if want shots && ! command -v magick >/dev/null; then
	echo "FAIL  magick (ImageMagick 7) not found; the shots check needs it"
	exit 1
fi

# ---- logs -----------------------------------------------------------------------------------------
LOG_V4="$OUT_DIR/V4.mpnl"
LOG_VS4="$OUT_DIR/Vs4.mpnl"
LOG_LE4="$OUT_DIR/Le4.mpnl"
LOG_L2="$OUT_DIR/L2.mpnl"
if [ "$NEED_DISC" = 1 ]; then
	echo
	echo "== Logs"
	SYNTH=(python3 "$NETLOG" synth --seed "$SEED" --synth-seed "$SYNTH_SEED" --vs-stage "$STAGE")
	for spec in "V4|--ticks 18000 --players 4" "Vs4|--ticks 3600 --players 4" \
		"Le4|--ticks 9000 --players 4 --event 2400:3 --event 4800:1" "L2|--ticks 18000"; do
		IFS='|' read -r n a <<<"$spec"
		read -r -a args <<<"$a"
		if "${SYNTH[@]}" "${args[@]}" -o "$OUT_DIR/$n.mpnl" >/dev/null; then
			info "synth $n ($a)"
		else
			echo "FAIL  synth $n ($a)"
			exit 1
		fi
	done
fi

# ---- lines ------------------------------------------------------------------------------------------
if want lines; then
	echo
	echo "== lines"
	problems=()
	if ! git rev-parse --verify -q "$BASE^{commit}" >/dev/null; then
		problems+=("base $BASE is not a commit")
	else
		changed=0
		while IFS= read -r f; do
			[ -n "$f" ] || continue
			changed=$((changed + 1))
			if [ ! -f "$f" ] || ! git cat-file -e "$BASE:$f" 2>/dev/null; then
				problems+=("$f added or removed")
				continue
			fi
			a=$(git show "$BASE:$f" | wc -l)
			b=$(wc -l <"$f")
			[ "$a" = "$b" ] || problems+=("$f: $a lines at $BASE, $b now")
		done < <(git diff --name-only "$BASE" -- src include)
		while IFS= read -r f; do
			[ -n "$f" ] && problems+=("$f: untracked new file")
		done < <(git ls-files --others --exclude-standard -- src include)
		unguarded=$(git diff -U0 "$BASE" -- src include | grep '^+[^+]' | grep -v 'PC_ONLY(\|PC_SWITCH(' | head -n 5)
		[ -z "$unguarded" ] || problems+=("added lines without PC_ONLY( or PC_SWITCH(: $(printf '%s' "$unguarded" | tr '\n' '|')")
		git diff --quiet "$BASE" -- tools/net/golden || problems+=("tools/net/golden changed since $BASE")
		[ -z "$(git ls-files --others --exclude-standard -- tools/net/golden)" ] || problems+=("new files in tools/net/golden")
	fi
	if [ "${#problems[@]}" = 0 ]; then
		pass "lines: $changed files changed in src/ and include/ since $BASE, line counts equal, every added line guarded; tools/net/golden unchanged"
	else
		for p in "${problems[@]}"; do info "$p"; done
		fail "lines: ${#problems[@]} problems (${problems[0]})"
	fi
fi

# ---- selftest ---------------------------------------------------------------------------------------
if want selftest; then
	echo
	echo "== selftest"
	problems=()
	if python3 "$NETLOG" selftest >"$OUT_DIR/netlog_selftest.txt" 2>&1; then
		info "netlog.py selftest: $(tail -n 1 "$OUT_DIR/netlog_selftest.txt")"
	else
		problems+=("netlog.py selftest ($(tail -n 1 "$OUT_DIR/netlog_selftest.txt"))")
	fi
	disc_arg=()
	[ -n "$DISC" ] && disc_arg=("$DISC")
	run_mppm selftest_rng "MPPM_RNG_SELFTEST=1" "${disc_arg[@]}"
	if [ "$RC" = 0 ] && grep -q 'mppm: net: rng selftest PASS' "$OUT_DIR/selftest_rng.log"; then
		info "MPPM_RNG_SELFTEST: PASS"
	else
		problems+=("MPPM_RNG_SELFTEST exit $RC")
	fi
	# The self-tests exit before main; a build without one boots instead, so the run is bounded
	# (--exit-after-ticks counts pad reads without the harness) and its timeout is short.
	saved_timeout=$RUN_TIMEOUT
	RUN_TIMEOUT=180
	for part in core piki vs view; do
		run_mppm "selftest_$part" "MPPM_NPLAYER_SELFTEST=$part" "${disc_arg[@]}" --exit-after-ticks 300
		if [ "$RC" = 0 ] && grep -q "mppm: nplayer selftest $part PASS" "$OUT_DIR/selftest_$part.log"; then
			info "MPPM_NPLAYER_SELFTEST=$part: PASS"
		else
			problems+=("MPPM_NPLAYER_SELFTEST=$part exit $RC, no 'mppm: nplayer selftest $part PASS' line")
		fi
	done
	RUN_TIMEOUT=$saved_timeout
	if [ "${#problems[@]}" = 0 ]; then
		pass "selftest: netlog.py, rng and nplayer core, piki, vs, view"
	else
		fail "selftest: $(IFS=";"; echo "${problems[*]}" | sed "s/;/; /g")"
	fi
fi

# ---- golden ---------------------------------------------------------------------------------------
if want golden; then
	echo
	echo "== golden"
	qa=()
	[ "$QUICK" = 1 ] && qa=(--quick)
	info "running $DESYNC ${qa[*]} -d $DISPLAY_NUM -o $OUT_DIR/desync (output in $OUT_DIR/desync.txt)"
	MPPM_TEST_DISC="$DISC" MPPM_DESYNC_TIMEOUT="$RUN_TIMEOUT" bash "$DESYNC" -b "$BUILD_DIR" -d "$DISPLAY_NUM" -o "$OUT_DIR/desync" "${qa[@]}" \
		>"$OUT_DIR/desync.txt" 2>&1
	g=$?
	last=$(tail -n 1 "$OUT_DIR/desync.txt")
	if [ "$g" = 0 ]; then
		pass "golden: desync_ci.sh${qa[*]:+ ${qa[*]}} exit 0 ($(grep -c '^PASS' "$OUT_DIR/desync.txt") PASS lines; $last)"
	else
		grep '^FAIL' "$OUT_DIR/desync.txt" | head -n 5 | while IFS= read -r l; do info "$l"; done
		fail "golden: desync_ci.sh exit $g ($last)"
	fi
fi

# ---- the runs of V4 -----------------------------------------------------------------------------
need_v4_r1() { replay v4_r1 "MPPM_NPLAYER_STATS=1 MPPM_PERF=1" "$LOG_V4" --record "$OUT_DIR/v4_r1.mpnl"; }

if want v4; then
	echo
	echo "== v4"
	need_v4_r1
	rc1=$RC
	replay v4_r2 "" "$LOG_V4" --checksum-verify "$OUT_DIR/v4_r1.cksum"
	rc2=$RC
	problems=()
	notes=()
	r1="$OUT_DIR/v4_r1.cksum"
	[ "$rc1" = 0 ] || problems+=("v4_r1 exit $rc1")
	[ "$rc2" = 0 ] || problems+=("v4_r2 exit $rc2 (3 = checksum mismatch)")
	if [ -s "$r1" ] && [ -f "$OUT_DIR/v4_r2.cksum" ] && cmp -s -- "$r1" "$OUT_DIR/v4_r2.cksum"; then
		notes+=("v4_r1 and v4_r2 checksum logs identical")
	else
		problems+=("v4_r1 and v4_r2 differ: $(diff_summary "$r1" "$OUT_DIR/v4_r2.cksum")")
	fi
	e_tick=$(awk '$1 == "E" { t = $2 } END { print t }' "$r1" 2>/dev/null)
	[ "$e_tick" = 18000 ] || problems+=("E line at '${e_tick:-none}', expected 18000")
	nc=$(grep -c '^C ' "$r1" 2>/dev/null)
	nc4=$(grep '^C ' "$r1" 2>/dev/null | grep -c ' n=4/')
	if [ "${nc:-0}" -ge 600 ] && [ "$nc" = "$nc4" ]; then
		notes+=("$nc C lines, all n=4/")
	else
		problems+=("${nc:-0} C lines (need 600), ${nc4:-0} with n=4/")
	fi
	grep -qx '# players=4' "$r1" 2>/dev/null || problems+=("no '# players=4' line")
	if ci=$(python3 "$NETLOG" cmp-input "$LOG_V4" "$OUT_DIR/v4_r1.mpnl" 2>&1); then
		notes+=("recording equals the input")
	else
		problems+=("cmp-input V4 v4_r1.mpnl: $(printf '%s' "$ci" | tr '\n' ' ')")
	fi
	dmp=$(python3 "$NETLOG" dump "$OUT_DIR/v4_r1.mpnl" --events 2>&1)
	if grep -q '^version:     4$' <<<"$dmp" && grep -q '^players:     4$' <<<"$dmp"; then
		notes+=("recording version 4, players 4")
	else
		problems+=("dump of v4_r1.mpnl: not version 4 / players 4")
	fi
	for n in v4_r1 v4_r2; do
		o=$(rng_other "$n")
		[ "$o" = 0 ] || problems+=("$n other-in-session=${o:-?}")
	done
	stats=$(python3 "$NETLOG" stats "$r1" 2>/dev/null)
	navi=$(sed -n 's/^distinct:.* navi=\([0-9]*\).*/\1/p' <<<"$stats")
	piki=$(sed -n 's/^max counts:.* piki=\([0-9]*\).*/\1/p' <<<"$stats")
	[ "${navi:-0}" -gt 100 ] && notes+=("$navi distinct navi hashes") || problems+=("${navi:-no} distinct navi hashes (need more than 100)")
	[ "${piki:-0}" -gt 0 ] && notes+=("max Pikmin $piki") || problems+=("no Pikmin counted")
	bad=$(bad_lines v4_r1 v4_r2)
	[ -z "$bad" ] || problems+=("$(head -n 1 <<<"$bad")")
	if [ "${#problems[@]}" = 0 ]; then
		pass "v4: $(IFS=';'; echo "${notes[*]}" | sed 's/;/; /g')"
	else
		fail "v4: $(IFS=';'; echo "${problems[*]}" | sed 's/;/; /g')"
	fi
fi

if want activity; then
	echo
	echo "== activity"
	need_v4_r1
	problems=()
	notes=()
	last_t=$(sed -n 's/.*mppm: nplayer: tick=\([0-9]*\) captain=.*/\1/p' "$OUT_DIR/v4_r1.log" 2>/dev/null | sort -n | tail -n 1)
	[ "$last_t" = 18000 ] || problems+=("last stats block at tick '${last_t:-none}', expected 18000 (MPPM_NPLAYER_STATS)")
	for i in 0 1 2 3; do
		l=$(stats_line v4_r1 "${last_t:-18000}" "$i")
		if [ -z "$l" ]; then
			problems+=("no stats line for captain $i")
			continue
		fi
		w=$(field walked "$l")
		m=$(field squadmax "$l")
		notes+=("captain $i walked=$w squadmax=$m")
		[ "${w:-0}" -ge 300 ] || problems+=("captain $i walked ${w:-?} (need 300)")
		[ "${m:-0}" -ge 1 ] || problems+=("captain $i squadmax ${m:-?} (need 1)")
	done
	heap=$(grep -m 1 'mppm: nplayer: captains=4 heap free=' "$OUT_DIR/v4_r1.log" 2>/dev/null)
	[ -n "$heap" ] && notes+=("${heap#*mppm: nplayer: }") || problems+=("no 'captains=4 heap free=' line")
	if [ "${#problems[@]}" = 0 ]; then
		pass "activity: tick $last_t: $(IFS=';'; echo "${notes[*]}" | sed 's/;/; /g')"
	else
		fail "activity: $(IFS=';'; echo "${problems[*]}" | sed 's/;/; /g')"
	fi
fi

# ---- deaths -------------------------------------------------------------------------------------
if want deaths; then
	echo
	echo "== deaths"
	replay le4_r1 "MPPM_NPLAYER_STATS=1" "$LOG_LE4"
	rc1=$RC
	replay le4_r2 "" "$LOG_LE4" --checksum-verify "$OUT_DIR/le4_r1.cksum"
	rc2=$RC
	problems=()
	notes=()
	c1="$OUT_DIR/le4_r1.cksum"
	[ "$rc1" = 0 ] || problems+=("le4_r1 exit $rc1")
	[ "$rc2" = 0 ] || problems+=("le4_r2 exit $rc2")
	if [ -s "$c1" ] && [ -f "$OUT_DIR/le4_r2.cksum" ] && cmp -s -- "$c1" "$OUT_DIR/le4_r2.cksum"; then
		notes+=("le4_r1 and le4_r2 identical")
	else
		problems+=("le4 runs differ: $(diff_summary "$c1" "$OUT_DIR/le4_r2.cksum")")
	fi
	early_l=$(awk '$1 == "L" && $2 > 2400 && $2 <= 4800 { print $2; exit }' "$c1" 2>/dev/null)
	late_l=$(awk '$1 == "L" && $2 > 4800 && $2 <= 9000 { print $2; exit }' "$c1" 2>/dev/null)
	last_l=$(awk '$1 == "L" && $2 > 4800 && $2 <= 9000 { t = $2 } END { print t }' "$c1" 2>/dev/null)
	[ -z "$early_l" ] && notes+=("no L line in 2401-4800 (captain 2 down, match goes on)") ||
		problems+=("L line at tick $early_l: the match stopped after captain 2 went down")
	[ -n "$late_l" ] && notes+=("L lines from tick $late_l (Red out)") || problems+=("no L line in 4801-9000: the match did not end after captain 0 went down")
	if grep -q 'mppm: net: floor 1 reseed' "$OUT_DIR/le4_r1.log" 2>/dev/null; then
		notes+=("floor 1 reseed")
	else
		problems+=("no 'floor 1 reseed' line: no second match within 9000 ticks")
	fi
	if [ -n "$last_l" ]; then
		after=$(awk -v t="$last_l" '$1 == "C" && $2 > t && / n=4\// { print $2; exit }' "$c1")
		[ -n "$after" ] && notes+=("C line with n=4/ at tick $after after the result screen") ||
			problems+=("no C line with n=4/ after tick $last_l (the next match lacks 4 captains)")
	fi
	l2=$(stats_line le4_r1 3600 2)
	l0=$(stats_line le4_r1 3600 0)
	a2=$(field alive "$l2")
	a0=$(field alive "$l0")
	if [ "$a2" = 0 ] && [ "$a0" = 1 ]; then
		notes+=("tick 3600: captain 2 alive=0, captain 0 alive=1")
	else
		problems+=("tick-3600 stats: captain 2 alive=${a2:-?}, captain 0 alive=${a0:-?} (want 0 and 1)")
	fi
	bad=$(bad_lines le4_r1 le4_r2)
	[ -z "$bad" ] || problems+=("$(head -n 1 <<<"$bad")")
	if [ "${#problems[@]}" = 0 ]; then
		pass "deaths: $(IFS=';'; echo "${notes[*]}" | sed 's/;/; /g')"
	else
		fail "deaths: $(IFS=';'; echo "${problems[*]}" | sed 's/;/; /g') (Le4 = synth --event 2400:3 --event 4800:1; move the ticks with a note if the match ends by marbles)"
	fi
fi

# ---- view ---------------------------------------------------------------------------------------
if want view; then
	echo
	echo "== view"
	replay vs4_ref "" "$LOG_VS4"
	r0=$RC
	replay vs4_vc0 "" "$LOG_VS4" --view-captain 0
	r1=$RC
	replay vs4_vc3 "" "$LOG_VS4" --view-captain 3
	r2=$RC
	problems=()
	[ "$r0$r1$r2" = 000 ] || problems+=("exit codes ref $r0, vc0 $r1, vc3 $r2")
	for v in vc0 vc3; do
		if [ -s "$OUT_DIR/vs4_ref.cksum" ] && [ -f "$OUT_DIR/vs4_$v.cksum" ] && cmp -s -- "$OUT_DIR/vs4_ref.cksum" "$OUT_DIR/vs4_$v.cksum"; then
			:
		else
			problems+=("vs4_$v differs from vs4_ref: $(diff_summary "$OUT_DIR/vs4_ref.cksum" "$OUT_DIR/vs4_$v.cksum")")
		fi
	done
	if [ "${#problems[@]}" = 0 ]; then
		pass "view: stock split, --view-captain 0 and --view-captain 3 give identical checksum logs ($(grep -c '^C ' "$OUT_DIR/vs4_ref.cksum") C lines)"
	else
		fail "view: $(IFS=';'; echo "${problems[*]}" | sed 's/;/; /g')"
	fi
fi

# ---- neg ----------------------------------------------------------------------------------------
if want neg; then
	echo
	echo "== neg"
	need_v4_r1
	neg_in="$OUT_DIR/neg_in.mpnl"
	if python3 "$NETLOG" mutate "$LOG_V4" --tick "$NEG_TICK" --port 2 --ticks "$NEG_END" -o "$neg_in" >/dev/null; then
		replay neg "" "$neg_in"
		[ "$RC" = 0 ] || info "neg run exited $RC"
		neg_out=$(python3 "$NETLOG" cmp-cksum "$OUT_DIR/v4_r1.cksum" "$OUT_DIR/neg.cksum" 2>&1)
		neg_rc=$?
		neg_t=$(sed -n 's/^differ at line [0-9]*, tick \([0-9]*\).*/\1/p' <<<"$neg_out" | head -n 1)
		if [ "$neg_rc" = 1 ] && grep -q '^  A: C ' <<<"$neg_out" && grep -q '^  B: C ' <<<"$neg_out" &&
			[ -n "$neg_t" ] && [ "$neg_t" -gt "$NEG_TICK" ]; then
			pass "neg: port 2 changed at record $NEG_TICK: matches v4_r1 before, differs at tick $neg_t; $(sed -n 's/^  fields: //p' <<<"$neg_out")"
		else
			fail "neg: $(head -n 4 <<<"$neg_out" | tr '\n' ' ')"
		fi
	else
		fail "neg: mutate failed"
	fi
fi

# ---- shots --------------------------------------------------------------------------------------
if want shots; then
	echo
	echo "== shots"
	shot shot_split
	shot shot_vc2 --view-captain 2
	shot shot_vc3 --view-captain 3
	problems=()
	notes=()
	for s in shot_split shot_vc2 shot_vc3; do
		f="$OUT_DIR/$s.png"
		if [ ! -s "$f" ]; then
			problems+=("$s.png missing (see $s.log)")
			continue
		fi
		m=$(mean_of "$f")
		if [ -n "$m" ] && awk -v m="$m" 'BEGIN { exit !(m > 0.03) }'; then
			notes+=("$s mean $(awk -v m="$m" 'BEGIN { printf "%.1f%%", m * 100 }')")
		else
			problems+=("$s.png is black (mean ${m:-?})")
		fi
	done
	if [ -s "$OUT_DIR/shot_vc2.png" ] && [ -s "$OUT_DIR/shot_vc3.png" ] && [ -s "$OUT_DIR/shot_split.png" ]; then
		px=$(pixels_of "$OUT_DIR/shot_vc2.png")
		d23=$(ae "$OUT_DIR/shot_vc2.png" "$OUT_DIR/shot_vc3.png" 5)
		ds2=$(ae "$OUT_DIR/shot_split.png" "$OUT_DIR/shot_vc2.png" 5)
		pct() { awk -v a="${1:-0}" -v p="${2:-1}" 'BEGIN { printf "%.1f", 100 * a / p }'; }
		if [ -n "$d23" ] && [ -n "$px" ] && awk -v a="$d23" -v p="$px" 'BEGIN { exit !(a > 0.05 * p) }'; then
			notes+=("vc2/vc3 differ in $(pct "$d23" "$px")%")
		else
			problems+=("vc2 and vc3 differ in ${d23:-?} of ${px:-?} pixels (need more than 5%)")
		fi
		if [ -n "$ds2" ] && [ -n "$px" ] && awk -v a="$ds2" -v p="$px" 'BEGIN { exit !(a > 0.05 * p) }'; then
			notes+=("split/vc2 differ in $(pct "$ds2" "$px")%")
		else
			problems+=("split and vc2 differ in ${ds2:-?} of ${px:-?} pixels (need more than 5%)")
		fi
	fi
	if [ "${#problems[@]}" = 0 ]; then
		pass "shots: $(IFS=';'; echo "${notes[*]}" | sed 's/;/; /g'); review $OUT_DIR/shot_*.png for the captain markers"
	else
		fail "shots: $(IFS=';'; echo "${problems[*]}" | sed 's/;/; /g')"
	fi
fi

# ---- perf ---------------------------------------------------------------------------------------
if want perf; then
	echo
	echo "== perf"
	replay l2_perf "MPPM_PERF=1 MPPM_NPLAYER_STATS=1" "$LOG_L2"
	need_v4_r1
	replay v4_h10 "MPPM_VS_HANDICAP=10 MPPM_PERF=1 MPPM_NPLAYER_STATS=1" "$LOG_V4"
	problems=()
	printf '      %-8s %6s %8s %8s %8s %8s %9s %6s  %s\n' run ticks "sim avg" p50 p99 max "pass avg" piki "heap"
	for n in l2_perf v4_r1 v4_h10; do
		l=$(perf_line "$n")
		mp=$(max_piki "$OUT_DIR/$n.cksum")
		heap=$(grep -m 1 'mppm: nplayer: captains=' "$OUT_DIR/$n.log" 2>/dev/null | sed 's/.*mppm: nplayer: //')
		if [ -z "$l" ]; then
			printf '      %-8s (no perf line, exit %s)\n' "$n" "$(cat "$OUT_DIR/$n.exit" 2>/dev/null)"
			problems+=("$n: no 'mppm: net: perf' line")
			continue
		fi
		tk=$(sed -n 's/.*perf ticks=\([0-9]*\).*/\1/p' <<<"$l")
		sim=$(sed -n 's/.* sim avg=\([0-9.]*\).*/\1/p' <<<"$l")
		p50=$(sed -n 's/.* p50=\([0-9.]*\).*/\1/p' <<<"$l")
		p99=$(sed -n 's/.* p99=\([0-9.]*\).*/\1/p' <<<"$l")
		mx=$(sed -n 's/.* max=\([0-9.]*\).*/\1/p' <<<"$l")
		pa=$(sed -n 's/.* pass avg=\([0-9.]*\).*/\1/p' <<<"$l")
		printf '      %-8s %6s %8s %8s %8s %8s %9s %6s  %s\n' "$n" "$tk" "$sim" "$p50" "$p99" "$mx" "$pa" "${mp:--}" "${heap:--}"
		if [ "$n" = v4_h10 ]; then
			H10_SIM=$sim
			H10_PIKI=${mp:-0}
		fi
	done
	HEAP2=$(sed -n 's/.*mppm: nplayer: captains=2 heap free=\([0-9]*\).*/\1/p' "$OUT_DIR/l2_perf.log" 2>/dev/null | head -n 1)
	HEAP4=$(sed -n 's/.*mppm: nplayer: captains=4 heap free=\([0-9]*\).*/\1/p' "$OUT_DIR/v4_r1.log" 2>/dev/null | head -n 1)
	[ -n "$HEAP2" ] || problems+=("l2_perf: no 'captains=2 heap free=' line")
	[ -n "$HEAP4" ] || problems+=("v4_r1: no 'captains=4 heap free=' line")
	if [ -n "$HEAP2" ] && [ -n "$HEAP4" ]; then
		echo "      heap free: 2 captains $HEAP2, 4 captains $HEAP4 (4 captains use $((HEAP2 - HEAP4)) bytes more)"
	fi
	grep -m 1 'mppm: net: perf aid: VS handicap' "$OUT_DIR/v4_h10.log" >/dev/null 2>&1 ||
		problems+=("v4_h10: no 'perf aid: VS handicap' line (MPPM_VS_HANDICAP not applied)")
	if [ -n "${H10_SIM:-}" ]; then
		awk -v s="$H10_SIM" -v m="$PERF_MAX_MS" 'BEGIN { exit !(s <= m) }' ||
			problems+=("v4_h10 sim avg $H10_SIM ms above $PERF_MAX_MS ms")
		[ "${H10_PIKI:-0}" -ge "$PERF_MIN_PIKI" ] || problems+=("v4_h10 largest Pikmin count ${H10_PIKI:-0} below $PERF_MIN_PIKI")
	fi
	if [ "${#problems[@]}" = 0 ]; then
		pass "perf: v4_h10 sim avg $H10_SIM ms (limit $PERF_MAX_MS) with up to $H10_PIKI Pikmin; heap free 2 captains $HEAP2, 4 captains $HEAP4"
	else
		fail "perf: $(IFS=';'; echo "${problems[*]}" | sed 's/;/; /g')"
	fi
fi

# ---- summary ------------------------------------------------------------------------------------
echo
if [ "${#FAIL_LIST[@]}" -gt 0 ]; then
	echo "Failures (${#FAIL_LIST[@]}):"
	for f in "${FAIL_LIST[@]}"; do echo "  $f"; done
fi
echo "m53a_test: $([ "$FAILS" = 0 ] && echo PASS || echo "FAIL ($FAILS checks failed)") in $((SECONDS - T_START)) s; outputs in $OUT_DIR"
[ "$FAILS" = 0 ]
