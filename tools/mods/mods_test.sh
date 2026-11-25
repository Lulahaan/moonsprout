#!/usr/bin/env bash
# mods_test.sh - the M5.2 done test (data-mod overlay, docs/online/M5_2_MODS.md "Done test").
# Builds the test mods from your disc (tools/mods/make_test_mods.py), replays the synthetic
# 2P Battle log Ls with each mods folder and checks the checksum logs, the log lines, the
# screenshots, the replay content gate and the launcher's Mods window.
#
# Usage: tools/mods/mods_test.sh [-b BUILD_DIR] [-d DISPLAY] [-o OUT_DIR] [-c CHECK[,CHECK...]] [-l]
#   -b  build directory holding mppm                 (default ./build-pc)
#   -d  Xvfb display number for xvfb-run -n           (default 84)
#   -o  output directory                              (default /var/tmp/mppm-m52-mods)
#   -c  run only these checks (names or numbers; -l lists them); the runs they need always run
#   -l  list the checks and exit
#
# Run from the repo root on the host. The disc comes from MPPM_TEST_DISC (required). Every run is
# headless under Xvfb with its own home inside OUT_DIR. Logs, checksum logs and PNGs stay in
# OUT_DIR. Environment: MPPM_MODS_TIMEOUT (seconds per run, default 900); MPPM_MODS_SHOT_TICK
# (default 60) and MPPM_MODS_SHOT_FUZZ (colour tolerance in percent for the picture compare,
# default 15; a second stock picture marks the run-to-run noise, which the compare leaves out);
# for the launcher check,
# MPPM_MODS_UI_LIST_DOWNS (Down presses from Overview to the second mod, default 3),
# MPPM_MODS_UI_OFF_KEYS (keys from the detail pane's first control to Turn Off, default none) and
# MPPM_MODS_UI_UP_KEYS (keys from Turn Off to Move Up, default Down).
# Exit 0 only if every selected check passed; one PASS or FAIL line per check.

set -u

BUILD_DIR="./build-pc"
DISPLAY_NUM=84
OUT_DIR="/var/tmp/mppm-m52-mods"
ONLY=""
LIST=0
DISC="${MPPM_TEST_DISC:-}"
RUN_TIMEOUT="${MPPM_MODS_TIMEOUT:-900}"
UI_LIST_DOWNS="${MPPM_MODS_UI_LIST_DOWNS:-3}"
UI_OFF_KEYS="${MPPM_MODS_UI_OFF_KEYS:-}"
UI_UP_KEYS="${MPPM_MODS_UI_UP_KEYS:-Down}"
SHOT_TICK="${MPPM_MODS_SHOT_TICK:-60}"
SHOT_MIN_AE=900
SHOT_FUZZ="${MPPM_MODS_SHOT_FUZZ:-15}"

CHECKS=(stock stock-empty identity arena enemy pellet archive conflict bad record refuse launcher whole)
CHECK_TEXT=(
	"no mods folder, and --no-mods with the arena folder: checksum log equals the golden Ls, no 'mppm: mods:' line"
	"empty mods folder (MPPM_MODS_DIR, no load_order.txt): golden Ls, only the 'none on' line"
	"unchanged files: golden C lines, both archive overrides applied (mem), '# mods=' comment, hash equals modhash.py"
	"arena twice: identical, first C line differs from golden in n=, tick-60 screenshots differ"
	"enemyParms.szs override twice: identical, differs from golden in enemy"
	"pelletlist_us.szs override: applied (mem), differs from golden (checksum or screenshot)"
	"both archive mods twice: identical, differs from golden"
	"conflict and conflict-rev: override lines, newfiles' caveinfo used, reverse equals arena"
	"bad mods: one error and one skipped line each, exit 0, arena still mounted; bad folder name and gone off line warned"
	"record with arena: version 3 log with the arena mods hash and list"
	"replay the recording: refused without mods and with identity (exit 2), accepted with arena"
	"Mods window: keyboard turn off and move up, load_order.txt rewritten, screenshots"
	"whole-archive replacement vs inner files: load order decides, conflict lines, golden / enemy C lines"
)

usage() {
	sed -n '2,22p' "$0" | sed 's/^# \{0,1\}//'
	exit 2
}

while [ $# -gt 0 ]; do
	case "$1" in
	-b) BUILD_DIR="${2:?-b needs a value}"; shift 2 ;;
	-d) DISPLAY_NUM="${2:?-d needs a value}"; shift 2 ;;
	-o) OUT_DIR="${2:?-o needs a value}"; shift 2 ;;
	-c) ONLY="${2:?-c needs a value}"; shift 2 ;;
	-l) LIST=1; shift ;;
	-h | --help) usage ;;
	*) echo "unknown argument: $1" >&2; usage ;;
	esac
done

if [ "$LIST" = 1 ]; then
	for i in "${!CHECKS[@]}"; do
		printf '%2d  %-12s %s\n' $((i + 1)) "${CHECKS[$i]}" "${CHECK_TEXT[$i]}"
	done
	exit 0
fi

case "$DISPLAY_NUM" in '' | *[!0-9]*) echo "bad -d: $DISPLAY_NUM" >&2; exit 2 ;; esac
case "$RUN_TIMEOUT" in '' | *[!0-9]*) echo "bad MPPM_MODS_TIMEOUT: $RUN_TIMEOUT" >&2; exit 2 ;; esac
case "$UI_LIST_DOWNS" in '' | *[!0-9]*) echo "bad MPPM_MODS_UI_LIST_DOWNS: $UI_LIST_DOWNS" >&2; exit 2 ;; esac

