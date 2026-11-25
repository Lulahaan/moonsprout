#include "bool_button.hpp"

#include "sound.hpp"

namespace mppm::ui {

BoolButton::BoolButton(Rml::Element* parent, Props props)
    : BaseControlledSelectButton(parent,
          {
              .key = std::move(props.key),
              .icon = std::move(props.icon),
          }),
      mGetValue(std::move(props.getValue)), mSetValue(std::move(props.setValue)),
      mIsDisabled(std::move(props.isDisabled)), mIsModified(std::move(props.isModified)),
      mValueOverride(std::move(props.valueOverride)) {
    // DESIGN.md 5.5: the value text stays, and a switch follows it, so the state is told by text,
    // position, colour and shape. The `on` attribute on the row drives the switch's styles.
    mRoot->SetClass("bool-button", true);
    auto* toggle = append(mRoot, "toggle");
    append(toggle, "track-fill");
    auto* knob = append(toggle, "knob");
    append(knob, "leaf")->SetClass("l", true);
    append(knob, "leaf")->SetClass("r", true);
    // Set before the first layout so a window opens with the switch already in place.
    sync_state();
}

void BoolButton::update() {
    sync_state();
    BaseControlledSelectButton::update();
}

void BoolButton::sync_state() {
    const bool on = mGetValue && mGetValue();
    if (mRoot->HasAttribute("on") == on) {
        return;
    }
    if (on) {
        mRoot->SetAttribute("on", "");
    } else {
        mRoot->RemoveAttribute("on");
    }
}

bool BoolButton::modified() const {
    if (mIsModified) {
        return mIsModified();
    }
    return BaseControlledSelectButton::modified();
}

bool BoolButton::disabled() const {
    if (mIsDisabled) {
        return mIsDisabled();
    }
    return BaseControlledSelectButton::disabled();
}

Rml::String BoolButton::format_value() {
    if (mValueOverride) {
        if (std::string value = mValueOverride(); !value.empty()) {
            return value;
        }
    }

    return mGetValue && mGetValue() ? "On" : "Off";
}

bool BoolButton::handle_nav_command(NavCommand cmd) {
    // Confirm flips the switch; the arrows move the knob: Left turns it off, Right turns it on.
    // An arrow towards the side the knob is already on is not ours, so it can move focus (Right
    // on an On switch reaches the help pane's buttons).
    bool newValue = false;
    if (cmd == NavCommand::Confirm) {
        newValue = !mGetValue();
    } else if (cmd == NavCommand::Left || cmd == NavCommand::Right) {
        newValue = cmd == NavCommand::Right;
        if (newValue == mGetValue()) {
            return false;
        }
    } else {
        return false;
    }
    mSetValue(newValue);
    play_sound(newValue ? kSoundItemEnable : kSoundItemDisable);
    return true;
}

}  // namespace mppm::ui