#include "online_overlay.hpp"

#include "menu_bar.hpp"
#include "online_ui.hpp"

#include "ui/ui.hpp"

#include <fmt/format.h>
#include <p2_online.hpp>

#include <cmath>
#include <string>
#include <vector>

namespace mppm::ui {
namespace {

namespace online = mppm::online;

const Rml::String kDocumentSource = R"RML(
<rml>
<head>
    <link type="text/rcss" href="res/rml/theme.rcss" />
    <link type="text/rcss" href="res/rml/controls.rcss" />
    <link type="text/rcss" href="res/rml/online.rcss" />
</head>
<body class="online-overlay">
    <wait-row>
        <online-wait id="wait">
            <icon class="spinner" />
            <wait-label><wait-text id="wait-text" /><wait-seconds id="wait-seconds" /></wait-label>
        </online-wait>
    </wait-row>
</body>
</rml>
)RML";

constexpr const char* kEllipsis = "\xE2\x80\xA6";

OnlineOverlay* sOverlay = nullptr;

// "Louie (P2)" -> "Louie".
std::string bare_name(const std::string& name) {
    const auto open = name.rfind(" (P");
    return open != std::string::npos && !name.empty() && name.back() == ')' ? name.substr(0, open) : name;
}

std::string join_names(const std::vector<std::string>& names) {
    if (names.size() == 1) {
        return names.front();
    }
    std::string out;
    for (size_t i = 0; i < names.size(); ++i) {
        if (i > 0) {
            out += i + 1 == names.size() ? " and " : ", ";
        }
        out += bare_name(names[i]);
    }
    return out;
}

std::string wait_text(const online::Stall& stall) {
    if (stall.loading) {
        if (stall.names.empty()) {
            return fmt::format("Waiting for players to load{}", kEllipsis);
        }
        return fmt::format("Waiting for {} to load{}", join_names(stall.names), kEllipsis);
    }
    if (stall.names.empty()) {
        return fmt::format("Waiting for the host{}", kEllipsis);
    }
    return fmt::format("Waiting for {}{}", join_names(stall.names), kEllipsis);
}

} // namespace

OnlineOverlay::OnlineOverlay() : Document(kDocumentSource, false, DocumentScope::Overlay) {
    sOverlay = this;
    if (mDocument == nullptr) {
        return;
    }
    mPill = mDocument->GetElementById("wait");
    mText = mDocument->GetElementById("wait-text");
    mSeconds = mDocument->GetElementById("wait-seconds");
    mButton = std::make_unique<Button>(mPill, "");
    mButton->root()->SetClass("compact", true);
    mButton->root()->SetClass("danger", true);
    mButton->on_pressed([this] { press(); });
    set_display(mButton->root(), Rml::Style::Display::None);
}

OnlineOverlay::~OnlineOverlay() {
    if (sOverlay == this) {
        sOverlay = nullptr;
    }
}

void OnlineOverlay::hide(bool close) {
    // Only ever closed with the UI itself; close_all_documents (the standalone menu handing over to
    // the boot) hides the RML document, and update() shows it again.
    if (close) {
        Document::hide(true);
    }
}

bool OnlineOverlay::others_visible() const {
    bool any = false;
    for_each_visible_document([this, &any](Document& doc) {
        if (&doc != this) {
            any = true;
        }
    });
    return any;
}

void OnlineOverlay::set_action(Action action, const std::string& label) {
    if (action == mAction && label == mActionLabel) {
        return;
    }
    mAction = action;
    mActionLabel = label;
    mDeclined = false; // a new button asks again
    if (action == Action::None) {
        set_display(mButton->root(), Rml::Style::Display::None);
    } else {
        mButton->set_text(label);
        set_display(mButton->root(), Rml::Style::Display::Block);
    }
}

void OnlineOverlay::press() {
    if (mAction == Action::Drop) {
        const int slot = mDropSlot;
        online_ui::confirm(fmt::format("Drop {}?", mDropName), "Their captain will stand still for the rest of the match.",
            "Drop", [slot] { online::drop_slot(slot); }, true);
    } else if (mAction == Action::Leave) {
        online_ui::confirm("Leave the game?", "Your captain will stand still for the rest of the match.", "Leave",
            [] { online_ui::leave_to_start_menu(); }, true);
    }
}

void OnlineOverlay::update() {
    if (mDocument == nullptr) {
        return;
    }
    if (!mDocument->IsVisible()) {
        mDocument->Show(Rml::ModalFlag::None, Rml::FocusFlag::None, Rml::ScrollFlag::None);
    }
    const online::Stall stall = online::in_game() ? online::stall() : online::Stall{};
    const bool open = stall.waiting;
    if (open != mOpen) {
        mOpen = open;
        if (open) {
            mPill->SetAttribute("open", "");
        } else {
            mPill->RemoveAttribute("open");
        }
    }
    if (open) {
        const std::string text = wait_text(stall);
        if (text != mShownText) {
            mShownText = text;
            set_text_content(mText, text);
        }
        const std::string seconds =
            stall.seconds >= 3.0 ? fmt::format("{} s", static_cast<int>(std::floor(stall.seconds))) : std::string{};
        if (seconds != mShownSeconds) {
            mShownSeconds = seconds;
            set_text_content(mSeconds, seconds);
            mSeconds->SetClass("shown", !seconds.empty());
        }
    }
    // One button at most: the host drops the first seat it waits for, a client may leave.
    Action action = Action::None;
    std::string label;
    if (open && stall.canDrop && !stall.slots.empty() && !stall.names.empty()) {
        action = Action::Drop;
        mDropSlot = stall.slots.front();
        mDropName = bare_name(stall.names.front());
        label = fmt::format("Drop {}", mDropName);
    } else if (open && stall.canLeave) {
        action = Action::Leave;
        label = "Leave game";
    }
    set_action(action, label);

    // Below the in-game bar while it is open (the bar floats at the top centre too).
    const bool barOpen = MenuBar::current() != nullptr && MenuBar::current()->visible();
    if (barOpen != mBarOpen) {
        mBarOpen = barOpen;
        mDocument->SetClass("bar-open", barOpen);
    }

    const bool want = action != Action::None && !mDeclined && !others_visible();
    if (want != mWantFocus) {
        mWantFocus = want;
        if (!want && has_focus()) {
            mButton->root()->Blur();
        }
    }
    if (mWantFocus && !has_focus()) {
        focus();
    }
    mButton->update();
}

bool OnlineOverlay::focus() {
    return mWantFocus && mButton->root()->Focus(true);
}

bool OnlineOverlay::handle_nav_command(Rml::Event& event, NavCommand cmd) {
    switch (cmd) {
    case NavCommand::Confirm:
        play_sound(kSoundClick);
        press();
        return true;
    case NavCommand::Cancel:
        // Back to the game; the button stays on the pill (a click still works) until it changes.
        mDeclined = true;
        mWantFocus = false;
        mButton->root()->Blur();
        play_sound(kSoundWindowClose);
        return true;
    case NavCommand::Menu:
        // The in-game bar opens over the pill, as from the game.
        if (auto* bar = MenuBar::current()) {
            mWantFocus = false;
            mButton->root()->Blur();
            return bar->handle_nav_event(event);
        }
        return false;
    default:
        return true; // the only control: nothing else to move to
    }
}

void install_online_overlay() {
    if (sOverlay != nullptr) {
        return;
    }
    push_document(std::make_unique<OnlineOverlay>(), true, false);
}

} // namespace mppm::ui
