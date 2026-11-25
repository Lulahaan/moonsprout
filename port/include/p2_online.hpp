// M5.3b online sessions as the launcher sees them (docs/online/M5_3B_LOCKSTEP.md, "Launcher API").
//
// Owner: the lockstep package (step 0 file; only the owner changes it, through the coordinator).
// The mppm::online functions are implemented in port/net/session and called by the launcher on the
// game thread (launcher::update and its windows): each returns at once and never blocks; the work runs
// on the net thread. The p2_launcher_* hooks at the end are implemented by the launcher package and
// called by port/net on the game thread.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mppm::online {

enum class Mode : uint8_t { Battle = 0, Battle2v2 = 1, Challenge = 2 }; // seats: 2, 4, 2
enum class Phase : uint8_t {
    Idle,       // no lobby
    Connecting, // talking to the server or connecting to the host
    Lobby,      // in the lobby room
    Starting,   // the host pressed Start: the game is on its way into the match
    InGame,     // the match runs (the session's first tick was consumed)
    Ended,      // over: `Snapshot::error` and `endReason` say why; leave() returns to Idle
};
enum class Role : uint8_t { None, Host, Player, Spectator };
enum class Link : uint8_t { Unknown, Direct, Ice, Relay };

enum EndReason : int {
    kEndNone = 0,
    kEndLeft,           // this player left
    kEndHostLeft,       // the host left or closed the lobby
    kEndKicked,         // the host removed this player
    kEndDesync,         // the games stopped matching (`Snapshot::desyncTick`, `reportPath`)
    kEndConnectionLost, // the connection to the host or the server was lost
    kEndRefused,        // the join or the connection was refused (bad code, full, different version)
    kEndServer,         // the lobby server could not be reached or failed
};

struct Member {
    uint32_t id = 0;            // lobby member id (0 = host)
    std::string name;
    int slot = -1;              // seat 0..3; -1 = not seated (spectators, or a player who stood up)
    int team = -1;              // 0 = Red (Olimar's side), 1 = Blue (Louie's side); -1 when not seated
    bool host = false;
    bool self = false;          // this machine
    bool spectator = false;
    bool ready = false;
    bool connected = false;     // transport connected and Hello accepted by the host
    bool contentOk = true;      // build hash and content hash equal to the host's
    std::string contentNote;    // when not: what differs, one sentence ("Different mods: heave-arenas 1.2.0 (host has 1.1.0).")
    int pingMs = -1;            // round trip to the host, -1 unknown (the host shows its own as 0)
    int delayTicks = 0;         // the member's current input delay d_i (InGame), 0 unknown
    Link link = Link::Unknown;
};

struct Config {                 // chosen by the host, shown to everyone
    Mode mode = Mode::Battle;
    int stage = 0;              // Battle: VS stage index (0..9 on the stock list); Challenge: course index (0..29)
    int handicap[2] = {0, 0};   // Battle (1v1) only: 0 = stock (3), else 1..10; [0] Red, [1] Blue
};

struct Snapshot {
    Phase phase = Phase::Idle;
    Role role = Role::None;
    std::string code;           // "ABCD-EFGH" once in a lobby
    std::string serverUrl;      // the server in use
    Config config;
    int seats = 2;              // seats of config.mode: 2 or 4
    std::vector<Member> members;// host first, then seated members by slot, then the others by id
    bool canStart = false;      // host: every seat taken, every seated member ready, connected and matching
    std::string status;         // one line for the room, e.g. "Waiting for Louie to get ready."
    std::string error;          // Ended: the message to show (one or two sentences)
    int endReason = kEndNone;
    uint32_t desyncTick = 0;    // kEndDesync
    std::string reportPath;     // kEndDesync: the report this machine wrote
    uint32_t generation = 0;    // changes whenever anything above changes
};

// The in-game wait overlay (a tick or a load barrier waits for someone).
struct Stall {
    bool waiting = false;       // true once a wait has lasted 300 ms
    bool loading = false;       // the wait is a load barrier ("Waiting for players to load")
    uint32_t tick = 0;          // the tick being waited for
    double seconds = 0.0;       // how long this wait has lasted
    std::vector<std::string> names; // whom it waits for, e.g. {"Louie (P2)"}; empty = the host (clients)
    std::vector<int> slots;
    bool canDrop = false;       // host, once the wait has lasted 5 s: drop_slot() is allowed
    bool canLeave = false;      // client, once the wait has lasted 10 s
};

struct Notice {                 // short news for a toast (DESIGN.md 5.14), oldest first
    enum Kind : int { Info = 0, Success = 1, Warning = 2, Error = 3 };
    int kind = Info;
    std::string title;          // "Louie joined"
    std::string body;           // may be empty
};

struct Settings {
    std::string serverUrl;      // online.serverUrl; "" = the build's default (MPPM_DEFAULT_SERVER_URL), may be empty
    std::string playerName;     // online.playerName; "" = "Player"
};

// Launcher, at start and whenever an online.* setting changes. The command line (--server, --name)
// wins over these.
void configure(const Settings& settings);
// Lobby. False and `err` (one sentence to show) when it cannot start at all (no server set, a session
// already exists, bad code); otherwise Phase goes to Connecting, then Lobby or Ended.
bool host(const Config& config, const std::string& password, std::string& err);
bool join(const std::string& code, bool spectator, const std::string& password, std::string& err);
void leave();                   // lobby or game; also acknowledges an Ended session (back to Idle)
// After leave(), before a relaunch: waits (at most `maxMs`) for the net thread to send its BYE and
// close the links, so the others learn of it at once rather than by a timeout.
void wait_left(int maxMs);
void set_ready(bool ready);
void take_slot(int slot);       // 0..seats-1; -1 = stand up
void set_config(const Config& config); // host, Lobby phase only
void start();                   // host, when canStart
void kick(uint32_t memberId);   // host
void drop_slot(int slot);       // host, while Stall::canDrop names that slot
Snapshot snapshot();
Stall stall();
std::vector<Notice> take_notices();
// A lobby or a game exists (Phase is Connecting, Lobby, Starting or InGame).
bool active();
// 1 while the game runs an online match or is on its way into one (Starting or InGame).
bool in_game();
// Join-code check for the input field (same rule as mppm::net::normalize_code).
bool normalize_code(const std::string& in, std::string& out);

} // namespace mppm::online

// ---- Hooks implemented by the launcher package (port/launcher/online_hooks.cpp) -----------------
extern "C" {
// The Synced settings of this machine as JSON text (net_synced_settings().dump()). The pointer stays
// valid until the next call. Used by the host for SessionConfig.
const char* p2_launcher_net_synced_json(void);
// Locks every Forced and Synced setting with the host's values (net_lock_settings(json), SETTINGS.md
// 14); the settings window then says "Set by the host of this online game." 1 = locked, 0 = refused.
int p2_launcher_net_lock(const char* syncedJson);
// Releases the lock (net_unlock_settings), unless a harness run holds it (then it re-locks with defaults).
void p2_launcher_net_unlock(void);
// The host started the match: close the start menu the way Play does (take-off, then the game goes
// to the title, which leaves by itself into the match). No-op when no start menu is open.
void p2_launcher_online_started(void);
}
