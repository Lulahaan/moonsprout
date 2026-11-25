#pragma once
// The Mods window (docs/launcher/SETTINGS.md 17, docs/online/M5_2_MODS.md): the data mods in the
// mods folder (<data>/mods by default) in load order, their state, conflicts and problems, and the
// content hash an online game compares. Shaped like the Save Manager: a list on the left (Overview,
// then one entry per mod), the selected item on the right.
//
// Start menu: editable. Every Turn On / Turn Off / Move rewrites load_order.txt at once (through
// mppm::mods::write_load_order). Before the boot nothing is pending: the mount reads the file when
// the game starts. Once the game has booted behind the start menu, a list whose mods hash differs
// from the folder's at the boot (mounted().folderHash) raises mods_restart_pending(), which the
// restart flow (SETTINGS 11) treats like the Save Manager's: Play restarts first.
// In game (F1 bar) the window is read-only, and so is it while a replay or harness session runs
// (mppm::mods::locked_reason()).
//
// The folder is read on a worker thread (mods::scan opens and hashes every listed file, which can
// take a while for large mods): the window shows the last result it has, says "Reading the mods
// folder" and keeps the load-order buttons disabled until the new one arrives.
//
// Every string from a mod (name, author, description, ids, issue text) is untrusted: it goes into
// text nodes, or through ui::escape when it is part of RML.

#include "ui/window.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace mppm::mods {
struct Scan;
}

namespace mppm::ui {

class Pane;
struct ModsScanResult; // a finished folder scan and its content hash (mods_window.cpp)

class ModsWindow final : public Window {
public:
    explicit ModsWindow(bool inGame);
    ~ModsWindow() override;
    void update() override;

    // Which load-order button keeps the focus across a rebuild.
    enum class Action : uint8_t { None, Toggle, Up, Down };

private:
    void build_content(Rml::Element* content);
    void build_overview(Pane& pane);
    void build_mod(Pane& pane, size_t index);
    void apply(size_t index, Action action);
    bool take_result(); // the newest finished scan, if it differs from the shown one
    std::string hash_text(size_t digits) const;
    std::string locked_reason() const;
    const mods::Scan& scan() const;

    bool mInGame = false;
    std::shared_ptr<const ModsScanResult> mResult; // shown; null before the first scan
    std::string mSelected;              // "overview" or "mod:<id>"
    Action mFocusAction = Action::None; // put the focus back on this button after the rebuild
    bool mRebuild = false;              // rebuild in update(), never inside a button's own event
    Component* mFocusTarget = nullptr;  // set while building, valid until the next rebuild

    std::unique_ptr<Component> mPrompts; // button prompts under the content (DESIGN 5.17)
};

// Pushes the Mods window over the top document. `inGame`: opened from the in-game bar (read-only).
void open_mods_window(bool inGame);

// True when the game has booted with a mod set (mounted().ran) and the mods folder, as the Mods
// window last read it, differs from the folder the boot read: Play must restart first (SETTINGS 11,
// 17). Never waits, except for a scan that follows a load-order change made in the window.
bool mods_restart_pending();

} // namespace mppm::ui
