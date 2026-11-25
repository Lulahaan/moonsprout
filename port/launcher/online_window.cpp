#include "online_window.hpp"

#include "online_ui.hpp"
#include "prelaunch.hpp"
#include "settings.hpp"

#include "ui/bool_button.hpp"
#include "ui/button.hpp"
#include "ui/icon_button.hpp"
#include "ui/input.hpp"
#include "ui/pane.hpp"
#include "ui/select_button.hpp"
#include "ui/settings_window.hpp"
#include "ui/text_field.hpp"
#include "ui/ui.hpp"

#include <fmt/format.h>

#include <SDL3/SDL_clipboard.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <functional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace mppm::ui {
namespace {

namespace online = mppm::online;
using online::Member;
using online::Mode;
using online::Phase;
using online::Role;

constexpr const char* kGlyphInfo    = "\xEE\xA2\x8E"; // info
constexpr const char* kGlyphWarning = "\xEE\x80\x82"; // warning
constexpr const char* kGlyphError   = "\xEE\x80\x80"; // error
constexpr const char* kGlyphCheck   = "\xEE\x97\x8A"; // check (e5ca)
constexpr const char* kGlyphCrown   = "\xEE\xA3\x90"; // stars (e8d0): the host
constexpr const char* kEllipsis     = "\xE2\x80\xA6";
constexpr const char* kBullet       = "\xE2\x80\xA2";
constexpr const char* kDot          = "\xC2\xB7";

constexpr const char* kCodeHint = "A code has 8 letters and numbers, like ABCD-EFGH.";
constexpr const char* kNoServer = "No lobby server is set. Choose one in Settings \xE2\x80\xBA Online.";

OnlineWindow* sOpen = nullptr;

Rml::String note_rml(const char* glyph, const char* cls, const Rml::String& text) {
    return fmt::format("<help-note class=\"{}\"><note-icon>{}</note-icon><note-text>{}</note-text></help-note>", cls,
                       glyph, text);
}

int stage_count(Mode mode) {
    return mode == Mode::Challenge ? 30 : 10;
}

std::vector<int> range(int first, int last) {
    std::vector<int> v;
    for (int i = first; i <= last; ++i) {
        v.push_back(i);
    }
    return v;
}

// "ABCD-EFGH" for a valid code, else the text as typed.
std::string shown_code(const std::string& raw) {
    std::string canon;
    if (online::normalize_code(raw, canon) && canon.size() == 8) {
        return canon.substr(0, 4) + "-" + canon.substr(4);
    }
    return raw;
}

bool code_valid(const std::string& raw) {
    std::string canon;
    return online::normalize_code(raw, canon);
}

bool is_focused(const Component* component) {
    if (component == nullptr) {
        return false;
    }
    auto* context = component->root()->GetContext();
    return context != nullptr && context->GetFocusElement() != nullptr &&
           component->contains(context->GetFocusElement());
}

// ---- Components ----

// Text whose content follows a function (only touched when it changes). Never a focus target.
class LiveText final : public Component {
public:
    LiveText(Rml::Element* parent, const Rml::String& tag, std::function<std::string()> text)
        : Component(append(parent, tag)), mText(std::move(text)) {
        update();
    }
    void update() override {
        std::string text = mText();
        if (text != mCurrent) {
            set_text_content(mRoot, text);
            set_display(mRoot, text.empty() ? Rml::Style::Display::None : Rml::Style::Display::Block);
            mCurrent = std::move(text);
        }
        Component::update();
    }
    bool focus() override { return false; }

private:
    std::function<std::string()> mText;
    std::string mCurrent = "\x01";
};

// Like LiveText, as RML (help notes); hidden while empty.
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

// A row that steps through a list with Left / Right (Confirm steps forward and wraps), as the
// settings window's steppers (DESIGN 5.6): chevrons either side of the value.
class Stepper final : public BaseControlledSelectButton {
public:
    struct Props {
        Rml::String key;
        std::vector<int> values;
        std::function<std::string(int)> format;
        std::function<int()> getValue;
        std::function<void(int)> setValue;
        std::function<bool()> isDisabled;
    };

    Stepper(Rml::Element* parent, Props props)
        : BaseControlledSelectButton(parent, {.key = props.key, .submit = false}), mProps(std::move(props)) {
        mRoot->SetClass("stepper-button", true);
        auto* doc = mRoot->GetOwnerDocument();
        mChevLeft = mRoot->InsertBefore(doc->CreateElement("step-chev"), mValueElem);
        mChevLeft->SetClass("l", true);
        mChevRight = append(mRoot, "step-chev");
        mChevRight->SetClass("r", true);
        for (Rml::Element* chev : {mChevLeft, mChevRight}) {
            const int direction = chev == mChevLeft ? -1 : 1;
            Component::listen(chev, Rml::EventId::Mousedown, [this](Rml::Event& event) {
                if (!disabled()) {
                    mRoot->Focus(true);
                }
                event.StopPropagation();
            });
            Component::listen(chev, Rml::EventId::Click, [this, direction](Rml::Event& event) {
                if (!disabled()) {
                    step(direction, false);
                }
                event.StopPropagation();
            });
        }
        update();
    }

