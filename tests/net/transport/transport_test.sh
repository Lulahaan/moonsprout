#!/usr/bin/env bash
# M5.3b transport tests (docs/online/M5_3B_TRANSPORT.md "Tests", design 7.7). Console only: no game, no
# display. Everything binds 127.0.0.1 (the TURN case also 127.0.0.2, see below).
#
#   tests/net/transport/transport_test.sh [-b build-pc] [-o OUT] [-c case,...] [-p BASEPORT] [-B] [-l]
#                                         [--relay-server http://127.0.0.1:PORT [--server-log FILE]]
#
#   -b DIR    build directory with p2transport_test and p2net_turn (default build-pc)
#   -o DIR    output directory (default /var/tmp/mppm-online/m53b-transport/tt)
#   -c LIST   cases to run (default all): selftest direct fanout ice turn relay wake teardown
#   -p PORT   first of the fixed test ports (default 47050; uses PORT..PORT+29)
#   -B        build the two test targets first (distrobox mppm-build when not already inside it)
#   -l        list the cases
#   --relay-server URL   run the relay case against a running mppm-server (--relay) instead of
#                        tests/net/transport/relay_stub.py: tokens come from a real session
#                        (relay_session.py: REST create + join, WebSocket attach)
#   --server-log FILE    with --relay-server: also require the server's "relay mapping" line with
#                        up_bytes and down_bytes above 50000 once the session ended
#
# One "PASS <case>" or "FAIL <case>" line per case and a final table; exit 0 only if all pass.
set -u
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
HERE="$ROOT/tests/net/transport"
BUILD="build-pc"
OUT="/var/tmp/mppm-online/m53b-transport/tt"
CASES="selftest,direct,fanout,ice,turn,relay,wake,teardown"
BASE=47050
DO_BUILD=0
RELAY_SERVER=""
SERVER_LOG=""

while [ $# -gt 0 ]; do
    case "$1" in
        -b) BUILD="$2"; shift 2 ;;
        -o) OUT="$2"; shift 2 ;;
        -c) CASES="$2"; shift 2 ;;
        -p) BASE="$2"; shift 2 ;;
        -B) DO_BUILD=1; shift ;;
        -l) echo "selftest direct fanout ice turn relay wake teardown"; exit 0 ;;
        --relay-server) RELAY_SERVER="$2"; shift 2 ;;
        --server-log) SERVER_LOG="$2"; shift 2 ;;
        -h|--help) sed -n '2,25p' "$0"; exit 0 ;;
        *) echo "transport_test.sh: unknown option $1" >&2; exit 2 ;;
    esac
done

