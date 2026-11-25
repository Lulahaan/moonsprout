// Disc resources of the start-menu scene (docs/launcher/SCENE.md 3, 6.2).
//
// Everything is read from the player's disc at run time, with the same archives and loader flags
// gameplay uses. All of it lives in the scene section's heap and goes away with it, except
// pikis.szs, which is mounted on the system heap exactly as NaviMgr::load does (the title screen
// mounts it there right afterwards anyway).
#pragma once

#include "SceneWater.h"

#include "types.h"
#include "Sys/MatBaseAnimation.h"

struct J3DModelData;
struct JKRArchive;

namespace SysShape {
struct AnimMgr;
struct AnimInfo;
} // namespace SysShape

namespace Game {
struct GameLightMgrSetting;
} // namespace Game

namespace LauncherScene {

struct StageInfo;

enum PikminColour { PIKI_Red = 0, PIKI_Yellow, PIKI_Blue, PIKI_ColourCount };
enum HappaKind { HAPPA_Leaf = 0, HAPPA_Bud, HAPPA_Flower, HAPPA_Count };

struct Assets {
	Assets();

	// DVD thread. Returns false and sets mError when the disc lacks something; nothing asserts on
	// that path (SCENE.md R13). The map, the light settings and the ship are optional extras: a
	// missing one is logged and the scene runs without it.
	bool load(const StageInfo& stage);

	// Characters (pikis.szs).
	JKRArchive* mPikiArc;
	J3DModelData* mOlimarData;
	J3DModelData* mPikiData[PIKI_ColourCount];
	J3DModelData* mHappaData[HAPPA_Count];
	SysShape::AnimMgr* mAnimMgr; // the shared captain / Pikmin clip set (IPikiAnims), owned here

	// Ship (Kando/ufo/arc.szs).
	J3DModelData* mShipData;
	SysShape::AnimMgr* mShipAnimMgr; // 0 suction, 1-3 pmotion1-3
	Sys::MatTevRegAnimation mShipTev[2];

	// Onions (Kando/onyon/arc.szs, texts.szs; SCENE.md 11.2). One model data for all three, coloured
	// per Onion by its kidou_*.brk at the last frame (Onyon::changeMaterial). Null when missing.
	J3DModelData* mOnionData;
	SysShape::AnimMgr* mOnionAnimMgr; // 0 flowwait, 1 flowrot, 2 in, 3 suction, 4 bootup
	Sys::MatTevRegAnimation mOnionTev[PIKI_ColourCount]; // kidou_red, kidou_yellow, kidou_blue

	// Area map (<map folder>/arc.szs).
	J3DModelData* mMapData;
	int mMapTexAnimCount;
	Sys::MatTexAnimation* mMapTexAnims;

	// Water (<map folder>/texts.szs waterbox.txt and the two-player piece; SCENE.md 10.5).
	SceneWater mWater;

	// Area colours (<abe folder>/light.ini); null when missing.
	Game::GameLightMgrSetting* mLightSetting;

	const char* mError; // first fatal problem, for the log

private:
	bool loadCharacters();
	void loadShip();
	void loadOnions();
	void loadMap(const StageInfo& stage);
	void loadLight(const StageInfo& stage);
};

} // namespace LauncherScene
