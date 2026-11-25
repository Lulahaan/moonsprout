// The launcher's side of online sessions (M5.3b): the p2_launcher_* hooks of p2_online.hpp, the
// settings mirror into mppm::online::configure, notices as toasts and the end-of-session messages.
// See online_ui.hpp.

#include "online_ui.hpp"

#include "menu_bar.hpp"
#include "online_overlay.hpp"
#include "online_window.hpp"
#include "prelaunch.hpp"
#include "restart.hpp"
#include "settings.hpp"

#include "ui/modal.hpp"
#include "ui/ui.hpp"

#include <borealis/config.hpp>
#include <fmt/format.h>
#include <mppm/launcher.hpp>
#include <nlohmann/json.hpp>
#include <p2_net.h>
#include <p2_online.hpp>

#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace mppm::ui::online_ui {
namespace {

namespace online = mppm::online;

constexpr const char* kDot = "\xC2\xB7"; // middle dot

std::vector<borealis::config::Connection> sConnections;
bool sInitialized = false;
online::Phase sLastPhase = online::Phase::Idle;
bool sWasInMatch = false;   // the session reached Starting or InGame
bool sEndShown = false;     // the message for the current Ended state is up (or was handled)

void push_configure() {
    const auto& s = settings().online;
    online::configure({.serverUrl = s.serverUrl.get(), .playerName = s.playerName.get()});
}

const char* toast_type(int kind) {
    switch (kind) {
    case online::Notice::Success:
        return "success";
    case online::Notice::Warning:
        return "warning";
    case online::Notice::Error:
        return "error";
    default:
        return "info";
    }
}

class DangerModal final : public Modal {
public:
    explicit DangerModal(Props props) : Modal(std::move(props)) {
        Rml::ElementList buttons;
        mDocument->QuerySelectorAll(buttons, "modal-actions button");
        if (!buttons.empty()) {
            buttons.back()->SetClass("danger", true);
        }
    }
};

void push_modal(std::unique_ptr<Modal> modal, Document* over) {
    Document& ref = *modal;
    if (over != nullptr) {
        over->push(std::move(modal));
    } else {
        // Not covering the document below: the room or the game stays visible under the scrim,
        // and gets the focus back when the modal closes (uncover_top_document).
        push_document(std::move(modal));
    }
    ref.focus();
}

} // namespace

// ---- Text ----

std::string server_url() {
    const std::string& url = settings().online.serverUrl.get();
    return url.empty() ? std::string(default_server_url()) : url;
}

std::string server_host(const std::string& url) {
    std::string host = url;
    if (const auto scheme = host.find("://"); scheme != std::string::npos) {
        host = host.substr(scheme + 3);
    }
    if (const auto at = host.rfind('@'); at != std::string::npos) {
        host = host.substr(at + 1); // never show credentials
    }
    if (const auto slash = host.find_first_of("/?#"); slash != std::string::npos) {
        host = host.substr(0, slash);
    }
    return host;
}

std::string ping_text(int pingMs) {
    return pingMs < 0 ? std::string("\xE2\x80\x93") : fmt::format("{} ms", pingMs);
}

const char* link_label(online::Link link) {
    switch (link) {
    case online::Link::Direct:
        return "Direct";
    case online::Link::Ice:
        return "Direct (ICE)";
    case online::Link::Relay:
        return "Relay";
    case online::Link::Unknown:
        break;
    }
    return "";
}

const char* link_glyph(online::Link link) {
    switch (link) {
    case online::Link::Direct:
    case online::Link::Ice:
        return "\xEE\x85\x97"; // link (e157)
    case online::Link::Relay:
        return "\xEE\x8C\xA8"; // router (e328)
    case online::Link::Unknown:
        break;
    }
    return "";
}

const char* mode_label(online::Mode mode) {
    switch (mode) {
    case online::Mode::Battle:
        return "Battle";
    case online::Mode::Battle2v2:
        return "Battle 2 vs 2";
    case online::Mode::Challenge:
        return "Challenge";
    }
    return "Battle";
}

