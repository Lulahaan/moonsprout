#include "preset.hpp"

#include "bool_button.hpp"
#include "button.hpp"
#include "config_bind.hpp"
#include "input.hpp"
#include "ui.hpp"

#include "../branding.hpp"
#include "../settings.hpp"

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_timer.h>
#include <fmt/format.h>

#include <RmlUi/Core/FileInterface.h>

#include <algorithm>
#include <optional>

namespace mppm::ui {
namespace {

// The preset table (SETTINGS 12, README 6). Only these keys change; never cheats, gameplay rules or
// experimental options. A key that is locked by an override keeps its locked value.
struct PresetValues {
    AspectRatio aspectRatio;
    HudLayout hudLayout;
    int internalResolution;
    Resampler resampler;
    int anisotropy;
    bool vsync;
    int msaa;
    bool skipBootLogos;
};

constexpr PresetValues kOriginalValues{
    AspectRatio::Original, HudLayout::Centered, 1, Resampler::Bilinear, 1, true, 1, false};
// Modern behaves as Original until the wide model ships (ASPECT.md phase 1), then fills the window.
constexpr PresetValues kModernValues{AspectRatio::Auto, HudLayout::Edges, 0, Resampler::Area, 8, true, 1, true};

const PresetValues& values_of(Preset preset) {
    return preset == Preset::Modern ? kModernValues : kOriginalValues;
}

void apply_values(const PresetValues& v) {
    auto& s = settings();
    s.video.aspectRatio.set(v.aspectRatio);
    s.interface.hudLayout.set(v.hudLayout);
    s.video.internalResolution.set(v.internalResolution);
    s.video.resampler.set(v.resampler);
    s.video.anisotropy.set(v.anisotropy);
    s.video.vsync.set(v.vsync);
    s.video.msaa.set(v.msaa);
    s.game.skipBootLogos.set(v.skipBootLogos);
}

bool matches(const PresetValues& v) {
    const auto& s = settings();
    return s.video.aspectRatio.get() == v.aspectRatio && s.interface.hudLayout.get() == v.hudLayout &&
           s.video.internalResolution.get() == v.internalResolution && s.video.resampler.get() == v.resampler &&
           s.video.anisotropy.get() == v.anisotropy && s.video.vsync.get() == v.vsync &&
           s.video.msaa.get() == v.msaa && s.game.skipBootLogos.get() == v.skipBootLogos;
}

struct PresetInfo {
    Preset preset;
    const char* cls;
    const char* name;
    const char* tag; // both cards carry one, so their blurbs start on the same line
    const char* blurb;
    bool recommended;
};

// The first-run card opens right after the menu's entrance, which a confirm press skips: presses
// this soon after the card appeared, or after an earlier too-soon press, are a mash or a double tap,
// not an answer to a question the player has not read yet (so mashing never picks for them).
constexpr uint64_t kFirstRunGraceMs = 700;
constexpr uint64_t kMashGapMs       = 500;
// The mash guard gives up this long after the card opened, so a fast but deliberate tapper still
// gets through; a mash through the entrance and onto the card is over well before then.
constexpr uint64_t kMashGuardMaxMs  = 6000;

// Both blurbs are short enough to wrap to the same two or three lines at every card width; a
// U+00A0 before the last word keeps it off a line of its own. Longer glue makes a ragged break when
// the card is narrow (an 800-wide window).

constexpr PresetInfo kPresets[] = {
    {Preset::Original, "original", "Original", "As on GameCube",
        "A 4:3 picture at GameCube resolution, the way it first\u00A0looked.", false},
    {Preset::Modern, "modern", "Modern", "Recommended",
        "Sharper, and fills wide screens and windows. Plays exactly the\u00A0same.", true},
};

// Which preset the current settings match exactly, for the Tools screen (focus and the "in use"
// mark). A hand-tuned setup matches neither: no mark, and the focus starts on the recommended card.
std::optional<Preset> current_preset() {
    if (matches(kModernValues)) {
        return Preset::Modern;
    }
    if (matches(kOriginalValues)) {
        return Preset::Original;
    }
    return std::nullopt;
}

// Additions to settings.rcss for this screen only: a deeper scrim on the first run (the menu
// behind is half covered and should not compete with the card), the neutral tag, the "in use"
// mark and the key hints.
constexpr const char* kPresetStyles = R"RCSS(
body.dialog.first-run {
    background-color: rgba(5, 11, 26, 80%);
    animation: 0.35s cubic-out 1 preset-scrim-in;
}
/* The scrim leaves with the card (window.rcss: 0.2 s), so closing never dips to near-black and then
   pops back to the full-brightness menu. */
body.dialog.closing {
    background-color: rgba(5, 11, 26, 0%);
    animation: none;
    transition: background-color 0.22s cubic-out;
}
@keyframes preset-scrim-in {
    from { background-color: rgba(5, 11, 26, 0%); }
    to { background-color: rgba(5, 11, 26, 80%); }
}
preset-tag.neutral {
    color: var(--color-text);
    background-color: rgba(255, 255, 255, 14%);
}
/* The tag and the "In use" mark share one centred row under the name. */
preset-tags {
    display: flex;
    flex-flow: row;
    justify-content: center;
    align-items: center;
    gap: 6dp;
}
preset-current {
    display: block;
    padding: 2dp 12dp;
    border-radius: 12dp;
    font-size: var(--font-size-sm);
    font-weight: bold;
    color: #0A1A0C;
    background-color: #8CE04A;
}
/* The miniature keeps a true 16:9 screen at every window shape: its width follows its (vh) height
   instead of the card's width, so the 4:3 picture between the bars stays 4:3 on a wide window. */
art-screen {
    width: 35.5vh;
    max-width: 100%;
    margin: 0 auto;
}
@media (min-height: 900dp) {
    art-screen { width: 320dp; }
}
@media (max-height: 800dp) {
    art-screen { width: 30.2vh; }
}
@media (max-height: 600dp) {
    art-screen { width: 24.9vh; }
    preset-current { padding: 0 10dp; }
}
button-prompts.preset-hints {
    width: 100%;
    flex: 0 0 auto;
    justify-content: flex-end;
}
@media (max-height: 600dp) {
    button-prompts.preset-hints { gap: 12dp; }
    button-prompts.preset-hints glyph { height: 22dp; line-height: 22dp; min-width: 22dp; }
}
)RCSS";

// The settings window's sheet also styles this screen (res/rml/settings.rcss).
Rml::String load_sheet(const Rml::String& path) {
    Rml::String rcss;
    if (auto* files = Rml::GetFileInterface(); files == nullptr || !files->LoadFile(path, rcss)) {
        return {};
    }
    return rcss;
}

bool sShownThisRun = false;

}  // namespace