NETLOG="tools/net/netlog.py"
GOLDEN="tools/net/golden/Ls.cksum"
GEN="tools/mods/make_test_mods.py"
MODHASH="tools/mods/modhash.py"
if [ ! -f "$NETLOG" ] || [ ! -f "$GEN" ] || [ ! -f "$GOLDEN" ]; then
	echo "run from the repo root ($NETLOG, $GEN or $GOLDEN not found)" >&2
	exit 2
fi
if [ -z "$DISC" ]; then
	echo "MPPM_TEST_DISC is not set: point it at your own Pikmin 2 (USA) disc image" >&2
	exit 2
fi
case "$OUT_DIR" in /*) ;; *) OUT_DIR="$PWD/$OUT_DIR" ;; esac
case "$BUILD_DIR" in /*) ;; *) BUILD_DIR="$PWD/$BUILD_DIR" ;; esac

# ---- selection ----------------------------------------------------------------------------------
SELECTED=()
if [ -n "$ONLY" ]; then
	IFS=',' read -r -a req <<<"$ONLY"
	for c in "${req[@]}"; do
		found=""
		for i in "${!CHECKS[@]}"; do
			if [ "$c" = "${CHECKS[$i]}" ] || [ "$c" = "$((i + 1))" ]; then found="${CHECKS[$i]}"; fi
		done
		[ -n "$found" ] || { echo "unknown check: $c (-l lists them)" >&2; exit 2; }
		SELECTED+=("$found")
	done
else
	SELECTED=("${CHECKS[@]}")
fi
want() {
	local x
	for x in "${SELECTED[@]}"; do [ "$x" = "$1" ] && return 0; done
	return 1
}

# ---- state and helpers ------------------------------------------------------------------------
FAILS=0
FAILED=()
CHILD_PID=""
declare -A RAN

# A check prints its findings as "ok"/"no" lines and ends with one PASS or FAIL line.
CUR=""
CUR_NO=""
CUR_OK=0
info() { echo "      $*"; }
pass() { echo "  ok  $*"; CUR_OK=$((CUR_OK + 1)); }
fail() {
	echo "  no  $*"
	if [ -n "$CUR" ]; then
		[ -n "$CUR_NO" ] || CUR_NO="$*"
	else
		echo "FAIL  $*"
		FAILS=$((FAILS + 1))
		FAILED+=("$*")
	fi
}
begin_check() { # begin_check NUMBER NAME (ends the check before it)
	[ -z "$CUR" ] || end_check
	CUR="$1 $2"
	CUR_NO=""
	CUR_OK=0
	echo
	echo "== $CUR"
}
end_check() {
	if [ -n "$CUR_NO" ]; then
		echo "FAIL  $CUR: $CUR_NO"
		FAILS=$((FAILS + 1))
		FAILED+=("$CUR: $CUR_NO")
	else
		echo "PASS  $CUR ($CUR_OK findings)"
	fi
	CUR=""
}

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

# mods_dir SCENARIO: the generated folder of a scenario ("" = none)
mods_dir() { [ -n "$1" ] && printf '%s/mods-test/%s' "$OUT_DIR" "$1"; }

# launch NAME SCENARIO ENVS ARGS...: start mppm in the background under Xvfb (CHILD_PID).
# ENVS is a space-separated list of VAR=value (no value holds a space).
launch() {
	local name="$1" scen="$2" envs="$3"
	shift 3
	local home="$OUT_DIR/home/$name"
	local -a env_arr=() mods_env=()
	[ -n "$envs" ] && read -r -a env_arr <<<"$envs"
	[ -n "$scen" ] && mods_env=(MPPM_MODS_DIR="$(mods_dir "$scen")")
	rm -rf -- "$home"
	mkdir -p "$home/.local/share/mppm" "$home/.config" || return 2
	env -u WAYLAND_DISPLAY -u XDG_RUNTIME_DIR -u MPPM_MODS_DIR -u MPPM_NO_MODS GDK_BACKEND=x11 \
		DBUS_SESSION_BUS_ADDRESS=unix:path=/nonexistent SDL_VIDEODRIVER=x11 SDL_AUDIODRIVER=dummy \
		HOME="$home" XDG_DATA_HOME="$home/.local/share" \
		XDG_CONFIG_HOME="$home/.config" XDG_CACHE_HOME="$home/.cache" \
		"${mods_env[@]}" "${env_arr[@]}" \
		timeout "$RUN_TIMEOUT" xvfb-run -n "$DISPLAY_NUM" -s "-screen 0 1280x960x24" \
		"$BUILD_DIR/mppm" "$@" >"$OUT_DIR/$name.log" 2>&1 </dev/null &
	CHILD_PID=$!
}

# finish NAME: wait for the run, record its exit code in RC and OUT_DIR/NAME.exit
finish() {
	local name="$1"
	wait "$CHILD_PID"
	RC=$?
	CHILD_PID=""
	echo "$RC" >"$OUT_DIR/$name.exit"
	rm -rf -- "$OUT_DIR/home/$name/.cache"
}

# run_replay NAME SCENARIO INPUT [extra args...]: headless replay of INPUT with the scenario's mods
# folder; checksum log OUT_DIR/NAME.cksum, console OUT_DIR/NAME.log. Sets RC. Runs once per name.
run_replay() {
	local name="$1" scen="$2" input="$3"
	shift 3
	if [ -n "${RAN[$name]:-}" ]; then
		RC=$(cat "$OUT_DIR/$name.exit" 2>/dev/null || echo 255)
		return
	fi
	RAN[$name]=1
	rm -f -- "$OUT_DIR/$name.cksum" "$OUT_DIR/$name.exit"
	local t0=$SECONDS attempt
	info "running $name (${scen:-no mods folder}: $(basename "$input")${*:+, $*})"
	for attempt in 1 2; do
		wait_display_free
		launch "$name" "$scen" "MPPM_SKIP_LAUNCHER=1" "$DISC" --replay "$input" \
			--checksum-log "$OUT_DIR/$name.cksum" --headless-ticks "$@"
		finish "$name"
		if [ "$RC" = 134 ] && [ "$attempt" = 1 ] && grep -q "x11 not available" "$OUT_DIR/$name.log"; then
			info "$name: Xvfb not ready, retrying"
			sleep 2
			continue
		fi
		break
	done
	info "$name exit $RC after $((SECONDS - t0)) s"
}

# run_shot NAME SCENARIO: paced replay of Ls held at tick SHOT_TICK; screenshot OUT_DIR/NAME.png
run_shot() {
	local name="$1" scen="$2"
	if [ -n "${RAN[$name]:-}" ]; then return; fi
	RAN[$name]=1
	rm -f -- "$OUT_DIR/$name.png"
	local t0=$SECONDS w attempt
	info "running $name (${scen:-no mods folder}: paced, screenshot at tick $SHOT_TICK)"
	for attempt in 1 2; do
		wait_display_free
		launch "$name" "$scen" "MPPM_SKIP_LAUNCHER=1 MPPM_HOLD_AT_TICK=$SHOT_TICK" "$DISC" \
			--replay "$LS" --exit-after-ticks $((SHOT_TICK + 30))
		w=0
		while [ "$w" -lt 1200 ] && kill -0 "$CHILD_PID" 2>/dev/null; do
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

# ae A.png B.png [FUZZ]: number of differing pixels (empty on error)
ae() {
	magick compare -fuzz "${3:-0}%" -metric AE "$1" "$2" null: 2>&1 >/dev/null | awk '{ printf "%d", $1 }'
}

# masked_ae A.png B.png KEEP.png: ae of A and B with the pixels that KEEP has black painted out
masked_ae() {
	local t="$OUT_DIR/.shot_tmp" k="$3"
	magick "$1" -alpha off "$k" -compose multiply -composite "$t.a.png" 2>/dev/null &&
		magick "$2" -alpha off "$k" -compose multiply -composite "$t.b.png" 2>/dev/null &&
		ae "$t.a.png" "$t.b.png" "$SHOT_FUZZ"
	rm -f "$t.a.png" "$t.b.png"
}

# shot_differs NAME: pass/fail whether OUT_DIR/NAME.png differs from the stock picture by more than
# SHOT_MIN_AE pixels once the run-to-run noise is taken out. The onion's beam and lights are not
# tick-locked: they come in one of two phases (about 740 pixels apart at fuzz 15%), so a single
# stock picture is not a fixed reference. The pixels where the two stock pictures differ (grown by
# a few pixels) are painted out of every picture, and the picture is compared with both stock
# pictures; the smaller count is the mod's own change. When the two stock pictures share a phase and
# the mod's picture has the other one, the onion still counts, which is why SHOT_MIN_AE sits above
# one phase difference: a mod that changes nothing scores 0 or about 740, the arena mod's extra
# pellet posies about 1100 (or 1850 with the onion).
shot_differs() {
	local name="$1" label="$2" n na nb noise keep="$OUT_DIR/.shot_keep_$1.png"
	run_shot shot_stock ""
	run_shot shot_stock_b ""
	if [ ! -s "$OUT_DIR/shot_stock.png" ] || [ ! -s "$OUT_DIR/shot_stock_b.png" ] || [ ! -s "$OUT_DIR/$name.png" ]; then
		fail "$label: missing screenshot (shot_stock.png, shot_stock_b.png or $name.png; see their logs)"
		return 1
	fi
	noise=$(ae "$OUT_DIR/shot_stock.png" "$OUT_DIR/shot_stock_b.png" "$SHOT_FUZZ")
	magick compare -fuzz "$SHOT_FUZZ%" -compose src -highlight-color white -lowlight-color black \
		"$OUT_DIR/shot_stock.png" "$OUT_DIR/shot_stock_b.png" png:- 2>/dev/null |
		magick - -alpha off -colorspace gray -morphology Dilate Disk:4 -negate "$keep" 2>/dev/null
	if [ -s "$keep" ]; then
		na=$(masked_ae "$OUT_DIR/shot_stock.png" "$OUT_DIR/$name.png" "$keep")
		nb=$(masked_ae "$OUT_DIR/shot_stock_b.png" "$OUT_DIR/$name.png" "$keep")
	else
		na=""
		nb=""
	fi
	rm -f "$keep"
	n=$na
	[ -n "$nb" ] && { [ -z "$n" ] || [ "$nb" -lt "$n" ]; } && n=$nb
	if [ -n "$n" ] && [ "$n" -gt "$SHOT_MIN_AE" ]; then
		pass "$label: tick-$SHOT_TICK picture differs from stock in $n pixels outside the stock-to-stock noise (fuzz $SHOT_FUZZ%, noise ${noise:-?}, need more than $SHOT_MIN_AE; $name.png)"
		return 0
	fi
	fail "$label: tick-$SHOT_TICK picture differs from stock in ${n:-?} pixels outside the stock-to-stock noise, need more than $SHOT_MIN_AE (fuzz $SHOT_FUZZ%, noise ${noise:-?})"
	return 1
}

# not_blank PNG: the picture is not one flat colour
not_blank() {
	local sd
	sd=$(magick "$1" -format '%[fx:standard_deviation]' info: 2>/dev/null)
	[ -n "$sd" ] && awk -v s="$sd" 'BEGIN { exit !(s > 0.01) }'
}

cmp_golden() { python3 "$NETLOG" cmp-cksum "$GOLDEN" "$1" 2>&1; } # exit 0 identical
first_c() { grep -m 1 '^C ' "$1" 2>/dev/null; }
counts_of() { sed -n 's/.* n=\([0-9/]*\).*/\1/p' <<<"$1"; }
enemy_count() { counts_of "$1" | cut -d/ -f3; }
mods_lines() { grep 'mppm: mods:' "$1" 2>/dev/null; }

