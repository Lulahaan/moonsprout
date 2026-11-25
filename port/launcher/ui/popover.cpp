#include "popover.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>

namespace mppm::ui {
namespace {

const Rml::String kDocumentSource = R"RML(
<rml>
<head>
    <link type="text/rcss" href="res/rml/theme.rcss" />
    <link type="text/rcss" href="res/rml/controls.rcss" />
    <link type="text/rcss" href="res/rml/popover.rcss" />
</head>
<body>
    <popover id="popover"/>
</body>
</rml>
)RML";

constexpr float kAnchorGapDp = 8.0f;
constexpr float kViewportMarginDp = 8.0f;

}  // namespace

Popover::Popover(Rml::Element* anchor, Side side, const Rml::String& windowClass)
    : Document{kDocumentSource}, mAnchor{anchor->GetObserverPtr()}, mSide{side},
      mBody{mDocument->GetElementById("popover")} {
    if (!windowClass.empty()) {
        mBody->SetClass(windowClass, true);
    }

    listen(mBody, Rml::EventId::Transitionend, [this](Rml::Event& event) {
        if (event.GetTargetElement() == mBody && !mBody->HasAttribute("open") &&
            Document::visible())
        {
            Document::hide(mPendingClose);
        }
    });

    listen(
        mDocument->GetContext()->GetRootElement(), Rml::EventId::Mousedown,
        [this](const Rml::Event& event) {
            const auto within = [](const Rml::Element* element, const Rml::Element* container) {
                while (element != nullptr && element != container) {
                    element = element->GetParentNode();
                }
                return element != nullptr;
            };
            if (visible() && !within(event.GetTargetElement(), mBody)) {
                dismiss();
            }
        },
        true);

    listen(
        mBody, Rml::EventId::Mousemove,
        [this](const Rml::Event& event) {
            if (!mHoverHold) {
                return;
            }
            const Rml::Vector2f at(event.GetParameter("mouse_x", 0.0f), event.GetParameter("mouse_y", 0.0f));
            if (!mHoverAnchorSet) {
                mHoverAnchorSet = true;
                mHoverAnchor = at;
                return;
            }
            const Rml::Vector2f d = at - mHoverAnchor;
            if (d.x * d.x + d.y * d.y >= 24.0f * 24.0f) {
                mHoverHold = false;
                mBody->SetClass("pointer-idle", false);
            }
        },
        true);
}

Popover::~Popover() {
    notify_close(false);
}

void Popover::show() {
    Document::show();
    mHoverHold = true;
    mHoverAnchorSet = false;
    mBody->SetClass("pointer-idle", true);
    reposition();
    mBody->SetAttribute("open", "");
    focus();
}

void Popover::hide(bool close) {
    notify_close(true);
    mBody->RemoveAttribute("open");
    mPendingClose = close;
}

bool Popover::focus() {
    if (mOnFocus && mOnFocus()) {
        return true;
    }
    return mBody != nullptr && mBody->Focus(true);
}

bool Popover::visible() const {
    return mBody != nullptr && mBody->HasAttribute("open");
}

void Popover::update() {
    Document::update();
    if (!visible()) {
        return;
    }
    if (mAnchor == nullptr || !mAnchor->IsVisible(true)) {
        dismiss();
        return;
    }
    reposition();
}

void Popover::dismiss(bool restoreFocus) {
    if (visible()) {
        if (!restoreFocus) {
            mAnchor = nullptr;
        }
        hide(true);
    }
}

bool Popover::handle_nav_command(Rml::Event&, NavCommand cmd) {
    if (cmd == NavCommand::Cancel || cmd == NavCommand::Menu) {
        dismiss();
        return true;
    }
    return false;
}

