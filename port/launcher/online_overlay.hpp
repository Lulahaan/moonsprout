#pragma once
// The in-game wait overlay of an online match (M5.3b design 10.3, SETTINGS 18): a glass pill at the
// top centre while a tick or a load barrier waits for someone, polled from mppm::online::stall()
// every frame:
//
//   "Waiting for Louie (P2)... 6 s"   (several: "Waiting for Louie and Navi..."; a client whose host
//   is late: "Waiting for the host..."; a load barrier: "Waiting for players to load...")
//
// with the turning leaf (DESIGN 5.15), the seconds once the wait passes 3 s, and, when the session
// allows it, one button: "Drop Louie" (host, Stall::canDrop, danger confirmation, drop_slot) or
// "Leave game" (client, Stall::canLeave, confirmation, leave and back to the start menu).
//
// The document sits in the focus stack just above the in-game bar, but counts as active and visible
// only while it offers a button and nothing else is open: then it takes the focus (A presses the
// button, B gives the focus back to the game until the button changes) and the game sees no pad
// input (PADBlockInput, ui::any_document_visible). Otherwise the pill is only drawn: the bar,
// windows and modals open over it as usual. The session's hold loop keeps calling
// launcher::update(), so the pill animates and takes input while the game waits.

#include "ui/button.hpp"
#include "ui/document.hpp"

#include <memory>
#include <string>
#include <vector>

namespace mppm::ui {

class OnlineOverlay final : public Document {
public:
    OnlineOverlay();
    ~OnlineOverlay() override;

    void update() override;
    bool focus() override;
    // In the focus stack only while the button wants the focus (see above).
    bool visible() const override { return mWantFocus; }
    bool active() const override { return Document::active() && mWantFocus; }
    // Lives as long as the UI (close_all_documents keeps it).
    bool permanent() const override { return true; }
    // Never hidden by a window pushed over it: the pill stays on screen.
    void cover() override {}
    void uncover() override {}
    void hide(bool close) override;

protected:
    bool handle_nav_command(Rml::Event& event, NavCommand cmd) override;

private:
    enum class Action { None, Drop, Leave };
    void set_action(Action action, const std::string& label);
    void press();
    bool others_visible() const;

    Rml::Element* mPill = nullptr;
    Rml::Element* mText = nullptr;
    Rml::Element* mSeconds = nullptr;
    std::unique_ptr<Button> mButton;
    Action mAction = Action::None;
    std::string mActionLabel;
    std::string mDropName;  // "Louie": the first waited-for name without " (P2)"
    int mDropSlot = -1;
    std::string mShownText;
    std::string mShownSeconds;
    bool mOpen = false;
    bool mWantFocus = false;
    bool mDeclined = false; // B gave the focus back; until the button changes
    bool mBarOpen = false;
};

// Creates the overlay (once), above the in-game bar in the focus stack.
void install_online_overlay();

} // namespace mppm::ui