    void update() override {
        BaseControlledSelectButton::update();
        const int current = mProps.getValue ? mProps.getValue() : 0;
        const bool atStart = mProps.values.empty() || current <= mProps.values.front();
        const bool atEnd = mProps.values.empty() || current >= mProps.values.back();
        if (atStart != mAtStart) {
            mAtStart = atStart;
            mChevLeft->SetClass("end", atStart);
        }
        if (atEnd != mAtEnd) {
            mAtEnd = atEnd;
            mChevRight->SetClass("end", atEnd);
        }
    }

    bool disabled() const override { return mProps.isDisabled && mProps.isDisabled(); }

protected:
    Rml::String format_value() override {
        const int v = mProps.getValue ? mProps.getValue() : 0;
        return mProps.format ? mProps.format(v) : fmt::to_string(v);
    }

    bool handle_nav_command(NavCommand cmd) override {
        if (cmd == NavCommand::Right) {
            step(1, false);
        } else if (cmd == NavCommand::Left) {
            step(-1, false);
        } else if (cmd == NavCommand::Confirm) {
            step(1, true);
        } else {
            return false;
        }
        return true;
    }

private:
    void step(int direction, bool wrap) {
        const auto& values = mProps.values;
        if (values.empty() || !mProps.setValue) {
            return;
        }
        const int current = mProps.getValue();
        int next = current;
        if (direction > 0) {
            const auto it = std::ranges::find_if(values, [current](int v) { return v > current; });
            next = it != values.end() ? *it : (wrap ? values.front() : current);
        } else {
            const auto it = std::ranges::find_if(values.rbegin(), values.rend(), [current](int v) { return v < current; });
            next = it != values.rend() ? *it : values.front();
        }
        if (next == current) {
            return;
        }
        mProps.setValue(next);
        play_sound(kSoundItemChange);
    }

    Props mProps;
    Rml::Element* mChevLeft = nullptr;
    Rml::Element* mChevRight = nullptr;
    int mAtStart = -1;
    int mAtEnd = -1;
};

// A button that stays focusable while disabled (the pane allows it, so the note next to it can say
// why) and gives the warning cue when pressed then.
ControlledButton& add_guarded_button(Pane& pane, Rml::String text, std::function<bool()> isDisabled,
                                     std::function<void()> pressed) {
    auto& button = pane.add_button(ControlledButton::Props{.text = std::move(text), .isDisabled = isDisabled});
    button.on_pressed(std::move(pressed));
    button.set_disabled(isDisabled && isDisabled());
    const auto guard = [&button](Rml::Event& event) {
        if (button.disabled()) {
            play_sound(kSoundWarning);
            event.StopPropagation();
        }
    };
    button.listen(Rml::EventId::Keydown, [&button, guard](Rml::Event& event) {
        if (button.disabled() && map_nav_event(event) == NavCommand::Confirm) {
            guard(event);
        }
    });
    button.listen(Rml::EventId::Click, guard);
    return button;
}

// One seat of the room (design 10.2): team marker, "P<n>", the name with the host's star and "You",
// then ping, link and the ready pill; an open seat says so and offers Take seat; a member whose
// game differs gets the note on a second line. Rebuilt from the window's snapshot when it changes.
class SeatRow final : public FluentComponent<SeatRow> {
public:
    SeatRow(Rml::Element* parent, const OnlineWindow& window, int slot)
        : FluentComponent(append(parent, "select-button")), mWindow(window), mSlot(slot) {
        mRoot->SetClass("seat-row", true);
        on_nav_command([this](Rml::Event&, NavCommand cmd) {
            if (cmd == NavCommand::Confirm) {
                press();
                return true;
            }
            return false;
        });
        update();
    }

