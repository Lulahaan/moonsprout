#!/usr/bin/env bash
# m53b_test.sh - the done test of M5.3b part 1: local online sessions (docs/online/M5_3B_NETCODE.md,
# "Done test").
#
# Usage: tools/net/m53b_test.sh [-b BUILD_DIR] [-d DISPLAY] [-o OUT_DIR] [-B BASE] [-q] [-P] [-k]
#                               [-c CHECK[,CHECK...]] [-l] [--dry-run]
#   -b  build directory holding mppm and the test tools   (default ./build-pc)
#   -d  first Xvfb display: the test uses D to D+7        (default 72)
#   -o  output directory                                  (default /var/tmp/mppm-online/m53b)
#   -B  base commit of the lines check                    (default aa6b6985e)
#   -q  quick: 3600-tick bat2, bat4 and ch2, m53a_test.sh -q
#   -P  run bat2 and bat4 at the same time
#   -k  skip the build check
#   -c  run only these checks
#   -l  list the checks and exit
#   --dry-run  print every command line the selected checks would run, run nothing
#
# Run from the repo root on the host with MPPM_TEST_DISC set. Every game runs under its own Xvfb
# display with its own home directory inside OUT_DIR; the lobby server, the relay and the TURN
# server bind 127.0.0.1 only (ports 47200-47299). Environment: MPPM_M53B_TIMEOUT (seconds per game
# run, default 1800), MPPM_GOLDEN (passed on, default strict), MPPM_SERVER_BIN (default
# server/target/release/mppm-server), MPPM_DISTROBOX (default mppm-build). One PASS or FAIL line per
# check and a final table; exit 0 only if every selected check passed.

set -u

BUILD_DIR="./build-pc"
DISPLAY_BASE=72
OUT_DIR="/var/tmp/mppm-online/m53b"
BASE="aa6b6985e"
QUICK=0
PARALLEL=0
SKIP_BUILD=0
ONLY=""
LIST=0
DRY=0
DISC="${MPPM_TEST_DISC:-}"
RUN_TIMEOUT="${MPPM_M53B_TIMEOUT:-1800}"
export MPPM_GOLDEN="${MPPM_GOLDEN:-strict}"
SERVER_BIN="${MPPM_SERVER_BIN:-server/target/release/mppm-server}"
DISTROBOX="${MPPM_DISTROBOX:-mppm-build}"
SEED=20261122
SYNTH_SEED=7
# ports (the tests package's range 47200-47299)
PORT_HTTP=47200
PORT_RELAY=47201
PORT_TURN=47202
PORT_HTTP2=47203
RELAY_PORTS="47210-47299"
URL="http://127.0.0.1:$PORT_HTTP"
URL2="http://127.0.0.1:$PORT_HTTP2"
WAN_SPEC="delay=46ms,jitter=8ms"           # W: 100 ms mean RTT between host and client
LOSS_SPEC="delay=20ms,jitter=4ms,loss=2%"
CODE_WAIT=240       # seconds the host may take to create its lobby
DROP_AT=1500        # drop: kill -9 seat 3 once a client's sync tick reaches this
LEAVE_AT=900
HANG_AT=900       # stuck: the client's game thread sleeps after this tick ...
HANG_MS=30000     # ... this long, its net thread running on
UI_HANG_AT=450    # ui: the client's game hangs 7 s after this tick, so the host's Drop button shows
DESYNC_AT=900
LAT_PRESSES=30

usage() {
	sed -n '2,23p' "$0" | sed 's/^# \{0,1\}//'
	exit 2
}

while [ $# -gt 0 ]; do
	case "$1" in
	-b) BUILD_DIR="${2:?-b needs a value}"; shift 2 ;;
	-d) DISPLAY_BASE="${2:?-d needs a value}"; shift 2 ;;
	-o) OUT_DIR="${2:?-o needs a value}"; shift 2 ;;
	-B) BASE="${2:?-B needs a value}"; shift 2 ;;
	-q) QUICK=1; shift ;;
	-P) PARALLEL=1; shift ;;
	-k) SKIP_BUILD=1; shift ;;
	-c) ONLY="${2:?-c needs a value}"; shift 2 ;;
	-l) LIST=1; shift ;;
	--dry-run) DRY=1; shift ;;
	-h | --help) usage ;;
	*) echo "unknown argument: $1" >&2; usage ;;
	esac
done

CHECKS=(lines build selftest golden bat2 bat4 ch2 relay ice turn loss drop leave stuck desync spectator ui latency report)
declare -A CHECK_DESC=(
	[lines]="src/ and include/: only vsGS_Title.cpp changed since -B, same line counts, added lines guarded; tools/net/golden unchanged"
	[build]="distrobox ninja mppm p2net_selftest p2transport_test p2net_turn p2lobby_test p2libm_selftest; cargo build --release --locked"
	[selftest]="netlog.py, netstats.py, p2net_selftest, transport_test.sh, lobby_test.sh, cargo test, MPPM_RNG_SELFTEST, MPPM_NPLAYER_SELFTEST"
	[golden]="tools/net/m53a_test.sh (-q with -q) exits 0: offline and harness behaviour unchanged"
	[bat2]="(a) 2P Battle, host + 1 client, B2, WAN, 18000 ticks: identical, replay verifies, stall < 2%, phase p99 < 2 ms"
	[bat4]="(b) 2v2 Battle, host + 3 clients, B4, WAN, 18000 ticks: as bat2 over 4 logs, n=4/, version 4 recording"
	[ch2]="(c) 2P Challenge course 0, C2, WAN, 5400 ticks: as bat2, version 5 recording (start mode 2, course 0)"
	[relay]="(d) S2 through the builtin relay, 3600 ticks: identical, link=relay, relay mapping up/down bytes > 100000"
	[ice]="(d) S2 over ICE, 1800 ticks: identical, link=ice, ice connected local=host remote=host"
	[turn]="(d) S2 over ICE relay-only through p2net_turn and a static-mode server, 1800 ticks: identical, ice connected ... relay"
	[loss]="(e) S2 with $LOSS_SPEC, 3600 ticks: identical, replay verifies, stall rate < 5%"
	[drop]="(f) 2v2 B4 4500 ticks, WAN, seat 3 killed at sync tick >= $DROP_AT: 3 peers finish identical, same drop tick E, seat 3 neutral from E, stall < 12 s"
	[leave]="(f) S2 2700 ticks, client leaves at $LEAVE_AT: client reason=left exit 0, host drop slot=1 reason=left within 2 ticks, host stall < 1 s"
	[stuck]="(f) S2 2700 ticks, client MPPM_NET_TEST_HANG_AT=$HANG_AT:$HANG_MS (game stuck, net thread alive): host 'made no progress for 10 s', drop slot=1 reason=timeout, exit 0, stall 9-12 s; client exit 8"
	[desync]="(g) host + client + spectator, S2, client MPPM_NET_TEST_DESYNC=$DESYNC_AT: all exit 7 within 30 s, desync at tick $DESYNC_AT, reports"
	[spectator]="(h) S2 + a spectator, 3600 ticks: all exit 0, spectator checksum log = host's, players' stall < 2%"
	[ui]="(i) two UI peers driven by tools/net/m53b_ui_drive.sh: 7 screenshots and the lobby/session log lines"
	[latency]="(j) --latency-probe offline (M0) and online at RTT 0/30/80 ms: median <= M0 + d x 33.4 + 15 ms"
	[report]="the final table: results, stall rates, d, phase p99, kbps, relay bytes, latency medians"
)

if [ "$LIST" = 1 ]; then
	for c in "${CHECKS[@]}"; do printf '%-10s %s\n' "$c" "${CHECK_DESC[$c]}"; done
	echo "logs: B2 (vs-stage 0, 18000), B4 (vs-stage 0, 18000, 4 players), C2 (ch-stage 0, 5400), S2 (vs-stage 0, 3600)"
	echo "      from netlog.py synth --seed $SEED --synth-seed $SYNTH_SEED; WAN = --net-impair $WAN_SPEC,seed=<peer index + 1>"
	echo "displays: D..D+7 (default 72..79); ports: server $PORT_HTTP, relay $PORT_RELAY, relay ports $RELAY_PORTS, TURN $PORT_TURN, second server $PORT_HTTP2"
	exit 0
fi

case "$DISPLAY_BASE" in '' | *[!0-9]*) echo "bad -d: $DISPLAY_BASE" >&2; exit 2 ;; esac
case "$RUN_TIMEOUT" in '' | *[!0-9]*) echo "bad MPPM_M53B_TIMEOUT: $RUN_TIMEOUT" >&2; exit 2 ;; esac

NETLOG="tools/net/netlog.py"
NETSTATS="tools/net/netstats.py"
M53A="tools/net/m53a_test.sh"
UI_DRIVE="tools/net/m53b_ui_drive.sh"
if [ ! -f "$NETLOG" ] || [ ! -f "$NETSTATS" ] || [ ! -f "$M53A" ]; then
	echo "run from the repo root ($NETLOG not found)" >&2
	exit 2
fi
REPO_ROOT=$(pwd)

SEL=()
if [ -n "$ONLY" ]; then
	IFS=',' read -r -a SEL <<<"$ONLY"
	for c in "${SEL[@]}"; do
		[ -n "${CHECK_DESC[$c]:-}" ] || { echo "unknown check: $c (-l lists them)" >&2; exit 2; }
	done
else
	SEL=("${CHECKS[@]}")
fi
if [ "$SKIP_BUILD" = 1 ]; then
	tmp=()
	for c in "${SEL[@]}"; do [ "$c" = build ] || tmp+=("$c"); done
	SEL=("${tmp[@]}")
fi
want() { local x; for x in "${SEL[@]}"; do [ "$x" = "$1" ] && return 0; done; return 1; }

if [ "$QUICK" = 1 ]; then
	TICKS_BAT=3600
	TICKS_CH=3600
else
	TICKS_BAT=18000
	TICKS_CH=5400
fi
D0=$DISPLAY_BASE
D1=$((D0 + 1))
D2=$((D0 + 2))
D3=$((D0 + 3))
D4=$((D0 + 4))
D5=$((D0 + 5))

# ---- state and helpers ------------------------------------------------------------------------
FAILS=0
FAIL_LIST=()
declare -A RESULT
declare -A PEER_SUB    # check/peer -> pid of the subshell that runs (and retries) the game
declare -A PEER_DIR    # check/peer -> output directory
PEER_KEYS=()
SERVER_PIDS=()
MAIN_SERVER_PID=""
PROBLEMS=()
NOTES=()

exec 3>&1
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; FAILS=$((FAILS + 1)); FAIL_LIST+=("$*"); }
info() { echo "      $*"; }
# show CMD...: in a dry run, print a command line (quoted so that it can be pasted)
show() {
	local a out=""
	for a in "$@"; do
		if [[ "$a" =~ ^[A-Za-z0-9_./:=,@%+-]+$ ]]; then
			out+=" $a"
		else
			out+=" '${a//\'/\'\\\'\'}'"
		fi
	done
	echo "  \$${out}" >&3
}
problem() { PROBLEMS+=("$*"); }
fresh() { [ "$DRY" = 1 ] && return 0; rm -rf -- "$1"; mkdir -p "$1"; } # fresh DIR: empty output directory
note() { NOTES+=("$*"); }
joined() { local out="" x; for x in "$@"; do out+="${out:+; }$x"; done; echo "$out"; }