# diff_line CKSUM: the first difference from golden and its fields, as one line
diff_line() {
	cmp_golden "$1" | awk '/^differ/ { d = $0 } /^  fields:/ { f = $0; sub(/^  fields: /, "", f) }
		END { if (d != "") print d "; " f; else print "identical" }'
}

# expect_golden NAME LABEL: pass when the run exited 0 and its C lines equal golden
expect_golden() {
	local name="$1" label="$2" out
	out=$(cmp_golden "$OUT_DIR/$name.cksum")
	if [ "$RC" = 0 ] && grep -q '^identical' <<<"$out"; then
		pass "$label: exit 0, C lines equal golden Ls ($(tail -n 1 <<<"$out"))"
		return 0
	fi
	fail "$label: exit $RC, $(diff_line "$OUT_DIR/$name.cksum") (see $OUT_DIR/$name.log)"
	return 1
}

# twice NAME SCENARIO: replay Ls twice; pass when both exit 0 and the logs are cmp-identical
twice() {
	local name="$1" scen="$2"
	run_replay "${name}_1" "$scen" "$LS"
	local rc1=$RC
	run_replay "${name}_2" "$scen" "$LS"
	if [ "$rc1" = 0 ] && [ "$RC" = 0 ] && [ -s "$OUT_DIR/${name}_1.cksum" ] &&
		cmp -s "$OUT_DIR/${name}_1.cksum" "$OUT_DIR/${name}_2.cksum"; then
		pass "$name: two runs exit 0 and their checksum logs are cmp-identical"
		return 0
	fi
	fail "$name: runs exit $rc1 and $RC, logs $(cmp "$OUT_DIR/${name}_1.cksum" "$OUT_DIR/${name}_2.cksum" 2>&1 | head -n 1)"
	return 1
}

