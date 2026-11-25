#pragma once
// A text row whose shown value can differ from the text being edited (DESIGN 5.8): a placeholder
// while empty, a mask (passwords) or a formatted form (join codes). Editing always starts from the
// raw value. `invalid` draws the error ring of DESIGN 5.8 while it holds.

#include "string_button.hpp"

#include <functional>

namespace mppm::ui {

class TextField : public BaseStringButton {
public:
    struct Props {
        Rml::String key;
        std::function<Rml::String()> getValue;
        std::function<void(Rml::String)> setValue;
        // The shown value for a non-empty raw one (unset: the raw value). Empty shows the placeholder.
        std::function<Rml::String(const Rml::String&)> display;
        Rml::String placeholder;
        std::function<bool()> isDisabled;
        std::function<bool()> isModified;
        std::function<bool()> isInvalid;
        int maxLength = -1;
        bool setOnChange = false; // setValue on every keystroke (Esc puts the old value back)
    };

    TextField(Rml::Element* parent, Props props);

    void update() override;
    bool modified() const override;
    bool disabled() const override;

protected:
    Rml::String format_value() override;
    Rml::String input_value() override { return mProps.getValue ? mProps.getValue() : Rml::String{}; }
    void set_value(Rml::String value) override {
        if (mProps.setValue) {
            mProps.setValue(std::move(value));
        }
    }

private:
    Props mProps;
    int mPlaceholderShown = -1;
    int mInvalidShown = -1;
};

}  // namespace mppm::ui