# finish_check NAME: one PASS or FAIL line from PROBLEMS/NOTES; result file for the report
finish_check() {
	local name="$1"
	if [ "$DRY" = 1 ]; then
		echo "DRY   $name (commands above)"
		PROBLEMS=()
		NOTES=()
		return
	fi
	mkdir -p "$OUT_DIR/$name"
	if [ "${#PROBLEMS[@]}" = 0 ]; then
		RESULT[$name]=PASS
		pass "$name: $(joined "${NOTES[@]}")"
	else
		RESULT[$name]=FAIL
		fail "$name: $(joined "${PROBLEMS[@]}")"
	fi
	echo "${RESULT[$name]}" >"$OUT_DIR/$name/result"
	PROBLEMS=()
	NOTES=()
}

mkdir -p "$OUT_DIR" || exit 2
MARKER="$OUT_DIR/.started"
[ "$DRY" = 1 ] || : >"$MARKER"

kill_tree_of() { # kill_tree_of PIDFILE SIGNAL: the game's timeout process (its process group)
	local p
	p=$(cat "$1" 2>/dev/null)
	[ -n "$p" ] && kill -0 "$p" 2>/dev/null && kill "-$2" "$p" 2>/dev/null
}

cleanup() {
	local k p
	for k in "${PEER_KEYS[@]}"; do
		kill_tree_of "${PEER_DIR[$k]}/${k##*/}.pid" TERM
	done
	for k in "${PEER_KEYS[@]}"; do
		p="${PEER_SUB[$k]:-}"
		[ -n "$p" ] && wait "$p" 2>/dev/null
	done
	for p in "${SERVER_PIDS[@]}"; do
		kill -0 "$p" 2>/dev/null && kill -TERM "$p" 2>/dev/null && wait "$p" 2>/dev/null
	done
	if [ "$DRY" != 1 ]; then
		find "$OUT_DIR" . -maxdepth 4 -type f \( -name core -o -name 'core.[0-9]*' \) \
			-newer "$MARKER" -delete 2>/dev/null
		rm -f -- "$MARKER"
	fi
}
trap 'cleanup; exit 130' INT TERM
trap cleanup EXIT

# Every game runs under this headless environment (section 0 of the design), with inherited online and
# harness variables removed so that only the flags given below apply.
GAME_ENV=(env -u WAYLAND_DISPLAY -u XDG_RUNTIME_DIR -u MPPM_MODS_DIR -u MPPM_NO_MODS
	-u MPPM_SERVER_URL -u MPPM_NET_NAME -u MPPM_NET_INPUT -u MPPM_NET_IMPAIR -u MPPM_NET_MODE
	-u MPPM_NET_FORCE_RELAY -u MPPM_NET_BIND -u MPPM_NET_ALLOW_PLAINTEXT -u MPPM_NET_ICE_RELAY_ONLY
	-u MPPM_NET_TEST_DESYNC -u MPPM_NET_TEST_LEAVE_AT -u MPPM_NET_TEST_BOT_LOAD_MS -u MPPM_NET_TEST_SLEW_NS
	-u MPPM_CH_STAGE -u MPPM_VS_STAGE -u MPPM_VS_PLAYERS -u MPPM_VS_HANDICAP -u MPPM_REPLAY -u MPPM_RECORD
	-u MPPM_CHECKSUM_LOG -u MPPM_CHECKSUM_VERIFY -u MPPM_SEED -u MPPM_EXIT_AFTER_TICKS -u MPPM_SKIP_LAUNCHER
	-u MPPM_WINDOW GDK_BACKEND=x11 DBUS_SESSION_BUS_ADDRESS=unix:path=/nonexistent SDL_VIDEODRIVER=x11
	SDL_AUDIODRIVER=dummy)

wait_display_free() { # wait_display_free D: the previous run's Xvfb may still hold it
	local w=0
	while [ -e "/tmp/.X$1-lock" ] && [ "$w" -lt 50 ]; do
		sleep 0.2
		w=$((w + 1))
	done
	sleep 1
}

# peer_start CHECK PEER DISPLAY KIND "ENVS" ARGS...: start one game in the background under Xvfb.
#   KIND: cli (command-line peer: MPPM_SKIP_LAUNCHER=1, 640x480 window), ui (launcher, 1280x960),
#         off (offline run: MPPM_SKIP_LAUNCHER=1, 640x480)
#   ENVS: space-separated VAR=value (no value holds a space).
# Files in OUT_DIR/CHECK: PEER.log (console), PEER.pid (the timeout process, leader of the game's
# process group), PEER.exit (exit code), PEER.end (epoch seconds at exit); home OUT_DIR/CHECK/PEER/home.
# One retry when Xvfb was not ready ("x11 not available").
peer_start() {
	local check="$1" peer="$2" disp="$3" kind="$4" envs="$5"
	shift 5
	local dir="$OUT_DIR/$check" key="$check/$peer"
	local home="$dir/$peer/home"
	local -a env_arr=() kind_env=()
	[ -n "$envs" ] && read -r -a env_arr <<<"$envs"
	case "$kind" in
	cli | off) kind_env=(MPPM_SKIP_LAUNCHER=1 MPPM_WINDOW=640x480) ;;
	ui) kind_env=(MPPM_WINDOW=1280x960) ;;
	esac
	local -a own=(HOME="$home" XDG_DATA_HOME="$home/.local/share"
		XDG_CONFIG_HOME="$home/.config" XDG_CACHE_HOME="$home/.cache"
		"${kind_env[@]}" "${env_arr[@]}"
		timeout "$RUN_TIMEOUT" xvfb-run -n "$disp" -s "-screen 0 1280x960x24" "$BUILD_DIR/mppm" "$@")
	local -a cmd=("${GAME_ENV[@]}" "${own[@]}")
	if [ "$DRY" = 1 ]; then
		echo "  [$key on :$disp] (GAME_ENV, then)" >&3
		show "${own[@]}"
		return 0
	fi
	mkdir -p "$dir" || return 2
	rm -rf -- "$dir/$peer"
	rm -f -- "$dir/$peer".{log,pid,exit,end,cksum,mpnl}
	mkdir -p "$home/.local/share/mppm" "$home/.config" || return 2
	if [ "$kind" = ui ]; then
		python3 - "$home/.local/share/mppm/config.json" "$DISC" "$URL" "$peer" <<'EOF' || return 2
import json, sys
path, disc, url, name = sys.argv[1:5]
cfg = {"$version": 0, "backend.discPath": disc, "interface.presetChosen": True,
       "online.serverUrl": url, "online.playerName": name.capitalize()}
with open(path, "w") as f:
    json.dump(cfg, f, indent=4)
    f.write("\n")
EOF
	fi
	(
		rc=255
		for attempt in 1 2; do
			wait_display_free "$disp"
			"${cmd[@]}" >"$dir/$peer.log" 2>&1 </dev/null &
			echo "$!" >"$dir/$peer.pid"
			wait "$!"
			rc=$?
			if [ "$rc" = 134 ] && [ "$attempt" = 1 ] && grep -q "x11 not available" "$dir/$peer.log"; then
				echo "      $key: Xvfb not ready (x11 not available), retrying" >&3
				sleep 2
				continue
			fi
			break
		done
		date +%s.%N >"$dir/$peer.end"
		echo "$rc" >"$dir/$peer.exit"
		rm -rf -- "$home/.cache"
	) &
	PEER_SUB[$key]=$!
	PEER_DIR[$key]=$dir
	PEER_KEYS+=("$key")
}

peer_alive() { [ "$DRY" != 1 ] && [ ! -f "${PEER_DIR[$1]}/${1##*/}.exit" ]; }
peer_rc() { cat "$OUT_DIR/$1.exit" 2>/dev/null || echo none; }
peer_wait() { # peer_wait KEY...: until each game has exited
	local k
	[ "$DRY" = 1 ] && return 0
	for k in "$@"; do
		[ -n "${PEER_SUB[$k]:-}" ] && wait "${PEER_SUB[$k]}" 2>/dev/null
	done
	return 0
}
peer_stop() { # peer_stop KEY...: SIGTERM to the game's process group, then wait
	local k
	[ "$DRY" = 1 ] && return 0
	for k in "$@"; do kill_tree_of "${PEER_DIR[$k]}/${k##*/}.pid" TERM; done
	peer_wait "$@"
}
# game_pid KEY: the pid of the mppm process itself (in the timeout's process group)
game_pid() {
	# by executable, not by name: the game renames its main thread ("Main thread"), so comm is not mppm
	local t p
	t=$(cat "${PEER_DIR[$1]}/${1##*/}.pid" 2>/dev/null)
	[ -n "$t" ] || return 0
	for p in $(pgrep -g "$t" 2>/dev/null); do
		case "$(readlink "/proc/$p/exe" 2>/dev/null)" in
		*/mppm) echo "$p"; return 0 ;;
		esac
	done
}
# log_wait FILE REGEX SECONDS [KEY]: 0 once FILE has a line matching REGEX (extended); 1 after
# SECONDS or when the game KEY has exited
log_wait() {
	local f="$1" re="$2" t="$3" k="${4:-}" n=0
	[ "$DRY" = 1 ] && return 0
	while [ "$n" -lt $((t * 4)) ]; do
		grep -qE -- "$re" "$f" 2>/dev/null && return 0
		[ -n "$k" ] && ! peer_alive "$k" && { grep -qE -- "$re" "$f" 2>/dev/null; return; }
		sleep 0.25
		n=$((n + 1))
	done
	return 1
}
# sync_tick FILE: the newest 'sync tick=' of a client log (0 when none)
sync_tick() {
	local t
	t=$(grep -oE 'mppm: net: sync tick=[0-9]+' "$1" 2>/dev/null | tail -n 1 | sed 's/.*=//')
	echo "${t:-0}"
}
# sync_wait FILE TICK SECONDS [KEY]: until the client's sync tick reaches TICK
sync_wait() {
	local f="$1" want="$2" t="$3" k="${4:-}" n=0
	[ "$DRY" = 1 ] && return 0
	while [ "$n" -lt $((t * 4)) ]; do
		[ "$(sync_tick "$f")" -ge "$want" ] && return 0
		[ -n "$k" ] && ! peer_alive "$k" && return 1
		sleep 0.25
		n=$((n + 1))
	done
	return 1
}

