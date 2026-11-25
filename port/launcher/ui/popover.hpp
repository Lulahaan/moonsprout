#pragma once

#include "document.hpp"

#include <optional>

namespace mppm::ui {

class Popover : public Document {
public:
    enum class Side {
        Below,
        Above,
        Left,
        Right,
    };

    Popover(Rml::Element* anchor, Side side, const Rml::String& windowClass = "");
    ~Popover() override;

    void show() override;
    void hide(bool close) override;
    bool focus() override;
    bool visible() const override;
    void update() override;

    Rml::Element* body() const { return mBody; }

    void set_position(Rml::Vector2f position) { mPosition = position; }

    void dismiss(bool restoreFocus = true);
    void on_close(std::function<void()> callback) { mOnClose = std::move(callback); }
    void on_focus(std::function<bool()> callback) { mOnFocus = std::move(callback); }

protected:
    bool handle_nav_command(Rml::Event& event, NavCommand cmd) override;

private:
    void reposition();
    // The tallest border-box height up to `room` that shows only whole items.
    float whole_items_height(float room) const;
    void notify_close(bool restoreFocus);

    Rml::ObserverPtr<Rml::Element> mAnchor;
    std::optional<Rml::Vector2f> mPosition;
    Side mSide;
    bool mFlipDecided = false;
    bool mOpenAbove = false;
    float mMaxHeight = -1.0f;
    // Until the pointer really moves, an item laid out under the resting pointer must not look
    // hovered next to the focused one (two rings read as two selections).
    bool mHoverHold = false;
    bool mHoverAnchorSet = false;
    Rml::Vector2f mHoverAnchor;
    Rml::Element* mBody = nullptr;
    std::function<void()> mOnClose;
    std::function<bool()> mOnFocus;
};

}  // namespace mppm::ui
