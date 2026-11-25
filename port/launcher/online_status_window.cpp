#include "online_status_window.hpp"

#include "menu_bar.hpp"
#include "online_ui.hpp"

#include "ui/input.hpp"
#include "ui/pane.hpp"
#include "ui/ui.hpp"

#include <fmt/format.h>
#include <p2_online.hpp>

#include <algorithm>
#include <functional>
#include <string>
#include <tuple>

namespace mppm::ui {
namespace {

namespace online = mppm::online;
using online::Member;

constexpr const char* kGlyphInfo    = "\xEE\xA2\x8E"; // info
constexpr const char* kGlyphWarning = "\xEE\x80\x82"; // warning
constexpr const char* kGlyphCrown   = "\xEE\xA3\x90"; // stars (e8d0): the host
constexpr const char* kDot          = "\xC2\xB7";

Rml::String note_rml(const char* glyph, const char* cls, const Rml::String& text) {
    return fmt::format("<help-note class=\"{}\"><note-icon>{}</note-icon><note-text>{}</note-text></help-note>", cls,
                       glyph, text);
}

class LiveRml final : public Component {
public:
    LiveRml(Rml::Element* parent, const Rml::String& tag, std::function<Rml::String()> rml)
        : Component(append(parent, tag)), mRml(std::move(rml)) {
        update();
    }
    void update() override {
        Rml::String rml = mRml();
        if (rml != mCurrent) {
            mRoot->SetInnerRML(rml);
            set_display(mRoot, rml.empty() ? Rml::Style::Display::None : Rml::Style::Display::Block);
            mCurrent = std::move(rml);
        }
        Component::update();
    }
    bool focus() override { return false; }

private:
    std::function<Rml::String()> mRml;
    Rml::String mCurrent = "\x01";
};

// One member, read-only: marker, P<n>, name, host star, "You"; then ping, delay, link and
// "Lagging" while the match waits for their seat.
class MemberRow final : public Component {
public:
    MemberRow(Rml::Element* parent, uint32_t id) : Component(append(parent, "member-row")), mId(id) {
        mRoot->SetClass("seat-row", true);
        update();
    }
    void update() override {
        const online::Snapshot snap = online::snapshot();
        const online::Stall stall = online::stall();
        const Member* m = nullptr;
        for (const Member& x : snap.members) {
            if (x.id == mId) {
                m = &x;
            }
        }
        if (m == nullptr) {
            return;
        }
        const bool lagging = stall.waiting && m->slot >= 0 && std::ranges::find(stall.slots, m->slot) != stall.slots.end();
        const std::string sig = fmt::format("{}|{}|{}|{}|{}|{}|{}|{}|{}", m->name, m->slot, m->host, m->self, m->pingMs,
                                            m->delayTicks, static_cast<int>(m->link), lagging, m->connected);
        if (sig == mSig) {
            return;
        }
        mSig = sig;
        ui::clear_children(mRoot);
        auto* line = append(mRoot, "seat-line");
        online_ui::append_team_marker(line, m->slot >= 0 ? (m->slot & 1) : -1);
        append_text_element(line, "seat-num", m->slot >= 0 ? fmt::format("P{}", m->slot + 1) : std::string("Watching"));
        append_text_element(line, "seat-name", m->name);
        if (m->host) {
            append_text_element(line, "seat-crown", kGlyphCrown);
        }
        if (m->self) {
            append_text_element(line, "you-tag", "You");
        }
        append(line, "seat-gap");
        if (lagging) {
            append_text_element(line, "lag-tag", "Lagging");
        }
        if (!m->connected) {
            append_text_element(line, "ready-pill", "Dropped")->SetClass("waiting", true);
        }
        // Connection figures on a second line, so the name keeps the first.
        auto* stats = append(mRoot, "member-stats");
        if (!m->host && m->pingMs >= 0) {
            append_text_element(stats, "ping-badge", online_ui::ping_text(m->pingMs));
        }
        if (m->delayTicks > 0) {
            append_text_element(stats, "delay-badge",
                                fmt::format("Delay {} tick{}", m->delayTicks, m->delayTicks == 1 ? "" : "s"));
        }
        if (!m->host && m->link != online::Link::Unknown) {
            auto* link = append_text_element(stats, "link-icon", online_ui::link_glyph(m->link));
            link->SetClass("relay", m->link == online::Link::Relay);
            append_text_element(stats, "link-label", online_ui::link_label(m->link));
        }
        if (stats->GetNumChildren() == 0) {
            mRoot->RemoveChild(stats);
        }
    }
    bool focus() override { return false; }

private:
    uint32_t mId;
    std::string mSig;
};

class Prompts final : public Component {
public:
    explicit Prompts(Rml::Element* parent) : Component(append(parent, "button-prompts")) {
        mRoot->SetClass("online-prompts", true);
        mSelect = add("Enter", "Select");
        mBack = add("Esc", "Back");
        update();
    }
    void update() override {
        const bool hidden = input::last_input_pointer();
        if (hidden != mRoot->IsClassSet("hidden")) {
            mRoot->SetClass("hidden", hidden);
        }
        const int pad = input::last_input_gamepad() ? 1 : 0;
        if (pad != mPad) {
            mPad = pad;
            for (auto [glyph, key, button] : {std::tuple{mSelect, "Enter", "A"}, std::tuple{mBack, "Esc", "B"}}) {
                glyph->SetClass("key", pad == 0);
                set_text_content(glyph, pad == 1 ? button : key);
            }
        }
    }
    bool focus() override { return false; }

private:
    Rml::Element* add(const char* key, const char* label) {
        auto* prompt = append(mRoot, "prompt");
        auto* glyph = append_text_element(prompt, "glyph", key);
        append_text_element(prompt, "label", label);
        return glyph;
    }
    Rml::Element* mSelect = nullptr;
    Rml::Element* mBack = nullptr;
    int mPad = -1;
};

std::string members_key(const online::Snapshot& snap) {
    std::string key = fmt::format("{}|{}|", static_cast<int>(snap.phase), snap.seats);
    for (const Member& m : snap.members) {
        key += fmt::format("{},", m.id);
    }
    return key;
}

} // namespace

OnlineStatusWindow::OnlineStatusWindow() : Window(Props{.tabBar = false, .styleSheets = {"res/rml/online.rcss"}}) {
    mRoot->SetClass("online", true);
    mRoot->SetClass("online-status", true);
    mBuiltKey = members_key(online::snapshot());
    set_content([this](Rml::Element* content) { build_content(content); });
    mPrompts = std::make_unique<Prompts>(mRoot);
}

OnlineStatusWindow::~OnlineStatusWindow() = default;

void OnlineStatusWindow::build_content(Rml::Element* content) {
    const online::Snapshot snap = online::snapshot();
    auto& left = add_child<Pane>(content, Pane::Type::Uncontrolled);
    left.root()->SetClass("seats", true);
    auto& right = add_child<Pane>(content, Pane::Type::Uncontrolled);
    right.root()->SetClass("detail", true);

    auto* header = append(left.root(), "room-header");
    auto* row = append(header, "room-title-row");
    append_text_element(row, "heading", "Online");
    if (!snap.code.empty()) {
        append_text_element(row, "code-pill", snap.code);
    }
    append_text_element(header, "small", online_ui::config_line(snap.config));
    left.add_section("Players");
    for (const Member& m : snap.members) {
        left.add_child<MemberRow>(m.id);
    }

    right.add_section("Match");
    append_text_element(right.root(), "p",
                        online::in_game() ? "The game is still running. Online games never pause."
                                          : "The match hasn't started yet.")
        ->SetClass("status-body", true);
    right.add_child<LiveRml>("help-notes", []() -> Rml::String {
        const online::Snapshot s = online::snapshot();
        const online::Stall st = online::stall();
        Rml::String rml;
        if (st.waiting) {
            rml += note_rml(kGlyphWarning, "warning",
                            st.names.empty() ? Rml::String("Waiting for the host.")
                                             : escape(fmt::format("Waiting for {}.", st.names.front())));
        }
        if (!s.status.empty()) {
            rml += note_rml(kGlyphInfo, "info", escape(s.status));
        }
        return rml;
    });
    append_text_element(right.root(), "small",
                        fmt::format("Lobby server: {} {} code {}", online_ui::server_host(snap.serverUrl), kDot,
                                    snap.code.empty() ? std::string("-") : snap.code));
    auto& leave = right.add_button("Leave game");
    leave.root()->SetClass("danger", true);
    leave.on_pressed([] {
        online_ui::confirm("Leave the game?", "Your captain will stand still for the rest of the match.", "Leave",
                           [] { online_ui::leave_to_start_menu(); }, true);
    });
}

void OnlineStatusWindow::update() {
    const online::Snapshot snap = online::snapshot();
    if (!pending_close() && (snap.phase == online::Phase::Idle || snap.phase == online::Phase::Ended)) {
        pop(); // the session is over: its own message takes over
    } else if (const std::string key = members_key(snap); key != mBuiltKey) {
        const bool hadFocus = has_focus();
        mBuiltKey = key;
        rebuild_content();
        if (hadFocus) {
            focus();
        }
    }
    if (mPrompts) {
        mPrompts->update();
    }
    Window::update();
}

void open_online_status_window() {
    auto window = std::make_unique<OnlineStatusWindow>();
    OnlineStatusWindow& ref = *window;
    if (auto* bar = MenuBar::current(); bar != nullptr && bar->visible()) {
        bar->open_over(std::move(window));
    } else if (auto* host = top_document()) {
        host->push(std::move(window));
    } else {
        push_document(std::move(window));
    }
    ref.focus();
}

} // namespace mppm::ui
