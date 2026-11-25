// Reset Game from the launcher's in-game menu (docs/launcher/SETTINGS.md 13). Host-only file, built
// into the game library (not in the matching build).
#include "JSystem/JUtility/JUTGamePad.h"
#include "p2_host.h"
#include <stdio.h>

// The same state the controller's B+X+Start combo leaves (JUTGamePad::checkResetCallback): the
// game's ResetManager then stops the sound, waits for card writes, fades out and calls
// OSResetSystem, which the port turns into p2_host_reset_system. If the game does not allow a
// reset at that moment, ResetManager drops the request, exactly as for the combo.
extern "C" void p2_game_request_reset(void)
{
	// A harness run keeps the wall-clock pad logic off (docs/ONLINE_MULTIPLAYER.md 4 item 2).
	if (p2_net_pad_wallclock_off()) {
		static bool warned = false;
		if (!warned) {
			warned = true;
			fprintf(stderr, "mppm: net: Reset Game ignored in a harness run\n");
		}
		return;
	}
	JUTGamePad::C3ButtonReset::sResetOccurredPort = JUTGamePad::PORT_0;
	JUTGamePad::C3ButtonReset::sResetOccurred     = true;
}
