// Light, fog, sky and shadows of the start-menu scene (docs/launcher/SCENE.md 10).
#include "SceneLight.h"
#include "SceneStage.h"

#include "Graphics.h"
#include "Viewport.h"
#include "FogMgr.h"
#include "Game/GameLight.h"
#include "Game/TimeMgr.h"

#include <math.h>

namespace LauncherScene {
namespace {

const f32 kPi = 3.14159265f;

// Fog range without a light.ini (the game's night range).
const f32 kFogNear = 0.0f;
const f32 kFogFar  = 4500.0f;

// Gameplay lights its characters from high above a camera that looks down on them; seen from the
// side at knee height they come out dull with the same colours. The scene lifts the key light and
// the ambient a little so the cast reads as well as it does in play.
const f32 kKeyGain     = 1.35f;
const f32 kAmbientGain = 1.6f;

Color4 scaled(const Color4& c, f32 gain)
{
	f32 r = c.r * gain, g = c.g * gain, b = c.b * gain;
	return Color4(r > 255.0f ? 255 : (u8)r, g > 255.0f ? 255 : (u8)g, b > 255.0f ? 255 : (u8)b, c.a);
}

// Where the key light comes from, in the stage frame: behind the camera (+Z) and over its left
// shoulder (-X), so faces are lit and shadows fall to the right and away.
const f32 kSunAzimuthDeg = 32.0f;

JUtility::TColor lerpColor(const Color4& a, const Color4& b, f32 t)
{
	return JUtility::TColor((u8)(a.r + (b.r - a.r) * t), (u8)(a.g + (b.g - a.g) * t), (u8)(a.b + (b.b - a.b) * t), 255);
}

// light.ini block for a scene time of day (Game::SunLightTimes order differs).
int timeBlock(int timeOfDay)
{
	switch (timeOfDay) {
	case TIME_Day:
		return Game::SUNTIME_Noon;
	case TIME_Evening:
		return Game::SUNTIME_Evening;
	case TIME_Night:
		return Game::SUNTIME_Night;
	default:
		return Game::SUNTIME_Morning;
	}
}

} // namespace

SceneLightMgr::SceneLightMgr()
    : LightMgr("SceneLightMgr")
    , mSkyTop(110, 160, 235, 255)
    , mSkyBottom(200, 210, 220, 255)
    , mSkyZenith(110, 160, 235, 255)
    , mShadowColor(0, 0, 50, 65)
    , mSunDir(0.0f, 1.0f, 0.0f)
    , mBlend(1.0f)
    , mBlendTime(0.0f)
    , mDistance(20000.0f)
    , mOrigin(0.0f, 0.0f, 0.0f)
{
	// The rig of GameLightMgr::GameLightMgr (gameLightMgr.cpp:386-415).
	mMainLight             = new LightObj("main", GX_LIGHT0, TYPE_Spot, JUtility::TColor(255, 255, 255, 255));
	mMainLight->mSpotFn    = GX_SP_OFF;
	mMainLight->mElevation = Vector3f(0.0f, -1.0f, 0.0f);
	registLightObj(mMainLight);

	mSubLight             = new LightObj("sub", GX_LIGHT1, TYPE_Spot, JUtility::TColor(128, 64, 64, 255));
	mSubLight->mSpotFn    = GX_SP_OFF;
	mSubLight->mElevation = Vector3f(0.0f, 1.0f, 0.0f);
	registLightObj(mSubLight);

	mSpecLight          = new LightObj("spec", GX_LIGHT7, TYPE_Spec, JUtility::TColor(255, 255, 255, 255));
	mSpecLight->mKScale = 40.0f;
	registLightObj(mSpecLight);

	mFogMgr        = new FogMgr;
	mFogMgr->mType = GX_FOG_LINEAR;
	mFogMgr->mNearZ = kFogNear;
	mFogMgr->mFarZ  = kFogFar;
	add(mFogMgr);

	mAmbientLight.mColor = Color4(50, 50, 60, 255);
}

void SceneLightMgr::setup(Game::GameLightMgrSetting* setting, int timeOfDay, const StageFrame& frame)
{
	// Colours: one light.ini block, read with the accessors GameLightMgr::calcSetting uses.
	Color4 fog(170, 190, 200, 255);
	if (setting != nullptr && !setting->mIsCave) {
		Game::GameLightTimeSetting& t = setting->mSunLight.mLightTimes[timeBlock(timeOfDay)];
		Color4 c;
		t.mMainLight.getColor(c);
		c = scaled(c, kKeyGain);
		mMainLight->setColor(c);
		t.mSubLight.getColor(c);
		mSubLight->setColor(c);
		t.mSpecLight.getColor(c);
		mSpecLight->setColor(c);
		t.mAmbientLight.getColor(c);
		mAmbientLight.mColor = scaled(c, kAmbientGain);
		t.mFog.getColor(fog);
		t.mShadow.getColor(mShadowColor);
		// The block's own fog range: fog depends on the distance from the eye, so the map gets the
		// fog gameplay would give the same spots.
		mFogMgr->mNearZ = t.mFog.mFogParms.mStartZ();
		mFogMgr->mFarZ  = t.mFog.mFogParms.mEndZ();
	} else {
		Color4 main(220, 210, 200, 255), sub(40, 30, 30, 255), amb(70, 70, 80, 255);
		mMainLight->setColor(main);
		mSubLight->setColor(sub);
		mAmbientLight.mColor = amb;
	}
	fog.a = 255;
	mFogMgr->setColor(fog);

	// Sky: the fog colour at the horizon, leaning toward the time of day's zenith at the top.
	static const Color4 kZenith[4] = { Color4(110, 160, 235, 255), Color4(96, 150, 232, 255), Color4(52, 40, 92, 255),
		                               Color4(8, 10, 28, 255) };
	int t      = timeOfDay < 0 || timeOfDay > 3 ? 0 : timeOfDay;
	mSkyTop    = lerpColor(fog, kZenith[t], 0.72f);
	mSkyBottom = JUtility::TColor(fog.r, fog.g, fog.b, 255);
	mSkyZenith = JUtility::TColor(kZenith[t].r, kZenith[t].g, kZenith[t].b, 255);

	// Sun height. Gameplay puts the sun high over a camera that looks down from above; this camera
	// stands at knee height and looks across, so a sun that high would light the tops of the heads
	// and leave the faces dark. A lower key light per time of day keeps the faces lit; the distance
	// is light.ini's.
	static const f32 kSunElevDeg[4] = { 32.0f, 46.0f, 20.0f, 40.0f };
	f32 distance                    = setting != nullptr ? setting->mSunLight.mMoveParms.mDistance() : 20000.0f;
	f32 elev                        = sinf(kSunElevDeg[t] * kPi / 180.0f);
	f32 horiz = sqrtf(1.0f - elev * elev);
	f32 az    = kSunAzimuthDeg * kPi / 180.0f;
	Vector3f stageDir(-sinf(az) * horiz, elev, cosf(az) * horiz);
	mSunDir = frame.dirToWorld(stageDir);

	mDistance = distance;
	mOrigin   = frame.mOrigin;
	Vector3f sun(frame.mOrigin.x + mSunDir.x * distance, frame.mOrigin.y + mSunDir.y * distance, frame.mOrigin.z + mSunDir.z * distance);
	mMainLight->mPosition = sun;
	mSpecLight->mPosition = sun;
	mSubLight->mPosition  = Vector3f(frame.mOrigin.x - mSunDir.x * distance, frame.mOrigin.y - mSunDir.y * distance,
	                                 frame.mOrigin.z - mSunDir.z * distance);
}

void SceneLightMgr::capture(Look& look) const
{
	look.mMain    = mMainLight->mColor;
	look.mSub     = mSubLight->mColor;
	look.mSpec    = mSpecLight->mColor;
	look.mAmbient = mAmbientLight.mColor;
	look.mFog     = Color4(mFogMgr->mColor.r, mFogMgr->mColor.g, mFogMgr->mColor.b, 255);
	look.mShadow  = mShadowColor;
	look.mFogNear = mFogMgr->mNearZ;
	look.mFogFar  = mFogMgr->mFarZ;
	look.mSkyTop    = mSkyTop;
	look.mSkyBottom = mSkyBottom;
	look.mSkyZenith = mSkyZenith;
	look.mSunDir    = mSunDir;
}

void SceneLightMgr::retarget(Game::GameLightMgrSetting* setting, int timeOfDay, const StageFrame& frame, f32 seconds)
{
	Look from;
	capture(from); // where the scene is now, mid-blend included
	setup(setting, timeOfDay, frame);
	capture(mTo);
	mFrom = from;
	if (seconds <= 0.0f) {
		mBlendTime = 0.0f;
		mBlend     = 1.0f;
		return;
	}
	mBlendTime = seconds;
	mBlend     = 0.0f;
	apply(mFrom, mTo, 0.0f);
}

void SceneLightMgr::step(f32 dt)
{
	if (mBlendTime <= 0.0f) {
		return;
	}
	mBlend += dt / mBlendTime;
	if (mBlend >= 1.0f) {
		mBlend     = 1.0f;
		mBlendTime = 0.0f;
	}
	// Smoothstep: no kink at either end.
	f32 t = mBlend * mBlend * (3.0f - 2.0f * mBlend);
	apply(mFrom, mTo, t);
}

namespace {

Color4 lerp4(const Color4& a, const Color4& b, f32 t)
{
	return Color4((u8)(a.r + (b.r - a.r) * t + 0.5f), (u8)(a.g + (b.g - a.g) * t + 0.5f), (u8)(a.b + (b.b - a.b) * t + 0.5f),
	              (u8)(a.a + (b.a - a.a) * t + 0.5f));
}

JUtility::TColor lerpT(const JUtility::TColor& a, const JUtility::TColor& b, f32 t)
{
	return JUtility::TColor((u8)(a.r + (b.r - a.r) * t + 0.5f), (u8)(a.g + (b.g - a.g) * t + 0.5f),
	                        (u8)(a.b + (b.b - a.b) * t + 0.5f), 255);
}

} // namespace

void SceneLightMgr::apply(const Look& a, const Look& b, f32 t)
{
	Color4 c = lerp4(a.mMain, b.mMain, t);
	mMainLight->setColor(c);
	c = lerp4(a.mSub, b.mSub, t);
	mSubLight->setColor(c);
	c = lerp4(a.mSpec, b.mSpec, t);
	mSpecLight->setColor(c);
	mAmbientLight.mColor = lerp4(a.mAmbient, b.mAmbient, t);
	c                    = lerp4(a.mFog, b.mFog, t);
	mFogMgr->setColor(c);
	mFogMgr->mNearZ = a.mFogNear + (b.mFogNear - a.mFogNear) * t;
	mFogMgr->mFarZ  = a.mFogFar + (b.mFogFar - a.mFogFar) * t;
	mShadowColor    = lerp4(a.mShadow, b.mShadow, t); // in place: SceneShadows holds a pointer to it
	mSkyTop         = lerpT(a.mSkyTop, b.mSkyTop, t);
	mSkyBottom      = lerpT(a.mSkyBottom, b.mSkyBottom, t);
	mSkyZenith      = lerpT(a.mSkyZenith, b.mSkyZenith, t);

	Vector3f dir(a.mSunDir.x + (b.mSunDir.x - a.mSunDir.x) * t, a.mSunDir.y + (b.mSunDir.y - a.mSunDir.y) * t,
	             a.mSunDir.z + (b.mSunDir.z - a.mSunDir.z) * t);
	f32 len = sqrtf(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
	if (len > 0.0001f) {
		dir.x /= len;
		dir.y /= len;
		dir.z /= len;
		mSunDir = dir;
	}
	Vector3f sun(mOrigin.x + mSunDir.x * mDistance, mOrigin.y + mSunDir.y * mDistance, mOrigin.z + mSunDir.z * mDistance);
	mMainLight->mPosition = sun;
	mSpecLight->mPosition = sun;
	mSubLight->mPosition  = Vector3f(mOrigin.x - mSunDir.x * mDistance, mOrigin.y - mSunDir.y * mDistance, mOrigin.z - mSunDir.z * mDistance);
}

void SceneLightMgr::set(Graphics& gfx)
{
	setView(*gfx.mCurrentViewport->getMatrix(false));
	applyFog(gfx);
}

void SceneLightMgr::applyFog(Graphics& gfx)
{
	const f32 nearZ = mFogMgr->mNearZ, farZ = mFogMgr->mFarZ;
	mFogMgr->mNearZ += mFogPush;
	mFogMgr->mFarZ += mFogPush;
	mFogMgr->set(gfx);
	mFogMgr->mNearZ = nearZ;
	mFogMgr->mFarZ  = farZ;
}

void SceneLightMgr::setView(Matrixf& viewMtx)
{
	LightMgr::set(viewMtx);
}

void SceneLightMgr::setFog(Graphics& gfx)
{
	applyFog(gfx);
}

void SceneLightMgr::fogOff(Graphics& gfx)
{
	mFogMgr->off(gfx);
}

// ---- Shadows -----------------------------------------------------------------------------------

SceneShadows::SceneShadows()
    : mCylinder(nullptr)
    , mCount(0)
{
	for (int i = 0; i < MAX_CASTERS; i++) {
		mType[i] = 0;
	}
}

void SceneShadows::init(Color4* color)
{
	mCylinder = new Game::ShadowCylinder2(&mParms, color);
}

void SceneShadows::begin(Camera* camera)
{
	mCount = 0;
	if (mCylinder != nullptr) {
		mCylinder->setCameraParms(camera, 0);
	}
}

// Navi::getShadowParam / ShadowMgr::update: a vertical cylinder at the feet.
void SceneShadows::add(const Vector3f& pos, f32 radius, f32 height, f32 lift, Matrixf& viewMtx)
{
	if (mCylinder == nullptr || mCount >= MAX_CASTERS || radius <= 0.0f) {
		return;
	}
	Game::ShadowParam param;
	param.mPosition                 = pos;
	param.mPosition.y               += lift;
	param.mBoundingSphere.mRadius   = height;
	param.mBoundingSphere.mPosition = Vector3f(0.0f, 1.0f, 0.0f);
	param.mSize                     = radius;

	Matrixf world;
	mCylinder->makeSRT(world, param);
	PSMTXConcat(viewMtx.mMatrix.mtxView, world.mMatrix.mtxView, mMtx[mCount].mMatrix.mtxView);
	mType[mCount] = mCylinder->getCylinderType(param, 0);
	mCount++;
}

// ShadowMgr::draw (shadowMgr.cpp:275-310).
void SceneShadows::draw(Viewport* vp)
{
	if (mCylinder == nullptr || mCount == 0) {
		return;
	}
	Rectf box = vp->mBounds2;
	mCylinder->setShadowRect(box);
	mCylinder->setFilterTextureID(0);
	mCylinder->drawInit();
	vp->setProjection();
	vp->setViewport();
	for (int i = 0; i < mCount; i++) {
		mCylinder->drawCylinder(mMtx[i], mType[i]);
	}
	mCylinder->drawFinish();
}

} // namespace LauncherScene