# applied LOG ARCHIVE: the log has an 'archive override ARCHIVE:... applied (mem' line
applied() { grep -iF "mppm: mods: archive override $2:" "$1" 2>/dev/null | grep -q 'applied (mem'; }

# expected hash line of a scenario from modhash.py with this build's id
modhash_line() {
	python3 -I "$MODHASH" "$(mods_dir "$1")" --build-id "$BUILD_ID" | grep '^mppm: mods: hash '
}

# ---- preconditions ------------------------------------------------------------------------------
T_START=$SECONDS
echo "mods_test: build $BUILD_DIR, display :$DISPLAY_NUM, out $OUT_DIR, checks: ${SELECTED[*]}"
if [ ! -x "$BUILD_DIR/mppm" ]; then
	echo "FAIL  no executable at $BUILD_DIR/mppm"
	exit 1
fi
if [ ! -f "$DISC" ]; then
	echo "FAIL  disc not found: $DISC"
	exit 1
fi
if [ -e "/tmp/.X$DISPLAY_NUM-lock" ]; then
	echo "FAIL  display :$DISPLAY_NUM is in use (pick another with -d)"
	exit 1
fi
for tool in xvfb-run magick timeout; do
	command -v "$tool" >/dev/null || { echo "FAIL  $tool not found"; exit 1; }
done

echo
echo "== Setup"
if python3 -I "$GEN" --disc "$DISC" --out "$OUT_DIR/mods-test" >"$OUT_DIR/make_test_mods.log" 2>&1; then
	echo "PASS  test mods: $(tail -n 1 "$OUT_DIR/make_test_mods.log")"
else
	fail "make_test_mods.py: $(tail -n 1 "$OUT_DIR/make_test_mods.log")"
	exit 1
fi
SCEN_JSON="$OUT_DIR/mods-test/scenarios.json"
# sj EXPR: one value of scenarios.json (EXPR is Python over d)
sj() { python3 -I -c 'import json,sys; d=json.load(open(sys.argv[1])); r=eval(sys.argv[2]); print("\n".join(r) if isinstance(r, list) else r)' "$SCEN_JSON" "$1"; }

LS="$OUT_DIR/Ls.mpnl"
if python3 "$NETLOG" synth --seed 20261122 --synth-seed 7 --vs-stage 0 --ticks 3600 -o "$LS" >/dev/null; then
	echo "PASS  synth Ls (3600 ticks, the log of tools/net/golden/Ls.cksum)"
else
	fail "netlog.py synth"
	exit 1
fi
BUILD_ID=$(sed -n 's/^# build=\([0-9a-fA-F]*\).*/\1/p' "$GOLDEN" | head -n 1)

# ---- 1 stock ------------------------------------------------------------------------------------
if want stock; then
	begin_check 1 stock
	run_replay stock "" "$LS"
	expect_golden stock "stock"
	n=$(mods_lines "$OUT_DIR/stock.log" | wc -l)
	if [ "$n" = 0 ]; then pass "stock: no 'mppm: mods:' line"; else fail "stock: $n 'mppm: mods:' lines: $(mods_lines "$OUT_DIR/stock.log" | head -n 2 | tr '\n' ' ')"; fi
	# --no-mods beats MPPM_MODS_DIR: the arena folder is ignored, stock content, no mods line
	run_replay stock_nomods arena "$LS" --no-mods
	expect_golden stock_nomods "stock --no-mods (arena folder)"
	n=$(mods_lines "$OUT_DIR/stock_nomods.log" | wc -l)
	if [ "$n" = 0 ]; then pass "stock --no-mods: no 'mppm: mods:' line"; else fail "stock --no-mods: $n 'mppm: mods:' lines: $(mods_lines "$OUT_DIR/stock_nomods.log" | head -n 2 | tr '\n' ' ')"; fi
