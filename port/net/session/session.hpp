// M5.3b session, game-thread side (docs/online/M5_3B_LOCKSTEP.md "Seam"). Used by port/net/net.cpp
// (the input seam) and port/main.cpp; the launcher uses mppm::online (port/include/p2_online.hpp).
// Every function is a no-op (or returns its "offline" value) until an online flag was given or the
// online UI created a session.
#pragma once

#include "types.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace mppm::session {

// ---- command line (p2_net_init, before the launcher's preinit) ----
// Parses the online flags and the environment (exits 2 on bad or conflicting flags) and, for an
// online command-line run (--host, --join, --net-test-bots), creates the session and starts its net
// thread. `replayOrHeadless`, `vsStage`, `chStage`: the harness flags, for the conflict checks.
void cli_init(int argc, char** argv, bool replayOrHeadless, int vsStage, int chStage);
bool cli_online();               // an online role was given on the command line
const char* cli_net_input();     // --net-input / MPPM_NET_INPUT, "" none

// ---- game thread ----
bool exists();                   // a session object exists (lobby or game)
// p2_net_game_poll (gameRunning) and the standalone start menu's frame (no game yet; START is then
// applied there, so p2_launcher_online_started can boot the game): applies START and END.
void game_poll(bool gameRunning = true);
bool started();                  // START applied on the game thread
bool in_session();               // started and p2_net_session_begin ran
const SessionConfig& config();
int local_seat();                // -1 spectator (or no session)
std::string code();              // "ABCD-EFGH", "local" for the test bots
bool ready(uint32_t tick);       // GameSide::ready
bool over(uint32_t tick);        // the session cannot deliver `tick` any more
bool take(uint32_t tick, Input out[kMaxSeats], uint8_t& mask);
void push_read(uint32_t tick, int64_t dueNs, int64_t slewNs, bool haveInput, const Input& in);
void push_checksum(uint32_t tick, uint32_t total, const char* line);
void push_loaded(uint32_t tick);
void session_began();            // p2_net_session_begin ran (tick 0)
void note_read(uint32_t tick, int64_t waitedPastDueNs, bool barrier);
void note_prewait(uint32_t tick, int64_t waitedPastDueNs); // the late-sampling wait held past due
void wait_state(bool waiting, uint32_t tick, int64_t sinceNs, bool loading);
bool barrier_pending();
void reached_tick(uint32_t tick); // after a read: the bots follow the host's reads; test hooks
void leave_now(uint8_t byeReason);// the game asks to leave (MPPM_NET_TEST_LEAVE_AT, window close)
// p2_net_crash_stop (any thread): the net thread says BYE (error) at once, so the others drop this
// seat (a crashed host ends the session) instead of waiting on a game that cannot go on. True when a
// started session was asked to leave.
bool crash_leave();
bool ended();                    // over for good (END, desync, lobby failure, left)
int exit_code();                 // D30: 0 done, 7 desync, 8 session ended, 9 lobby failure
bool command_line_run();
void set_exit_tick(uint32_t tick);   // --exit-after-ticks: reaching it is no "host gone" end
// p2_net_shutdown: the host lingers until every peer acknowledged the last tick (<= 10 s), BYE,
// the net thread stops, the end and summary lines are printed. `exitTick`: --exit-after-ticks reached.
void shutdown(bool exitTickReached, uint32_t tick);
// Bots: the host's own load barriers, read by the in-process bots (net thread).
void host_loaded(uint32_t tick, uint16_t count);

// Implemented by port/net/net.cpp for the session.
namespace seam {
void online_on();                         // the seam runs from now on (sessions started from the UI)
void online_off();
std::vector<std::string> last_lines();    // the last 64 checksum lines of this run
std::string record_path();                // the session recording ("" none)
uint32_t seed();
} // namespace seam

// D19: SHA-256("mppm-build-v1" || SHA-256(exe) || u32le(len) || flags || shared objects of the
// executable's directory). Computed once (about 1 s for 170 MB), thread-safe.
void build_hash(uint8_t out[32]);

} // namespace mppm::session