cd "$ROOT" || exit 2
case "$BUILD" in /*) ;; *) BUILD="$ROOT/$BUILD" ;; esac
mkdir -p "$OUT"
T="$BUILD/p2transport_test"
TURN="$BUILD/p2net_turn"

if [ "$DO_BUILD" = 1 ]; then
    if [ -f /run/.containerenv ] || ! command -v distrobox >/dev/null 2>&1; then
        ninja -C "$BUILD" p2transport_test p2net_turn || exit 1
    else
        distrobox enter mppm-build -- bash -lc "cd '$ROOT' && ninja -C '$BUILD' p2transport_test p2net_turn" || exit 1
    fi
fi
for b in "$T" "$TURN"; do
    [ -x "$b" ] || { echo "transport_test.sh: $b missing (ninja -C build-pc p2transport_test p2net_turn, or -B)" >&2; exit 2; }
done

PIDS=()
cleanup() {
    for p in "${PIDS[@]:-}"; do
        [ -n "$p" ] && kill "$p" 2>/dev/null
    done
}
trap cleanup EXIT

# wait_for FILE PATTERN SECONDS
wait_for() {
    local i=0
    while [ $i -lt $(( $3 * 10 )) ]; do
        grep -q "$2" "$1" 2>/dev/null && return 0
        sleep 0.1
        i=$(( i + 1 ))
    done
    return 1
}

# The RESULT line of a log, for the summary.
result_of() { grep -h '^RESULT' "$@" 2>/dev/null | sed 's/^RESULT //'; }

case_selftest() {
    "$T" selftest > "$OUT/selftest.log" 2>&1
}

# One host process and one client process; extra client options in "$@".
direct_run() {
    local tag="$1" himp="$2" cimp="$3"; shift 3
    local port=$(( BASE + 0 ))
    "$T" serve --bind 127.0.0.1 --port $port ${himp:+--impair "$himp"} --clients 1 --timeout-s 60 > "$OUT/direct_$tag.host.log" 2>&1 &
    local sp=$!
    PIDS+=("$sp")
    wait_for "$OUT/direct_$tag.host.log" '^serve: listening' 5 || { echo "direct $tag: host did not start"; return 1; }
    "$T" client --connect 127.0.0.1:$port ${cimp:+--impair "$cimp"} --name "$tag" "$@" > "$OUT/direct_$tag.client.log" 2>&1
    local rc=$?
    wait "$sp"
    local hrc=$?
    echo "  direct $tag: $(result_of "$OUT/direct_$tag.client.log")"
    [ $rc = 0 ] && [ $hrc = 0 ]
}

case_direct() {
    local ok=0
    direct_run clean "" "" --count 2000 --reliable 200 || ok=1
    direct_run delay "delay=50ms,seed=1" "delay=50ms,seed=2" --count 2000 --reliable 200 --rtt-ms 100 --rtt-tol 3 || ok=1
    direct_run loss "loss=5%,seed=1" "loss=5%,seed=2" --count 2000 --reliable 200 --loss-pct 5 --loss-tol 1.5 || ok=1
    grep -q 'mppm: net: impairment delay_us=50000 jitter_us=0 loss_ppm=0 dup_ppm=0 seed=2 (test only)' "$OUT/direct_delay.client.log" \
        || { echo "  direct: impairment log line missing"; ok=1; }
    return $ok
}

case_fanout() {
    local port=$(( BASE + 1 )) ok=0
    "$T" serve --bind 127.0.0.1 --port $port --clients 4 --timeout-s 60 > "$OUT/fanout.host.log" 2>&1 &
    local sp=$!
    PIDS+=("$sp")
    wait_for "$OUT/fanout.host.log" '^serve: listening' 5 || return 1
    local cps=()
    for i in 1 2 3 4; do
        "$T" client --connect 127.0.0.1:$port --name "c$i" --count 1000 --reliable 100 --connect-data $(( 0xF0000000 + i )) \
            > "$OUT/fanout.c$i.log" 2>&1 &
        cps+=($!)
    done
    for p in "${cps[@]}"; do wait "$p" || ok=1; done
    wait "$sp" || ok=1
    for i in 1 2 3 4; do echo "  fanout c$i: $(result_of "$OUT/fanout.c$i.log" | cut -c1-150)"; done
    echo "  fanout host: $(result_of "$OUT/fanout.host.log")"
    [ "$(grep -c 'connected data=0xf000000' "$OUT/fanout.host.log")" = 4 ] || { echo "  fanout: host did not see 4 connect data values"; ok=1; }
    return $ok
}

case_ice() {
    "$T" ice --count 1000 > "$OUT/ice.log" 2>&1 || return 1
    echo "  $(result_of "$OUT/ice.log")"
    local n
    n=$(grep -cE '^mppm: net: ice connected peer=[0-9]+ local=host remote=host$' "$OUT/ice.log")
    [ "$n" = 2 ] || { echo "  ice: expected 2 'ice connected ... local=host remote=host' lines, got $n"; return 1; }
}

# TURN through p2net_turn. The strict run puts the TURN server on 127.0.0.2: TURN permissions are per
# IP, so with the server on its own address a host candidate cannot reach a relay address and both
# sides must select relay/relay. The second run is the done test's form (server on 127.0.0.1), where
# loopback lets one side pair its host candidate with the remote relay candidate.
case_turn() {
    local ok=0 port=$(( BASE + 2 )) port1=$(( BASE + 4 ))
    "$TURN" --bind 127.0.0.2 --port $port --user u --pass p --relay-ports $(( BASE + 20 ))-$(( BASE + 24 )) > "$OUT/turn.server.log" 2>&1 &
    local tp=$!
    PIDS+=("$tp")
    wait_for "$OUT/turn.server.log" 'p2net_turn: listening' 5 || { echo "  turn: server did not start"; return 1; }
    "$T" ice --turn 127.0.0.2:$port --user u --pass p --relay-only --count 1000 > "$OUT/turn.log" 2>&1 || ok=1
    kill "$tp" 2>/dev/null; wait "$tp" 2>/dev/null
    echo "  turn (127.0.0.2): $(result_of "$OUT/turn.log")"
    local n
    n=$(grep -cE '^mppm: net: ice connected peer=[0-9]+ local=relay remote=relay$' "$OUT/turn.log")
    [ "$n" = 2 ] || { echo "  turn: expected 2 'local=relay remote=relay' lines, got $n"; ok=1; }

    "$TURN" --bind 127.0.0.1 --port $port1 --user u --pass p --relay-ports $(( BASE + 25 ))-$(( BASE + 29 )) > "$OUT/turn1.server.log" 2>&1 &
    tp=$!
    PIDS+=("$tp")
    wait_for "$OUT/turn1.server.log" 'p2net_turn: listening' 5 || { echo "  turn: server 2 did not start"; return 1; }
    "$T" ice --turn 127.0.0.1:$port1 --user u --pass p --relay-only --count 500 > "$OUT/turn1.log" 2>&1 || ok=1
    kill "$tp" 2>/dev/null; wait "$tp" 2>/dev/null
    echo "  turn (127.0.0.1): $(result_of "$OUT/turn1.log")"
    grep -qE '^mppm: net: ice connected peer=[0-9]+ .*relay' "$OUT/turn1.log" || { echo "  turn: no relay candidate selected on 127.0.0.1"; ok=1; }
    return $ok
}

case_relay() {
    local ok=0 relay ht ct
    if [ -n "$RELAY_SERVER" ]; then
        local fifo="$OUT/relay_session.in"
        rm -f "$fifo"; mkfifo "$fifo"
        python3 -I "$HERE/relay_session.py" --server "$RELAY_SERVER" < "$fifo" > "$OUT/relay_session.json" 2> "$OUT/relay_session.log" &
        local sp=$!
        PIDS+=("$sp")
        exec 9> "$fifo"
        wait_for "$OUT/relay_session.json" 'relay' 10 || { echo "  relay: no session from $RELAY_SERVER"; cat "$OUT/relay_session.log"; exec 9>&-; return 1; }
        read -r relay ht ct < <(python3 -I -c 'import json,sys; d=json.loads(open(sys.argv[1]).readline()); print(d["relay"], d["host_token"], d["client_token"])' "$OUT/relay_session.json")
        "$T" relay --relay "$relay" --host-token "$ht" --client-token "$ct" --count 2000 > "$OUT/relay.log" 2>&1 || ok=1
        exec 9>&-   # stdin EOF: the helper leaves and the session ends
        wait "$sp" 2>/dev/null
        echo "  relay (server $RELAY_SERVER, relay $relay): $(result_of "$OUT/relay.log")"
        if [ -n "$SERVER_LOG" ]; then
            local line=""
            for _ in $(seq 1 50); do
                line=$(grep 'relay mapping' "$SERVER_LOG" | grep "port=${relay##*:}\b" | tail -1)
                [ -z "$line" ] && line=$(grep 'relay mapping' "$SERVER_LOG" | tail -1)
                [ -n "$line" ] && break
                sleep 0.1
            done
            echo "  relay server log: ${line:-none}"
            check_mapping "$line" || ok=1
        fi
        return $ok
    fi
    ht=$(python3 -I -c 'import os; print(os.urandom(16).hex())')
    ct=$(python3 -I -c 'import os; print(os.urandom(16).hex())')
    local port=$(( BASE + 3 ))
    python3 -I "$HERE/relay_stub.py" --bind 127.0.0.1 --port $port --ports $(( BASE + 10 ))-$(( BASE + 19 )) \
        --token "0:$ht" --token "1:$ct" > "$OUT/relay_stub.log" 2>&1 &
    local rp=$!
    PIDS+=("$rp")
    wait_for "$OUT/relay_stub.log" 'relay_stub: listening' 5 || { echo "  relay: stub did not start"; return 1; }
    "$T" relay --relay 127.0.0.1:$port --host-token "$ht" --client-token "$ct" --count 2000 > "$OUT/relay.log" 2>&1 || ok=1
    kill -TERM "$rp" 2>/dev/null; wait "$rp" 2>/dev/null
    echo "  relay (stub): $(result_of "$OUT/relay.log")"
    local line
    line=$(grep 'relay mapping' "$OUT/relay_stub.log" | tail -1)
    echo "  relay stub: ${line#relay_stub: }"
    check_mapping "$line" || ok=1
    return $ok
}

check_mapping() {
    local up down
    up=$(sed -n 's/.*up_bytes=\([0-9]*\).*/\1/p' <<< "$1")
    down=$(sed -n 's/.*down_bytes=\([0-9]*\).*/\1/p' <<< "$1")
    [ -n "$up" ] && [ -n "$down" ] && [ "$up" -gt 50000 ] && [ "$down" -gt 50000 ] \
        || { echo "  relay: mapping bytes too small or missing (up=${up:-?} down=${down:-?})"; return 1; }
}