fi
# the build id of this build, from any checksum log it wrote
build_id_of() { sed -n 's/^# build=\([0-9a-fA-F]*\).*/\1/p' "$1" 2>/dev/null | head -n 1; }

# ---- 2 stock-empty --------------------------------------------------------------------------------
if want stock-empty; then
	begin_check 2 stock-empty
	run_replay stock_empty stock-empty "$LS"
	expect_golden stock_empty "stock-empty"
	lines=$(mods_lines "$OUT_DIR/stock_empty.log")
	if [ "$(printf '%s' "$lines" | grep -c .)" = 1 ] && grep -q 'mppm: mods: folder .*: none on$' <<<"$lines"; then
		pass "stock-empty: one mods line: ${lines#*mppm: mods: }"
	else
		fail "stock-empty: expected exactly one 'mppm: mods: folder <dir>: none on' line, got: ${lines:-none}"
	fi
fi

# ---- 3 identity -----------------------------------------------------------------------------------
if want identity; then
	begin_check 3 identity
	run_replay identity identity "$LS"
	expect_golden identity "identity"
	while read -r arch; do
		if applied "$OUT_DIR/identity.log" "$arch"; then
			pass "identity: override in $arch applied (mem)"
		else
			fail "identity: no 'archive override $arch:... applied (mem' line"
		fi
	done < <(sj 'd["expect"]["identity_applied"]')
	if grep -q '^# mods=' "$OUT_DIR/identity.cksum" 2>/dev/null; then
		pass "identity: checksum log has '$(grep -m 1 '^# mods=' "$OUT_DIR/identity.cksum")'"
	else
		fail "identity: no '# mods=' comment in $OUT_DIR/identity.cksum"
	fi
	bid=$(build_id_of "$OUT_DIR/identity.cksum")
	got=$(grep -m 1 'mppm: mods: hash ' "$OUT_DIR/identity.log" | sed 's/.*\(mppm: mods: hash \)/\1/')
	BUILD_ID="${bid:-$BUILD_ID}"
	want_line=$(modhash_line identity)
	if [ -n "$got" ] && [ "$got" = "$want_line" ]; then
		pass "identity: engine hash line equals modhash.py (build $BUILD_ID)"
	else
		fail "identity: engine '${got:-no hash line}' vs modhash.py '$want_line' (build $BUILD_ID)"
	fi
	if grep -q 'mppm: mods: hash 0\{64\} ' <<<"$got"; then fail "identity: mods hash is zero"; fi
fi

# ---- 4 arena --------------------------------------------------------------------------------------
[ -z "$CUR" ] || end_check
ARENA_N=""
if want arena || want conflict || want bad || want refuse; then
	run_replay arena_1 arena "$LS"
	ARENA_N=$(counts_of "$(first_c "$OUT_DIR/arena_1.cksum")")
fi
if want arena; then
	begin_check 4 arena
	twice arena arena
	d=$(diff_line "$OUT_DIR/arena_1.cksum")
	if grep -q '^differ at line 1,' <<<"$d" && grep -q 'counts (' <<<"$d"; then
		pass "arena: $d"
	else
		fail "arena: expected the first C line to differ from golden in n=, got: $d"
	fi
	run_shot shot_arena arena
	shot_differs shot_arena arena
fi

# ---- 5 enemy --------------------------------------------------------------------------------------
if want enemy; then
	begin_check 5 enemy
	twice enemy enemy
	d=$(diff_line "$OUT_DIR/enemy_1.cksum")
	if grep -q '^differ' <<<"$d" && grep -Eq '(; |, )enemy(,|$)' <<<"$d"; then
		pass "enemy: $d"
	else
		fail "enemy: expected a difference from golden with enemy among the fields, got: $d"
	fi
	arch=$(sj 'd["archives"]["enemyparms"]')
	if applied "$OUT_DIR/enemy_1.log" "$arch"; then pass "enemy: override in $arch applied (mem)"; else fail "enemy: no 'archive override $arch:... applied (mem' line"; fi
fi

# ---- 6 pellet -------------------------------------------------------------------------------------
if want pellet; then
	begin_check 6 pellet
	evidence=$(sj 'd["pellet"]["evidence"]')
	arch=$(sj 'd["archives"]["pelletlist"]')
	run_replay pellet pellet "$LS"
	if applied "$OUT_DIR/pellet.log" "$arch"; then pass "pellet: override in $arch applied (mem)"; else fail "pellet: no 'archive override $arch:... applied (mem' line"; fi
	d=$(diff_line "$OUT_DIR/pellet.cksum")
	used=""
	if [ "$evidence" != screenshot ] && [ "$RC" = 0 ] && grep -q '^differ' <<<"$d"; then
		used=checksum
		pass "pellet: evidence checksum: $d"
	elif [ "$evidence" = checksum ]; then
		fail "pellet: evidence checksum, but the run (exit $RC) shows no difference from golden: $d"
	else
		info "pellet: no checksum difference in 3600 ticks ($d); trying the screenshot evidence"
		run_shot shot_pellet_bmd "$(sj 'd["pellet"]["screenshot_scenario"]')"
		if shot_differs shot_pellet_bmd "pellet: evidence screenshot"; then
			used=screenshot
		fi
	fi
	echo "${used:-none}" >"$OUT_DIR/pellet_evidence.txt"
fi

# ---- 7 archive ------------------------------------------------------------------------------------
if want archive; then
	begin_check 7 archive
	twice archive archive
	d=$(diff_line "$OUT_DIR/archive_1.cksum")
	if grep -q '^differ' <<<"$d"; then pass "archive: $d"; else fail "archive: no difference from golden ($d)"; fi
