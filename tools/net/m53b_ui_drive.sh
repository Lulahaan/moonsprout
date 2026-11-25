#!/usr/bin/env bash
# Drives the Moonsprout online UI (M5.3b) of one mppm window with the keyboard only, for tests.
#
#   tools/net/m53b_ui_drive.sh <display> <command> [args]
#
# The window is found with `xdotool search --name '^(mppm|Moonsprout)$'` on X display :<display>
# (one Xvfb per game). The script prepares nothing: the caller starts the game on that display with
# its own HOME whose <data>/mppm/config.json has `backend.discPath`, `interface.presetChosen: true`
# and `online.serverUrl` (and does not set MPPM_SKIP_LAUNCHER, so the start menu shows). Keys are
# held 0.12 s (the launcher ignores shorter taps under load) and every step waits for the UI.
#
# Commands and their key sequences (the start menu reads Play, Multiplayer, Mods, Settings, Quit;
# the script remembers which menu item has the focus in $MPPM_UI_STATE_DIR/m53b_ui_<display>.state,
# default /tmp, so only use these commands between wait-menu and the end of the run):
#
#   wait-menu [seconds]   wait for the window, then for the start menu to settle (default 25 s,
#                         MPPM_UI_MENU_WAIT); the focus is on Play. With MPPM_UI_LOG=<game log> it
#                         returns 4 s after the log shows the live scene has loaded.
#   open-multiplayer      Up/Down to "Multiplayer", Enter. The window opens on "Host a game".
#   host [battle|2v2|challenge] [stage]
#                         from the window's home (focus on "Host a game"): Enter (to the form),
#                         Right x0/1/2 on Mode, Down, Right x(stage-1) on Stage (1-based, default 1),
#                         Down x6 (to "Create lobby", the last control), Enter. The room opens with
#                         the focus on Ready.
#   join <CODE> [spectate]
#                         from the window's home: Down ("Join with a code"), Enter (to the form,
#                         focus on Code), Enter, type CODE, Enter; with `spectate` Down, Enter (the
#                         toggle); Down x4 (to "Join"), Enter. An invalid code leaves the error under
#                         the field (Join stays disabled). The room opens with the focus on Ready.
#                         With M53B_JOIN_SHOT=<file.png> the pane is shot after the code is entered.
#   ready                 Enter (the room's focus is on Ready after host / join).
#   start                 Down (Ready -> Start game), Enter. Host only.
#   open-settings-online  from the start menu: Up/Down to "Settings", Enter, then 8 (the eighth tab,
#                         ONLINE, after PRELAUNCH ... INTERFACE). The focus is on Player Name.
#   close                 Esc, Esc: closes the top window (from its right pane: back to the list,
#                         then closed). In a lobby the room asks "Leave the lobby?" instead.
#   key <keysym>...       raw key presses (xdotool names: Down, Return, Escape, ...).
#   type <text>           types text into the focused field (xdotool type; Enter starts editing).
#   shot <file.png>       screenshot of the whole display (magick import -window root).
#
# Exit status: 0 done, 1 window not found, 2 bad usage.
set -u

usage() {
    sed -n '2,45p' "$0" >&2
    exit 2
}

