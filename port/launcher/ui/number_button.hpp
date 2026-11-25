#pragma once

#include "string_button.hpp"

#include <chrono>
#include <climits>

namespace mppm::ui {

class NumberButton : public BaseStringButton {
public:
    struct Props {
        Rml::String key;
        std::function<int()> getValue;
        std::function<void(int)> setValue;
        std::function<bool()> isDisabled;
        std::function<bool()> isModified;
        int min = 0;
        int max = INT_MAX;
        int step = 1;
        Rml::String prefix;
        Rml::String suffix;
    };

    NumberButton(Rml::Element* parent, Props props);

    void update() override;
    bool modified() const override;
    bool disabled() const override;

protected:
    Rml::String format_value() override;
    Rml::String input_value() override;
    void set_value(Rml::String value) override;
    bool handle_nav_command(NavCommand cmd) override;

private:
    bool has_meter() const { return mMeter != nullptr; }
    void sync_meter();
    void set_from_pointer(float mouseX);
    void step_to(int value);

    std::function<int()> mGetValue;
    std::function<void(int)> mSetValue;
    std::function<bool()> mIsDisabled;
    std::function<bool()> mIsModified;
    int mMin;
    int mMax;
    int mStep;
    Rml::String mPrefix;
    Rml::String mSuffix;

    // The slider (DESIGN.md 5.6), present only for a finite range.
    Rml::Element* mMeter = nullptr;
    Rml::Element* mFill = nullptr;
    Rml::Element* mKnob = nullptr;
    int mShownValue = INT_MIN;
    std::chrono::steady_clock::time_point mStepEnd{};
};

}  // namespace mppm::ui