# ---- servers ----------------------------------------------------------------------------------
# server_start DIR NAME ARGS...: start mppm-server with --log-file DIR/NAME.log (stderr DIR/NAME.err),
# wait for its ready line; sets LAST_SERVER_PID
server_start() {
	local dir="$1" name="$2"
	shift 2
	LAST_SERVER_PID=""
	if [ "$DRY" = 1 ]; then
		show "$SERVER_BIN" "$@" --log-file "$dir/$name.log"
		return 0
	fi
	if [ ! -x "$SERVER_BIN" ]; then
		problem "no server binary at $SERVER_BIN (build check, or cargo build --release --locked in server/)"
		return 1
	fi
	mkdir -p "$dir"
	rm -f -- "$dir/$name.log" "$dir/$name.err"
	"$SERVER_BIN" "$@" --log-file "$dir/$name.log" >"$dir/$name.out" 2>"$dir/$name.err" </dev/null &
	LAST_SERVER_PID=$!
	SERVER_PIDS+=("$LAST_SERVER_PID")
	local n=0
	while [ "$n" -lt 80 ]; do
		if grep -q 'mppm-server: listening' "$dir/$name.err" "$dir/$name.log" 2>/dev/null; then
			return 0
		fi
		kill -0 "$LAST_SERVER_PID" 2>/dev/null || break
		sleep 0.25
		n=$((n + 1))
	done
	problem "$name did not print 'mppm-server: listening' (see $dir/$name.err)"
	return 1
}
server_stop() { # server_stop PID: SIGTERM (members get closed, relay mappings are logged), wait
	[ "$DRY" = 1 ] && return 0
	[ -n "${1:-}" ] && kill -0 "$1" 2>/dev/null && kill -TERM "$1" 2>/dev/null
	[ -n "${1:-}" ] && wait "$1" 2>/dev/null
	return 0
}
main_server_start() { # main_server_start DIR: the builtin-relay server of every online check
	server_start "$1" server --bind "127.0.0.1:$PORT_HTTP" --relay --relay-bind 127.0.0.1 \
		--relay-port "$PORT_RELAY" --relay-ports "$RELAY_PORTS" --limits-scale 20
	local rc=$?
	MAIN_SERVER_PID=$LAST_SERVER_PID
	return $rc
}
main_server_stop() {
	server_stop "$MAIN_SERVER_PID"
	MAIN_SERVER_PID=""
}
server_lines() { cat "$1/server.err" "$1/server.log" 2>/dev/null; } # server_lines DIR

# ---- sessions ---------------------------------------------------------------------------------
# session_start CHECK MODE STAGE TICKS URL EXPECT HOSTSPEC [PEERSPEC...]
#   spec: NAME|DISPLAY|SEAT|INPUT|ENVS|ARGS. SEAT 0..3 or 'spec' (spectator); INPUT the --net-input log
#   ('' = live input); ENVS VAR=value list; ARGS extra flags (both split on spaces).
#   The first spec is the host (--host --ready --slot 0 --code-file); the others join once the code
#   file exists. EXPECT: --expect N for the host ('' = none). Sets SESSION_PEERS (keys); returns 1
#   when the host made no lobby.
session_start() {
	local check="$1" mode="$2" stage="$3" ticks="$4" url="$5" expect="$6"
	shift 6
	local dir="$OUT_DIR/$check" spec name disp seat input envs args code="" first=1
	SESSION_PEERS=()
	[ "$DRY" = 1 ] || mkdir -p "$dir"
	[ "$DRY" = 1 ] || rm -f -- "$dir/code"
	for spec in "$@"; do
		IFS='|' read -r name disp seat input envs args <<<"$spec"
		local -a a=("$DISC" --server "$url" --net-bind 127.0.0.1 --name "$name"
			--checksum-log "$dir/$name.cksum" --record "$dir/$name.mpnl" --exit-after-ticks "$ticks"
			--cvar game.idleDemo=false)
		local -a extra=()
		[ -n "$args" ] && read -r -a extra <<<"$args"
		if [ "$first" = 1 ]; then
			a+=(--host --ready --slot 0 --code-file "$dir/code" --mode "$mode" --stage "$stage")
			[ -n "$expect" ] && a+=(--expect "$expect")
		else
			if [ -z "$code" ]; then
				if [ "$DRY" = 1 ]; then
					code="<CODE from $dir/code>"
				else
					local n=0
					while [ ! -s "$dir/code" ] && [ "$n" -lt $((CODE_WAIT * 4)) ] && peer_alive "${SESSION_PEERS[0]}"; do
						sleep 0.25
						n=$((n + 1))
					done
					code=$(tr -d ' \r\n' <"$dir/code" 2>/dev/null)
					if [ -z "$code" ]; then
						problem "the host wrote no lobby code to $dir/code within $CODE_WAIT s (exit $(peer_rc "${SESSION_PEERS[0]}"); see $dir/${SESSION_PEERS[0]##*/}.log)"
						peer_stop "${SESSION_PEERS[@]}"
						return 1
					fi
					info "$check: lobby $code"
				fi
			fi
			if [ "$seat" = spec ]; then
				a+=(--join "$code" --spectate)
			else
				a+=(--join "$code" --slot "$seat" --ready)
			fi
		fi
		[ -n "$input" ] && a+=(--net-input "$input")
		peer_start "$check" "$name" "$disp" cli "$envs" "${a[@]}" "${extra[@]}"
		SESSION_PEERS+=("$check/$name")
		first=0
	done
	return 0
}

wan() { echo "--net-impair $WAN_SPEC,seed=$1"; } # wan PEER_INDEX+1

# ---- evaluation helpers (each adds to PROBLEMS or NOTES) --------------------------------------
diff_summary() {
	python3 "$NETLOG" cmp-cksum "$1" "$2" 2>&1 | awk '
		/^differ/ { d = $0 } /^  fields:/ { f = $0; sub(/^  /, "", f) }
		END { if (d != "") print d (f != "" ? "; " f : ""); else print "no difference found by cmp-cksum" }'
}
ev_exit() { # ev_exit KEY WANT
	local rc
	[ "$DRY" = 1 ] && return
	rc=$(peer_rc "$1")
	[ "$rc" = "$2" ] || problem "${1##*/} exit $rc, expected $2 (see $OUT_DIR/$1.log)"
}
ev_cmp() { # ev_cmp DIR REF OTHER...: checksum logs identical to REF's
	local dir="$1" ref="$2" o
	shift 2
	for o in "$@"; do
		if [ "$DRY" = 1 ]; then
			show cmp "$dir/$ref.cksum" "$dir/$o.cksum"
		elif [ -s "$dir/$ref.cksum" ] && [ -f "$dir/$o.cksum" ] && cmp -s -- "$dir/$ref.cksum" "$dir/$o.cksum"; then
			note "$o.cksum = $ref.cksum"
		else
			problem "$o.cksum differs from $ref.cksum: $(diff_summary "$dir/$ref.cksum" "$dir/$o.cksum")"
		fi
	done
}
ev_eline() { # ev_eline FILE TICKS
	local e
	[ "$DRY" = 1 ] && return
	e=$(awk '$1 == "E" { t = $2 } END { print t }' "$1" 2>/dev/null)
	[ "$e" = "$2" ] && note "E line at $2" || problem "$(basename "$1"): E line at '${e:-none}', expected $2"
}
# ev_clines FILE MIN: sets NC (C lines). One sample line every 30 ticks: a sample taken while a floor
# loads is an L line, not a C line (a Battle round that ends inside the run reloads the floor, and when
# that happens depends on the session's random seed), so the rule counts C and L lines together: at
# least MIN of them, the E line's lines= equal to their count, and at least 95% of MIN in game (C lines).
ev_clines() {
	local nl e min95=$(($2 - $2 / 20))
	NC=0
	[ "$DRY" = 1 ] && return
	NC=$(grep -c '^C ' "$1" 2>/dev/null)
	nl=$(grep -c '^L ' "$1" 2>/dev/null)
	e=$(awk '$1 == "E" { for (i = 3; i <= NF; ++i) if ($i ~ /^lines=/) { sub(/^lines=/, "", $i); v = $i } } END { print v }' "$1" 2>/dev/null)
	if [ $((${NC:-0} + ${nl:-0})) -ge "$2" ] && [ "${e:-x}" = $((${NC:-0} + ${nl:-0})) ] && [ "${NC:-0}" -ge "$min95" ]; then
		note "$NC C lines + ${nl:-0} L lines (E lines=$e)"
	else
		problem "$(basename "$1"): ${NC:-0} C + ${nl:-0} L lines, E lines=${e:-none} (need C+L >= $2 = E lines, C >= $min95)"
	fi
}
ev_navi() { # ev_navi CKSUM: more than min(100, C lines / 2) distinct navi hashes
	local navi need
	if [ "$DRY" = 1 ]; then show python3 "$NETLOG" stats "$1"; return; fi
	navi=$(python3 "$NETLOG" stats "$1" 2>/dev/null | sed -n 's/^distinct:.* navi=\([0-9]*\).*/\1/p')
	need=$(( ${NC:-0} / 2 ))
	[ "$need" -gt 100 ] && need=100
	[ "${navi:-0}" -gt "$need" ] && note "$navi distinct navi hashes" ||
		problem "${navi:-no} distinct navi hashes (need more than $need)"
}
ev_cmp_input() { # ev_cmp_input DIR REF OTHER...
	local dir="$1" ref="$2" o ci
	shift 2
	for o in "$@"; do
		if [ "$DRY" = 1 ]; then show python3 "$NETLOG" cmp-input "$dir/$ref.mpnl" "$dir/$o.mpnl"; continue; fi
		if ci=$(python3 "$NETLOG" cmp-input "$dir/$ref.mpnl" "$dir/$o.mpnl" 2>&1); then
			note "cmp-input $ref $o: $(tail -n 1 <<<"$ci")"
		else
			problem "cmp-input $ref.mpnl $o.mpnl: $(tr '\n' ' ' <<<"$ci")"
		fi
	done
}
ev_rng() { # ev_rng DIR NAME...: other-in-session=0 in each log
	local dir="$1" n o
	shift
	[ "$DRY" = 1 ] && return
	for n in "$@"; do
		o=$(grep 'mppm: net: rng ' "$dir/$n.log" 2>/dev/null | grep -v selftest | tail -n 1 |
			sed -n 's/.*other-in-session=\([0-9]*\).*/\1/p')
		[ "$o" = 0 ] || problem "$n: other-in-session=${o:-? (no rng line)}"
	done
}
ev_bad() { # ev_bad DIR NAME...: no PANIC, failed assertion or crash screen
	local dir="$1" n b
	shift
	[ "$DRY" = 1 ] && return
	for n in "$@"; do
		b=$(grep -m 1 -E 'PANIC|Failed assertion|crash screen' "$dir/$n.log" 2>/dev/null)
		[ -z "$b" ] || problem "$n: $b"
	done
}
ev_netstats() { # ev_netstats DIR "CHECK ARGS" NAME...: netstats.py check; also writes DIR/netstats.tsv
	local dir="$1" cargs="$2" n out rc
	shift 2
	local -a logs=() ca=()
	for n in "$@"; do logs+=("$dir/$n.log"); done
	read -r -a ca <<<"$cargs"
	if [ "$DRY" = 1 ]; then
		show python3 "$NETSTATS" check "${ca[@]}" "${logs[@]}"
		show python3 "$NETSTATS" summary "${logs[@]}"
		return
	fi
	python3 "$NETSTATS" summary --format tsv "${logs[@]}" >"$dir/netstats.tsv" 2>/dev/null
	python3 "$NETSTATS" summary "${logs[@]}" >"$dir/netstats.txt" 2>&1
	sed 's/^/      /' "$dir/netstats.txt"
	out=$(python3 "$NETSTATS" check "${ca[@]}" "${logs[@]}" 2>&1)
	rc=$?
	if [ "$rc" = 0 ]; then
		note "netstats ${out#ok: }"
	else
		problem "netstats: $(grep -v '^ok' <<<"$out" | sed 's/^violation: //' | tr '\n' ';' | sed 's/;$//; s/;/; /g')"
	fi
}
# ev_replay CHECK NAME DISPLAY: offline headless replay of NAME.mpnl verifying NAME.cksum
ev_replay() {
	local check="$1" name="$2" disp="$3" dir="$OUT_DIR/$1"
	peer_start "$check" "replay_$name" "$disp" off "" "$DISC" --replay "$dir/$name.mpnl" \
		--checksum-log "$dir/replay_$name.cksum" --checksum-verify "$dir/$name.cksum" --headless-ticks
	peer_wait "$check/replay_$name"
	[ "$DRY" = 1 ] && return
	local rc
	rc=$(peer_rc "$check/replay_$name")
	if [ "$rc" = 0 ]; then
		note "offline replay of $name.mpnl verifies"
	else
		problem "offline replay of $name.mpnl exit $rc (3 = checksum mismatch; see $dir/replay_$name.log)"
	fi
	ev_rng "$dir" "replay_$name"
}
# ev_dump_has FILE REGEX WHAT: a line of 'netlog.py dump FILE --events' matches
ev_dump_has() {
	if [ "$DRY" = 1 ]; then show python3 "$NETLOG" dump "$1" --events; return; fi
	if python3 "$NETLOG" dump "$1" --events 2>/dev/null | grep -qE -- "$2"; then
		note "$3"
	else
		problem "$(basename "$1"): not $3 ($(python3 "$NETLOG" dump "$1" --events 2>&1 | grep -E '^(version|start mode|course|vs stage|players):' | tr -s ' ' | tr '\n' ' '))"
	fi
}

