// OSReport / OSPanic (Aurora leaves these to the game; its own versions are compiled out), and the
// gP2Config settings mirror that game code reads.
#include <dolphin/card.h>
#include <dolphin/os.h>

#include "p2_config.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>

extern "C" {

void OSVReport(const char* msg, va_list args)
{
	std::vfprintf(stdout, msg, args);
	std::fflush(stdout);
}

void OSReport(const char* msg, ...)
{
	va_list args;
	va_start(args, msg);
	OSVReport(msg, args);
	va_end(args);
}

void OSPanic(const char* file, int line, const char* msg, ...)
{
	std::fprintf(stderr, "OSPanic: %s:%d: ", file, line);
	va_list args;
	va_start(args, msg);
	std::vfprintf(stderr, msg, args);
	va_end(args);
	std::fputc('\n', stderr);
	std::abort();
}

} // extern "C"

// The settings mirror for game code (p2_config.h). These defaults are the original game's
// behaviour; the launcher overwrites them from its settings in launcher::preinit
// (mppm::bind_game_config) and keeps them current afterwards.
extern "C" P2Config gP2Config = {
	/* daySpeed */ 1.0f,
	/* sunsetOnDayOne */ 0,
	/* unlockChallenge */ 0,
	/* invincibleCaptains */ 0,
	/* infiniteSprays */ 0,
	/* idleDemo */ 1,
	/* skipBootLogos */ 0,
	/* titleSeason */ -1,
	/* hideHud */ 0,
	/* cardFileType */ CARD_GCIFOLDER,
	/* audioEnabled */ 1,
	/* panicScreen */ 0,
	/* launcherOpen */ 0,
};