std::string stage_label(online::Mode mode, int stage) {
    return fmt::format("{} {}", mode == online::Mode::Challenge ? "Course" : "Stage", stage + 1);
}

std::string handicap_label(int value) {
    return value <= 0 ? std::string("Normal") : fmt::to_string(value);
}

std::string config_line(const online::Config& config) {
    return fmt::format("{} {} {}", mode_label(config.mode), kDot, stage_label(config.mode, config.stage));
}

Rml::Element* append_team_marker(Rml::Element* parent, int team) {
    auto* marker = append(parent, "team-marker");
    if (marker == nullptr) {
        return nullptr;
    }
    marker->SetClass(team == 0 ? "red" : team == 1 ? "blue" : "none", true);
    if (team == 1) {
        append(marker, "team-fill"); // the blue square inside its white frame
    }
    return marker;
}

std::string text_rml(const std::string& text) {
    return "<span>" + escape(text) + "</span>";
}

// ---- Modals ----

void confirm(const std::string& title, const std::string& body, const std::string& confirmLabel,
    std::function<void()> action, bool danger, Document* over) {
    const auto dismiss = [](Modal& modal) {
        play_sound(kSoundWindowClose);
        modal.pop();
    };
    Modal::Props props{
        .title = title,
        .bodyText = body,
        .actions =
            {
                {.label = "Cancel", .onPressed = dismiss},
                {.label = confirmLabel,
                    .onPressed =
                        [action = std::move(action)](Modal& modal) {
                            play_sound(kSoundClick);
                            modal.pop();
                            if (action) {
                                action();
                            }
                        }},
            },
        .onDismiss = dismiss,
        .variant = danger ? "danger" : "",
        .icon = danger ? "warning" : "info",
    };
    if (danger) {
        push_modal(std::make_unique<DangerModal>(std::move(props)), over);
    } else {
        push_modal(std::make_unique<Modal>(std::move(props)), over);
    }
}

std::string end_title(const online::Snapshot& snap) {
    switch (snap.endReason) {
    case online::kEndDesync:
        return "The games stopped matching";
    case online::kEndHostLeft:
        return "The host left the game";
    case online::kEndConnectionLost:
        return "Connection lost";
    case online::kEndKicked:
        return "Removed from the game";
    case online::kEndRefused:
        return "Couldn't join";
    case online::kEndServer:
        return "Couldn't reach the lobby server";
    default:
        return "The online game ended";
    }
}

std::string end_body(const online::Snapshot& snap) {
    switch (snap.endReason) {
    case online::kEndDesync:
        if (!snap.reportPath.empty()) {
            return fmt::format("Desync at tick {}. The match was stopped on every machine. A report was saved to {}.",
                snap.desyncTick, snap.reportPath);
        }
        return fmt::format("Desync at tick {}. The match was stopped on every machine.", snap.desyncTick);
    case online::kEndHostLeft:
        return sWasInMatch ? "The match can't go on without the host." : "The host closed the lobby.";
    case online::kEndConnectionLost:
        return sWasInMatch ? "You were disconnected from the game." : "You were disconnected from the lobby.";
    case online::kEndKicked:
        return sWasInMatch ? "The host removed you from the game." : "The host removed you from the lobby.";
    default:
        return snap.error.empty() ? std::string("The session is over.") : snap.error;
    }
}

void show_end_message(const online::Snapshot& snap, bool inMatch, std::function<void()> onClose) {
    const std::string label = inMatch ? "Back to start menu" : "OK";
    const bool error = snap.endReason == online::kEndDesync || snap.endReason == online::kEndConnectionLost ||
                       snap.endReason == online::kEndServer;
    const auto act = [inMatch, onClose = std::move(onClose)](Modal& modal) {
        play_sound(kSoundClick);
        modal.pop();
        if (inMatch) {
            leave_to_start_menu();
        } else {
            online::leave();
            if (onClose) {
                onClose();
            }
        }
    };
    auto modal = std::make_unique<Modal>(Modal::Props{
        .title = end_title(snap),
        .bodyText = end_body(snap),
        .actions = {{.label = label, .onPressed = act}},
        // Over a finished match Esc does nothing: the only way on is the button. In the lobby it
        // is the same as OK.
        .onDismiss = inMatch ? std::function<void(Modal&)>([](Modal&) {}) : std::function<void(Modal&)>(act),
        .icon = error ? "error" : "info",
    });
    push_modal(std::move(modal), nullptr);
    play_sound(kSoundWarning);
}