# ---- preconditions ------------------------------------------------------------------------------
T_START=$SECONDS
if [ "$DRY" = 1 ]; then
	echo "GAME_ENV (the prefix of every game command below):"
	show "${GAME_ENV[@]}"
fi
echo "m53b_test: build $BUILD_DIR, displays :$D0-:$((D0 + 7)), out $OUT_DIR, base $BASE$([ "$QUICK" = 1 ] && echo ', quick')$([ "$PARALLEL" = 1 ] && echo ', bat2+bat4 in parallel')$([ "$DRY" = 1 ] && echo ', DRY RUN'), checks ${SEL[*]}"
NEED_GAME=0
NEED_SERVER=0
for c in "${SEL[@]}"; do
	case "$c" in lines | build | report) ;; *) NEED_GAME=1 ;; esac
	case "$c" in bat2 | bat4 | ch2 | relay | ice | turn | loss | drop | leave | stuck | desync | spectator | ui | latency) NEED_SERVER=1 ;; esac
done
if [ "$DRY" != 1 ] && [ "$NEED_GAME" = 1 ]; then
	pre=()
	want build || [ -x "$BUILD_DIR/mppm" ] || pre+=("no executable at $BUILD_DIR/mppm")
	[ -n "$DISC" ] && [ -f "$DISC" ] || pre+=("disc not found (set MPPM_TEST_DISC to your own disc image)")
	for d in $(seq "$D0" $((D0 + 7))); do
		[ -e "/tmp/.X$d-lock" ] && pre+=("display :$d is in use (pick another base with -d)")
	done
	for tool in xvfb-run timeout python3 pgrep; do
		command -v "$tool" >/dev/null || pre+=("$tool not found")
	done
	if want ui || want latency; then
		command -v xdotool >/dev/null || pre+=("xdotool not found (ui, latency)")
	fi
	want ui && ! command -v magick >/dev/null && pre+=("magick (ImageMagick 7) not found (ui)")
	if [ "$NEED_SERVER" = 1 ] && command -v ss >/dev/null; then
		busy=$(ss -Hltnu 2>/dev/null | awk '{ print $5 }' | grep -E ":(4720[0-3])$" | head -n 3 | tr '\n' ' ')
		[ -z "$busy" ] || pre+=("ports in use: $busy (another server is running)")
	fi
	if [ "${#pre[@]}" -gt 0 ]; then
		for p in "${pre[@]}"; do echo "FAIL  $p"; done
		exit 1
	fi
fi

# ---- logs -----------------------------------------------------------------------------------------
LOG_B2="$OUT_DIR/B2.mpnl"
LOG_B4="$OUT_DIR/B4.mpnl"
LOG_C2="$OUT_DIR/C2.mpnl"
LOG_S2="$OUT_DIR/S2.mpnl"
if [ "$NEED_SERVER" = 1 ]; then
	echo
	echo "== Logs"
	SYNTH=(python3 "$NETLOG" synth --seed "$SEED" --synth-seed "$SYNTH_SEED")
	for spec in "B2|--vs-stage 0 --ticks 18000" "B4|--vs-stage 0 --ticks 18000 --players 4" \
		"C2|--ch-stage 0 --ticks 5400" "S2|--vs-stage 0 --ticks 3600"; do
		IFS='|' read -r n a <<<"$spec"
		read -r -a args <<<"$a"
		if [ "$DRY" = 1 ]; then
			show "${SYNTH[@]}" "${args[@]}" -o "$OUT_DIR/$n.mpnl"
		elif "${SYNTH[@]}" "${args[@]}" -o "$OUT_DIR/$n.mpnl" >/dev/null; then
			info "synth $n ($a)"
		else
			echo "FAIL  synth $n ($a)"
			exit 1
		fi
	done
fi

# ==== lines ======================================================================================
if want lines; then
	echo
	echo "== lines"
	if ! git rev-parse --verify -q "$BASE^{commit}" >/dev/null; then
		problem "base $BASE is not a commit"
	else
		changed=0
		while IFS= read -r f; do
			[ -n "$f" ] || continue
			changed=$((changed + 1))
			if [ ! -f "$f" ] || ! git cat-file -e "$BASE:$f" 2>/dev/null; then
				problem "$f added or removed"
				continue
			fi
			[ "$f" = src/plugProjectKandoU/vsGS_Title.cpp ] || problem "$f changed (only vsGS_Title.cpp may change in M5.3b)"
			a=$(git show "$BASE:$f" | wc -l)
			b=$(wc -l <"$f")
			[ "$a" = "$b" ] || problem "$f: $a lines at $BASE, $b now"
		done < <(git diff --name-only "$BASE" -- src include)
		while IFS= read -r f; do
			[ -n "$f" ] && problem "$f: untracked new file"
		done < <(git ls-files --others --exclude-standard -- src include)
		unguarded=$(git diff -U0 "$BASE" -- src include | grep '^+[^+]' | grep -v 'PC_ONLY(\|PC_SWITCH(' | head -n 5)
		[ -z "$unguarded" ] || problem "added lines without PC_ONLY( or PC_SWITCH(: $(tr '\n' '|' <<<"$unguarded")"
		git diff --quiet "$BASE" -- tools/net/golden || problem "tools/net/golden changed since $BASE"
		[ -z "$(git ls-files --others --exclude-standard -- tools/net/golden)" ] || problem "new files in tools/net/golden"
		hunks=$(git diff -U0 "$BASE" -- src/plugProjectKandoU/vsGS_Title.cpp | sed -n 's/^@@ -[0-9,]* +\([0-9,]*\) @@.*/\1/p' | tr '\n' ' ' | sed 's/ $//')
		if [ "$changed" = 0 ]; then
			note "no file changed in src/ or include/ since $BASE (the vsGS_Title.cpp:192 edit of the lockstep package is not merged here)"
		else
			note "$changed file(s) changed since $BASE, line counts equal, every added line guarded, vsGS_Title.cpp hunks at ${hunks:-none}"
			grep -q 'p2_net_ch_inject' src/plugProjectKandoU/vsGS_Title.cpp 2>/dev/null &&
				note "vsGS_Title.cpp calls p2_net_ch_inject"
		fi
		note "tools/net/golden unchanged"
	fi
	if [ "$DRY" = 1 ]; then show git diff --name-only "$BASE" -- src include; fi
	finish_check lines
fi

# ==== build ======================================================================================
if want build; then
	echo
	echo "== build"
	targets=(mppm p2net_selftest p2transport_test p2net_turn p2lobby_test p2libm_selftest)
	inner="cd $(printf '%q' "$REPO_ROOT") && cmake $(printf '%q' "$BUILD_DIR") >/dev/null && ninja -C $(printf '%q' "$BUILD_DIR") ${targets[*]}"
	if command -v distrobox >/dev/null; then
		bcmd=(distrobox enter "$DISTROBOX" -- bash -lc "$inner")
	else
		bcmd=(bash -lc "$inner")
	fi
	if [ "$DRY" = 1 ]; then
		show "${bcmd[@]}"
		show bash -c "cd server && ~/.cargo/bin/cargo build --release --locked"
	else
		mkdir -p "$OUT_DIR/build"
		info "ninja ${targets[*]} (output in $OUT_DIR/build/ninja.txt)"
		"${bcmd[@]}" >"$OUT_DIR/build/ninja.txt" 2>&1
		r=$?
		[ "$r" = 0 ] && note "ninja ${targets[*]} exit 0" || problem "ninja exit $r ($(tail -n 1 "$OUT_DIR/build/ninja.txt"))"
		CARGO=$(command -v cargo || echo "$HOME/.cargo/bin/cargo")
		info "cargo build --release --locked (output in $OUT_DIR/build/cargo.txt)"
		(cd server && "$CARGO" build --release --locked) >"$OUT_DIR/build/cargo.txt" 2>&1
		r=$?
		[ "$r" = 0 ] && note "cargo build --release --locked exit 0" || problem "cargo build exit $r ($(tail -n 1 "$OUT_DIR/build/cargo.txt"))"
	fi
	finish_check build
fi