void apply_preset(Preset preset) {
    apply_values(values_of(preset));
}

PresetWindow::PresetWindow(bool firstRun) : WindowSmall("modal"), mFirstRun(firstRun) {
    mRoot->SetClass("preset", true);
    set_document_styles(load_sheet("res/rml/settings.rcss") + kPresetStyles);
    if (mDocument != nullptr) {
        mDocument->SetClass("first-run", firstRun);
    }
    // Reopened from Tools: start on the preset in use and mark it.
    const std::optional<Preset> inUse = current_preset();

    auto* header = append(mDialog, "modal-header");
    append_text(append(header, "modal-title"),
        firstRun ? fmt::format("Welcome to {}", branding::kProductName) : Rml::String{"Choose a Preset"});
    append(header, "icon")->SetClass("celebration", true);

    append_text(append(mDialog, "modal-body"),
        "How would you like Pikmin 2 to look? You can change everything later in Settings.");

    auto* grid = append(mDialog, "preset-grid");
    for (int i = 0; i < static_cast<int>(std::size(kPresets)); i++) {
        const PresetInfo& info = kPresets[i];
        auto card = std::make_unique<Button>(grid, Rml::String{});
        Rml::Element* root = card->root();
        root->SetClass("preset-card", true);
        root->SetClass(info.cls, true);
        // A drawn miniature of the picture: a 4:3 frame with bars, or one filling a wide screen.
        auto* art = append(root, "preset-art");
        auto* screen = append(art, "art-screen");
        auto* picture = append(screen, "art-picture");
        append(picture, "art-sun");
        append(picture, "art-hill");
        append(picture, "art-sprout");
        auto* hud = append(picture, "art-hud");
        append(hud, "hud-l");
        append(hud, "hud-r");
        auto* title = append(root, "preset-name");
        append_text(title, info.name);
        auto* tags = append(root, "preset-tags");
        auto* tag  = append(tags, "preset-tag");
        tag->SetClass("neutral", !info.recommended);
        append_text(tag, info.tag);
        if (!firstRun && inUse == info.preset) {
            root->SetClass("current", true);
            append_text(append(tags, "preset-current"), "In use");
            mLastCard = i;
        }
        append_text(append(root, "preset-blurb"), info.blurb);
        card->on_pressed([this, i] {
            if (mFirstRun) {
                const uint64_t now  = SDL_GetTicks();
                const bool tooSoon  = now - mOpenedAt < kFirstRunGraceMs;
                const bool mashing  = mLastEarlyPress != 0 && now - mLastEarlyPress < kMashGapMs &&
                                      now - mOpenedAt < kMashGuardMaxMs;
                if (tooSoon || mashing) {
                    mLastEarlyPress = now;
                    return;
                }
            }
            mPendingChoice = i;
        });
        mCards.push_back(std::move(card));
    }

    auto* updates = append(mDialog, "preset-updates");
    // A question, not a setting changed from its default: no "modified" marker.
    mUpdateToggle = std::make_unique<BoolButton>(updates,
        bind(settings().interface.checkForUpdates,
            {.key = "Check for Updates", .isModified = [] { return false; }}));
    append_text(append(updates, "preset-caption"),
        fmt::format("Tell me when a new version of {0} is out. {0} asks GitHub at startup; "
                    "nothing\u00A0is\u00A0downloaded.",
            branding::kProductName));

    // The card's own key hints (DESIGN 5.17): the start menu's prompt is dimmed behind the scrim.
    mHints = append(mDialog, "button-prompts");
    mHints->SetClass("preset-hints", true);
    refresh_hints();

    mOpenedAt = SDL_GetTicks();
    play_sound(kSoundWindowOpen);
}

