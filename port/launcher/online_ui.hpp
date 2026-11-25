#pragma once
// Online play in the launcher (M5.3b; docs/launcher/SETTINGS.md 18, DESIGN.md "Online screens").
//
// The launcher only shows and drives an online session; the session itself (lobby server, host,
// lockstep) lives in port/net/session behind mppm::online (port/include/p2_online.hpp). Every call
// into it returns at once, so the UI polls it each frame:
//
//   - online_window.cpp: the start menu's Multiplayer window (host, join by code, the lobby room);
//   - online_overlay.cpp: the in-game wait pill ("Waiting for Louie (P2)...") with Drop / Leave;
//   - online_status_window.cpp: the in-game bar's Online window (members, ping, delay, Leave game);
//   - online_hooks.cpp: the p2_launcher_* hooks the session calls (settings lock, take-off), the
//     settings mirror (configure), notices as toasts and the end-of-session messages.
//
// Every string that comes from the network (names, notes, errors, codes) is untrusted: it goes into
// text nodes, or through ui::escape when it is part of RML.

#include <p2_online.hpp>

#include <RmlUi/Core.h>

#include <functional>
#include <string>

namespace mppm::ui {
class Document;
}

namespace mppm::ui::online_ui {

// launcher::integration::init: mirrors online.serverUrl and online.playerName into
// mppm::online::configure (now and on every change) and installs the wait overlay.
void init();
// Once per launcher frame (integration::update): notices become toasts; a session that ended
// during a match gets its message (SETTINGS 18).
void update();

// The host of a lobby server URL, for "Lobby server: <host>" ("" when none is set).
std::string server_host(const std::string& url);
// The lobby server in use: online.serverUrl, else the build's default (may be empty).
std::string server_url();
// "23 ms", "-" when unknown.
std::string ping_text(int pingMs);
// "Direct", "Direct (ICE)", "Relay", "" when unknown.
const char* link_label(mppm::online::Link link);
// The Material Symbols glyph for a link (UTF-8), "" when unknown.
const char* link_glyph(mppm::online::Link link);
// "Battle · Stage 3", "Battle 2 vs 2 · Stage 1", "Challenge · Course 12".
std::string config_line(const mppm::online::Config& config);
const char* mode_label(mppm::online::Mode mode);
// "Stage 3" / "Course 12" for a 0-based index.
std::string stage_label(mppm::online::Mode mode, int stage);
// "Normal" for 0, else the number.
std::string handicap_label(int value);
// The team marker of M5.3a (team 0: vermillion square; team 1: blue square in a white frame), as a
// child element of `parent`. Team -1 draws an empty outline.
Rml::Element* append_team_marker(Rml::Element* parent, int team);
// A short network string as safe RML, for places that parse RML (toast titles): escaped, in a span.
std::string text_rml(const std::string& text);

// A confirmation (DESIGN 5.11). `danger`: the danger variant, focus on Cancel, the affirmative
// button red. `over` is covered while it is up (nullptr: the top document).
void confirm(const std::string& title, const std::string& body, const std::string& confirmLabel,
    std::function<void()> action, bool danger, Document* over = nullptr);

// The end-of-session message (SETTINGS 18, design 10.4) for a snapshot in Phase::Ended. `inMatch`:
// "Back to start menu" (leave, then restart into the start menu); otherwise "OK" (leave) and
// `onClose` runs.
void show_end_message(const mppm::online::Snapshot& snap, bool inMatch, std::function<void()> onClose = {});
// Title and body of the end message for a snapshot (used by the modal and the status window).
std::string end_title(const mppm::online::Snapshot& snap);
std::string end_body(const mppm::online::Snapshot& snap);

// Leave the session and go back to the start menu (in game: a restart into it, SETTINGS 13).
void leave_to_start_menu();

} // namespace mppm::ui::online_ui