    void update() override {
        const Member* m = mWindow.member_in(mSlot);
        const Member* self = mWindow.self();
        const bool canTake = m == nullptr && self != nullptr && self->slot != mSlot &&
                             mWindow.snap().phase == Phase::Lobby;
        std::string sig = fmt::format("{}|{}", mSlot, canTake ? 1 : 0);
        if (m != nullptr) {
            sig += fmt::format("|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}", m->id, m->name, m->host ? 1 : 0, m->self ? 1 : 0,
                               m->ready ? 1 : 0, m->connected ? 1 : 0, m->contentOk ? 1 : 0, m->contentNote, m->pingMs,
                               static_cast<int>(m->link));
        }
        if (sig != mSig) {
            mSig = sig;
            rebuild(m, canTake);
        }
        FluentComponent::update();
    }

private:
    void rebuild(const Member* m, bool canTake) {
        ui::clear_children(mRoot);
        mRoot->SetClass("open-seat", m == nullptr);
        mRoot->SetClass("self", m != nullptr && m->self);
        mRoot->SetClass("mismatch", m != nullptr && !m->contentOk);
        auto* line = append(mRoot, "seat-line");
        online_ui::append_team_marker(line, mSlot & 1);
        append_text_element(line, "seat-num", fmt::format("P{}", mSlot + 1));
        auto* name = append(line, "seat-name");
        append_text(name, m != nullptr ? m->name : std::string("Open seat"));
        if (m != nullptr && m->host) {
            append_text_element(line, "seat-crown", kGlyphCrown);
        }
        if (m != nullptr && m->self) {
            append_text_element(line, "you-tag", "You");
        }
        append(line, "seat-gap");
        if (m != nullptr) {
            if (!m->host && m->pingMs >= 0) {
                append_text_element(line, "ping-badge", online_ui::ping_text(m->pingMs));
            }
            if (!m->host && m->link != online::Link::Unknown) {
                auto* link = append_text_element(line, "link-icon", online_ui::link_glyph(m->link));
                link->SetClass("relay", m->link == online::Link::Relay);
            }
            if (!m->connected) {
                append_text_element(line, "ready-pill", fmt::format("Connecting{}", kEllipsis))->SetClass("waiting", true);
            } else {
                append_text_element(line, "ready-pill", m->ready ? "Ready" : "Not ready")->SetClass("on", m->ready);
            }
        } else if (canTake) {
            append_text_element(line, "take-seat", "Take seat");
        }
        if (m != nullptr && !m->contentOk) {
            auto* note = append(mRoot, "seat-note");
            append_text_element(note, "note-icon", kGlyphWarning);
            append_text_element(note, "note-text",
                                m->contentNote.empty() ? std::string("Their game content differs from the host's.")
                                                       : m->contentNote);
        }
    }

    void press() {
        const Member* m = mWindow.member_in(mSlot);
        const Member* self = mWindow.self();
        if (mWindow.snap().phase != Phase::Lobby || self == nullptr) {
            play_sound(kSoundWarning);
            return;
        }
        if (m == nullptr) {
            play_sound(kSoundItemChange);
            online::take_slot(mSlot);
        } else if (m->self) {
            if (!m->contentOk) {
                play_sound(kSoundWarning);
                return;
            }
            play_sound(m->ready ? kSoundItemDisable : kSoundItemEnable);
            online::set_ready(!m->ready);
        } else if (mWindow.is_host()) {
            const uint32_t id = m->id;
            online_ui::confirm(fmt::format("Remove {}?", m->name), "They go back to their start menu.", "Remove",
                               [id] { online::kick(id); }, true);
        } else {
            play_sound(kSoundWarning);
        }
    }

    const OnlineWindow& mWindow;
    int mSlot;
    std::string mSig = "\x01";
};

// "Lobby", the code pill and Copy, and the match line under them.
class RoomHeader final : public Component {
public:
    RoomHeader(Rml::Element* parent, const OnlineWindow& window) : Component(append(parent, "room-header")), mWindow(window) {
        auto* row = append(mRoot, "room-title-row");
        append_text_element(row, "heading", "Lobby");
        mCode = append(row, "code-pill");
        auto& copy = add_child<IconButton>(IconButton::Props{.icon = "content_copy", .label = "Copy code"});
        row->AppendChild(mRoot->RemoveChild(copy.root()));
        copy.on_pressed([this] {
            const std::string code = mWindow.snap().code;
            if (!code.empty() && SDL_SetClipboardText(code.c_str())) {
                play_sound(kSoundClick);
                push_toast({.type = "success", .title = "Code copied", .content = online_ui::text_rml(code)});
            } else {
                play_sound(kSoundWarning);
            }
        });
        mLine = append(mRoot, "small");
        update();
    }
    // The header itself is text: the focus goes to its Copy button.
    bool focus() override {
        for (const auto& child : mChildren) {
            if (child->focus()) {
                return true;
            }
        }
        return false;
    }
    void update() override {
        const std::string code = mWindow.snap().code.empty() ? std::string(kEllipsis) : mWindow.snap().code;
        if (code != mShownCode) {
            mShownCode = code;
            set_text_content(mCode, code);
        }
        const std::string line = online_ui::config_line(mWindow.snap().config);
        if (line != mShownLine) {
            mShownLine = line;
            set_text_content(mLine, line);
        }
        Component::update();
    }

private:
    const OnlineWindow& mWindow;
    Rml::Element* mCode = nullptr;
    Rml::Element* mLine = nullptr;
    std::string mShownCode;
    std::string mShownLine;
};

// A heading block at the top of a form pane.
void add_form_header(Pane& pane, const std::string& title, const std::string& text) {
    auto* header = append(pane.root(), "form-header");
    append_text_element(header, "heading", title);
    if (!text.empty()) {
        append_text_element(header, "small", text);
    }
}

// Button prompts at the bottom of the window (DESIGN 5.17), as in the Mods window.
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

} // namespace

