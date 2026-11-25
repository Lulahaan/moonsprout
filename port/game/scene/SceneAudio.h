// Music of the start-menu scene (docs/launcher/SCENE.md 9).
#pragma once

#include "types.h"

namespace PSGame {
struct PikSceneMgr;
} // namespace PSGame
namespace PSSystem {
struct BgmSeq;
} // namespace PSSystem
namespace JAInter {
struct SoundInfo;
} // namespace JAInter

namespace LauncherScene {

struct SceneAudio {
	// DVD thread: creates the TITLE_18 sound scene with the chosen track (P2SceneConfig.music:
	// 0 off, 1 Piklopedia, 2 world map, 3 challenge menu, 4 high scores) and loads its waves.
	static void createScene(int music);
	static void startMusic();          // main thread, once the scene is on screen
	static void fadeOut(f32 seconds);  // on Play (title section: TMainTitleMgr::kFadeOutTime)
	static void destroy();             // doExit
};

} // namespace LauncherScene

// Hook H2: called by PSGame::PikSceneMgr::initMainBgm for the TITLE_18 scene type.
PSSystem::BgmSeq* p2_launcher_scene_bgm(PSGame::PikSceneMgr* mgr, JAInter::SoundInfo& soundInfo, u8* wScene);
