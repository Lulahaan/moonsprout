#pragma once
// The first-run preset screen (docs/launcher/SETTINGS.md 12, README 6; model: Dusklight's
// src/dusk/ui/preset.cpp).
//
// On the first start the player picks how Pikmin 2 should look: Modern (fills wide screens, HUD at
// the edges, sharper picture) or Original (4:3 and GameCube-faithful). A preset only sets the keys
// in its table, never cheats, gameplay or experimental options. One toggle under the cards asks for
// update-check consent.

#include "window.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace mppm::ui {

class PresetWindow : public WindowSmall {
public:
    // firstRun: the screen can't be dismissed without a choice, and when the choice changed a
    // restart-only setting the app restarts at once, silently (nobody is playing yet). Reopened
    // from TOOLS > Choose Preset, Back closes it and the settings window's restart banner applies.
    explicit PresetWindow(bool firstRun);

    void update() override;
    bool focus() override;

protected:
    bool handle_nav_command(Rml::Event& event, NavCommand cmd) override;

private:
    void choose(int index);
    void close_with_scrim();
    void refresh_hints();
    int focused_card() const;

    bool mFirstRun;
    int mLastCard = -1; // focus memory
    Rml::Element* mHints = nullptr;
    bool mHintsBuilt = false;
    bool mHintsPad   = false;
    std::vector<std::unique_ptr<Component>> mCards;
    std::unique_ptr<Component> mUpdateToggle;
    int mPendingChoice = -1; // applied from update(), outside the button's event handler
    uint64_t mOpenedAt = 0;  // SDL_GetTicks when the card opened: a press too soon after is ignored
    uint64_t mLastEarlyPress = 0; // the last ignored press: one soon after it is a mash, ignored too
};

// The two presets, for the window and for TOOLS > Choose Preset.
enum class Preset { Original, Modern };
void apply_preset(Preset preset);

// Shows the preset screen when interface.presetChosen is false. The start menu calls it once its
// own document is up, so the screen sits on top of it and takes focus first. Returns true when the
// screen was shown.
bool show_first_run_preset_if_needed();

}  // namespace mppm::ui