// ---------------------------------------------------------------------------------------------
// OnlineWindow
// ---------------------------------------------------------------------------------------------

OnlineWindow::OnlineWindow() : Window(Props{.tabBar = false, .styleSheets = {"res/rml/online.rcss"}}) {
    sOpen = this;
    mRoot->SetClass("online", true);
    mSnap = online::snapshot();
    mBuiltKey = view_key();
    set_content([this](Rml::Element* content) { build_content(content); });
    mPrompts = std::make_unique<Prompts>(mRoot);
}

OnlineWindow::~OnlineWindow() {
    if (sOpen == this) {
        sOpen = nullptr;
    }
}

const Member* OnlineWindow::self() const {
    for (const Member& m : mSnap.members) {
        if (m.self) {
            return &m;
        }
    }
    return nullptr;
}

const Member* OnlineWindow::member_in(int slot) const {
    for (const Member& m : mSnap.members) {
        if (m.slot == slot) {
            return &m;
        }
    }
    return nullptr;
}

std::string OnlineWindow::view_key() const {
    switch (mSnap.phase) {
    case Phase::Connecting:
        return "connecting";
    case Phase::Lobby:
    case Phase::Starting: {
        const Member* s = self();
        return fmt::format("room|{}|{}|{}|{}|{}|{}", static_cast<int>(mSnap.role), mSnap.seats,
                           static_cast<int>(mSnap.config.mode), s != nullptr && s->slot >= 0 ? 1 : 0,
                           s != nullptr && s->contentOk ? 1 : 0, s != nullptr && s->spectator ? 1 : 0);
    }
    default:
        return fmt::format("home|{}|{}|{}", online_ui::server_url(), static_cast<int>(mHostConfig.mode),
                           mNeedPassword ? 1 : 0);
    }
}

void OnlineWindow::remember(const std::string& key, Component& component) {
    mFocusTargets[key] = &component;
}

std::string OnlineWindow::focused_key() const {
    for (const auto& [key, component] : mFocusTargets) {
        if (is_focused(component)) {
            return key;
        }
    }
    return {};
}

void OnlineWindow::forget_detail() {
    std::erase_if(mFocusTargets, [](const auto& entry) { return entry.first != "host" && entry.first != "join"; });
}

std::string OnlineWindow::default_focus() const {
    switch (mSnap.phase) {
    case Phase::Connecting:
        return "cancel";
    case Phase::Lobby:
    case Phase::Starting:
        return mFocusTargets.contains("ready") ? "ready" : "seat0";
    default:
        return mSelected;
    }
}

bool OnlineWindow::focus_key(const std::string& key) {
    const auto it = mFocusTargets.find(key);
    if (it == mFocusTargets.end()) {
        return false;
    }
    // A disabled control still takes the focus where its pane allows it (its note says why).
    return it->second->focus() || it->second->root()->Focus(true);
}

void OnlineWindow::uncover() {
    Window::uncover();
    // Back from a confirmation or Settings: the control that opened it gets the focus again. The
    // content may have been rebuilt meanwhile (a server chosen in Settings): then the view's
    // default control.
    if (!focus_key(mLastFocus) && focused_key().empty() && !focus_key(default_focus())) {
        focus();
    }
}

void OnlineWindow::build_content(Rml::Element* content) {
    mFocusTargets.clear();
    if (mSnap.phase == Phase::Lobby || mSnap.phase == Phase::Starting) {
        build_room(content);
    } else {
        build_home(content);
    }
}

// ---- Home ----

