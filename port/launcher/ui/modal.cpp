#include "modal.hpp"

#include <algorithm>
#include <utility>

namespace mppm::ui {

Modal::Modal(Props props) : WindowSmall("modal"), mProps(std::move(props)) {
    if (!mProps.variant.empty()) {
        mRoot->SetClass(mProps.variant, true);
    }

    auto* header = append(mDialog, "modal-header");

    auto* title = append(header, "modal-title");
    append_text(title, mProps.title);

    if (!mProps.icon.empty()) {
        auto* icon = append(header, "icon");
        icon->SetClass(mProps.icon, true);
    }

    auto* body = append(mDialog, "modal-body");
    if (mProps.bodyText) {
        append_text(body, *mProps.bodyText);
    } else {
        body->SetInnerRML(mProps.bodyRml);
    }

    mContentRoot = append(mDialog, "modal-content");

    auto* actions = append(mDialog, "modal-actions");
    if (props.isVertical) {
        actions->SetClass("vertical", true);
    }

    for (auto& action : mProps.actions) {
        add_action(std::move(action));
    }

    // Keys RmlUi handles itself (Tab, Space) after a click left nothing focused: give focus back
    // to the default button first, as the direction keys do (handle_nav_command).
    listen(nullptr, Rml::EventId::Keydown, [this](Rml::Event& event) {
        const auto key = static_cast<Rml::Input::KeyIdentifier>(
            event.GetParameter<int>("key_identifier", Rml::Input::KI_UNKNOWN));
        if ((key == Rml::Input::KI_TAB || key == Rml::Input::KI_SPACE) &&
            !owns_focus(event.GetTargetElement()) && focus())
        {
            play_sound(kSoundItemFocus);
            event.StopPropagation();
        }
    });

    play_sound(kSoundWindowOpen);
}

bool Modal::owns_focus(Rml::Element* target) const {
    if (target == nullptr) {
        return false;
    }
    if (mContentPane != nullptr && mContentPane->contains(target)) {
        return true;
    }
    return std::ranges::any_of(mButtons, [target](const auto& button) { return button->contains(target); });
}

void Modal::update() {
    if (mContentPane != nullptr) {
        mContentPane->update();
    }
    for (const auto& button : mButtons) {
        button->update();
    }
    if (mPendingAction) {
        auto action = std::exchange(mPendingAction, {});
        action(*this);
    }
    WindowSmall::update();
}

Pane& Modal::content_pane() {
    if (mContentPane == nullptr) {
        mContentRoot->SetClass("active", true);
        mContentPane = std::make_unique<Pane>(mContentRoot, Pane::Type::Uncontrolled);
    }
    return *mContentPane;
}

void Modal::add_action(ModalAction action) {
    auto* actions = mDialog->QuerySelector("modal-actions");
    auto btn = std::make_unique<ControlledButton>(
        actions, ControlledButton::Props{
                     .text = action.icon.empty() ? std::move(action.label) : "",
                     .isDisabled = std::move(action.isDisabled),
                 });
    btn->root()->SetClass("modal-btn", true);
    if (!action.icon.empty()) {
        auto* icon = append(btn->root(), "icon");
        icon->SetClass(action.icon, true);
        append_text(btn->root(), action.label);
    }
    btn->on_pressed([this, callback = std::move(action.onPressed)] {
        if (!callback) {
            return;
        }
        if (mContentPane != nullptr) {
            mPendingAction = callback;
        } else {
            callback(*this);
        }
    });
    mButtons.push_back(std::move(btn));
}

void Modal::set_body(const Rml::String& bodyRml) {
    mDialog->QuerySelector("modal-body")->SetInnerRML(bodyRml);
}

void Modal::set_body_text(const Rml::String& bodyText) {
    set_text_content(mDialog->QuerySelector("modal-body"), bodyText);
}

void Modal::set_icon(const Rml::String& icon) {
    auto* iconElem = mDialog->QuerySelector("icon");
    if (icon.empty()) {
        if (iconElem != nullptr) {
            iconElem->GetParentNode()->RemoveChild(iconElem);
        }
        return;
    }
    if (iconElem == nullptr) {
        // The constructor only creates the icon element when Props.icon is set.
        iconElem = append(mDialog->QuerySelector("modal-header"), "icon");
    }
    iconElem->SetClassNames(icon);
}

bool Modal::focus() {
    if (mContentPane != nullptr && mContentPane->focus()) {
        return true;
    }
    for (const auto& button : mButtons) {
        if (button->focus()) {
            return true;
        }
    }
    return false;
}

void Modal::dismiss() {
    if (mProps.onDismiss) {
        mProps.onDismiss(*this);
        return;
    }
    pop();
}

bool Modal::handle_nav_command(Rml::Event& event, NavCommand cmd) {
    if (cmd == NavCommand::Cancel || cmd == NavCommand::Menu) {
        play_sound(kSoundWindowClose);
        dismiss();
        return true;
    }

    auto* target = event.GetTargetElement();
    // Nothing of ours is focused (a click on the dialog's body or backdrop took focus away): the
    // first key press brings focus back to the default button instead of being lost, as a window
    // does (Window::handle_content_nav). Confirm only refocuses; it never presses blind.
    if (!owns_focus(target)) {
        if (focus()) {
            play_sound(kSoundItemFocus);
        }
        return true;
    }
    if (mContentPane != nullptr && mContentPane->contains(target) && cmd == NavCommand::Down) {
        for (const auto& button : mButtons) {
            if (button->focus()) {
                play_sound(kSoundItemFocus);
                return true;
            }
        }
    }
    if (mContentPane != nullptr && cmd == NavCommand::Up &&
        std::ranges::any_of(
            mButtons, [target](const auto& button) { return button->contains(target); }) &&
        mContentPane->focus_last())
    {
        play_sound(kSoundItemFocus);
        return true;
    }

    int direction = 0;
    NavCommand prevCommand = mProps.isVertical ? NavCommand::Up : NavCommand::Left;
    NavCommand nextCommand = mProps.isVertical ? NavCommand::Down : NavCommand::Right;
    if (cmd == prevCommand) {
        direction = -1;
    } else if (cmd == nextCommand) {
        direction = 1;
    } else {
        return false;
    }

    for (int i = 0; i < static_cast<int>(mButtons.size()); ++i) {
        if (mButtons[i]->contains(target)) {
            for (int next = i + direction; next >= 0 && next < static_cast<int>(mButtons.size());
                next += direction)
            {
                if (mButtons[next]->focus()) {
                    play_sound(kSoundItemFocus);
                    return true;
                }
            }
            return false;
        }
    }
    return false;
}

}  // namespace mppm::ui