void Popover::reposition() {
    if (!mAnchor) {
        return;
    }
    auto* context = mDocument->GetContext();
    const auto dimensions = Rml::Vector2f{context->GetDimensions()};
    const float dpRatio = context->GetDensityIndependentPixelRatio();
    const float gap = kAnchorGapDp * dpRatio;
    const float margin = kViewportMarginDp * dpRatio;

    const Rml::Vector2f anchorPos = mAnchor->GetAbsoluteOffset(Rml::BoxArea::Border);
    const Rml::Vector2f anchorSize = mAnchor->GetBox().GetSize(Rml::BoxArea::Border);

    // The popover stays inside the window its anchor sits in (a dropdown never hangs out of the
    // settings frame), else inside the viewport.
    Rml::Vector2f boundsMin{margin, margin};
    Rml::Vector2f boundsMax{dimensions.x - margin, dimensions.y - margin};
    // Vertically a list opening from a row in a scrolling list (the settings rows) stays within
    // that list: it never covers the window's tab bar or title, nor its footer.
    std::optional<std::pair<float, float>> listBounds;
    for (Rml::Element* el = mAnchor->GetParentNode(); el != nullptr; el = el->GetParentNode()) {
        if (!listBounds && (mSide == Side::Below || mSide == Side::Above) && !mPosition) {
            const auto overflow = el->GetComputedValues().overflow_y();
            if (overflow == Rml::Style::Overflow::Auto || overflow == Rml::Style::Overflow::Scroll) {
                const float top = el->GetAbsoluteOffset(Rml::BoxArea::Padding).y;
                listBounds = {top, top + el->GetClientHeight()};
            }
        }
        if (el->GetTagName() == "window") {
            const Rml::Vector2f winPos = el->GetAbsoluteOffset(Rml::BoxArea::Border);
            const Rml::Vector2f winSize = el->GetBox().GetSize(Rml::BoxArea::Border);
            boundsMin = {std::max(boundsMin.x, winPos.x + margin), std::max(boundsMin.y, winPos.y + margin)};
            boundsMax = {std::min(boundsMax.x, winPos.x + winSize.x - margin),
                std::min(boundsMax.y, winPos.y + winSize.y - margin)};
            break;
        }
    }
    if (listBounds) {
        const float listMin = std::max(boundsMin.y, listBounds->first);
        const float listMax = std::min(boundsMax.y, listBounds->second);
        const float roomBelow = listMax - (anchorPos.y + anchorSize.y + gap);
        const float roomAbove = (anchorPos.y - gap) - listMin;
        // A list too short to hold a few items keeps the window as its bounds.
        if (std::max(roomBelow, roomAbove) >= 160.0f * dpRatio) {
            boundsMin.y = listMin;
            boundsMax.y = listMax;
        }
    }

    // Below / Above: open on the side with room and never over the anchor itself. The side is
    // chosen once, from the popover's natural height, so it does not flip back and forth after
    // the height is limited to the room on that side (the list then scrolls).
    if (!mPosition && (mSide == Side::Below || mSide == Side::Above)) {
        const float roomBelow = boundsMax.y - (anchorPos.y + anchorSize.y + gap);
        const float roomAbove = (anchorPos.y - gap) - boundsMin.y;
        if (!mFlipDecided) {
            const float natural = mBody->GetBox().GetSize(Rml::BoxArea::Border).y;
            if (natural <= 0.0f) {
                return; // not laid out yet
            }
            const bool preferBelow = mSide == Side::Below;
            const float preferred = preferBelow ? roomBelow : roomAbove;
            const float other = preferBelow ? roomAbove : roomBelow;
            mOpenAbove = natural <= preferred ? !preferBelow
                       : natural <= other     ? preferBelow
                       : (other > preferred ? preferBelow : !preferBelow);
            mFlipDecided = true;
            mBody->SetClass("above", mOpenAbove);
        }
        float room = std::max(mOpenAbove ? roomAbove : roomBelow, 48.0f * dpRatio);
        room = whole_items_height(room);
        if (room != mMaxHeight) {
            mMaxHeight = room;
            mBody->SetProperty(Rml::PropertyId::MaxHeight, Rml::Property{room, Rml::Unit::PX});
        }
    }

    const Rml::Vector2f size = mBody->GetBox().GetSize(Rml::BoxArea::Border);
    Rml::Vector2f pos;
    switch (mSide) {
    case Side::Left:
        pos = {anchorPos.x - size.x - gap, anchorPos.y + (anchorSize.y - size.y) * 0.5f};
        break;
    case Side::Right:
        pos = {anchorPos.x + anchorSize.x + gap, anchorPos.y + (anchorSize.y - size.y) * 0.5f};
        break;
    case Side::Above:
    case Side::Below:
    default:
        pos = {anchorPos.x + (anchorSize.x - size.x) * 0.5f,
            mOpenAbove ? anchorPos.y - size.y - gap : anchorPos.y + anchorSize.y + gap};
        break;
    }

    if (mPosition) {
        pos = *mPosition;
    }
    pos.x = std::clamp(pos.x, boundsMin.x, std::max(boundsMin.x, boundsMax.x - size.x));
    pos.y = std::clamp(pos.y, boundsMin.y, std::max(boundsMin.y, boundsMax.y - size.y));
    mBody->SetProperty(Rml::PropertyId::Left, Rml::Property{pos.x, Rml::Unit::PX});
    mBody->SetProperty(Rml::PropertyId::Top, Rml::Property{pos.y, Rml::Unit::PX});
}

float Popover::whole_items_height(float room) const {
    // Keep only whole items inside the frame (padding and border; max-height is a border-box
    // height here, global.rcss), so the last visible one is never cut in half (the list scrolls
    // by whole items with the focus).
    const Rml::Box& box = mBody->GetBox();
    const float frame = box.GetEdge(Rml::BoxArea::Padding, Rml::BoxEdge::Top) +
                        box.GetEdge(Rml::BoxArea::Padding, Rml::BoxEdge::Bottom) +
                        box.GetEdge(Rml::BoxArea::Border, Rml::BoxEdge::Top) +
                        box.GetEdge(Rml::BoxArea::Border, Rml::BoxEdge::Bottom);
    const float content = room - frame;
    if (mBody->GetNumChildren() < 2) {
        return room;
    }
    Rml::Element* first = mBody->GetChild(0);
    Rml::Element* second = mBody->GetChild(1);
    const float itemHeight = first->GetBox().GetSize(Rml::BoxArea::Border).y;
    const float step = second->GetOffsetTop() - first->GetOffsetTop();
    if (itemHeight <= 0.0f || step < itemHeight) {
        return room;
    }
    const float total = mBody->GetScrollHeight() - box.GetEdge(Rml::BoxArea::Padding, Rml::BoxEdge::Top) -
                        box.GetEdge(Rml::BoxArea::Padding, Rml::BoxEdge::Bottom);
    if (total <= content) {
        return room; // everything fits
    }
    const float items = std::floor((content - itemHeight) / step) + 1.0f;
    return items >= 1.0f ? frame + (items - 1.0f) * step + itemHeight : room;
}

void Popover::notify_close(bool restoreFocus) {
    if (mOnClose) {
        auto callback = std::move(mOnClose);
        mOnClose = nullptr;
        callback();
    }
    if (restoreFocus && mAnchor != nullptr && mAnchor->IsVisible(true)) {
        mAnchor->Focus(true);
    }
}

}  // namespace mppm::ui