void OnlineWindow::build_home(Rml::Element* content) {
    auto& list = add_child<Pane>(content, Pane::Type::Controlled);
    list.root()->SetClass("list", true);
    auto& detail = add_child<Pane>(content, Pane::Type::Uncontrolled);
    detail.root()->SetClass("detail", true);
    detail.set_focus_disabled(true);

    const bool connecting = mSnap.phase == Phase::Connecting;
    list.add_section("Online");
    const auto add_entry = [&](const std::string& key, const char* label) {
        auto& entry = list.add_group_button({
            .text = label,
            .isSelected = [this, key] { return mSelected == key; },
            .isDisabled = [this] { return mSnap.phase == Phase::Connecting; },
        });
        remember(key, entry);
        list.register_control(entry, detail, [this, key](Pane& pane) {
            forget_detail(); // the pane was just cleared: its controls are gone
            const bool changed = mSelected != key;
            if (changed) {
                mSelected = key;
                play_sound(kSoundItemChange);
            }
            if (key == "host") {
                build_host_form(pane);
            } else {
                build_join_form(pane);
            }
        });
    };
    add_entry("host", "Host a game");
    add_entry("join", "Join with a code");
    const std::string host = online_ui::server_host(online_ui::server_url());
    append_text_element(list.root(), "list-note",
                        host.empty() ? std::string(kNoServer) : fmt::format("Lobby server: {}", host));

    if (connecting) {
        build_connecting(detail);
    } else if (mSelected == "join") {
        build_join_form(detail);
    } else {
        build_host_form(detail);
    }
}

void OnlineWindow::build_no_server(Pane& pane, bool host) {
    add_form_header(pane, host ? "Host a game" : "Join with a code", "");
    pane.add_rml(note_rml(kGlyphInfo, "info", kNoServer))->SetClass("online-notes", true);
    auto& open = pane.add_button("Open Settings");
    remember(host ? "open-settings-host" : "open-settings-join", open);
    open.on_pressed([this] {
        play_sound(kSoundClick);
        SettingsWindow& window = open_settings_window(Prelaunch::current() != nullptr, this);
        window.show_tab("Online");
    });
}

void OnlineWindow::build_host_form(Pane& pane) {
    if (online_ui::server_url().empty()) {
        build_no_server(pane, true);
        return;
    }
    add_form_header(pane, "Host a game", "Pick the match. The others join with the code you get.");
    auto& mode = pane.add_child<Stepper>(Stepper::Props{
        .key = "Mode",
        .values = {0, 1, 2},
        .format = [](int v) { return std::string(online_ui::mode_label(static_cast<Mode>(v))); },
        .getValue = [this] { return static_cast<int>(mHostConfig.mode); },
        .setValue =
            [this](int v) {
                mHostConfig.mode = static_cast<Mode>(v);
                mHostConfig.stage = std::min(mHostConfig.stage, stage_count(mHostConfig.mode) - 1);
                mHostError.clear();
                mPendingFocus = "mode";
                mRebuild = true; // the stage list and the handicap rows change
            },
    });
    remember("mode", mode);
    auto& stage = pane.add_child<Stepper>(Stepper::Props{
        .key = mHostConfig.mode == Mode::Challenge ? "Course" : "Stage",
        .values = range(0, stage_count(mHostConfig.mode) - 1),
        .format = [this](int v) { return online_ui::stage_label(mHostConfig.mode, v); },
        .getValue = [this] { return mHostConfig.stage; },
        .setValue = [this](int v) { mHostConfig.stage = v; },
    });
    remember("stage", stage);
    if (mHostConfig.mode == Mode::Battle) {
        pane.add_section("Handicaps");
        for (int team = 0; team < 2; ++team) {
            auto& h = pane.add_child<Stepper>(Stepper::Props{
                .key = team == 0 ? "Red team" : "Blue team",
                .values = range(0, 10),
                .format = [](int v) { return online_ui::handicap_label(v); },
                .getValue = [this, team] { return mHostConfig.handicap[team]; },
                .setValue = [this, team](int v) { mHostConfig.handicap[team] = v; },
            });
            online_ui::append_team_marker(h.root()->GetFirstChild(), team);
            remember(team == 0 ? "hcap0" : "hcap1", h);
        }
    } else {
        mHostConfig.handicap[0] = mHostConfig.handicap[1] = 0;
    }
    pane.add_section("Lobby");
    auto& password = pane.add_child<TextField>(TextField::Props{
        .key = "Password",
        .getValue = [this] { return mHostPassword; },
        .setValue = [this](Rml::String v) { mHostPassword = std::move(v); },
        .display = [](const Rml::String& v) {
            std::string dots;
            for (size_t i = 0; i < std::min<size_t>(v.size(), 12); ++i) {
                dots += kBullet;
            }
            return dots;
        },
        .placeholder = "None",
        .maxLength = 32,
    });
    remember("password", password);
    append_text_element(pane.root(), "small", "Optional. Players need it to join.");
    pane.add_child<LiveRml>("help-notes", [this]() -> Rml::String {
        return mHostError.empty() ? Rml::String{} : note_rml(kGlyphError, "error", escape(mHostError));
    });
    auto& create = pane.add_button("Create lobby");
    create.root()->SetClass("primary", true);
    create.on_pressed([this] { create_lobby(); });
    remember("create", create);
}

