#include "menu_registry.hpp"

#include <algorithm>
#include <memory>

namespace mppm::launcher {
namespace {

struct Registry {
    // Stable addresses: menus keep MenuEntry pointers while they are built.
    std::vector<std::unique_ptr<MenuEntry>> entries;
};

// Function-local so registrations from static initialisers in other files are safe.
Registry& registry(Menu menu) {
    static Registry sRegistries[2];
    return sRegistries[menu == Menu::StartMenu ? 0 : 1];
}

std::vector<const MenuEntry*> sorted(Menu menu, bool visibleOnly) {
    std::vector<const MenuEntry*> out;
    for (const auto& entry : registry(menu).entries) {
        if (!visibleOnly || !entry->visible || entry->visible()) {
            out.push_back(entry.get());
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const MenuEntry* a, const MenuEntry* b) { return a->order < b->order; });
    return out;
}

} // namespace

void register_menu_entry(Menu menu, MenuEntry entry) {
    auto& entries = registry(menu).entries;
    for (auto& existing : entries) {
        if (existing->id == entry.id) {
            *existing = std::move(entry);
            return;
        }
    }
    entries.push_back(std::make_unique<MenuEntry>(std::move(entry)));
}

void unregister_menu_entry(Menu menu, const std::string& id) {
    std::erase_if(registry(menu).entries, [&](const auto& entry) { return entry->id == id; });
}

std::vector<const MenuEntry*> menu_entries(Menu menu) {
    return sorted(menu, true);
}

std::vector<const MenuEntry*> all_menu_entries(Menu menu) {
    return sorted(menu, false);
}

} // namespace mppm::launcher
