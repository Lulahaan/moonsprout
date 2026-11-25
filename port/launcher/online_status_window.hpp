#pragma once
// The in-game bar's Online window (M5.3b design 10.5, SETTINGS 18): shown while a lobby or a match
// exists (mppm::online::active). One row per member: seat and team marker, name, ping, input delay
// ("Delay 2 ticks"), link, and a "Lagging" tag for the seats the match is waiting for; the status
// line; Leave game (danger confirmation, then back to the start menu). The match keeps running
// underneath (online games never pause).

#include "ui/window.hpp"

#include <memory>

namespace mppm::ui {

class OnlineStatusWindow final : public Window {
public:
    OnlineStatusWindow();
    ~OnlineStatusWindow() override;
    void update() override;

private:
    void build_content(Rml::Element* content);
    std::string mBuiltKey;
    std::unique_ptr<Component> mPrompts;
};

// Opens it over the in-game bar.
void open_online_status_window();

} // namespace mppm::ui
