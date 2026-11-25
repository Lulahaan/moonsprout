#pragma once
// Registries for the two launcher menus, so new features add an entry without editing the menu
// that shows it (docs/launcher/SETTINGS.md 13, DESIGN.md 5.3 and 8.1).
//
//   - start menu items: the buttons of the start menu (Play, Settings, Multiplayer, Quit, ...),
//     built by Prelaunch (package W3);
//   - in-game bar entries: the F1 bar (Resume, Settings, Controllers, Mods, Online, Reset Game,
//     Start Menu, Quit), built by the menu bar (package W5).
//
// Register at startup from any file, e.g. with a static MenuRegistration object. Entries are
// listed by `order` (ties keep registration order); `visible` is asked each time a menu is built.
// Note: the launcher is a static library, so a file that only holds a static MenuRegistration and
// is referenced by nothing else is dropped at link time; register from code that runs anyway.

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace mppm::launcher {

struct MenuEntry {
    std::string id;                 // stable, e.g. "settings"; unique within its registry
    std::string label;              // as shown, e.g. "Settings"
    int order = 0;                  // position: lower first
    std::function<bool()> visible;  // null = always visible
    std::function<void()> open;     // what pressing it does
};

enum class Menu {
    StartMenu, // start menu buttons
    InGameBar, // F1 / Back bar
};

// Standard orders of the in-game bar (SETTINGS 13).
namespace bar_order {
inline constexpr int kResume      = 0;
inline constexpr int kSettings    = 10;
inline constexpr int kControllers = 20;
inline constexpr int kMods        = 30;
inline constexpr int kOnline      = 40;
inline constexpr int kResetGame   = 80;
inline constexpr int kStartMenu   = 90;
inline constexpr int kQuit        = 100;
} // namespace bar_order

// Adds or replaces (same id) an entry.
void register_menu_entry(Menu menu, MenuEntry entry);
void unregister_menu_entry(Menu menu, const std::string& id);
// The visible entries, sorted by order.
std::vector<const MenuEntry*> menu_entries(Menu menu);
// Every registered entry, visible or not, sorted by order.
std::vector<const MenuEntry*> all_menu_entries(Menu menu);

// Registers an entry during static initialisation: `static MenuRegistration r{Menu::InGameBar, {...}};`
struct MenuRegistration {
    MenuRegistration(Menu menu, MenuEntry entry) { register_menu_entry(menu, std::move(entry)); }
};

} // namespace mppm::launcher
