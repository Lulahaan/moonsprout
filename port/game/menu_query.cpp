// Game-state queries for the launcher's in-game menu (docs/launcher/SETTINGS.md 13). Built with
// the game (CMakeLists.txt: port/game/*), so it can read game structures; the launcher only sees
// the C function declared in p2_host.h.
//
// GameFlow::mActiveSectionFlag (the spec's first idea) names the *next* section and reads
// SN_MainTitle during gameplay, so the running section is identified instead: GameFlow's root
// section is the RootMenuSection, whose current child is the section on screen. The game is built
// without RTTI, so the child's type is told by its vtable pointer.

#include "GameFlow.h"
#include "RootMenuSection.h"
#include "System.h"
#include "Game/SingleGame.h"
#include "Game/SingleGameSection.h"
#include "Screen/Game2DMgr.h"
#include "Screen/screenMgr.h"

#include "p2_host.h"
#include "p2_vtable.h"

P2_VTABLE(_ZTVN5Title7SectionE, "??_7Section@Title@@6B@")
P2_VTABLE(_ZTVN4Game17SingleGameSectionE, "??_7SingleGameSection@Game@@6B@")
#define P2_HAVE_VTABLE_QUERY 1

extern "C" int p2_game_progress_at_risk(void)
{
#ifdef P2_HAVE_VTABLE_QUERY
	if (sys == nullptr || sys->mGameFlow == nullptr || sys->mGameFlow->mSection == nullptr) {
		return 0; // still booting
	}
	Section* root    = sys->mGameFlow->mSection;
	Section* current = root->getCurrentSection();
	if (current == nullptr || current == root) {
		return 0; // boot logos (the boot section is its own current section) or between sections
	}
	const void* vtable = *reinterpret_cast<void* const*>(current);
	if (vtable == P2_VPTR(_ZTVN5Title7SectionE)) {
		return 0;
	}
	if (vtable == P2_VPTR(_ZTVN4Game17SingleGameSectionE)) {
		Game::SingleGame::State* state = static_cast<Game::SingleGameSection*>(current)->getCurrState();
		if (state != nullptr && state->getCurrStateID() == Game::SingleGame::SGS_File) {
			return 0; // file select
		}
	}
	return 1;
#else
	return 1; // no way to tell: always ask
#endif
}

extern "C" int p2_game_pause_menu_open(void)
{
	if (Screen::gGame2DMgr == nullptr || Screen::gGame2DMgr->mScreenMgr == nullptr) {
		return 0;
	}
	switch (Screen::gGame2DMgr->mScreenMgr->getSceneType()) {
	case SCENE_PAUSE_MENU:
	case SCENE_PAUSE_MENU_DOUKUTU:
	case SCENE_PAUSE_MENU_ITEMS:
	case SCENE_PAUSE_MENU_MAP:
	case SCENE_PAUSE_MENU_VS:
	case SCENE_PAUSE_MENU_CONTROLS:
		return 1;
	default:
		return 0;
	}
}
