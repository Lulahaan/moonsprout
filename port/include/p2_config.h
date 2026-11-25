// Settings the game code reads, mirrored from the launcher's settings (docs/launcher/SETTINGS.md 1.4).
//
// The game never sees the config library: port/launcher/settings.cpp (bind_game_config) copies the
// few values game code needs into this plain struct and keeps it current when a setting changes.
// Game-side reads stay inside PC_ONLY(...), so the MWCC build is untouched.
//
// The defaults (port/sdk/os_report.cpp) equal the original game, so a build or run without the
// launcher behaves exactly as before. Writes happen on the game thread while no other game thread
// runs (launcher::update from p2_host_present), so plain fields need no locking. Values read off
// the game thread (audio mixer, scheduler timer) do not live here; they have setters next to their
// reader (p2_host.h).
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct P2Config {
	/* cheats / gameplay (read on the game thread) */
	float daySpeed;         /* sun-meter speed factor: 1.0 = normal, 0 = sun stopped (cheat.daySpeed) */
	int sunsetOnDayOne;     /* allow "Go to Sunset" on day 1 (cheat.sunsetOnDayOne) */
	int unlockChallenge;    /* Challenge Mode on the title menu without the key (cheat.unlockChallenge) */
	int invincibleCaptains; /* captains take no damage (cheat.invincibleCaptains) */
	int infiniteSprays;     /* using a spray does not use it up (cheat.infiniteSprays) */
	int idleDemo;           /* title attract movies (game.idleDemo) */
	int skipBootLogos;      /* skip the Nintendo / Dolby logos (game.skipBootLogos) */
	int titleSeason;        /* -1 = original behaviour, 0..3 = spring, summer, autumn, winter (game.titleSeason) */
	int hideHud;            /* hide the in-game HUD (interface.hideHud) */
	/* boot-time */
	int cardFileType;       /* CARDFileType: 0 CARD_RAWIMAGE, 1 CARD_GCIFOLDER (backend.cardFileType) */
	int audioEnabled;       /* JAudio runs (audio.enabled); latched once by p2_audio_enabled */
	/* diagnostics */
	int panicScreen;        /* show the game's crash screen instead of aborting (tools.panicScreen) */
	/* launcher state */
	int launcherOpen;       /* the start menu is up (title timers may hold while it is) */
} P2Config;

/* Defined in port/sdk/os_report.cpp. */
extern P2Config gP2Config;

#ifdef __cplusplus
}
#endif