# ==== selftest ===================================================================================
if want selftest; then
	echo
	echo "== selftest"
	dir="$OUT_DIR/selftest"
	[ "$DRY" = 1 ] || mkdir -p "$dir"
	# step NAME OUTFILE CMD...: one self-test step, exit 0 wanted
	step() {
		local name="$1" outf="$2"
		shift 2
		if [ "$DRY" = 1 ]; then show "$@"; return; fi
		"$@" >"$outf" 2>&1
		local r=$?
		[ "$r" = 0 ] && note "$name ok" || problem "$name exit $r ($(tail -n 1 "$outf"))"
	}
	step "netlog.py selftest" "$dir/netlog.txt" python3 "$NETLOG" selftest
	step "netstats.py --selftest" "$dir/netstats.txt" python3 "$NETSTATS" --selftest
	if [ -x "$BUILD_DIR/p2net_selftest" ] || [ "$DRY" = 1 ]; then
		step p2net_selftest "$dir/p2net_selftest.txt" "$BUILD_DIR/p2net_selftest"
	else
		problem "no $BUILD_DIR/p2net_selftest (build check)"
	fi
	if [ -f tests/net/transport/transport_test.sh ] || [ "$DRY" = 1 ]; then
		# the relay case runs against the real server's builtin relay (transport_test.sh --relay-server)
		main_server_start "$dir"
		step transport_test.sh "$dir/transport_test.txt" bash tests/net/transport/transport_test.sh -b "$BUILD_DIR" \
			-o "$dir/transport" --relay-server "$URL" --server-log "$dir/server.log"
		main_server_stop
	else
		problem "no tests/net/transport/transport_test.sh (transport package not merged)"
	fi
	if [ -f tests/net/lobby/lobby_test.sh ] || [ "$DRY" = 1 ]; then
		step lobby_test.sh "$dir/lobby_test.txt" env MPPM_LOBBY_TEST_OUT="$dir/lobby" bash tests/net/lobby/lobby_test.sh
	else
		problem "no tests/net/lobby/lobby_test.sh (server package not merged)"
	fi
	if [ -f server/Cargo.toml ] || [ "$DRY" = 1 ]; then
		CARGO=$(command -v cargo || echo "$HOME/.cargo/bin/cargo")
		step "cargo test --locked" "$dir/cargo_test.txt" bash -c "cd server && $(printf '%q' "$CARGO") test --locked"
	else
		problem "no server/Cargo.toml (server package not merged)"
	fi
	# the game's self-tests exit before main; bounded like m53a_test.sh
	saved_timeout=$RUN_TIMEOUT
	RUN_TIMEOUT=180
	peer_start selftest rng "$D0" off "MPPM_RNG_SELFTEST=1" "$DISC"
	peer_wait selftest/rng
	if [ "$DRY" != 1 ]; then
		if [ "$(peer_rc selftest/rng)" = 0 ] && grep -q 'mppm: net: rng selftest PASS' "$dir/rng.log"; then
			note "MPPM_RNG_SELFTEST PASS"
		else
			problem "MPPM_RNG_SELFTEST exit $(peer_rc selftest/rng)"
		fi
	fi
	for part in core piki vs view; do
		peer_start selftest "nplayer_$part" "$D0" off "MPPM_NPLAYER_SELFTEST=$part" "$DISC" --exit-after-ticks 300
		peer_wait "selftest/nplayer_$part"
		[ "$DRY" = 1 ] && continue
		if [ "$(peer_rc "selftest/nplayer_$part")" = 0 ] && grep -q "mppm: nplayer selftest $part PASS" "$dir/nplayer_$part.log"; then
			note "MPPM_NPLAYER_SELFTEST=$part PASS"
		else
			problem "MPPM_NPLAYER_SELFTEST=$part exit $(peer_rc "selftest/nplayer_$part")"
		fi
	done
	RUN_TIMEOUT=$saved_timeout
	finish_check selftest
fi

# ==== golden =====================================================================================
if want golden; then
	echo
	echo "== golden"
	qa=()
	[ "$QUICK" = 1 ] && qa=(-q)
	gcmd=(env MPPM_TEST_DISC="$DISC" MPPM_M53A_TIMEOUT="$RUN_TIMEOUT" bash "$M53A" -b "$BUILD_DIR" -d "$D0" -o "$OUT_DIR/m53a" "${qa[@]}")
	if [ "$DRY" = 1 ]; then
		show "${gcmd[@]}"
	else
		[ -x "$BUILD_DIR/p2libm_selftest" ] ||
			info "warning: no $BUILD_DIR/p2libm_selftest: desync_ci.sh's libm check will fail (the build check builds it)"
		mkdir -p "$OUT_DIR/golden"
		info "running $M53A${qa[*]:+ ${qa[*]}} -d $D0 -o $OUT_DIR/m53a (output in $OUT_DIR/golden/m53a.txt)"
		"${gcmd[@]}" >"$OUT_DIR/golden/m53a.txt" 2>&1
		g=$?
		last=$(tail -n 1 "$OUT_DIR/golden/m53a.txt")
		if [ "$g" = 0 ]; then
			note "m53a_test.sh${qa[*]:+ ${qa[*]}} exit 0 ($(grep -c '^PASS' "$OUT_DIR/golden/m53a.txt") PASS lines; $last)"
		else
			grep '^FAIL' "$OUT_DIR/golden/m53a.txt" | head -n 5 | while IFS= read -r l; do info "$l"; done
			problem "m53a_test.sh exit $g ($last)"
		fi
	fi
	finish_check golden
fi

# ==== bat2, bat4, ch2 ============================================================================
# The checks of (a)-(c): start and finish are split so that -P can run bat2 and bat4 together.
bat2_start() {
	session_start bat2 battle 0 "$TICKS_BAT" "$URL" "" \
		"host|$D0|0|$LOG_B2||$(wan 1)" \
		"c1|$D1|1|$LOG_B2||$(wan 2)"
}
bat4_start() {
	session_start bat4 2v2 0 "$TICKS_BAT" "$URL" "" \
		"host|$D2|0|$LOG_B4||$(wan 1)" \
		"c1|$D3|1|$LOG_B4||$(wan 2)" \
		"c2|$D4|2|$LOG_B4||$(wan 3)" \
		"c3|$D5|3|$LOG_B4||$(wan 4)"
}
ch2_start() {
	session_start ch2 challenge 0 "$TICKS_CH" "$URL" "" \
		"host|$D0|0|$LOG_C2||$(wan 1)" \
		"c1|$D1|1|$LOG_C2||$(wan 2)"
}
# lockstep_eval CHECK TICKS REPLAY_DISPLAY NAME...: the common pass criteria of bat2/bat4/ch2
lockstep_eval() {
	local check="$1" ticks="$2" rdisp="$3" dir="$OUT_DIR/$1"
	shift 3
	local -a names=("$@")
	local n
	for n in "${names[@]}"; do ev_exit "$check/$n" 0; done
	ev_cmp "$dir" host "${names[@]:1}"
	ev_eline "$dir/host.cksum" "$ticks"
	ev_clines "$dir/host.cksum" $((ticks / 30))
	ev_cmp_input "$dir" host "${names[@]:1}"
	ev_replay "$check" host "$rdisp"
	ev_netstats "$dir" "--max-stall-rate 2.0 --max-phase-p99-us 2000" "${names[@]}"
	ev_navi "$dir/host.cksum"
	ev_rng "$dir" "${names[@]}"
	ev_bad "$dir" "${names[@]}"
}
bat2_finish() {
	peer_wait "${SESSION_BAT2[@]}"
	lockstep_eval bat2 "$TICKS_BAT" "$D0" host c1
	finish_check bat2
}
bat4_finish() {
	peer_wait "${SESSION_BAT4[@]}"
	local dir="$OUT_DIR/bat4"
	lockstep_eval bat4 "$TICKS_BAT" "$D2" host c1 c2 c3
	if [ "$DRY" != 1 ]; then
		local nc nc4
		nc=$(grep -c '^C ' "$dir/host.cksum" 2>/dev/null)
		nc4=$(grep '^C ' "$dir/host.cksum" 2>/dev/null | grep -c ' n=4/')
		[ "${nc:-0}" -gt 0 ] && [ "$nc" = "$nc4" ] && note "every C line n=4/" || problem "${nc4:-0} of ${nc:-0} C lines with n=4/"
	fi
	ev_dump_has "$dir/host.mpnl" '^players:     4$' "players 4"
	ev_dump_has "$dir/host.mpnl" '^version:     4$' "a version 4 recording"
	finish_check bat4
}

if want bat2 || want bat4; then
	echo
	echo "== bat2 / bat4"
	SESSION_BAT2=()
	SESSION_BAT4=()
	[ "$DRY" = 1 ] || { want bat2 && rm -rf -- "$OUT_DIR/bat2"; want bat4 && rm -rf -- "$OUT_DIR/bat4"; }
	if [ "$PARALLEL" = 1 ] && want bat2 && want bat4; then
		[ "$DRY" = 1 ] || rm -rf -- "$OUT_DIR/bat2+bat4"
		main_server_start "$OUT_DIR/bat2+bat4"
		bat2_start && SESSION_BAT2=("${SESSION_PEERS[@]}")
		bat4_start && SESSION_BAT4=("${SESSION_PEERS[@]}")
		bat2_finish
		bat4_finish
		main_server_stop
	else
		if want bat2; then
			main_server_start "$OUT_DIR/bat2" && bat2_start && SESSION_BAT2=("${SESSION_PEERS[@]}")
			bat2_finish
			main_server_stop
		fi
		if want bat4; then
			main_server_start "$OUT_DIR/bat4" && bat4_start && SESSION_BAT4=("${SESSION_PEERS[@]}")
			bat4_finish
			main_server_stop
		fi
	fi
fi

if want ch2; then
	echo
	echo "== ch2"
	[ "$DRY" = 1 ] || rm -rf -- "$OUT_DIR/ch2"
	main_server_start "$OUT_DIR/ch2" && ch2_start && peer_wait "${SESSION_PEERS[@]}"
	main_server_stop
	lockstep_eval ch2 "$TICKS_CH" "$D0" host c1
	ev_dump_has "$OUT_DIR/ch2/host.mpnl" '^version:     5$' "a version 5 recording"
	ev_dump_has "$OUT_DIR/ch2/host.mpnl" '^start mode:  2 ' "start mode 2"
	ev_dump_has "$OUT_DIR/ch2/host.mpnl" '^course:      0$' "course 0"
	finish_check ch2
fi

# ==== relay, ice, turn ===========================================================================
# s2_pair CHECK URL TICKS "HOST ENVS" "HOST ARGS" "CLIENT ENVS" "CLIENT ARGS": a 2P S2 session
s2_pair() {
	local check="$1" url="$2" ticks="$3"
	session_start "$check" battle 0 "$ticks" "$url" "" \
		"host|$D0|0|$LOG_S2|$4|$5" \
		"c1|$D1|1|$LOG_S2|$6|$7"
}

