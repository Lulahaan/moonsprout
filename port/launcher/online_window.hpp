#pragma once
// The Multiplayer window (M5.3b design 10.2, SETTINGS 18): the start menu's way into online play,
// shaped like the Mods window (a list on the left, the selected item on the right).
//
//   - Not in a lobby: "Host a game" and "Join with a code", with the lobby server named under the
//     list. Host: mode, stage or course, handicaps (Battle), an optional password, Create lobby.
//     Join: the code (ABCD-EFGH, checked with mppm::online::normalize_code), Watch as a spectator,
//     Join; a password field once the lobby asks for one. While connecting the right pane shows the
//     bead loader and Cancel.
//   - In a lobby (Lobby, Starting): the room. Left: the code pill with Copy, the mode line, one row
//     per seat (team marker, P<n>, name, host crown, "You", ping, link, ready pill; open seats with
//     Take seat; a content mismatch note), the spectators and Leave lobby. Right: the host picks the
//     match, gets ready and starts once everyone can; members see the match and get ready.
//   - Starting: the window closes itself; p2_launcher_online_started runs the take-off.
//   - Ended: a message with OK, then back to the home panes (a lobby that asks for a password
//     brings back the join pane with a password field instead).
//
// Closing the window in a lobby asks to leave it first (a lobby nobody can see would be stuck).

#include "ui/window.hpp"

#include <p2_online.hpp>

#include <map>
#include <memory>
#include <string>

namespace mppm::ui {

class OnlineWindow final : public Window {
public:
    OnlineWindow();
    ~OnlineWindow() override;

    void update() override;

    // What the window shows; a change rebuilds the content.
    enum class View : uint8_t { Home, Connecting, Room };

    const mppm::online::Snapshot& snap() const { return mSnap; }
    const mppm::online::Member* self() const;
    const mppm::online::Member* member_in(int slot) const;
    bool is_host() const { return mSnap.role == mppm::online::Role::Host; }

    void uncover() override;

protected:
    bool consume_close_request() override;

private:
    void build_content(Rml::Element* content);
    void build_home(Rml::Element* content);
    void build_host_form(class Pane& pane);
    void build_join_form(class Pane& pane);
    void build_no_server(class Pane& pane, bool host);
    void build_connecting(class Pane& pane);
    void build_room(Rml::Element* content);
    void build_room_right(class Pane& pane);
    std::string view_key() const;
    void remember(const std::string& key, Component& component);
    std::string focused_key() const;
    std::string default_focus() const;
    bool focus_key(const std::string& key);
    void forget_detail();
    void create_lobby();
    void join_lobby();
    void ask_leave();
    void handle_ended();

    mppm::online::Snapshot mSnap;
    std::string mBuiltKey;            // view_key() of the built content
    std::string mSelected = "host";   // home: "host" or "join"
    std::map<std::string, Component*> mFocusTargets; // focusable controls by key, valid until the next rebuild
    std::string mPendingFocus;        // focus this key after the next rebuild
    std::string mLastFocus;           // the focused key, given back when a modal on top closes
    bool mRebuild = false;
    bool mEndShown = false;
    bool mClosing = false;

    // Host form (kept for the next lobby while the window is open).
    mppm::online::Config mHostConfig;
    std::string mHostPassword;
    std::string mHostError;
    // Join form.
    std::string mJoinCode;
    bool mJoinSpectate = false;
    bool mNeedPassword = false;
    std::string mJoinPassword;
    std::string mJoinError;

    std::unique_ptr<Component> mPrompts; // button prompts under the content (DESIGN 5.17)
};

// Pushes the Multiplayer window over the start menu (or the top document).
void open_online_window();
// Whether a Multiplayer window is open (it shows a lobby's end message itself).
bool online_window_open();

} // namespace mppm::ui