void OnlineWindow::build_join_form(Pane& pane) {
    if (online_ui::server_url().empty()) {
        build_no_server(pane, false);
        return;
    }
    add_form_header(pane, "Join with a code", "Ask the host for the code of their lobby.");
    auto& code = pane.add_child<TextField>(TextField::Props{
        .key = "Code",
        .getValue = [this] { return mJoinCode; },
        .setValue =
            [this](Rml::String v) {
                if (v != mJoinCode) {
                    mJoinError.clear();
                }
                mJoinCode = std::move(v);
            },
        .display = [](const Rml::String& v) { return shown_code(v); },
        .placeholder = "ABCD-EFGH",
        .isInvalid = [this] { return !mJoinCode.empty() && !code_valid(mJoinCode); },
        .maxLength = 12,
        .setOnChange = true,
    });
    code.root()->SetClass("code-field", true);
    remember("code", code);
    pane.add_child<LiveRml>("help-notes", [this]() -> Rml::String {
        if (!mJoinCode.empty() && !code_valid(mJoinCode)) {
            return note_rml(kGlyphError, "error", kCodeHint);
        }
        return mJoinError.empty() ? Rml::String{} : note_rml(kGlyphError, "error", escape(mJoinError));
    });
    auto& spectate = pane.add_child<BoolButton>(BoolButton::Props{
        .key = "Watch as a spectator",
        .getValue = [this] { return mJoinSpectate; },
        .setValue = [this](bool v) { mJoinSpectate = v; },
    });
    remember("spectate", spectate);
    if (mNeedPassword) {
        auto& password = pane.add_child<TextField>(TextField::Props{
            .key = "Password",
            .getValue = [this] { return mJoinPassword; },
            .setValue = [this](Rml::String v) { mJoinPassword = std::move(v); },
            .display = [](const Rml::String& v) {
                std::string dots;
                for (size_t i = 0; i < std::min<size_t>(v.size(), 12); ++i) {
                    dots += kBullet;
                }
                return dots;
            },
            .placeholder = "Type it here",
            .maxLength = 32,
        });
        remember("joinpw", password);
        pane.add_rml(note_rml(kGlyphInfo, "info", "This lobby has a password."))->SetClass("online-notes", true);
    }
    auto& join = add_guarded_button(pane, "Join", [this] { return !code_valid(mJoinCode); }, [this] { join_lobby(); });
    join.root()->SetClass("primary", true);
    remember("join-button", join);
}

void OnlineWindow::build_connecting(Pane& pane) {
    const bool host = mSnap.role == Role::Host;
    add_form_header(pane, fmt::format("Connecting{}", kEllipsis),
                    host ? fmt::format("Opening a lobby on {}.", online_ui::server_host(mSnap.serverUrl))
                         : fmt::format("Joining {}.", mSnap.code));
    auto* loader = append(pane.root(), "loader");
    loader->SetClass("connecting", true);
    for (int i = 0; i < 3; ++i) {
        append(loader, "bead");
    }
    auto& cancel = pane.add_button("Cancel");
    cancel.on_pressed([] {
        play_sound(kSoundWindowClose);
        online::leave();
    });
    remember("cancel", cancel);
}

void OnlineWindow::create_lobby() {
    std::string err;
    if (!online::host(mHostConfig, mHostPassword, err)) {
        play_sound(kSoundWarning);
        mHostError = err.empty() ? std::string("The lobby couldn't be created.") : err;
        return;
    }
    play_sound(kSoundClick);
    mHostError.clear();
    mPendingFocus = "cancel";
}

void OnlineWindow::join_lobby() {
    std::string err;
    if (!online::join(mJoinCode, mJoinSpectate, mNeedPassword ? mJoinPassword : std::string{}, err)) {
        play_sound(kSoundWarning);
        mJoinError = err.empty() ? std::string("Couldn't join that lobby.") : err;
        return;
    }
    play_sound(kSoundClick);
    mJoinError.clear();
    mPendingFocus = "cancel";
}

// ---- Room ----

void OnlineWindow::build_room(Rml::Element* content) {
    auto& left = add_child<Pane>(content, Pane::Type::Uncontrolled);
    left.root()->SetClass("seats", true);
    auto& right = add_child<Pane>(content, Pane::Type::Uncontrolled);
    right.root()->SetClass("detail", true);
    // The host's disabled Start game still takes the focus (the status says why); a member's
    // read-only match rows are no stops.
    right.set_focus_disabled(is_host());

    auto& header = left.add_child<RoomHeader>(*this);
    remember("copy", header); // its only control is the Copy button
    left.add_section("Players");
    for (int slot = 0; slot < mSnap.seats; ++slot) {
        auto& row = left.add_child<SeatRow>(*this, slot);
        remember(fmt::format("seat{}", slot), row);
    }
    left.add_child<LiveText>("watching", [this]() -> std::string {
        std::string names;
        for (const Member& m : mSnap.members) {
            if (m.slot < 0) {
                names += (names.empty() ? "" : ", ") + m.name;
            }
        }
        return names.empty() ? std::string{} : "Watching: " + names;
    });
    auto& leave = left.add_button(is_host() ? "Close lobby" : "Leave lobby");
    leave.root()->SetClass("danger", true);
    leave.root()->SetClass("leave-button", true);
    leave.on_pressed([this] { ask_leave(); });
    remember("leave", leave);

    build_room_right(right);
}