void PresetWindow::refresh_hints() {
    if (mHints == nullptr) {
        return;
    }
    const bool pad = input::last_input_gamepad();
    if (mHintsBuilt && pad == mHintsPad) {
        return;
    }
    mHintsBuilt = true;
    mHintsPad   = pad;
    clear_children(mHints);
    const auto add = [&](const char* key, const char* button, const char* label) {
        auto* prompt = append(mHints, "prompt");
        auto* glyph  = append(prompt, "glyph");
        glyph->SetClass("key", !pad);
        append_text(glyph, pad ? button : key);
        append_text(append(prompt, "label"), label);
    };
    add("Enter", "A", "Select");
    if (!mFirstRun) {
        add("Esc", "B", "Back");
    }
}

int PresetWindow::focused_card() const {
    auto* context = mDocument != nullptr ? mDocument->GetContext() : nullptr;
    auto* focused = context != nullptr ? context->GetFocusElement() : nullptr;
    for (int i = 0; i < static_cast<int>(mCards.size()); i++) {
        if (mCards[i]->contains(focused)) {
            return i;
        }
    }
    return -1;
}

void PresetWindow::update() {
    for (const auto& card : mCards) {
        card->update();
    }
    // Focus memory: Up from the toggle, or a key after a click outside, returns to this card.
    if (const int card = focused_card(); card >= 0) {
        mLastCard = card;
    }
    refresh_hints();
    if (mUpdateToggle) {
        mUpdateToggle->update();
    }
    if (mPendingChoice >= 0) {
        const int index = std::exchange(mPendingChoice, -1);
        choose(index);
    }
    WindowSmall::update();
}