case_wake() {
    "$T" wake --rounds 20 > "$OUT/wake.log" 2>&1
    local rc=$?
    echo "  $(result_of "$OUT/wake.log")"
    return $rc
}

case_teardown() {
    "$T" teardown --cycles 50 > "$OUT/teardown.log" 2>&1
    local rc=$?
    echo "  $(result_of "$OUT/teardown.log")"
    return $rc
}

declare -A STATUS
ORDER=()
ALL_OK=0
IFS=',' read -r -a WANT <<< "$CASES"
for c in "${WANT[@]}"; do
    if ! declare -F "case_$c" >/dev/null; then
        echo "transport_test.sh: unknown case '$c'" >&2
        exit 2
    fi
    t0=$(date +%s.%N)
    if "case_$c"; then STATUS[$c]=PASS; else STATUS[$c]=FAIL; ALL_OK=1; fi
    secs=$(awk -v a="$t0" -v b="$(date +%s.%N)" 'BEGIN { printf "%.1f", b - a }')
    echo "${STATUS[$c]} $c (${secs} s)"
    [ "$c" = selftest ] && [ "${STATUS[$c]}" = FAIL ] && grep -E 'FAIL|RESULT' "$OUT/selftest.log"
    ORDER+=("$c")
done
echo "---- transport tests ($OUT)"
for c in "${ORDER[@]}"; do printf '  %-9s %s\n' "$c" "${STATUS[$c]}"; done
exit $ALL_OK
