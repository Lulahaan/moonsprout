#pragma once
// The Save Manager (docs/launcher/SETTINGS.md 10): lists the Pikmin 2 (GPVE01) files on memory
// card A in the active save format, GCI folder or card image, and their backups; imports a .gci
// or the Pikmin 2 files of a Dolphin .raw card image; exports a save as .gci; backs up, restores
// and deletes. Every import, restore and delete takes an automatic backup first (the newest 10
// automatic backups are kept, manual ones never pruned).
//
// Backups live in <data>/backups/<YYYY-MM-DD_HHMMSS>/ as plain .gci files (whatever the save
// format), with a small backup.txt saying whether it was automatic and why. A backup restores
// into either format.
//
// Launcher only: opened from the start menu's settings (PRELAUNCH and TOOLS tabs, package W4),
// never while playing. If the game has already booted behind the start menu (live scene), it has
// read the card during boot: every change remounts the card and raises saves_restart_pending(),
// which the restart flow (SETTINGS 11) treats like a restart-only setting.

#include "ui/window.hpp"

#include <cstdint>
#include <string>

namespace mppm::ui {

class SavesWindow final : public Window {
public:
    SavesWindow();
    void update() override;

private:
    void build_content(Rml::Element* content);

    std::string mSelected; // "save:<card file name>" or "backup:<folder name>"; empty = first item
    uint64_t mGeneration = 0;
};

// Why the Save Manager can't be opened now, or "" when it can. `inGame`: called from the in-game
// menu (SETTINGS 9.2: "Return to the start menu to manage saves.").
std::string saves_unavailable_reason(bool inGame);

// Pushes the Save Manager over the top document. Returns false (and opens nothing) when
// saves_unavailable_reason(false) is not empty.
bool open_saves_window();

// True once the Save Manager changed the card of a game that has already booted: Play must
// restart first so the game reads the new saves (SETTINGS 10 and 11).
bool saves_restart_pending();

} // namespace mppm::ui