fi

# ---- 8 conflict -----------------------------------------------------------------------------------
if want conflict; then
	begin_check 8 conflict
	run_replay conflict conflict "$LS"
	exp=$(sj 'd["expect"]["conflict"][0]')
	if grep -qF "$exp" "$OUT_DIR/conflict.log"; then pass "conflict: '$exp'"; else fail "conflict: no line '$exp' (got: $(grep 'mppm: mods: conflict' "$OUT_DIR/conflict.log" | head -n 2 | tr '\n' ' '))"; fi
	ce=$(enemy_count "$(first_c "$OUT_DIR/conflict.cksum")")
	ae_=$(enemy_count "$(first_c "$OUT_DIR/arena_1.cksum")")
	if [ "$RC" = 0 ] && [ -n "$ce" ] && [ -n "$ae_" ] && [ "$ce" != "$ae_" ]; then
		pass "conflict: first C line enemy count $ce, arena $ae_ (newfiles' caveinfo was used)"
	else
		fail "conflict: exit $RC, first C line enemy count '${ce:-?}', arena '${ae_:-?}' (must differ)"
	fi
	run_replay conflict_rev conflict-rev "$LS"
	exp=$(sj 'd["expect"]["conflict-rev"][0]')
	if grep -qF "$exp" "$OUT_DIR/conflict_rev.log"; then pass "conflict-rev: '$exp'"; else fail "conflict-rev: no line '$exp'"; fi
	out=$(python3 "$NETLOG" cmp-cksum "$OUT_DIR/arena_1.cksum" "$OUT_DIR/conflict_rev.cksum" 2>&1)
	if [ "$RC" = 0 ] && grep -q '^identical' <<<"$out"; then
		pass "conflict-rev: C lines equal arena's ($(tail -n 1 <<<"$out"))"
	else
		fail "conflict-rev: exit $RC, $(head -n 1 <<<"$out") against arena"
	fi
fi

