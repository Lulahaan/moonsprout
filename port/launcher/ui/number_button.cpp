#include "number_button.hpp"

#include "sound.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <fmt/format.h>

namespace mppm::ui {
namespace {

// How long the pellet stays bumped after a step before it settles back.
constexpr auto kStepBump = std::chrono::milliseconds(90);

}  // namespace

NumberButton::NumberButton(Rml::Element* parent, Props props)
    : BaseStringButton(parent, {.key = std::move(props.key), .type = "number"}),
      mGetValue(std::move(props.getValue)), mSetValue(std::move(props.setValue)),
      mIsDisabled(std::move(props.isDisabled)), mIsModified(std::move(props.isModified)),
      mMin(props.min), mMax(props.max), mStep(props.step), mPrefix(std::move(props.prefix)),
      mSuffix(std::move(props.suffix)) {
    // A finite range gets a meter between the name and the value:
    // <meter><chev.l/><fill/><knob/><chev.r/></meter>. Unbounded numbers stay a plain row.
    if (mMax < INT_MAX && mMax > mMin) {
        mRoot->SetClass("number-button", true);
        auto meter = mRoot->GetOwnerDocument()->CreateElement("meter");
        mMeter = mRoot->InsertBefore(std::move(meter), mValueElem);
        append(mMeter, "chev")->SetClass("l", true);
        mFill = append(mMeter, "fill");
        mKnob = append(mMeter, "knob");
        append(mMeter, "chev")->SetClass("r", true);

        // Mouse: a press on the track jumps there, dragging follows the pointer. The press must
        // propagate (RmlUi starts a drag only for an unstopped mousedown); the click stops here
        // so it does not reach the row, which would start text entry.
        Component::listen(mMeter, Rml::EventId::Mousedown, [this](Rml::Event& event) {
            if (!disabled() && !is_editing()) {
                mRoot->Focus(true);
                set_from_pointer(event.GetParameter("mouse_x", 0.0f));
            }
        });
        Component::listen(mMeter, Rml::EventId::Dragstart, [this](Rml::Event&) {
            mMeter->SetClass("dragging", true);
        });
        Component::listen(mMeter, Rml::EventId::Drag, [this](Rml::Event& event) {
            if (!disabled() && !is_editing()) {
                set_from_pointer(event.GetParameter("mouse_x", 0.0f));
            }
        });
        Component::listen(mMeter, Rml::EventId::Dragend, [this](Rml::Event&) {
            mMeter->SetClass("dragging", false);
        });
        Component::listen(mMeter, Rml::EventId::Click, [](Rml::Event& event) { event.StopPropagation(); });
        sync_meter();
    }
}

void NumberButton::update() {
    if (has_meter()) {
        sync_meter();
        if (mMeter->IsClassSet("stepped") && std::chrono::steady_clock::now() >= mStepEnd) {
            mMeter->SetClass("stepped", false);
        }
    }
    BaseStringButton::update();
}

void NumberButton::sync_meter() {
    const int value = mGetValue ? mGetValue() : mMin;
    if (value == mShownValue) {
        return;
    }
    mShownValue = value;
    const double t =
        static_cast<double>(std::clamp(value, mMin, mMax) - mMin) / static_cast<double>(mMax - mMin);
    const float percent = static_cast<float>(t * 100.0);
    mFill->SetProperty(Rml::PropertyId::Width, Rml::Property{percent, Rml::Unit::PERCENT});
    mKnob->SetProperty(Rml::PropertyId::Left, Rml::Property{percent, Rml::Unit::PERCENT});
}

void NumberButton::set_from_pointer(float mouseX) {
    const float left = mMeter->GetAbsoluteOffset(Rml::BoxArea::Border).x;
    const float width = mMeter->GetBox().GetSize(Rml::BoxArea::Border).x;
    if (width <= 0.0f || !mSetValue) {
        return;
    }
    const double t = std::clamp(static_cast<double>((mouseX - left) / width), 0.0, 1.0);
    // Snap to the step grid, counted from the minimum.
    const double steps = std::round(t * (mMax - mMin) / std::max(mStep, 1));
    const int value = std::clamp(mMin + static_cast<int>(steps) * std::max(mStep, 1), mMin, mMax);
    if (value != mGetValue()) {
        mSetValue(value);
        play_sound(kSoundItemChange);
    }
}

void NumberButton::step_to(int value) {
    if (!mSetValue || value == mGetValue()) {
        return;
    }
    mSetValue(value);
    play_sound(kSoundItemChange);
    if (has_meter()) {
        // The pellet bumps up for a beat, then settles to its focused size.
        mMeter->SetClass("stepped", true);
        mStepEnd = std::chrono::steady_clock::now() + kStepBump;
    }
}

bool NumberButton::modified() const {
    if (mIsModified) {
        return mIsModified();
    }
    return BaseStringButton::modified();
}

bool NumberButton::disabled() const {
    if (mIsDisabled) {
        return mIsDisabled();
    }
    return BaseStringButton::disabled();
}

Rml::String NumberButton::format_value() {
    return fmt::format("{}{}{}", mPrefix, mGetValue(), mSuffix);
}

Rml::String NumberButton::input_value() {
    return fmt::to_string(mGetValue());
}

void NumberButton::set_value(Rml::String value) {
    if (!mSetValue) {
        return;
    }

    int parsedValue = 0;
    const char* begin = value.data();
    const char* end = begin + value.size();
    const auto result = std::from_chars(begin, end, parsedValue);
    if (result.ec != std::errc() || result.ptr != end) {
        return;
    }

    mSetValue(std::clamp(parsedValue, mMin, mMax));
}

bool NumberButton::handle_nav_command(NavCommand cmd) {
    if (!is_editing() && (cmd == NavCommand::Left || cmd == NavCommand::Right)) {
        const int value = mGetValue();
        const int next = std::clamp(value + (cmd == NavCommand::Right ? mStep : -mStep), mMin, mMax);
        // Left / Right are always the adjust keys here, also at the ends of the range: holding
        // Right to the maximum must not run on into the help pane's "Reset to Default", where the
        // next Confirm would undo it (as in Dusklight).
        step_to(next);
        return true;
    }
    return BaseStringButton::handle_nav_command(cmd);
}

}  // namespace mppm::ui