void OnlineWindow::build_room_right(Pane& pane) {
    const bool host = is_host();
    const Member* me = self();
    const bool seated = me != nullptr && me->slot >= 0;
    pane.add_section("Match");
    if (host) {
        auto& mode = pane.add_child<Stepper>(Stepper::Props{
            .key = "Mode",
            .values = {0, 1, 2},
            .format = [](int v) { return std::string(online_ui::mode_label(static_cast<Mode>(v))); },
            .getValue = [this] { return static_cast<int>(mSnap.config.mode); },
            .setValue =
                [this](int v) {
                    online::Config c = mSnap.config;
                    c.mode = static_cast<Mode>(v);
                    c.stage = std::min(c.stage, stage_count(c.mode) - 1);
                    if (c.mode != Mode::Battle) {
                        c.handicap[0] = c.handicap[1] = 0;
                    }
                    online::set_config(c);
                },
            .isDisabled = [this] { return mSnap.phase != Phase::Lobby; },
        });
        remember("r-mode", mode);
        auto& stage = pane.add_child<Stepper>(Stepper::Props{
            .key = mSnap.config.mode == Mode::Challenge ? "Course" : "Stage",
            .values = range(0, stage_count(mSnap.config.mode) - 1),
            .format = [this](int v) { return online_ui::stage_label(mSnap.config.mode, v); },
            .getValue = [this] { return mSnap.config.stage; },
            .setValue =
                [this](int v) {
                    online::Config c = mSnap.config;
                    c.stage = v;
                    online::set_config(c);
                },
            .isDisabled = [this] { return mSnap.phase != Phase::Lobby; },
        });
        remember("r-stage", stage);
        if (mSnap.config.mode == Mode::Battle) {
            for (int team = 0; team < 2; ++team) {
                auto& h = pane.add_child<Stepper>(Stepper::Props{
                    .key = team == 0 ? "Red handicap" : "Blue handicap",
                    .values = range(0, 10),
                    .format = [](int v) { return online_ui::handicap_label(v); },
                    .getValue = [this, team] { return mSnap.config.handicap[team]; },
                    .setValue =
                        [this, team](int v) {
                            online::Config c = mSnap.config;
                            c.handicap[team] = v;
                            online::set_config(c);
                        },
                    .isDisabled = [this] { return mSnap.phase != Phase::Lobby; },
                });
                online_ui::append_team_marker(h.root()->GetFirstChild(), team);
                remember(team == 0 ? "r-hcap0" : "r-hcap1", h);
            }
        }
    } else {
        // Read-only for members: the host decides (disabled rows are not focus stops here).
        const auto info = [&](const std::string& key, std::function<std::string()> value) {
            auto& row = pane.add_child<ControlledSelectButton>(ControlledSelectButton::Props{
                .key = key,
                .getValue = std::move(value),
                .isDisabled = [] { return true; },
                .submit = false,
            });
            row.root()->SetClass("info-row", true);
        };
        info("Mode", [this] { return std::string(online_ui::mode_label(mSnap.config.mode)); });
        info(mSnap.config.mode == Mode::Challenge ? "Course" : "Stage",
             [this] { return online_ui::stage_label(mSnap.config.mode, mSnap.config.stage); });
        if (mSnap.config.mode == Mode::Battle) {
            info("Handicaps", [this] {
                return fmt::format("Red {} {} Blue {}", online_ui::handicap_label(mSnap.config.handicap[0]), kDot,
                                   online_ui::handicap_label(mSnap.config.handicap[1]));
            });
        }
    }

    // Status: why the game can't start yet, or that it can (help-pane style).
    pane.add_child<LiveRml>("help-notes", [this]() -> Rml::String {
        Rml::String rml;
        const Member* s = self();
        if (s != nullptr && !s->contentOk) {
            rml += note_rml(kGlyphWarning, "warning",
                            escape(s->contentNote.empty() ? std::string("Your game content differs from the host's.")
                                                          : s->contentNote) +
                                " You can't get ready until it matches.");
        }
        // With our own content mismatch the warning above is the status (the session's status line
        // repeats the same note).
        if (!mSnap.status.empty() && (s == nullptr || s->contentOk)) {
            rml += note_rml(mSnap.canStart ? kGlyphCheck : kGlyphInfo, mSnap.canStart ? "ready" : "info",
                            escape(mSnap.status));
        }
        return rml;
    });

    if (seated) {
        auto& ready = pane.add_child<BoolButton>(BoolButton::Props{
            .key = "Ready",
            .getValue = [this] {
                const Member* s = self();
                return s != nullptr && s->ready;
            },
            .setValue = [](bool v) { online::set_ready(v); },
            .isDisabled = [this] {
                const Member* s = self();
                return mSnap.phase != Phase::Lobby || s == nullptr || s->slot < 0 || !s->contentOk;
            },
        });
        ready.root()->SetClass("ready-toggle", true);
        remember("ready", ready);
    } else if (me != nullptr && me->spectator) {
        append_text_element(pane.root(), "small", "You're watching this match. Take an open seat to play instead.");
    } else {
        append_text_element(pane.root(), "small", "Take an open seat to play.");
    }

    if (host) {
        auto& start = add_guarded_button(pane, "Start game", [this] { return !mSnap.canStart || mSnap.phase != Phase::Lobby; },
                                         [] {
                                             play_sound(kSoundPlay);
                                             online::start();
                                         });
        start.root()->SetClass("primary", true);
        start.root()->SetClass("start-button", true);
        remember("start", start);
    }
}