# ---- 9 bad ----------------------------------------------------------------------------------------
if want bad; then
	begin_check 9 bad
	run_replay bad bad "$LS"
	log="$OUT_DIR/bad.log"
	nbad=0
	okbad=0
	while IFS=$'\t' read -r folder ids path frag what; do
		nbad=$((nbad + 1))
		hit=""
		skip=""
		for id in $ids; do
			lines=$(grep -F "mppm: mods: error $id: " "$log")
			[ "$path" != "-" ] && lines=$(grep -F ": $path: " <<<"$lines")
			l=$(grep -Ei -m 1 -- "$frag" <<<"$lines")
			[ -n "$l" ] && hit="$l"
			grep -qF "mppm: mods: skipped $id" "$log" && skip=1
		done
		if [ -n "$hit" ] && [ -n "$skip" ]; then
			okbad=$((okbad + 1))
			info "bad $folder ($what): ${hit#*mppm: mods: }"
		else
			fail "bad: $folder ($what): $([ -n "$hit" ] || echo "no error line with $([ "$path" != - ] && echo "path '$path' and ")/$frag/; ")$([ -n "$skip" ] || echo "no 'skipped' line")"
		fi
	done < <(python3 -I -c '
import json, sys
d = json.load(open(sys.argv[1]))
for b in d["expect"]["bad"]:
    print("\t".join([b["folder"], " ".join(b["ids"]), b["path"] or "-", b["fragment"], b["what"]]))
' "$SCEN_JSON")
	[ "$okbad" = "$nbad" ] && pass "bad: all $nbad bad mods reported with their error and skipped"
	while read -r exp; do
		if grep -qF -- "$exp" "$log"; then pass "bad: '${exp#mppm: mods: }'"; else fail "bad: no line '$exp'"; fi
	done < <(sj 'd["expect"]["bad_warnings"]')
	if [ "$RC" = 0 ]; then pass "bad: exit 0 (no crash)"; else fail "bad: exit $RC (see $log)"; fi
	good=$(sj 'd["expect"]["bad_good_mod"]')
	bn=$(counts_of "$(first_c "$OUT_DIR/bad.cksum")")
	if grep -q "mppm: mods: folder .*: 1 on ($good@" "$log" && [ -n "$bn" ] && [ "$bn" = "$ARENA_N" ]; then
		pass "bad: $good mounted beside them (first C line n=$bn as arena)"
	else
		fail "bad: $good not mounted as expected (summary: $(grep -m 1 'mppm: mods: folder' "$log"); n=${bn:-?}, arena n=${ARENA_N:-?})"
	fi
fi

# ---- 10 record ------------------------------------------------------------------------------------
[ -z "$CUR" ] || end_check
REC="$OUT_DIR/rec.mpnl"
if want record || want refuse; then
	if [ -z "${RAN[record]:-}" ]; then rm -f -- "$REC"; fi
	run_replay record arena "$LS" --record "$REC"
fi
if want record; then
	begin_check 10 record
	dump=$(python3 "$NETLOG" dump "$REC" --events 2>&1)
	printf '%s\n' "$dump" >"$OUT_DIR/rec_dump.txt"
	ahash=$(python3 -I "$MODHASH" "$(mods_dir arena)" | sed -n 's/^mppm: mods: hash \([0-9a-f]*\) .*/\1/p')
	[ -n "$ahash" ] || ahash="(modhash.py failed)"
	ver=$(sj 'd["version"]')
	if [ "$RC" = 0 ] && grep -Eiq 'version[^0-9]*3\b' <<<"$dump" && grep -qiF -- "$ahash" <<<"$dump" &&
		grep -qF "mppm-test-vs-arena@$ver" <<<"$dump"; then
		pass "record: rec.mpnl is version 3 with mods hash ${ahash:0:16}... and mppm-test-vs-arena@$ver"
	else
		fail "record: exit $RC; netlog dump must show version 3, mods hash $ahash and mppm-test-vs-arena@$ver (see $OUT_DIR/rec_dump.txt)"
	fi
fi

# ---- 11 refuse ------------------------------------------------------------------------------------
if want refuse; then
	begin_check 11 refuse
	if [ ! -s "$REC" ]; then
		fail "refuse: no recording $REC"
	else
		run_replay refuse_stock "" "$REC"
		if [ "$RC" = 2 ] && grep -q 'mppm: net: the replay was recorded with other content' "$OUT_DIR/refuse_stock.log"; then
			pass "refuse: without mods exit 2: $(grep -m 1 'recorded with other content' "$OUT_DIR/refuse_stock.log" | sed 's/.*mppm: net: //')"
		else
			fail "refuse: without mods exit $RC, expected 2 and the 'recorded with other content' line"
		fi
		run_replay refuse_identity identity "$REC"
		if [ "$RC" = 2 ] && grep -q 'recorded with other content' "$OUT_DIR/refuse_identity.log"; then
			pass "refuse: with the identity mods exit 2"
		else
			fail "refuse: with the identity mods exit $RC, expected 2"
		fi
		run_replay refuse_arena arena "$REC"
		out=$(python3 "$NETLOG" cmp-cksum "$OUT_DIR/arena_1.cksum" "$OUT_DIR/refuse_arena.cksum" 2>&1)
		if [ "$RC" = 0 ] && grep -q '^identical' <<<"$out"; then
			pass "refuse: with arena exit 0 and C lines equal check 4's ($(tail -n 1 <<<"$out"))"
		else
			fail "refuse: with arena exit $RC, $(head -n 1 <<<"$out") against arena_1"
		fi
	fi
fi

# ---- 12 launcher ----------------------------------------------------------------------------------
# xkey KEY...: press keys on the Xvfb display, one at a time
xkey() {
	local k
	for k in "$@"; do
		DISPLAY=":$DISPLAY_NUM" xdotool key --clearmodifiers "$k" 2>/dev/null
		sleep 0.6
	done
}
xshot() { magick import -display ":$DISPLAY_NUM" -window root "$OUT_DIR/$1" 2>/dev/null; }
order_ids() { grep -v '^[[:space:]]*#' "$1" 2>/dev/null | sed 's/^[[:space:]]*-\{0,1\}//; s/[[:space:]]*$//' | grep . | tr '\n' ' '; }

if want launcher; then
	begin_check 12 launcher
	if ! command -v xdotool >/dev/null; then
		fail "launcher: xdotool not found"
	else
		lm="$OUT_DIR/launcher-mods"
		rm -rf -- "$lm"
		cp -a -- "$(mods_dir conflict)" "$lm"
		name=launcher
		home="$OUT_DIR/home/$name"
		rm -f -- "$OUT_DIR"/{start_initial,mods_overview,mods_detail,mods_pending,start_menu}.png
		wait_display_free
		# the remembered disc and a chosen preset: the live start menu over the booted game
		cfg=$(python3 -I -c 'import json,sys; print(json.dumps({"$version": 0, "backend.discPath": sys.argv[1],
			"interface.presetChosen": True, "interface.checkForUpdates": False, "interface.reduceMotion": True}, indent=4))' "$DISC")
		rm -rf -- "$home"
		mkdir -p "$home/.local/share/mppm" "$home/.config"
		printf '%s\n' "$cfg" >"$home/.local/share/mppm/config.json"
		t0=$SECONDS
		env -u WAYLAND_DISPLAY -u XDG_RUNTIME_DIR -u MPPM_SKIP_LAUNCHER -u MPPM_NO_MODS GDK_BACKEND=x11 \
			DBUS_SESSION_BUS_ADDRESS=unix:path=/nonexistent SDL_VIDEODRIVER=x11 SDL_AUDIODRIVER=dummy \
			HOME="$home" XDG_DATA_HOME="$home/.local/share" XDG_CONFIG_HOME="$home/.config" \
			XDG_CACHE_HOME="$home/.cache" MPPM_MODS_DIR="$lm" \
			timeout "$RUN_TIMEOUT" xvfb-run -n "$DISPLAY_NUM" -s "-screen 0 1280x960x24" \
			"$BUILD_DIR/mppm" >"$OUT_DIR/$name.log" 2>&1 </dev/null &
		CHILD_PID=$!
		win=""
		for _ in $(seq 1 120); do
			win=$(DISPLAY=":$DISPLAY_NUM" xdotool search --name '^(mppm|Moonsprout)$' 2>/dev/null | head -n 1)
			[ -n "$win" ] && break
			kill -0 "$CHILD_PID" 2>/dev/null || break
			sleep 0.5
		done
		if [ -z "$win" ]; then
			fail "launcher: no window appeared (see $OUT_DIR/$name.log)"
		else
			DISPLAY=":$DISPLAY_NUM" xdotool windowfocus "$win" 2>/dev/null
			sleep 10 # the start menu's entrance and the game booting behind it
			xshot start_initial.png
			xkey Down Return
			sleep 1.5
			xshot mods_overview.png
			for _ in $(seq 1 "$UI_LIST_DOWNS"); do xkey Down; done
			sleep 1
			xshot mods_detail.png
			# shellcheck disable=SC2086 # key lists are space-separated on purpose
			xkey Right $UI_OFF_KEYS Return
			sleep 1.5
			xshot mods_pending.png
			cp -- "$lm/load_order.txt" "$OUT_DIR/load_order_after_off.txt" 2>/dev/null
			# shellcheck disable=SC2086
			xkey $UI_UP_KEYS Return
			sleep 1.5
			cp -- "$lm/load_order.txt" "$OUT_DIR/load_order_after_up.txt" 2>/dev/null
			# The first Escape takes the focus from the detail pane back to the list (Pane
			# convention); the second closes the window and returns the focus to the Mods item.
			xkey Escape
			sleep 0.8
			xkey Escape
			sleep 1.5
			xshot start_menu.png
			# Quit: Mods -> Settings -> Quit
			xkey Down Down Return
		fi
		w=0
		while kill -0 "$CHILD_PID" 2>/dev/null && [ "$w" -lt 60 ]; do sleep 0.5; w=$((w + 1)); done
		if kill -0 "$CHILD_PID" 2>/dev/null; then
			info "launcher: still running after Quit; stopping it"
			kill -TERM "$CHILD_PID" 2>/dev/null
		fi
		finish "$name"
		info "launcher exit $RC after $((SECONDS - t0)) s"
		if [ -n "$win" ]; then
			if grep -qx -- '-mppm-test-vs-newfiles' "$OUT_DIR/load_order_after_off.txt" 2>/dev/null; then
				pass "launcher: Turn Off wrote '-mppm-test-vs-newfiles' to load_order.txt"
			else
				fail "launcher: after Turn Off load_order.txt is: $(tr '\n' '|' <"$OUT_DIR/load_order_after_off.txt" 2>/dev/null) (set MPPM_MODS_UI_LIST_DOWNS / MPPM_MODS_UI_OFF_KEYS if the keys missed)"
			fi
			ids=$(order_ids "$OUT_DIR/load_order_after_up.txt")
			if [ "$ids" = "mppm-test-vs-newfiles mppm-test-vs-arena " ]; then
				pass "launcher: Move Up put mppm-test-vs-newfiles above mppm-test-vs-arena"
			else
				fail "launcher: after Move Up the order is '${ids:-?}' (set MPPM_MODS_UI_UP_KEYS if the key missed)"
			fi
			shots=(start_initial mods_overview mods_detail mods_pending start_menu)
			bad=""
			for s in "${shots[@]}"; do
				{ [ -s "$OUT_DIR/$s.png" ] && not_blank "$OUT_DIR/$s.png"; } || bad="$bad $s"
			done
			same=""
			for p in "mods_overview mods_detail" "mods_detail mods_pending" "start_initial mods_overview" "mods_overview start_menu"; do
				read -r a b <<<"$p"
				n=$(ae "$OUT_DIR/$a.png" "$OUT_DIR/$b.png")
				[ -n "$n" ] && [ "$n" -gt 0 ] || same="$same $a=$b"
			done
			if [ -z "$bad" ] && [ -z "$same" ]; then
				pass "launcher: screenshots non-blank and different (${shots[*]/%/.png}); check mods_pending.png for the pending-restart note and start_menu.png for the focus on Mods"
			else
				fail "launcher: screenshots${bad:+ blank or missing:$bad}${same:+ identical:$same}"
			fi
		fi
	fi