if want relay; then
	echo
	echo "== relay"
	fresh "$OUT_DIR/relay"
	dir="$OUT_DIR/relay"
	main_server_start "$dir" && s2_pair relay "$URL" 3600 "" "--net-mode relay" "" "--net-mode relay" &&
		peer_wait "${SESSION_PEERS[@]}"
	main_server_stop # the relay mappings are logged when they are freed
	ev_exit relay/host 0
	ev_exit relay/c1 0
	ev_cmp "$dir" host c1
	if [ "$DRY" != 1 ]; then
		l=$(grep -m 1 -E 'mppm: net: peer [0-9]+ connected link=relay' "$dir/c1.log" 2>/dev/null)
		[ -n "$l" ] && note "client: ${l#*mppm: net: }" || problem "no 'peer ... connected link=relay' line in c1.log"
		best=$(server_lines "$dir" | grep 'mppm-server: relay mapping' | awk '{
			u = 0; d = 0
			for (i = 1; i <= NF; i++) { if ($i ~ /^up_bytes=/) { sub(/^up_bytes=/, "", $i); u = $i } if ($i ~ /^down_bytes=/) { sub(/^down_bytes=/, "", $i); d = $i } }
			m = (u < d) ? u : d
			if (m > bm) { bm = m; bu = u; bd = d }
		} END { if (bm != "") print bu, bd }')
		bu=${best% *}
		bd=${best#* }
		echo "${best:-0 0}" >"$dir/relay_bytes.txt"
		if [ -n "$best" ] && [ "$bu" -gt 100000 ] && [ "$bd" -gt 100000 ]; then
			note "relay mapping up_bytes=$bu down_bytes=$bd"
		else
			problem "no 'relay mapping' line with up and down bytes above 100000 in the server log (best: ${best:-none})"
		fi
	fi
	ev_netstats "$dir" "--max-stall-rate -1 --max-phase-p99-us -1 --require-link relay" host c1
	ev_rng "$dir" host c1
	ev_bad "$dir" host c1
	finish_check relay
fi

if want ice; then
	echo
	echo "== ice"
	fresh "$OUT_DIR/ice"
	dir="$OUT_DIR/ice"
	main_server_start "$dir" && s2_pair ice "$URL" 1800 "" "--net-mode ice" "" "--net-mode ice" &&
		peer_wait "${SESSION_PEERS[@]}"
	main_server_stop
	ev_exit ice/host 0
	ev_exit ice/c1 0
	ev_cmp "$dir" host c1
	if [ "$DRY" != 1 ]; then
		l=$(cat "$dir/host.log" "$dir/c1.log" 2>/dev/null | grep -m 1 -E 'mppm: net: peer [0-9]+ connected link=ice')
		[ -n "$l" ] && note "${l#*mppm: net: }" || problem "no 'peer ... connected link=ice' line"
		l=$(cat "$dir/host.log" "$dir/c1.log" 2>/dev/null | grep -m 1 -E 'mppm: net: ice connected .*local=host remote=host')
		[ -n "$l" ] && note "${l#*mppm: net: }" || problem "no 'ice connected ... local=host remote=host' line"
	fi
	ev_netstats "$dir" "--max-stall-rate -1 --max-phase-p99-us -1 --require-link ice" host c1
	ev_rng "$dir" host c1
	ev_bad "$dir" host c1
	finish_check ice
fi

if want turn; then
	echo
	echo "== turn"
	fresh "$OUT_DIR/turn"
	dir="$OUT_DIR/turn"
	TURN_PID=""
	turn_cmd=("$BUILD_DIR/p2net_turn" --bind 127.0.0.1 --port "$PORT_TURN" --user u --pass p)
	if [ "$DRY" = 1 ]; then
		show "${turn_cmd[@]}"
	elif [ -x "$BUILD_DIR/p2net_turn" ]; then
		"${turn_cmd[@]}" >"$dir/turn.log" 2>&1 </dev/null &
		TURN_PID=$!
		SERVER_PIDS+=("$TURN_PID")
		sleep 1
		kill -0 "$TURN_PID" 2>/dev/null || problem "p2net_turn exited at once (see $dir/turn.log)"
	else
		problem "no $BUILD_DIR/p2net_turn (build check)"
	fi
	if server_start "$dir" server --bind "127.0.0.1:$PORT_HTTP2" --relay-mode static \
		--turn "turn:127.0.0.1:$PORT_TURN" --turn-user u --turn-pass p --limits-scale 20; then
		S2_PID=$LAST_SERVER_PID
		[ "${#PROBLEMS[@]}" = 0 ] && s2_pair turn "$URL2" 1800 "MPPM_NET_ICE_RELAY_ONLY=1" "--net-mode ice" \
			"MPPM_NET_ICE_RELAY_ONLY=1" "--net-mode ice" && peer_wait "${SESSION_PEERS[@]}"
		server_stop "$S2_PID"
	fi
	server_stop "$TURN_PID"
	ev_exit turn/host 0
	ev_exit turn/c1 0
	ev_cmp "$dir" host c1
	if [ "$DRY" != 1 ]; then
		l=$(cat "$dir/host.log" "$dir/c1.log" 2>/dev/null | grep -m 1 -E 'mppm: net: ice connected .*(local|remote)=relay')
		[ -n "$l" ] && note "${l#*mppm: net: }" || problem "no 'ice connected ... relay' line"
	fi
	ev_netstats "$dir" "--max-stall-rate -1 --max-phase-p99-us -1 --require-link ice" host c1
	ev_bad "$dir" host c1
	finish_check turn
fi

# ==== loss =======================================================================================
if want loss; then
	echo
	echo "== loss"
	fresh "$OUT_DIR/loss"
	dir="$OUT_DIR/loss"
	main_server_start "$dir" && s2_pair loss "$URL" 3600 "" "--net-impair $LOSS_SPEC,seed=1" "" "--net-impair $LOSS_SPEC,seed=2" &&
		peer_wait "${SESSION_PEERS[@]}"
	main_server_stop
	ev_exit loss/host 0
	ev_exit loss/c1 0
	ev_cmp "$dir" host c1
	ev_replay loss host "$D0"
	ev_netstats "$dir" "--max-stall-rate 5.0 --max-phase-p99-us -1" host c1
	ev_rng "$dir" host c1
	ev_bad "$dir" host c1
	finish_check loss
fi

# ==== drop =======================================================================================
if want drop; then
	echo
	echo "== drop"
	fresh "$OUT_DIR/drop"
	dir="$OUT_DIR/drop"
	DROP_TICKS=4500
	if main_server_start "$dir" && session_start drop 2v2 0 "$DROP_TICKS" "$URL" "" \
		"host|$D0|0|$LOG_B4||$(wan 1)" "c1|$D1|1|$LOG_B4||$(wan 2)" \
		"c2|$D2|2|$LOG_B4||$(wan 3)" "c3|$D3|3|$LOG_B4||$(wan 4)"; then
		if [ "$DRY" = 1 ]; then
			show kill -9 "<pid of mppm in the process group of $dir/c3.pid, once c1.log shows sync tick >= $DROP_AT>"
		elif sync_wait "$dir/c1.log" "$DROP_AT" "$RUN_TIMEOUT" drop/c1; then
			gp=$(game_pid drop/c3)
			if [ -n "$gp" ]; then
				kill -9 "$gp"
				info "drop: kill -9 of seat 3 (pid $gp) at c1 sync tick $(sync_tick "$dir/c1.log")"
				note "seat 3 killed at sync tick $(sync_tick "$dir/c1.log")"
			else
				problem "seat 3's mppm process not found (c3 exit $(peer_rc drop/c3))"
			fi
		else
			problem "c1 never reached sync tick $DROP_AT (exit $(peer_rc drop/c1))"
		fi
		peer_wait "${SESSION_PEERS[@]}"
	fi
	main_server_stop
	for n in host c1 c2; do ev_exit "drop/$n" 0; done
	ev_cmp "$dir" host c1 c2
	ev_eline "$dir/host.cksum" "$DROP_TICKS"
	if [ "$DRY" != 1 ]; then
		es=()
		for n in host c1 c2; do
			e=$(grep -oE 'mppm: net: drop slot=3 tick=[0-9]+' "$dir/$n.log" 2>/dev/null | head -n 1 | sed 's/.*tick=//')
			es+=("${e:-none}")
		done
		if [ "${es[0]}" != none ] && [ "${es[0]}" = "${es[1]}" ] && [ "${es[0]}" = "${es[2]}" ]; then
			E=${es[0]}
			note "drop slot=3 tick=$E on all three"
			dmp=$(python3 "$NETLOG" dump "$dir/host.mpnl" --from $((E - 1)) --count 5 2>&1)
			rows=$(grep -cE '^ *[0-9]+  ' <<<"$dmp")
			neutral=$(grep -E '^ *[0-9]+  ' <<<"$dmp" | grep -cF 'P3 -      st( +0, +0) c( +0, +0) tr(  0,  0)')
			if [ "$rows" = 5 ] && [ "$neutral" = 5 ]; then
				note "host.mpnl: seat 3 neutral in records $((E - 1))-$((E + 3)) (ticks $E-$((E + 4)))"
			else
				problem "host.mpnl records $((E - 1))-$((E + 3)): seat 3 neutral in $neutral of $rows"
				grep -E '^ *[0-9]+  ' <<<"$dmp" | head -n 5 | while IFS= read -r l; do info "$l"; done
			fi
		else
			problem "drop slot=3 tick= on host/c1/c2: ${es[*]}"
		fi
	else
		show python3 "$NETLOG" dump "$dir/host.mpnl" --from "<E-1>" --count 5
	fi
	ev_netstats "$dir" "--max-stall-rate -1 --max-phase-p99-us -1 --max-stall-ms 12000" host c1 c2
	ev_rng "$dir" host c1 c2
	ev_bad "$dir" host c1 c2
	finish_check drop
fi

# ==== leave ======================================================================================
if want leave; then
	echo
	echo "== leave"
	fresh "$OUT_DIR/leave"
	dir="$OUT_DIR/leave"
	LEAVE_TICKS=2700
	main_server_start "$dir" && s2_pair leave "$URL" "$LEAVE_TICKS" "" "" "MPPM_NET_TEST_LEAVE_AT=$LEAVE_AT" "" &&
		peer_wait "${SESSION_PEERS[@]}"
	main_server_stop
	ev_exit leave/c1 0
	ev_exit leave/host 0
	ev_eline "$dir/host.cksum" "$LEAVE_TICKS"
	if [ "$DRY" != 1 ]; then
		grep -q 'mppm: net: session end reason=left' "$dir/c1.log" 2>/dev/null && note "client: session end reason=left" ||
			problem "no 'session end reason=left' in c1.log"
		e=$(grep -oE 'mppm: net: drop slot=1 tick=[0-9]+ reason=left' "$dir/host.log" 2>/dev/null | head -n 1 | sed 's/.*tick=\([0-9]*\).*/\1/')
		if [ -n "$e" ] && [ "$e" -ge $((LEAVE_AT - 2)) ] && [ "$e" -le $((LEAVE_AT + 2)) ]; then
			note "host: drop slot=1 tick=$e reason=left"
		else
			problem "host: drop slot=1 reason=left at tick '${e:-none}', expected $((LEAVE_AT - 2))-$((LEAVE_AT + 2))"
		fi
	fi
	ev_netstats "$dir" "--max-stall-rate -1 --max-phase-p99-us -1 --max-stall-ms 1000" host
	ev_rng "$dir" host
	ev_bad "$dir" host c1
	finish_check leave
fi

# ==== stuck ======================================================================================
# A client whose game thread hangs while its net thread keeps sending keepalives (fix round 2): the
# host drops it by progress, 10 s after its own game began to wait for that seat.
if want stuck; then
	echo
	echo "== stuck"
	fresh "$OUT_DIR/stuck"
	dir="$OUT_DIR/stuck"
	STUCK_TICKS=2700
	main_server_start "$dir" && s2_pair stuck "$URL" "$STUCK_TICKS" "" "" "MPPM_NET_TEST_HANG_AT=$HANG_AT:$HANG_MS" "" &&
		peer_wait "${SESSION_PEERS[@]}"
	main_server_stop
	ev_exit stuck/host 0
	ev_exit stuck/c1 8
	ev_eline "$dir/host.cksum" "$STUCK_TICKS"
	if [ "$DRY" != 1 ]; then
		grep -q "mppm: net: test: game thread hangs for $HANG_MS ms after tick $HANG_AT" "$dir/c1.log" 2>/dev/null &&
			note "client: game thread hangs after tick $HANG_AT" || problem "no 'test: game thread hangs' line in c1.log"
		grep -q 'mppm: net: peer [0-9]* made no progress for 10 s' "$dir/host.log" 2>/dev/null &&
			note "host: made no progress for 10 s" || problem "no 'made no progress for 10 s' line in host.log"
		e=$(grep -oE 'mppm: net: drop slot=1 tick=[0-9]+ reason=timeout' "$dir/host.log" 2>/dev/null | head -n 1 | sed 's/.*tick=\([0-9]*\).*/\1/')
		if [ -n "$e" ] && [ "$e" -gt "$HANG_AT" ] && [ "$e" -le $((HANG_AT + 20)) ]; then
			note "host: drop slot=1 tick=$e reason=timeout"
		else
			problem "host: drop slot=1 reason=timeout at tick '${e:-none}', expected $((HANG_AT + 1))-$((HANG_AT + 20))"
		fi
		grep -q 'mppm: net: session end reason=dropped' "$dir/c1.log" 2>/dev/null && note "client: session end reason=dropped" ||
			problem "no 'session end reason=dropped' in c1.log"
		mx=$(grep -oE 'mppm: net: summary .* max_ms=[0-9.]+' "$dir/host.log" 2>/dev/null | tail -n 1 | sed 's/.*max_ms=//')
		awk -v m="${mx:-0}" 'BEGIN { exit !(m >= 9000 && m <= 12000) }' && note "host: longest stall ${mx} ms" ||
			problem "host: longest stall '${mx:-none}' ms, expected 9000-12000"
	fi
	ev_rng "$dir" host
	ev_bad "$dir" host c1
	finish_check stuck
fi

# ==== desync =====================================================================================
if want desync; then
	echo
	echo "== desync"
	fresh "$OUT_DIR/desync"
	dir="$OUT_DIR/desync"
	T900=""
	if main_server_start "$dir" && session_start desync battle 0 1800 "$URL" 3 \
		"host|$D0|0|$LOG_S2||" "c1|$D1|1|$LOG_S2|MPPM_NET_TEST_DESYNC=$DESYNC_AT|" "spec|$D2|spec|||"; then
		if [ "$DRY" != 1 ]; then
			# T900: when tick 900 ran (the client's sync line, or the first desync line)
			while peer_alive desync/host || peer_alive desync/c1 || peer_alive desync/spec; do
				if [ -z "$T900" ]; then
					if [ "$(sync_tick "$dir/c1.log")" -ge "$DESYNC_AT" ] ||
						grep -q 'mppm: net: desync at tick' "$dir/host.log" "$dir/c1.log" "$dir/spec.log" 2>/dev/null; then
						T900=$(date +%s.%N)
					fi
				fi
				sleep 0.25
			done
		fi
		peer_wait "${SESSION_PEERS[@]}"
	fi
	main_server_stop
	if [ "$DRY" != 1 ]; then
		seed=$(sed -n 's/^# mppm-cksum v[0-9]* seed=\([0-9]*\).*/\1/p' "$dir/host.cksum" 2>/dev/null | head -n 1)
		for n in host c1 spec; do
			ev_exit "desync/$n" 7
			end=$(cat "$dir/$n.end" 2>/dev/null)
			if [ -n "$T900" ] && [ -n "$end" ]; then
				late=$(awk -v a="$end" -v b="$T900" 'BEGIN { printf "%.1f", a - b }')
				awk -v l="$late" 'BEGIN { exit !(l <= 30) }' && note "$n ended $late s after tick $DESYNC_AT" ||
					problem "$n ended $late s after tick $DESYNC_AT (limit 30 s)"
			else
				problem "$n: no tick-$DESYNC_AT time or end time"
			fi
			l=$(grep -m 1 'mppm: net: desync at tick' "$dir/$n.log" 2>/dev/null)
			if [[ "$l" == *"desync at tick $DESYNC_AT "* ]] || [[ "$l" == *"desync at tick $DESYNC_AT" ]]; then
				rep=$(sed -n 's/.*; report \(.*\)$/\1/p' <<<"$l")
				if [ -n "$rep" ] && [ -f "$rep" ]; then
					cp -- "$rep" "$dir/report_$n.txt"
					if grep -q "$DESYNC_AT" "$rep" && [ -n "$seed" ] &&
						{ grep -q "$seed" "$rep" || grep -qi "$(printf '%08x' "$seed")" "$rep"; }; then
						note "$n report names tick $DESYNC_AT and seed $seed"
					else
						problem "$n report $rep does not name tick $DESYNC_AT and the seed ${seed:-?}"
					fi
				else
					problem "$n: report '${rep:-none}' not found"
				fi
			else
				problem "$n: no 'desync at tick $DESYNC_AT' line (${l:-none})"
			fi
		done
	else
		show cat "<the report path of each 'desync at tick $DESYNC_AT ...; report PATH' line>"
	fi
	ev_bad "$dir" host c1 spec
	finish_check desync
fi

# ==== spectator ==================================================================================
if want spectator; then
	echo
	echo "== spectator"
	fresh "$OUT_DIR/spectator"
	dir="$OUT_DIR/spectator"
	main_server_start "$dir" && session_start spectator battle 0 3600 "$URL" 3 \
		"host|$D0|0|$LOG_S2||" "c1|$D1|1|$LOG_S2||" "spec|$D2|spec|||" &&
		peer_wait "${SESSION_PEERS[@]}"
	main_server_stop
	for n in host c1 spec; do ev_exit "spectator/$n" 0; done
	ev_cmp "$dir" host c1 spec
	ev_netstats "$dir" "--max-stall-rate 2.0 --max-phase-p99-us -1" host c1
	ev_rng "$dir" host c1 spec
	ev_bad "$dir" host c1 spec
	finish_check spectator
fi

# ==== ui =========================================================================================
if want ui; then
	echo
	echo "== ui"
	fresh "$OUT_DIR/ui"
	dir="$OUT_DIR/ui"
	# ui DISPLAY COMMAND [ARGS...]: one step of the launcher package's driver
	ui() {
		if [ "$DRY" = 1 ]; then show bash "$UI_DRIVE" "$@"; return 0; fi
		echo "+ $*" >>"$dir/ui_drive.log"
		bash "$UI_DRIVE" "$@" >>"$dir/ui_drive.log" 2>&1
		local r=$?
		[ "$r" = 0 ] || problem "m53b_ui_drive.sh $* exit $r (see $dir/ui_drive.log)"
		return $r
	}
	shots=(ui_settings ui_host_room ui_join ui_client_room ui_ready ui_waiting ui_host_left)
	if [ ! -f "$UI_DRIVE" ] && [ "$DRY" != 1 ]; then
		problem "no $UI_DRIVE (launcher package not merged)"
	elif main_server_start "$dir"; then
		peer_start ui host "$D0" ui "MPPM_NET_BIND=127.0.0.1" --cvar game.idleDemo=false
		peer_start ui client "$D1" ui "MPPM_NET_BIND=127.0.0.1 MPPM_NET_TEST_HANG_AT=$UI_HANG_AT:7000" --cvar game.idleDemo=false
		ok=1
		ui "$D0" wait-menu && ui "$D1" wait-menu || ok=0
		if [ "$ok" = 1 ]; then
			ui "$D0" open-settings-online && ui "$D0" shot "$dir/ui_settings.png" && ui "$D0" close
			ui "$D0" open-multiplayer && ui "$D0" host battle 0 || ok=0
		fi
		CODE=""
		if [ "$ok" = 1 ]; then
			if log_wait "$dir/host.log" 'mppm: net: lobby created code=' 60 ui/host; then
				CODE=$(grep -oE 'mppm: net: lobby created code=[0-9A-Z-]+' "$dir/host.log" 2>/dev/null | head -n 1 | sed 's/.*code=//')
				[ "$DRY" = 1 ] && CODE="<CODE from the host's 'lobby created' line>"
				note "lobby created code=$CODE"
				sleep 2
				ui "$D0" shot "$dir/ui_host_room.png"
			else
				problem "no 'lobby created' line in the host log"
				ok=0
			fi
		fi
		if [ "$ok" = 1 ]; then
			ui "$D1" open-multiplayer || ok=0
		fi
		if [ "$ok" = 1 ]; then
			# the driver shoots the join pane once the code is typed in (before the lobby opens)
			if [ "$DRY" = 1 ]; then
				show env "M53B_JOIN_SHOT=$dir/ui_join.png" bash "$UI_DRIVE" "$D1" join "$CODE"
			else
				echo "+ $D1 join $CODE (M53B_JOIN_SHOT)" >>"$dir/ui_drive.log"
				M53B_JOIN_SHOT="$dir/ui_join.png" bash "$UI_DRIVE" "$D1" join "$CODE" >>"$dir/ui_drive.log" 2>&1 ||
					{ problem "m53b_ui_drive.sh $D1 join $CODE failed"; ok=0; }
			fi
		fi
		if [ "$ok" = 1 ]; then
			log_wait "$dir/client.log" 'mppm: net: lobby joined' 60 ui/client && note "lobby joined" ||
				problem "no 'lobby joined' line in the client log"
			log_wait "$dir/host.log" 'mppm: net: peer [0-9]+ connected' 60 ui/host || problem "the host never saw the client connect"
			sleep 2
			ui "$D1" shot "$dir/ui_client_room.png"
			ui "$D1" ready && ui "$D0" ready || ok=0
			sleep 2
			ui "$D0" shot "$dir/ui_ready.png"
		fi
		if [ "$ok" = 1 ]; then
			ui "$D0" start || ok=0
			if log_wait "$dir/host.log" 'mppm: net: session start' 240 ui/host; then
				note "session start"
			else
				log_wait "$dir/client.log" 'mppm: net: session start' 5 ui/client && note "session start (client)" ||
					problem "no 'session start' line"
			fi
			sync_wait "$dir/client.log" 300 240 ui/client || { problem "the client never reached sync tick 300"; ok=0; }
		fi
		if [ "$ok" = 1 ]; then
			# The client's game thread hangs 7 s after tick UI_HANG_AT (its net thread runs on, so ENet
			# keeps the link): 6 s into it the host's wait pill shows the seconds and Drop (after 5 s).
			if log_wait "$dir/client.log" 'mppm: net: test: game thread hangs' 120 ui/client; then
				[ "$DRY" = 1 ] && show sleep 6 || sleep 6
				ui "$D0" shot "$dir/ui_waiting.png"
			else
				problem "no 'test: game thread hangs' line in the client log"
			fi
			log_wait "$dir/host.log" 'mppm: net: (stall|summary) ' 90 ui/host ||
				log_wait "$dir/client.log" 'mppm: net: (stall|summary) ' 5 ui/client && note "a stall line" ||
				problem "no 'mppm: net: stall' line"
			if [ "$DRY" = 1 ]; then
				show kill -9 "<host mppm pid>"
			else
				gp=$(game_pid ui/host)
				[ -n "$gp" ] && kill -9 "$gp"
			fi
			log_wait "$dir/client.log" 'mppm: net: session end reason=host_left' 60 ui/client && note "client: session end reason=host_left" ||
				problem "no 'session end reason=host_left' in the client log"
			sleep 3
			ui "$D1" shot "$dir/ui_host_left.png"
		fi
		peer_stop ui/host ui/client
		main_server_stop
		if [ "$DRY" != 1 ]; then
			for s in "${shots[@]}"; do
				f="$dir/$s.png"
				if [ ! -s "$f" ]; then
					problem "$s.png missing"
					continue
				fi
				m=$(magick "$f" -format '%[fx:mean]' info: 2>/dev/null)
				awk -v m="${m:-0}" 'BEGIN { exit !(m > 0.03) }' || problem "$s.png is black (mean ${m:-?})"
			done
			same=()
			for ((i = 0; i < ${#shots[@]}; i++)); do
				for ((j = i + 1; j < ${#shots[@]}; j++)); do
					a="$dir/${shots[$i]}.png"
					b="$dir/${shots[$j]}.png"
					[ -s "$a" ] && [ -s "$b" ] || continue
					px=$(magick "$a" -format '%[fx:w*h]' info: 2>/dev/null)
					ae=$(magick compare -fuzz 5% -metric AE "$a" "$b" null: 2>&1 >/dev/null | awk '{ printf "%d", $1 }')
					awk -v a="${ae:-0}" -v p="${px:-1}" 'BEGIN { exit !(a > 0.02 * p) }' && continue
					# ui_host_room and ui_ready are two states of the same host room: about 1% of the whole
					# picture changes. That pair passes when each region the change touches differs in more
					# than 2% of its pixels: the players pane (P2 row, ready pills) and the status line,
					# Ready toggle and Start game below the match settings.
					if [ "${shots[$i]}/${shots[$j]}" = ui_host_room/ui_ready ]; then
						rok=1
						for g in 430x125+105+240 600x150+576+405; do
							rae=$(magick compare -fuzz 5% -metric AE <(magick "$a" -crop "$g" +repage png:-) \
								<(magick "$b" -crop "$g" +repage png:-) null: 2>&1 >/dev/null | awk '{ printf "%d", $1 }')
							rpx=$(awk -v g="$g" 'BEGIN { split(g, d, /[x+]/); print d[1] * d[2] }')
							awk -v a="${rae:-0}" -v p="$rpx" 'BEGIN { exit !(a > 0.02 * p) }' || rok=0
							echo "host_room/ready region $g: ${rae:-?} of $rpx px" >>"$dir/ui_drive.log"
						done
						[ "$rok" = 1 ] && continue
					fi
					same+=("${shots[$i]}/${shots[$j]}")
				done
			done
			[ "${#same[@]}" = 0 ] && note "7 screenshots, each pair differs in more than 2% of pixels (host room / ready: in the players pane and the start controls)" ||
				problem "screenshot pairs differing in 2% of pixels or less: ${same[*]}"
			note "screenshots in $dir"
		fi
	fi
	finish_check ui
fi

# ==== latency ====================================================================================
# probe_presses DISPLAY: LAT_PRESSES held presses of space (0.1 s down, 0.7 s up) on the game's window
probe_presses() {
	local disp="$1" win="" i
	if [ "$DRY" = 1 ]; then
		show env DISPLAY=":$disp" xdotool search --name '^(mppm|Moonsprout)$'
		show env DISPLAY=":$disp" xdotool keydown --window "<WIN>" space "(0.1 s, then keyup, 0.7 s; $LAT_PRESSES times)"
		return 0
	fi
	for i in $(seq 1 60); do
		win=$(DISPLAY=":$disp" xdotool search --name '^(mppm|Moonsprout)$' 2>/dev/null | head -n 1)
		[ -n "$win" ] && break
		sleep 1
	done
	[ -n "$win" ] || return 1
	for i in $(seq 1 "$LAT_PRESSES"); do
		DISPLAY=":$disp" xdotool keydown --window "$win" space
		sleep 0.1
		DISPLAY=":$disp" xdotool keyup --window "$win" space
		sleep 0.7
	done
	sleep 1
}
# probe_stats LOG: "median min max" of the last LAT_PRESSES press->present samples
probe_stats() {
	grep 'mppm: probe: sim press->present' "$1" 2>/dev/null | tail -n "$LAT_PRESSES" |
		sed -n 's/.*press->present \([0-9.]*\) ms.*/\1/p' | sort -n | awk '
		{ v[NR] = $1 }
		END { if (NR == 0) exit; m = (NR % 2) ? v[(NR + 1) / 2] : (v[NR / 2] + v[NR / 2 + 1]) / 2
		      printf "%.2f %.2f %.2f %d\n", m, v[1], v[NR], NR }'
}

if want latency; then
	echo
	echo "== latency"
	fresh "$OUT_DIR/latency"
	dir="$OUT_DIR/latency"
	[ "$DRY" = 1 ] || : >"$dir/latency.tsv"
	# offline baseline M0
	peer_start latency offline "$D0" off "" "$DISC" --latency-probe --vs-stage 0
	M0=""
	if [ "$DRY" = 1 ]; then
		show sleep 25
		probe_presses "$D0"
	else
		log_wait "$dir/offline.log" 'mppm: net: (harness on|session)' 120 latency/offline
		sleep 25
		probe_presses "$D0" || problem "offline: no game window"
	fi
	peer_stop latency/offline
	if [ "$DRY" != 1 ]; then
		st=$(probe_stats "$dir/offline.log")
		M0=${st%% *}
		if [ -n "$M0" ]; then
			note "offline M0 median $M0 ms ($(awk '{ print $4 }' <<<"$st") samples)"
			printf 'offline\t-\t%s\n' "$st" >>"$dir/latency.tsv"
		else
			problem "offline: no probe samples (see $dir/offline.log)"
		fi
	fi
	printf '      %-8s %-3s %8s %8s %8s %8s\n' RTT d median min max limit
	for rtt in 0 30 80; do
		delay=$((rtt / 2))
		imp="delay=${delay}ms,jitter=1ms"
		ldir="$OUT_DIR/latency_rtt$rtt"
		fresh "$ldir"
		if main_server_start "$ldir" && session_start "latency_rtt$rtt" battle 0 3600 "$URL" "" \
			"host|$D0|0|$LOG_S2||--net-impair $imp,seed=1" \
			"c1|$D1|1|||--net-impair $imp,seed=2 --latency-probe"; then
			if sync_wait "$ldir/c1.log" 150 "$RUN_TIMEOUT" "latency_rtt$rtt/c1"; then
				probe_presses "$D1" || problem "rtt $rtt: no client window"
			else
				problem "rtt $rtt: the client never reached sync tick 150"
			fi
			peer_stop "${SESSION_PEERS[@]}"
		fi
		main_server_stop
		[ "$DRY" = 1 ] && continue
		st=$(probe_stats "$ldir/c1.log")
		d=$(grep -oE 'mppm: net: delay slot=1 d=[0-9]+' "$ldir/c1.log" 2>/dev/null | tail -n 1 | sed 's/.*d=//')
		if [ -z "$st" ]; then
			problem "RTT $rtt ms: no probe samples in the client log (see $ldir/c1.log)"
			continue
		fi
		if [ -z "$d" ]; then
			problem "RTT $rtt ms: no 'delay slot=1 d=' line in the client log (see $ldir/c1.log)"
			continue
		fi
		read -r med mn mx cnt <<<"$st"
		limit=$(awk -v m="${M0:-0}" -v d="$d" 'BEGIN { printf "%.1f", m + d * 33.4 + 15 }')
		printf '      %-8s %-3s %8s %8s %8s %8s\n' "${rtt}ms" "$d" "$med" "$mn" "$mx" "$limit"
		printf 'rtt%s\t%s\t%s\n' "$rtt" "$d" "$st" >>"$dir/latency.tsv"
		if [ -n "$M0" ] && awk -v a="$med" -v b="$limit" 'BEGIN { exit !(a <= b) }'; then
			note "RTT $rtt ms: d=$d median $med ms <= $limit"
		else
			problem "RTT $rtt ms: d=$d median $med ms above $limit ms (M0 ${M0:-?})"
		fi
	done
	for c in bat2 bat4; do
		if [ -f "$OUT_DIR/$c/netstats.txt" ]; then
			info "$c (phase error and d):"
			sed 's/^/  /' "$OUT_DIR/$c/netstats.txt"
		fi
	done
	finish_check latency
fi

# ==== report =====================================================================================
if want report; then
	echo
	echo "== report"
	if [ "$DRY" = 1 ]; then
		show python3 "$NETSTATS" summary "$OUT_DIR/<check>/<peer>.log ..."
	else
		printf '%-10s %-6s %s\n' check result "stall rate / d / phase p99 / kbps out,in per peer"
		for c in "${CHECKS[@]}"; do
			[ "$c" = report ] && continue
			r="${RESULT[$c]:-$(cat "$OUT_DIR/$c/result" 2>/dev/null || echo -)}"
			[ -z "${RESULT[$c]:-}" ] && [ "$r" != - ] && r="($r)"
			m=""
			tsv="$OUT_DIR/$c/netstats.tsv"
			if [ -s "$tsv" ]; then
				m=$(awk -F'\t' 'NR > 1 { sub(/\.log$/, "", $1); printf "%s%s %s%% d=%s p99=%s %s,%s", (n++ ? "  " : ""), $1, $6, $13, $11, $16, $17 }' "$tsv")
			fi
			[ "$c" = relay ] && [ -s "$OUT_DIR/relay/relay_bytes.txt" ] && m="$m  relay up,down bytes $(tr ' ' ',' <"$OUT_DIR/relay/relay_bytes.txt")"
			if [ "$c" = latency ] && [ -s "$OUT_DIR/latency/latency.tsv" ]; then
				m=$(awk -F'\t' '{ split($3, s, " "); printf "%s%s d=%s median %s ms", (n++ ? "  " : ""), $1, $2, s[1] }' "$OUT_DIR/latency/latency.tsv")
			fi
			printf '%-10s %-6s %s\n' "$c" "$r" "$m"
		done
		echo "(parenthesised results are from an earlier run in $OUT_DIR)"
	fi
fi

# ---- summary ------------------------------------------------------------------------------------
echo
if [ "${#FAIL_LIST[@]}" -gt 0 ]; then
	echo "Failures (${#FAIL_LIST[@]}):"
	for f in "${FAIL_LIST[@]}"; do echo "  $f"; done
fi
if [ "$DRY" = 1 ]; then
	echo "m53b_test: dry run, nothing was run"
	exit 0
fi
echo "m53b_test: $([ "$FAILS" = 0 ] && echo PASS || echo "FAIL ($FAILS checks failed)") in $((SECONDS - T_START)) s; outputs in $OUT_DIR"
[ "$FAILS" = 0 ]