void OnlineWindow::ask_leave() {
    if (is_host()) {
        online_ui::confirm("Close the lobby?", "Everyone in it will be sent back.", "Close lobby", [] { online::leave(); },
                           true);
    } else {
        online_ui::confirm("Leave the lobby?", "You can join again with the code while the lobby is open.", "Leave",
                           [] { online::leave(); }, true);
    }
}

bool OnlineWindow::consume_close_request() {
    switch (mSnap.phase) {
    case Phase::Connecting:
        play_sound(kSoundWindowClose);
        online::leave();
        return true;
    case Phase::Lobby:
        ask_leave();
        return true;
    default:
        return false;
    }
}

void OnlineWindow::handle_ended() {
    if (mEndShown) {
        return;
    }
    mEndShown = true;
    std::string lower = mSnap.error;
    std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (mSnap.endReason == online::kEndRefused && mSnap.role != Role::Host && lower.find("password") != std::string::npos) {
        // A lobby with a password (bad_password): back to the join pane, with the password field.
        mJoinError = mNeedPassword ? mSnap.error : std::string{};
        mNeedPassword = true;
        mSelected = "join";
        mPendingFocus = "joinpw";
        play_sound(kSoundWarning);
        online::leave();
        return;
    }
    online_ui::show_end_message(mSnap, false, [this] {
        mPendingFocus = mSnap.role == Role::Host ? "host" : "join";
    });
}

void OnlineWindow::update() {
    mSnap = online::snapshot();
    if (mSnap.phase != Phase::Ended) {
        mEndShown = false;
    }
    if (!mClosing && !pending_close() && mSnap.phase == Phase::Starting) {
        // The match is on its way: the take-off follows (p2_launcher_online_started).
        mClosing = true;
        play_sound(kSoundWindowClose);
        pop();
    }
    if (mSnap.phase == Phase::Ended && !mClosing) {
        handle_ended();
    }
    const std::string key = view_key();
    if (!mClosing && (key != mBuiltKey || mRebuild)) {
        std::string want = mPendingFocus.empty() ? focused_key() : mPendingFocus;
        const auto viewOf = [](const std::string& k) { return k.substr(0, k.find('|')); };
        const bool viewChanged = viewOf(key) != viewOf(mBuiltKey);
        const bool takeFocus = has_focus() || !mPendingFocus.empty() || viewChanged;
        mBuiltKey = key;
        mRebuild = false;
        mPendingFocus.clear();
        rebuild_content();
        if (viewChanged || !mFocusTargets.contains(want)) {
            want = viewChanged && !want.empty() && mFocusTargets.contains(want) ? want : default_focus();
        }
        if (takeFocus && !focus_key(want)) {
            focus();
        }
    }
    if (has_focus()) {
        if (std::string k = focused_key(); !k.empty()) {
            mLastFocus = std::move(k);
        }
    }
    if (mPrompts) {
        mPrompts->update();
    }
    Window::update();
}

void open_online_window() {
    if (sOpen != nullptr && !sOpen->pending_close()) {
        sOpen->focus();
        return;
    }
    auto window = std::make_unique<OnlineWindow>();
    OnlineWindow& ref = *window;
    if (auto* menu = Prelaunch::current(); menu != nullptr && menu->active()) {
        menu->push(std::move(window));
    } else if (auto* host = top_document()) {
        host->push(std::move(window));
    } else {
        push_document(std::move(window));
    }
    ref.focus();
}

bool online_window_open() {
    return sOpen != nullptr && !sOpen->pending_close() && !sOpen->closed();
}

} // namespace mppm::ui
