// Music of the start-menu scene (docs/launcher/SCENE.md 9).
//
// The scene uses the unused sound scene type TITLE_18. newGameScene already turns types it does
// not know into Scene_NoObjects; hook H2 in PikSceneMgr::initMainBgm (PSGame.cpp) asks
// p2_launcher_scene_bgm below for the main sequence, which makes the same calls the game makes
// for the screens those tracks belong to.
#include "SceneAudio.h"

#include "PSGame/PikScene.h"
#include "PSSystem/PSGame.h"
#include "PSSystem/PSScene.h"
#include "PSSystem/PSSeq.h"
#include "PSSystem/WaveScene.h"
#include "PSM/Scene.h"
#include "System.h"

namespace LauncherScene {
namespace {

int sMusic        = 1;     // P2SceneConfig.music for the scene being created
bool sSceneExists = false; // the TITLE_18 scene is current

} // namespace

void SceneAudio::createScene(int music)
{
	sMusic = music;
	PSGame::SceneInfo info;
	info.mSceneType          = PSGame::SceneInfo::TITLE_18;
	info.mCameras            = 0;
	PSGame::PikSceneMgr* mgr = static_cast<PSGame::PikSceneMgr*>(PSSystem::getSceneMgr());
	mgr->newAndSetCurrentScene(info);

	PSSystem::SceneMgr* sceneMgr = PSSystem::getSceneMgr();
	sceneMgr->checkScene();
	sceneMgr->mScenes->mChild->scene1stLoadSync();
	sSceneExists = true;
}

void SceneAudio::startMusic()
{
	// "Off" still creates the scene (sound effects need it); it just never starts the sequence.
	if (sSceneExists && sMusic != 0) {
		PSMGetSceneMgrCheck()->doStartMainSeq();
	}
}

void SceneAudio::fadeOut(f32 seconds)
{
	if (!sSceneExists || sMusic == 0) {
		return;
	}
	PSSystem::SceneMgr* mgr = PSSystem::getSceneMgr();
	mgr->checkScene();
	PSSystem::Scene* scene = mgr->mScenes->mChild;
	if (scene == nullptr) {
		return;
	}
	PSSystem::SeqBase* seq = scene->mSeqMgr.getSeq(0);
	if (seq != nullptr) {
		f32 frames = seconds * 60.0f; // sound frames: 60 a second at any frame rate (p2_host_sound_frames)
		seq->stopSeq(frames < 1.0f ? 1u : (u32)frames);
	}
}

void SceneAudio::destroy()
{
	if (sSceneExists) {
		PSSystem::SceneMgr* mgr = PSSystem::getSceneMgr();
		PSSystem::validateSceneMgr(mgr);
		mgr->deleteCurrentScene();
		sSceneExists = false;
	}
}

} // namespace LauncherScene

// Hook H2 (PSGame.cpp, PikSceneMgr::initMainBgm, case TITLE_18): the main sequence and wave scene
// of the start-menu scene. The calls are those of PSGame.cpp:966-988 for the same tracks.
PSSystem::BgmSeq* p2_launcher_scene_bgm(PSGame::PikSceneMgr* mgr, JAInter::SoundInfo& soundInfo, u8* wScene)
{
	switch (LauncherScene::sMusic) {
	case 2:
		*wScene = PSSystem::WaveScene::WSCENE16_WorldMap;
		return mgr->newBgmSeq("worldmap.bms", soundInfo);
	case 3:
		*wScene = PSSystem::WaveScene::WSCENE20_ChallengeSelect;
		return mgr->newBgmSeq("c_menu.bms", soundInfo);
	case 4:
		*wScene = PSSystem::WaveScene::WSCENE21_HighScores;
		return mgr->newBgmSeq("hiscore.bms", soundInfo);
	default: // 1 Piklopedia, and 0 Off (created but never started)
		*wScene = PSSystem::WaveScene::WSCENE36_Piklopedia;
		return mgr->newBgmSeq("book.bms", soundInfo);
	}
}
