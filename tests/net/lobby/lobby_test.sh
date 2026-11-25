#!/usr/bin/env bash
# M5.3b lobby test (server package): builds mppm-server (cargo, host), starts it on 127.0.0.1:0, builds
# p2lobby_test in the mppm-build distrobox and runs a host and two members against it, then the same
# against a server with the builtin relay. Local only: every socket is on 127.0.0.1.
#   tests/net/lobby/lobby_test.sh            (outputs in /var/tmp/mppm-online/m53b-server/lobby_test)
# Environment: CARGO (default ~/.cargo/bin/cargo), MPPM_LOBBY_TEST_OUT, MPPM_NO_DISTROBOX=1 (already inside it).
set -euo pipefail
root="$(cd "$(dirname "$0")/../../.." && pwd)"
out="${MPPM_LOBBY_TEST_OUT:-/var/tmp/mppm-online/m53b-server/lobby_test}"
cargo="${CARGO:-$HOME/.cargo/bin/cargo}"
mkdir -p "$out"
rm -f "$out"/*.port "$out"/*.log "$out"/*.stderr
closed_port=47049 # nothing listens here (test port range of the server package: 47000-47049)

pids=()
cleanup() { for p in "${pids[@]}"; do kill "$p" 2>/dev/null || true; done; }
trap cleanup EXIT
fails=0
result() { if [ "$1" = 0 ]; then echo "PASS $2"; else echo "FAIL $2"; fails=$((fails + 1)); fi; }

echo "== cargo build --release --locked"
(cd "$root/server" && "$cargo" build --release --locked --quiet)
server="$root/server/target/release/mppm-server"

echo "== build p2lobby_test"
build='{ cmake build-pc && ninja -C build-pc p2lobby_test; } >/tmp/mppm-p2lobby-build.log 2>&1 || { tail -40 /tmp/mppm-p2lobby-build.log; exit 1; }'
if [ "${MPPM_NO_DISTROBOX:-0}" = 1 ]; then
    (cd "$root" && bash -c "$build")
    run() { (cd "$root" && "$@"); }
else
    distrobox enter mppm-build -- bash -lc "cd '$root' && $build"
    run() { distrobox enter mppm-build -- bash -lc "cd '$root' && $(printf '%q ' "$@")"; }
fi

# start_server <name> <flags...>: sets port and relay_port
start_server() {
    local name="$1"; shift
    "$server" --bind 127.0.0.1:0 --port-file "$out/$name.port" --log-file "$out/$name.log" "$@" 2>"$out/$name.stderr" &
    pids+=($!)
    server_pid=$!
    for _ in $(seq 100); do [ -s "$out/$name.port" ] && break; sleep 0.05; done
    [ -s "$out/$name.port" ] || { echo "server $name did not start"; cat "$out/$name.stderr"; exit 1; }
    port=$(head -1 "$out/$name.port")
    relay_port=$(sed -n 's/^relay=//p' "$out/$name.port")
}

# stop_server <name>: SIGTERM, exit status 0 expected
stop_server() {
    kill -TERM "$server_pid"
    local st=0
    wait "$server_pid" || st=$?
    result "$st" "$1: mppm-server exits 0 on SIGTERM"
}

echo "== lobby flows (relay mode none)"
start_server plain
head -1 "$out/plain.stderr" | grep -q "^mppm-server: listening http=127.0.0.1:$port relay=- ports=- mode=none$"
result $? "plain: ready line"
st=0
run ./build-pc/p2lobby_test "http://127.0.0.1:$port" "$closed_port" >"$out/client_plain.log" 2>&1 || st=$?
grep -E '^(PASS|FAIL) ' "$out/client_plain.log" | sed 's/^/    /'
result "$st" "p2lobby_test: create/join/auth/signals/refusal/kick/host_left/not_found/URL rules"
stop_server plain

echo "== builtin relay"
start_server relay --relay --relay-port 0 --relay-ports 47040-47044 --stun stun:127.0.0.1:47045
grep -q "relay=127.0.0.1:$relay_port ports=47040-47044 mode=builtin$" "$out/relay.stderr"
result $? "relay: ready line"
st=0
run ./build-pc/p2lobby_test --relay "http://127.0.0.1:$port" "127.0.0.1:$relay_port" "stun:127.0.0.1:47045" \
    >"$out/client_relay.log" 2>&1 || st=$?
grep -E '^(PASS|FAIL) ' "$out/client_relay.log" | sed 's/^/    /'
result "$st" "p2lobby_test --relay: relay address, tokens, ICE list"
stop_server relay

if [ "$fails" = 0 ]; then echo "lobby_test: PASS"; else echo "lobby_test: FAIL ($fails)"; fi
exit $((fails > 0))
