#include "text_field.hpp"

#include <utility>

namespace mppm::ui {

TextField::TextField(Rml::Element* parent, Props props)
    : BaseStringButton(parent,
          {
              .key = props.key,
              .maxLength = props.maxLength,
              .setOnChange = props.setOnChange,
          }),
      mProps(std::move(props)) {
    mRoot->SetClass("text-field", true);
    update();
}

void TextField::update() {
    const bool placeholder = (mProps.getValue ? mProps.getValue() : Rml::String{}).empty();
    if (static_cast<int>(placeholder) != mPlaceholderShown) {
        mPlaceholderShown = placeholder;
        mValueElem->SetClass("placeholder", placeholder);
    }
    const bool invalid = mProps.isInvalid && mProps.isInvalid();
    if (static_cast<int>(invalid) != mInvalidShown) {
        mInvalidShown = invalid;
        mRoot->SetClass("invalid", invalid);
    }
    BaseStringButton::update();
}

Rml::String TextField::format_value() {
    const Rml::String raw = mProps.getValue ? mProps.getValue() : Rml::String{};
    if (raw.empty()) {
        return mProps.placeholder;
    }
    return mProps.display ? mProps.display(raw) : raw;
}

bool TextField::modified() const {
    return mProps.isModified && mProps.isModified();
}

bool TextField::disabled() const {
    return mProps.isDisabled && mProps.isDisabled();
}

}  // namespace mppm::ui
