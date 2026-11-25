// Water of the start-menu scene (docs/launcher/SCENE.md 10.5).
//
// The area maps have no water in model.bmd: gameplay draws it from <map folder>/texts.szs
// waterbox.txt, one scaled 100 x 100 piece per box (Game::SeaMgr, AABBWaterBox). Perplexing Pool's
// landing site is a platform in the middle of its pool, so without the water the view past it is
// bare fog. The single-player water samples the copied frame buffer for its refraction, which
// needs gameplay's frame copy; the scene uses the two-player piece instead (waterbox2p.szs 2p.bmd,
// its own scrolling texture), with the same layout.
#pragma once

#include "types.h"

struct J3DModelData;
namespace SysShape {
struct Model;
}
namespace Sys {
struct MatTexAnimation;
struct MatLoopAnimator;
} // namespace Sys

namespace LauncherScene {

struct SceneWater {
	SceneWater();

	void load(const char* mapFolder); // DVD thread; no water when anything is missing
	void init();                      // main thread, once loaded: the models
	void update(f32 dt);              // the texture scroll at 30 frames per second of `dt`
	void entry();
	void viewCalc();

	enum { kMaxBoxes = 12 };
	int mCount;
	f32 mBox[kMaxBoxes][4]; // min x, min z, max x, max z
	f32 mTop[kMaxBoxes];
	J3DModelData* mData;
	Sys::MatTexAnimation* mAnim;
	Sys::MatLoopAnimator* mAnimator;
	SysShape::Model* mModels[kMaxBoxes];
};

} // namespace LauncherScene
