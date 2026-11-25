#include "types.h"

#include "Dolphin/os.h"
#include "System.h"

/**
 * @note Address: 0x804111B4
 * @note Size: 0x50
 */
#if TARGET_PC
// The host entry point (port/main.cpp) owns main(); it calls this once Aurora is up.
extern "C" int p2_game_main()
#else
int main()
#endif
{
	OSReport("bootup -----------------------\n");

	System::initialize();
	return (new System())->run();
}
