#pragma once
// Bindings from settings vars to controls (docs/launcher/SETTINGS.md 1.5).
//
// A copy of borealis/ui/config.hpp (MIT, extern/borealis) retargeted to the launcher's own
// controls in mppm::ui. Each helper fills the props' unset getValue, setValue and isModified from
// the var, and disables the control while an overlay locks the var (the environment or netplay
// layer, VarBase::locked()), in addition to any isDisabled the caller supplies:
//
//     pane.add_child<BoolButton>(bind(settings().video.vsync, {.key = "VSync"}));
//
// Choices (bind_choices) are for enum vars and for int vars with a fixed set of values, such as
// Texture Filtering's 1, 2, 4, 8, 16: the dropdown lists exactly the given values, in order, so a
// value can be left out (not offered yet) or relabelled without touching the enum table.

#include "bool_button.hpp"
#include "dropdown_button.hpp"
#include "number_button.hpp"
#include "string_button.hpp"

#include <borealis/config.hpp>

#include <algorithm>
#include <climits>
#include <concepts>
#include <functional>
#include <utility>
#include <vector>

namespace mppm::ui {
namespace detail {

inline std::function<bool()> config_disabled(
    const borealis::config::VarBase& var, std::function<bool()> isDisabled) {
    return [&var, isDisabled = std::move(isDisabled)] {
        return var.locked() || (isDisabled && isDisabled());
    };
}

template <typename Props, typename T>
void bind_common(borealis::config::Var<T>& var, Props& props) {
    if (!props.isModified) {
        props.isModified = [&var] { return var.modified(); };
    }
    props.isDisabled = config_disabled(var, std::move(props.isDisabled));
}

}  // namespace detail

inline BoolButton::Props bind(borealis::config::Var<bool>& var, BoolButton::Props props = {}) {
    if (!props.getValue) {
        props.getValue = [&var] { return var.get(); };
    }
    if (!props.setValue) {
        props.setValue = [&var](bool value) { var.set(value); };
    }
    detail::bind_common(var, props);
    return props;
}

// Copies the var's min and max into the props when the var defines them.
template <std::integral T>
    requires(!std::same_as<T, bool>)
NumberButton::Props bind(borealis::config::Var<T>& var, NumberButton::Props props = {}) {
    const auto clampToInt = [](auto value) {
        return static_cast<int>(
            std::clamp<long long>(static_cast<long long>(value), INT_MIN, INT_MAX));
    };
    if (!props.getValue) {
        props.getValue = [&var, clampToInt] { return clampToInt(var.get()); };
    }
    if (!props.setValue) {
        props.setValue = [&var](int value) { var.set(static_cast<T>(value)); };
    }
    if (const auto& min = var.options().min) {
        props.min = clampToInt(*min);
    }
    if (const auto& max = var.options().max) {
        props.max = clampToInt(*max);
    }
    detail::bind_common(var, props);
    return props;
}

inline StringButton::Props bind(
    borealis::config::Var<std::string>& var, StringButton::Props props = {}) {
    if (!props.getValue) {
        props.getValue = [&var] { return Rml::String{var.get()}; };
    }
    if (!props.setValue) {
        props.setValue = [&var](Rml::String value) { var.set(std::move(value)); };
    }
    detail::bind_common(var, props);
    return props;
}

// One entry of a choice list: the stored value and its label.
template <typename T>
struct Choice {
    T value;
    Rml::String label;
    bool enabled = true; // listed but not selectable (e.g. "not available yet")
};

// A dropdown over a fixed list of values. A current value that is not in the list (set by hand in
// config.json, or by an override) shows as no selection rather than as a wrong label.
template <typename T>
DropdownButton::Props bind_choices(
    borealis::config::Var<T>& var, std::vector<Choice<T>> choices, DropdownButton::Props props) {
    props.options.clear();
    props.options.reserve(choices.size());
    std::vector<T> values;
    values.reserve(choices.size());
    for (auto& choice : choices) {
        props.options.push_back({.text = std::move(choice.label), .enabled = choice.enabled});
        values.push_back(choice.value);
    }
    if (!props.getValue) {
        props.getValue = [&var, values] {
            const auto it = std::ranges::find(values, var.get());
            return it != values.end() ? static_cast<int>(it - values.begin()) : -1;
        };
    }
    if (!props.setValue) {
        props.setValue = [&var, values](int index) {
            if (index >= 0 && static_cast<std::size_t>(index) < values.size()) {
                var.set(values[static_cast<std::size_t>(index)]);
            }
        };
    }
    detail::bind_common(var, props);
    return props;
}

// Options are listed in the order of the enum's config_enum_values table; `labels` gives one label
// per table entry, in that order.
template <borealis::config::NamedEnum E>
DropdownButton::Props bind_dropdown(borealis::config::Var<E>& var,
    std::initializer_list<const char*> labels, DropdownButton::Props props) {
    static constexpr auto table = borealis::config::enum_values<E>();
    std::vector<Choice<E>> choices;
    std::size_t i = 0;
    for (const char* label : labels) {
        if (i >= table.size()) {
            break;
        }
        choices.push_back({.value = table.entries[i++].first, .label = label});
    }
    return bind_choices(var, std::move(choices), std::move(props));
}

}  // namespace mppm::ui
