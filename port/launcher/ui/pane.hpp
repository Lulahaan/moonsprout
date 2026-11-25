#pragma once

#include "button.hpp"
#include "component.hpp"
#include "group_button.hpp"
#include "select_button.hpp"

namespace mppm::ui {

class Pane : public FluentComponent<Pane> {
public:
    enum class Type {
        Controlled,
        Uncontrolled,
    };

    explicit Pane(Rml::Element* parent, Type type);

    bool focus() override;
    bool focus_last();
    // Let disabled children take focus too (they still ignore input), so a help pane next to
    // this one can explain why they are disabled.
    void set_focus_disabled(bool value) { mFocusDisabled = value; }

    void set_selected_item(int index);
    Component& register_control(
        Component& component, Pane& nextPane, std::function<void(Pane&)> callback);

    ControlledButton& add_button(ControlledButton::Props props) {
        return add_child<ControlledButton>(std::move(props));
    }
    GroupButton& add_group_button(GroupButton::Props props) {
        return add_child<GroupButton>(std::move(props));
    }
    Button& add_button(Rml::String text) { return add_child<Button>(std::move(text)); }
    ControlledSelectButton& add_select_button(ControlledSelectButton::Props props) {
        return add_child<ControlledSelectButton>(std::move(props));
    }
    void clear();

private:
    bool focus_child(Component& child, NavCommand direction);

    Type mType;
    bool mFocusDisabled = false;
};

}  // namespace mppm::ui
