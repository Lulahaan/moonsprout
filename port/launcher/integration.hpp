#pragma once
// Glue between the launcher packages: hooks that belong to no single window (README 4, WINT).
//
//   - the settings window's external buttons (Verify Disc, Save Manager, Configure Controllers,
//     Reset All Controls, Check Now), registered through ui::set_settings_action;
//   - settings whose effect lives outside the UI: Player Colour Light, Mute in Background,
//     Menu Sounds (and the game-side UI sound handler).
//
// Called from launcher.cpp: init() once the UI is up, update() once per presented frame,
// handle_event() for every SDL event and handle_aurora_event() for Aurora's own.

#include <aurora/event.h>

union SDL_Event;

namespace mppm::launcher::integration {

void init();
void update();
// The game thread is held by Pause While Menu Is Open: its sound fades out until released.
void set_game_held(bool held);
void handle_event(const SDL_Event& event);
void handle_aurora_event(const AuroraEvent& event);

} // namespace mppm::launcher::integration