void PresetWindow::choose(int index) {
    // Texture Filtering applies at once everywhere; on the start menu MSAA does too, and anything
    // else restart-only is relaunched by live_apply (SETTINGS 12). In game the banner says so.
    apply_preset(kPresets[index].preset);
    settings().interface.presetChosen.set(true);
    borealis::config::Registry::global().flush();
    play_sound(kSoundClick);
    close_with_scrim();
}

void PresetWindow::close_with_scrim() {
    if (mDocument != nullptr) {
        mDocument->SetClass("closing", true);
    }
    hide(true);
    uncover_top_document();
}

bool PresetWindow::focus() {
    // The card the player was last on (or, from Tools, the preset in use)...
    if (mLastCard >= 0 && mLastCard < static_cast<int>(mCards.size()) && mCards[mLastCard]->focus()) {
        return true;
    }
    // ...else Modern (SETTINGS 12: the recommended card).
    for (int i = static_cast<int>(mCards.size()) - 1; i >= 0; i--) {
        if (kPresets[i].recommended && mCards[i]->focus()) {
            return true;
        }
    }
    return !mCards.empty() && mCards.back()->focus();
}

bool PresetWindow::handle_nav_command(Rml::Event& event, NavCommand cmd) {
    if (cmd == NavCommand::Cancel || cmd == NavCommand::Menu) {
        if (!mFirstRun && cmd == NavCommand::Cancel) {
            play_sound(kSoundWindowClose);
            close_with_scrim();
        }
        return true; // the first-run screen needs an answer
    }
    auto* target = event.GetTargetElement();
    const bool onToggle = mUpdateToggle && mUpdateToggle->contains(target);
    const bool onCard = std::ranges::any_of(mCards, [target](const auto& c) { return c->contains(target); });
    if (!onToggle && !onCard) {
        // Nothing focused (a click outside the card cleared it): any direction or confirm brings
        // the focus back first, so keyboard and gamepad never go dead.
        if (cmd == NavCommand::Up || cmd == NavCommand::Down || cmd == NavCommand::Left ||
            cmd == NavCommand::Right || cmd == NavCommand::Confirm || cmd == NavCommand::Next ||
            cmd == NavCommand::Previous)
        {
            if (focus()) {
                play_sound(kSoundItemFocus);
            }
            return true;
        }
        return false;
    }
    if (onToggle) {
        if (cmd == NavCommand::Up) {
            focus();
            play_sound(kSoundItemFocus);
            return true;
        }
        return cmd == NavCommand::Down;
    }
    for (int i = 0; i < static_cast<int>(mCards.size()); i++) {
        if (!mCards[i]->contains(target)) {
            continue;
        }
        if (cmd == NavCommand::Down && mUpdateToggle && mUpdateToggle->focus()) {
            play_sound(kSoundItemFocus);
            return true;
        }
        const int next = cmd == NavCommand::Left ? i - 1 : cmd == NavCommand::Right ? i + 1 : -1;
        if (next >= 0 && next < static_cast<int>(mCards.size()) && mCards[next]->focus()) {
            play_sound(kSoundItemFocus);
        }
        return cmd == NavCommand::Left || cmd == NavCommand::Right || cmd == NavCommand::Up;
    }
    return false;
}

bool show_first_run_preset_if_needed() {
    if (sShownThisRun || settings().interface.presetChosen.get()) {
        return false;
    }
    sShownThisRun = true;
    push_document(std::make_unique<PresetWindow>(true));
    if (auto* doc = top_document()) {
        doc->focus();
    }
    return true;
}

}  // namespace mppm::ui