[ $# -ge 2 ] || usage
D="$1"
CMD="$2"
shift 2
export DISPLAY=":$D"
STATE="${MPPM_UI_STATE_DIR:-/tmp}/m53b_ui_${D}.state"
HOLD="${MPPM_UI_HOLD:-0.12}"
GAP="${MPPM_UI_GAP:-0.35}"

find_window() {
    local w=""
    for _ in $(seq 1 "${1:-240}"); do
        w=$(xdotool search --name '^(mppm|Moonsprout)$' 2>/dev/null | head -n1)
        [ -n "$w" ] && break
        sleep 0.5
    done
    echo "$w"
}

WIN=""
need_window() {
    WIN=$(find_window "${1:-20}")
    if [ -z "$WIN" ]; then
        echo "m53b_ui_drive: no mppm window on :$D" >&2
        exit 1
    fi
    xdotool windowfocus "$WIN" 2>/dev/null || true
}

press() {
    local k
    for k in "$@"; do
        xdotool keydown "$k"
        sleep "$HOLD"
        xdotool keyup "$k"
        sleep "$GAP"
    done
}

repeat() { # repeat <n> <key>
    local i
    for ((i = 0; i < $1; i++)); do press "$2"; done
}

menu_index() { cat "$STATE" 2>/dev/null || echo 0; }

# Moves the start menu's focus from the remembered item to item $1 (0 Play, 1 Multiplayer, 2 Mods,
# 3 Settings, 4 Quit) and remembers it.
menu_go() {
    local from to
    from=$(menu_index)
    to="$1"
    if [ "$to" -gt "$from" ]; then repeat $((to - from)) Down; fi
    if [ "$to" -lt "$from" ]; then repeat $((from - to)) Up; fi
    echo "$to" >"$STATE"
}

case "$CMD" in
wait-menu)
    WIN=$(find_window 360)
    [ -n "$WIN" ] || { echo "m53b_ui_drive: no mppm window on :$D" >&2; exit 1; }
    xdotool windowfocus "$WIN" 2>/dev/null || true
    wait_s="${1:-${MPPM_UI_MENU_WAIT:-25}}"
    if [ -n "${MPPM_UI_LOG:-}" ]; then
        for _ in $(seq 1 $((wait_s * 2))); do
            grep -q "^scene: .* time \|mppm: scene: state 2\|mppm: start menu: booting" "$MPPM_UI_LOG" 2>/dev/null && break
            sleep 0.5
        done
        sleep 4
    else
        sleep "$wait_s"
    fi
    echo 0 >"$STATE"
    ;;
open-multiplayer)
    need_window
    menu_go 1
    press Return
    sleep 1.2
    ;;
host)
    need_window
    mode="${1:-battle}"
    stage="${2:-1}"
    press Return
    sleep 0.6
    case "$mode" in
    battle) ;;
    2v2) press Right; sleep 0.4 ;;
    challenge) press Right Right; sleep 0.4 ;;
    *) echo "m53b_ui_drive: host: mode is battle, 2v2 or challenge" >&2; exit 2 ;;
    esac
    press Down
    [ "$stage" -gt 1 ] 2>/dev/null && repeat $((stage - 1)) Right
    repeat 6 Down
    press Return
    sleep 1.5
    ;;
join)
    need_window
    code="${1:-}"
    [ -n "$code" ] || { echo "m53b_ui_drive: join needs a code" >&2; exit 2; }
    press Down
    sleep 0.4
    press Return
    sleep 0.6
    press Return
    sleep 0.4
    xdotool type --delay 90 "$code"
    sleep 0.3
    press Return
    sleep 0.4
    # M53B_JOIN_SHOT=<file.png>: shoot the join pane here (the code typed and committed, Join enabled)
    if [ -n "${M53B_JOIN_SHOT:-}" ]; then
        sleep 0.3
        magick import -display ":$D" -window root "$M53B_JOIN_SHOT"
    fi
    if [ "${2:-}" = spectate ]; then
        press Down Return
    fi
    repeat 4 Down
    press Return
    sleep 1.5
    ;;
ready)
    need_window
    press Return
    sleep 0.5
    ;;
start)
    need_window
    press Down Return
    sleep 0.5
    ;;
open-settings-online)
    need_window
    menu_go 3
    press Return
    sleep 1.5
    press 8
    sleep 1
    ;;
close)
    need_window
    press Escape
    sleep 0.5
    press Escape
    sleep 0.8
    ;;
key)
    need_window
    press "$@"
    ;;
type)
    need_window
    xdotool type --delay 90 "${1:-}"
    sleep 0.3
    ;;
shot)
    [ $# -ge 1 ] || usage
    magick import -display ":$D" -window root "$1"
    ;;
*)
    usage
    ;;
esac
exit 0