fi

# ---- 13 whole -------------------------------------------------------------------------------------
# A mod that replaces enemyParms.szs as a whole (the disc's own bytes) and a mod with inner files of
# it: the later one wins (README 5). Whole archive last: the inner files drop, so the run is stock.
# Inner files last: they patch the replaced archive, so the run equals check 5's.
if want whole; then
	begin_check 13 whole
	arch=$(sj 'd["archives"]["enemyparms"]')
	run_replay whole archive-whole "$LS"
	expect_golden whole "whole (inner files before the whole archive dropped)"
	while read -r exp; do
		if grep -qF -- "$exp" "$OUT_DIR/whole.log"; then pass "whole: '${exp#mppm: mods: }'"; else fail "whole: no line '$exp' (got: $(grep 'mppm: mods: conflict' "$OUT_DIR/whole.log" | head -n 1))"; fi
	done < <(sj 'd["expect"]["whole"]')
	if applied "$OUT_DIR/whole.log" "$arch"; then fail "whole: an inner override of $arch was applied"; else pass "whole: no inner override of $arch applied"; fi
	run_replay whole_rev archive-whole-rev "$LS"
	while read -r exp; do
		if grep -qF -- "$exp" "$OUT_DIR/whole_rev.log"; then pass "whole-rev: '${exp#mppm: mods: }'"; else fail "whole-rev: no line '$exp'"; fi
	done < <(sj 'd["expect"]["whole-rev"]')
	if applied "$OUT_DIR/whole_rev.log" "$arch"; then pass "whole-rev: inner overrides of $arch applied (mem)"; else fail "whole-rev: no 'archive override $arch:... applied (mem' line"; fi
	rc_rev=$RC
	run_replay enemy_1 enemy "$LS"
	out=$(python3 "$NETLOG" cmp-cksum "$OUT_DIR/enemy_1.cksum" "$OUT_DIR/whole_rev.cksum" 2>&1)
	if [ "$rc_rev" = 0 ] && grep -q '^identical' <<<"$out"; then
		pass "whole-rev: exit 0, C lines equal check 5's ($(tail -n 1 <<<"$out"))"
	else
		fail "whole-rev: exit $rc_rev, $(head -n 1 <<<"$out") against enemy_1"
	fi
fi

# ---- summary -------------------------------------------------------------------------------------
[ -z "$CUR" ] || end_check
echo
if [ "${#FAILED[@]}" -gt 0 ]; then
	echo "Failures (${#FAILED[@]}):"
	for f in "${FAILED[@]}"; do echo "  $f"; done
fi
echo "mods_test: $([ "$FAILS" = 0 ] && echo PASS || echo "FAIL ($FAILS checks failed)") in $((SECONDS - T_START)) s; outputs in $OUT_DIR"
[ "$FAILS" = 0 ]
