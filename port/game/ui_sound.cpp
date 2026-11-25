// Launcher UI sounds played through the game's own sound system (docs/launcher/README.md 6: the
// launcher is silent before a disc; once the game's sound system is up its menus use the game's
// system sound effects). Host-only file, built into the game library (not in the matching build).
#include "JSystem/JAudio/JAI/JAIBasic.h"
#include "JSystem/JAudio/JAI/JAISequence.h"
#include "JSystem/JAudio/JAI/JAInter/SeMgr.h"
#include "JSystem/JAudio/JAS/JASChannel.h"
#include "JSystem/JAudio/JAS/JASDsp.h"
#include "JSystem/JAudio/JAS/JASTrack.h"
#include "PSSystem/PSSystemIF.h"
#include "SoundID.h"
#include "p2_host.h"

// Cue numbers are port/launcher/ui/sound.hpp's UiSound values (the game side does not include
// launcher headers).
enum {
	P2_UI_SOUND_NONE,
	P2_UI_SOUND_CLICK,
	P2_UI_SOUND_PLAY,
	P2_UI_SOUND_BINDING_CHANGED,
	P2_UI_SOUND_MENU_OPEN,
	P2_UI_SOUND_MENU_CLOSE,
	P2_UI_SOUND_WINDOW_OPEN,
	P2_UI_SOUND_WINDOW_CLOSE,
	P2_UI_SOUND_TAB_CHANGED,
	P2_UI_SOUND_ITEM_FOCUS,
	P2_UI_SOUND_ITEM_CHANGE,
	P2_UI_SOUND_ITEM_ENABLE,
	P2_UI_SOUND_ITEM_DISABLE,
	P2_UI_SOUND_WARNING,
};

// The same effects og::Sound (src/plugProjectOgawaU/ogSE.cpp) gives the game's own menus.
static u32 systemSeFor(int cue)
{
	switch (cue) {
	case P2_UI_SOUND_CLICK:
	case P2_UI_SOUND_PLAY:
	case P2_UI_SOUND_BINDING_CHANGED:
		return PSSE_SY_MENU_DECIDE; // og::Sound::setDecide
	case P2_UI_SOUND_MENU_OPEN:
	case P2_UI_SOUND_WINDOW_OPEN:
		return PSSE_SY_MENU_OPEN; // setOpen
	case P2_UI_SOUND_MENU_CLOSE:
	case P2_UI_SOUND_WINDOW_CLOSE:
		return PSSE_SY_MENU_CLOSE; // setClose
	case P2_UI_SOUND_TAB_CHANGED:
		return PSSE_SY_MENU_PLUS_MINUS; // setSMenuLR: the pause menu's L/R page turn
	case P2_UI_SOUND_ITEM_FOCUS:
		return PSSE_SY_MENU_CURSOR; // setCursor
	case P2_UI_SOUND_ITEM_CHANGE:
	case P2_UI_SOUND_ITEM_ENABLE:
	case P2_UI_SOUND_ITEM_DISABLE:
		return PSSE_SY_SOUND_CONFIG; // the Options screen changing a value
	case P2_UI_SOUND_WARNING:
		return PSSE_SY_MENU_ERROR; // setError
	default:
		return 0;
	}
}

extern "C" int p2_game_play_ui_sound(int cue)
{
	// spSysIF first: p2_audio_enabled() latches audio.enabled on its first call, and the standalone
	// start menu (no game yet) must leave it free to change until the boot (live_options_design 3.5).
	if (PSSystem::spSysIF == nullptr || !p2_audio_enabled()) {
		return 0; // no sound system yet (no disc, or still booting): stay silent
	}
	const u32 id = systemSeFor(cue);
	if (id == 0) {
		return 0;
	}
	PSSystem::spSysIF->playSystemSe(id, 0);
	return 1;
}

// ---- Menu cues while the menu holds the game (Pause While Menu Is Open) ----

// The JAI frame work the game runs once a frame (PSSystem::SysIF::mainLoop) starts queued sound
// effects; with the game held only that part runs, so a menu cue starts and plays to its end.
// The game's own sounds keep their state (nothing moves while held) and stay ducked.
extern "C" void p2_game_audio_hold_frame(void)
{
	if (PSSystem::spSysIF == nullptr || !p2_audio_enabled() || JAIBasic::getInterface() == nullptr) {
		return;
	}
	JAIBasic::getInterface()->processFrameWork();
}

// The category of the system sound effects (PSSE_SY_*): JAInter::SeMgr::changeIDToCategory.
static const u8 kSystemSeCategory = (PSSE_SY_MENU_CURSOR >> 12) & 0xFF;

static void collectVoices(JASTrack* track, const JASDsp::TChannel* base, unsigned long long& mask, int depth)
{
	if (track == nullptr || depth > 4) {
		return;
	}
	for (JSULink<JASChannel>* link = track->getFirst(); link != nullptr; link = link->getNext()) {
		JASChannel* channel = link->getObject();
		if (channel == nullptr || channel->mDspChannel == nullptr || channel->mDspChannel->mChannel == nullptr) {
			continue;
		}
		const long index = static_cast<long>(channel->mDspChannel->mChannel - base);
		if (index >= 0 && index < 64) {
			mask |= 1ull << index;
		}
	}
	for (int i = 0; i < 16; i++) {
		collectVoices(track->mChildList[i], base, mask, depth + 1);
	}
}

extern "C" unsigned long long p2_game_ui_voice_mask(void)
{
	using namespace JAInter::SeMgr;
	if (seHandle == nullptr || !p2_audio_enabled() || sePlaySound == nullptr || categoryInfoTable == nullptr) {
		return 0;
	}
	const JASDsp::TChannel* base = JASDsp::getDSPHandle(0);
	JASTrack* root               = seHandle->getTrack();
	JAISound** slots             = sePlaySound[kSystemSeCategory];
	const u8 slotCount           = categoryInfoTable[seScene][kSystemSeCategory * 2];
	unsigned long long mask      = 0;
	for (u8 i = 0; slots != nullptr && i < slotCount; i++) {
		JAISound* se = slots[i];
		if (se == nullptr || se->_14 == 0xFF) {
			continue;
		}
		// The SE sequence's track tree: slot n plays on child (n >> 4), grandchild (n & 15), as in
		// JAInter::SeMgr::checkPlayingSe's port routes.
		JASTrack* group = root->mChildList[(se->_14 >> 4) & 0xF];
		collectVoices(group != nullptr ? group->mChildList[se->_14 & 0xF] : nullptr, base, mask, 0);
	}
	return mask;
}