void leave_to_start_menu() {
    online::leave();
    if (launcher::game_booted()) {
        // In game, or the take-off still running over the booted game: the title would come next
        // with no session to start. The session is over (p2_net_active() is 0 again), so the
        // restart relaunches into the start menu instead of ending a harness run (restart.cpp).
        // The relaunch execs at once: give the net thread its moment to send BYE first.
        online::wait_left(300);
        launcher::request_restart(launcher::RestartTarget::StartMenu);
    }
}

// ---- Per frame ----

void init() {
    if (sInitialized) {
        return;
    }
    sInitialized = true;
    auto& s = settings().online;
    sConnections.push_back(s.serverUrl.on_change([] { push_configure(); }));
    sConnections.push_back(s.playerName.on_change([] { push_configure(); }));
    push_configure();
    install_online_overlay();
}

void update() {
    if (!sInitialized) {
        return;
    }
    for (const online::Notice& n : online::take_notices()) {
        push_toast({
            .type = toast_type(n.kind),
            .title = text_rml(n.title),
            .content = n.body.empty() ? Rml::String{} : text_rml(n.body),
        });
    }
    const online::Phase phase = online::snapshot().phase;
    if (phase == online::Phase::Starting || phase == online::Phase::InGame) {
        sWasInMatch = true;
    }
    if (phase != online::Phase::Ended) {
        sEndShown = false;
        if (phase == online::Phase::Idle) {
            sWasInMatch = false;
        }
    } else if (!sEndShown) {
        // In the lobby the Multiplayer window shows it (and the join pane's password case). Over a
        // match, or with no window to show it, the message comes from here.
        if (sWasInMatch) {
            sEndShown = true;
            show_end_message(online::snapshot(), true);
        } else if (!online_window_open()) {
            sEndShown = true;
            online::leave();
        }
    }
    if (phase != sLastPhase) {
        std::fprintf(stderr, "mppm: launcher: online phase %d -> %d\n", static_cast<int>(sLastPhase),
            static_cast<int>(phase));
        sLastPhase = phase;
    }
}

} // namespace mppm::ui::online_ui

// ---- Hooks called by the session (p2_online.hpp), on the game thread ----

extern "C" const char* p2_launcher_net_synced_json(void) {
    static std::string sJson;
    sJson = mppm::net_synced_settings().dump();
    return sJson.c_str();
}

extern "C" int p2_launcher_net_lock(const char* syncedJson) {
    const nlohmann::json values = nlohmann::json::parse(syncedJson != nullptr ? syncedJson : "", nullptr, false);
    if (values.is_discarded()) {
        std::fprintf(stderr, "mppm: net: settings: the host's values are not JSON\n");
        return 0;
    }
    if (!mppm::net_lock_settings(values)) {
        return 0;
    }
    std::fprintf(stderr, "mppm: net: settings locked by the host (%d forced, %d synced)\n", mppm::net_forced_count(),
        mppm::net_synced_count());
    return 1;
}

extern "C" void p2_launcher_net_unlock(void) {
    mppm::net_unlock_settings();
    if (p2_net_active()) {
        // A harness run (--record, --checksum-log, ...) keeps its own lock (SETTINGS 14).
        mppm::net_lock_settings(nlohmann::json::object());
    }
}

extern "C" void p2_launcher_online_started(void) {
    // Command-line runs have no start menu: the title leaves by itself.
    if (auto* menu = mppm::ui::Prelaunch::current(); menu != nullptr && menu->active()) {
        menu->start_online();
    }
}
