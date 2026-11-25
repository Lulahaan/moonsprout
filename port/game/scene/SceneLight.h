// Light, fog, sky and shadows of the start-menu scene (docs/launcher/SCENE.md 10).
//
// GameLightMgr cannot be used here (it reads the day's TimeMgr and gameSystem), so this is the
// same light rig built by hand: main spot (spot function off) and sub light, the specular light,
// ambient and linear fog, coloured from the area's light.ini block for the chosen time of day.
#pragma once

#include "Light.h"
#include "Game/shadowMgr.h"

namespace Game {
struct GameLightMgrSetting;
} // namespace Game

struct Viewport;

namespace LauncherScene {

struct StageFrame;

enum SceneTimeOfDay { TIME_Morning = 0, TIME_Day, TIME_Evening, TIME_Night };

struct SceneLightMgr : public LightMgr {
	SceneLightMgr();

	// Colours from light.ini (null: built-in morning colours), the sun placed for the time of day
	// and turned so the key light comes from behind the camera's left shoulder.
	void setup(Game::GameLightMgrSetting* setting, int timeOfDay, const StageFrame& frame);

	// A new time of day on the running scene (live_options 3.3): the targets from setup(), reached
	// over `seconds` by step() (0 = at once).
	void retarget(Game::GameLightMgrSetting* setting, int timeOfDay, const StageFrame& frame, f32 seconds);
	void step(f32 dt); // per scene frame (Section::updateScene)

	virtual void set(Graphics& gfx);   // lights in view space of the current viewport, then fog
	void setView(Matrixf& viewMtx);   // lights in view space of an explicit view matrix
	void setFog(Graphics& gfx);
	void fogOff(Graphics& gfx);

	// Sky colours for the time of day by the elevation of the view ray (Section::drawSky): the fog
	// colour at the horizon (kSkyBottomDeg and below), mSkyTop at kSkyTopDeg (the top edge of the
	// idle picture), the time of day's zenith straight up (the take-off's sky shots).
	JUtility::TColor mSkyTop;
	JUtility::TColor mSkyBottom;
	JUtility::TColor mSkyZenith;

	LightObj* mMainLight;
	LightObj* mSubLight;
	LightObj* mSpecLight;
	FogMgr* mFogMgr;
	Color4 mShadowColor;
	Vector3f mSunDir; // world, unit, pointing from the ground toward the sun
	f32 mFogPush = 0.0f; // added to the fog's near and far: the aerial shot sees the ground from far
	                     // above, where the area's fog (tuned for a camera ~400 units off) whites it out

private:
	void applyFog(Graphics& gfx);

	// Everything a time of day sets, for the blend.
	struct Look {
		Color4 mMain, mSub, mSpec, mAmbient, mFog, mShadow;
		f32 mFogNear, mFogFar;
		JUtility::TColor mSkyTop, mSkyBottom, mSkyZenith;
		Vector3f mSunDir;
	};
	void capture(Look& look) const;
	void apply(const Look& a, const Look& b, f32 t); // the lerp, light positions from the sun
	Look mFrom;
	Look mTo;
	f32 mBlend;       // 0..1 through the blend
	f32 mBlendTime;   // seconds of the whole blend (0 = no blend running)
	f32 mDistance;    // the sun's distance (light.ini)
	Vector3f mOrigin; // the stage frame's origin, for the light positions
};

// Gameplay's shadow cylinders (ShadowMgr::update / draw) for a handful of casters, without
// ShadowMgr or Creatures.
struct SceneShadows {
	enum { MAX_CASTERS = 8 };

	SceneShadows();

	void init(Color4* color);
	void begin(Camera* camera); // once per frame, before add()
	// One caster at `pos` (feet), cylinder radius and height (gameplay: captain 4/10, Pikmin
	// 2/10), raised by `lift` (gameplay: 0.5). A radius of 0 adds nothing.
	void add(const Vector3f& pos, f32 radius, f32 height, f32 lift, Matrixf& viewMtx);
	void draw(Viewport* vp);

	Game::ShadowParms mParms;
	Game::ShadowCylinder2* mCylinder;
	Matrixf mMtx[MAX_CASTERS];
	int mType[MAX_CASTERS];
	int mCount;
};

} // namespace LauncherScene
