// The live start-menu scene section (docs/launcher/SCENE.md 6): lifecycle, per-frame update and
// the draw pass, which follows TTitleMgr::draw and BaseGameSection::newdraw_drawAll.
#include "SceneSection.h"
#include "SceneGround.h"
#include "SceneAudio.h"
#include "SceneHost.h"

#include "System.h"
#include "Graphics.h"
#include "IDelegate.h"
#include "JSystem/J3D/J3DDrawBuffer.h"
#include "JSystem/J3D/J3DModel.h"
#include "JSystem/J3D/J3DSys.h"
#include "JSystem/JFramework/JFWDisplay.h"
#include "JSystem/JUtility/JUTProcBar.h"
#include "PSM/ObjMgr.h"
#include "SysShape/Model.h"
#include "p2_wide.h"
#include <stdlib.h>

namespace LauncherScene {
namespace {

// Shadow cylinders (gameplay sizes: captain radius 4 / height 10, Pikmin 2 / 10, lifted 0.5;
// the ship like the pod, onyonMgr.cpp:377-381, scaled to its size).
const f32 kOlimarShadow[2] = { 4.0f, 10.0f };
const f32 kPikiShadow[2]   = { 2.0f, 10.0f };
const f32 kShipShadow[2]   = { 30.0f, 100.0f };
const f32 kShipShadowLift  = 60.0f;
// Sky gradient (drawSky): the fog colour up to kSkyBottomDeg above the horizon, mSkyTop at
// kSkyTopDeg, the zenith colour straight up. In the idle pose these two angles fall at 62% of the
// height and at the top edge, where the old two-band gradient had them, so the idle picture keeps
// its sky; the take-off's sky shots get the zenith.
const f32 kSkyBottomDeg = -1.5f;
const f32 kSkyTopDeg    = 21.0f;
const int kSkyCols      = 8;
const int kSkyRows      = 12;

// The ship's shadow is gone when it is this high. At 400 the shrinking disc still lay where the ship
// had stood long after the ship had left the top of the picture, a lone black smudge on the
// ground; gone by 120 it shrinks away while the ship is still in view.
const f32 kShipShadowGone  = 120.0f;
// The Onions' shadow: a disc under the body (feet reach 120 units out), gone by 150 units of lift.
const f32 kOnionShadow[2]  = { 50.0f, 110.0f };
const f32 kOnionShadowLift = 60.0f;
const f32 kOnionShadowGone = 150.0f;
// A Pikmin climbing an Onion's leg is off the ground: no shadow cylinder under it.
const f32 kOffGround = 2.0f;

// Live settings (live_options 3.3): a new time of day blends over this long; a music swap fades the
// old track out over kMusicFade first.
const f32 kLightBlend = 0.6f;
const f32 kMusicFade  = 0.25f;

JUtility::TColor fadeColor(unsigned rgb, f32 alpha)
{
	u8 a = alpha <= 0.0f ? 0 : (alpha >= 1.0f ? 255 : (u8)(alpha * 255.0f));
	return JUtility::TColor((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, a);
}

} // namespace

Section::Section(JKRHeap* heap)
    : Game::BaseHIOSection(heap)
    , mState(ST_Load)
    , mLoadOk(false)
    , mReadySent(false)
    , mFramesDrawn(0)
    , mLastRetrace(0)
    , mClockStarted(false)
    , mSpeed(1.0f)
    , mHold(0.0f)
    , mOpaBuffer(nullptr)
    , mXluBuffer(nullptr)
    , mLightMgr(nullptr)
    , mMap(nullptr)
    , mShip(nullptr)
    , mDebug(nullptr)
    , mFreeRun(false)
    , mLoaded(false)
    , mReload(false)
    , mConfigGen(0)
    , mLoadedArea(-1)
    , mPendingCmd(P2_SCENE_CMD_NONE)
    , mSwapPhase(SWAP_None)
    , mSwapMusic(0)
    , mSwapTime(0.0f)
{
	for (int i = 0; i < ACTOR_Count; i++) {
		mActors[i] = nullptr;
	}
	for (int i = 0; i < PIKI_ColourCount; i++) {
		mOnions[i] = nullptr;
	}
}

Section::~Section() { }

// Demo::Section::init (demoSection.cpp:65-92) and TTitleMgr::init (ebiP2Title.cpp:93-128).
void Section::init()
{
	initHIO(new Game::HIORootNode(this));
	// With an alpha channel in the frame buffer, as BaseGameSection: the shadow cylinders count in it.
	setDisplay(JFWDisplay::createManager(nullptr, mDisplayHeap, JUTXfb::DoubleBuffer, true), 1);
	sys->setFrameRate(1); // 60 fps, as the title screen
	PSM::ObjMgr::newInstance();
	mTimeStep = 0.0f; // the scene does its own fades
	JUTProcBar::sManager->setVisible(false);
	JUTProcBar::sManager->setVisibleHeapBar(false);

	mOpaBuffer = new J3DDrawBuffer(0x400);
	mXluBuffer = new J3DDrawBuffer(0x400);
	mXluBuffer->setZSort();
	j3dSys.setDrawBuffer(mOpaBuffer, J3DSys::SYSDRAW_Opa);
	j3dSys.setDrawBuffer(mXluBuffer, J3DSys::SYSDRAW_Xlu);
	mOpaBuffer->frameInit();
	mXluBuffer->frameInit();

	Graphics* gfx = sys->mGfx;
	gfx->allocateViewports(1);
	mViewport.mCamera = &mCamera;
	Rectf rect(0.0f, 0.0f, sys->getRenderModeObj()->fbWidth, sys->getRenderModeObj()->efbHeight);
	mViewport.setRect(rect);
	mViewport.updateCameraAspect();
	gfx->addViewport(&mViewport);

	mConfigGen = p2_launcher_scene_config_gen();
	p2_launcher_scene_config(&mWorld.mConfig);
	mLoadedArea = mWorld.mConfig.area;
	P2SceneDebug debug = { 0 };
	p2_launcher_scene_debug(&debug);
	mWorld.mTune   = debug.tune != 0;
	mSpeed         = debug.speed > 0.0f ? debug.speed : 1.0f;
	mHold          = debug.hold > 0.0f ? debug.hold : 0.0f;
	Actor::sClipLog = debug.clipLog != 0;
	if (debug.hasCam) {
		mCamera.mDebugCam            = true;
		mCamera.mDebugPose.mPos      = Vector3f(debug.cam[0], debug.cam[1], debug.cam[2]);
		mCamera.mDebugPose.mTarget   = Vector3f(debug.cam[3], debug.cam[4], debug.cam[5]);
		mCamera.mDebugPose.mFovY     = debug.cam[6];
		mCamera.mDebugPose.mUp       = Vector3f(debug.cam[7], debug.cam[8], debug.cam[9]);
	}
	mWorld.mStage  = &getStage(mWorld.mConfig.area);
	mWorld.mAssets = &mAssets;
	mWorld.mCamera = &mCamera;
	mWorld.mFrame.set(*mWorld.mStage);
	mCamera.setStage(&mWorld.mFrame);
	if (mWorld.mTune) {
		mDebug = new SceneDebug;
	}
	OSReport("scene: %s, time %d, music %d, take-off %d, live %d (system heap free %u)\n", mWorld.mStage->mName,
	         mWorld.mConfig.timeOfDay, mWorld.mConfig.music, mWorld.mConfig.takeoff, mWorld.mConfig.live,
	         JKRHeap::getSystemHeap() != nullptr ? JKRHeap::getSystemHeap()->getTotalFreeSize() : 0u);
	p2_launcher_scene_state(P2_SCENE_LOADING);
}

void Section::doLoadingStart()
{
	sys->dvdLoadUseCallBack(&mThreadCommand, new Delegate<Section>(this, &Section::loadResource));
}

// The sound scene always exists (the menu's sound effects use it); the area's assets only load for
// a live scene. A parked one loads them when it is first unparked (a reload).
void Section::loadResource()
{
	SceneAudio::createScene(mWorld.mConfig.music);
	if (!mWorld.mConfig.live) {
		mLoadOk = true;
		return;
	}
	mLoadOk = mAssets.load(*mWorld.mStage);
	if (mLoadOk) {
		mEffects.load();
	}
}

void Section::loadMusic()
{
	SceneAudio::createScene(mSwapMusic);
}

bool Section::doLoading()
{
	if (!sys->dvdLoadSyncNoBlock(&mThreadCommand)) {
		return true;
	}
	if (mLoadOk && !mWorld.mConfig.live) {
		OSReport("scene: parked\n");
		mState = ST_Parked;
		p2_launcher_scene_state(P2_SCENE_PARKED);
	} else if (mLoadOk) {
		buildScene();
		mLoaded = true;
		mState  = ST_Idle;
	} else {
		OSReport("scene: cannot start: %s\n", mAssets.mError != nullptr ? mAssets.mError : "unknown error");
		mState = ST_Failed;
		p2_launcher_scene_state(P2_SCENE_FAILED);
	}
	return false;
}

// Main thread: models and actors from the loaded data, then one update so the first frame drawn
// is complete. READY follows once that frame is on screen (doUpdate).
void Section::buildScene()
{
	mLightMgr     = new SceneLightMgr;
	mWorld.mLight = mLightMgr;
	mLightMgr->setup(mAssets.mLightSetting, mWorld.mConfig.timeOfDay, mWorld.mFrame);
	mShadows.init(&mLightMgr->mShadowColor);

	if (mAssets.mMapData != nullptr) {
		mMap = new MapActor;
		mMap->init(mAssets.mMapData, mAssets.mMapTexAnimCount, mAssets.mMapTexAnims);
	}
	mAssets.mWater.init();
	if (mAssets.mShipData != nullptr) {
		mShip = new ShipActor;
		mShip->init(mAssets.mShipData, mAssets.mShipAnimMgr, &mAssets.mShipTev[0], &mAssets.mShipTev[1]);
		mShip->mPos     = mWorld.mFrame.mOrigin;
		mShip->mFaceDir = mWorld.mStage->mShipYawDeg * 3.14159265f / 180.0f;
		mWorld.mShip    = mShip;
	}
	// The Onions stand round the landing site from the start, out of the idle picture (SCENE.md
	// 11.2); the take-off turns the camera on them.
	if (mAssets.mOnionData != nullptr) {
		for (int i = 0; i < PIKI_ColourCount; i++) {
			const OnionSpot& spot = onionSpot(*mWorld.mStage, i);
			mOnions[i]            = new OnionActor;
			mOnions[i]->mPos      = mWorld.stagePos(spot.mX, spot.mZ);
			mOnions[i]->mFaceDir  = mWorld.mFrame.yawToWorld(spot.mYawDeg * 3.14159265f / 180.0f);
			mOnions[i]->init(i, mAssets.mOnionData, mAssets.mOnionAnimMgr, &mAssets.mOnionTev[i], mEffects.onionFlightAnim(i));
			mWorld.mOnions[i] = mOnions[i];
		}
	}

	// Olimar as NaviMgr::createModel; the Pikmin as PikiMgr::createModelCallback.
	SysShape::Model* olimar = new SysShape::Model(mAssets.mOlimarData, 0, 2);
	mActors[ACTOR_Olimar]   = new Actor;
	mActors[ACTOR_Olimar]->init(ACTOR_Olimar, olimar, mAssets.mAnimMgr);
	for (int i = 0; i < PIKI_ColourCount; i++) {
		SysShape::Model* model = new SysShape::Model(mAssets.mPikiData[i], 0x20000, 1);
		model->getJ3DModel()->calcMaterial();
		model->getJ3DModel()->makeDL();
		model->getJ3DModel()->lock();
		mActors[ACTOR_Red + i] = new Actor;
		mActors[ACTOR_Red + i]->init(ACTOR_Red + i, model, mAssets.mAnimMgr);
	}
	for (int i = 0; i < ACTOR_Count; i++) {
		mWorld.mActors[i] = mActors[i];
	}
	mDirector.init(mWorld);
	mEffects.start(*mWorld.mStage, mWorld.mFrame);
	mTakeoff.init(&mEffects);
	updateScene(0.0f);
}

// The scene's time base. While the host runs the scene free (a Frame Rate above Original) it
// reports each tick's real length; otherwise frames are paced by whole retraces, so the retrace
// count since the last frame is exact and free of jitter (0 is valid: that frame does not advance;
// capped at 4 so a stall does not jump). Everything in the scene takes this dt, so it moves at
// the same speed at any frame rate. Test switches: MPPM_SCENE_SPEED scales it; MPPM_SCENE_HOLD
// stops it once the take-off script clock reaches the hold time.
f32 Section::frameDt()
{
	u32 now = VIGetRetraceCount();
	u32 n   = mClockStarted ? now - mLastRetrace : 1;
	mLastRetrace  = now;
	mClockStarted = true;
	if (n > 4) {
		n = 4;
	}
	f32 dt = p2_host_tick_dt();
	if (dt <= 0.0f) {
		dt = (f32)n * 1001.0f / 60000.0f;
	}
	dt *= mSpeed;
	if (mHold > 0.0f && mState == ST_Takeoff) {
		f32 left = mHold - mTakeoff.scriptTime();
		if (dt > left) {
			dt = left > 0.0f ? left : 0.0f;
		}
	}
	return dt;
}

bool Section::doUpdate()
{
	// M5.1b: the launcher scene is presentation; its rand() calls never touch the sim stream.
	P2RngPres p2RngPres;
	// The host's real tick length while it runs the scene free (FreeRun, a Frame Rate above
	// Original), else the original 1/60 (sys->setFrameRate(1) in init). Material animators scale by
	// it, and the host sums it for its speed check (p2_game_tick_seconds).
	f32 hostDt = p2_host_tick_dt();
	sys->mDeltaTime = hostDt > 0.0f ? hostDt : 1.0f / 60.0f;
	f32 dt  = frameDt();
	int cmd = p2_launcher_scene_command();

	// A Play or Skip during a music swap waits for it: the section must not end, nor the take-off
	// start, while the DVD thread builds the new sound scene.
	if (swapping()) {
		if (cmd != P2_SCENE_CMD_NONE) {
			mPendingCmd = cmd;
			cmd         = P2_SCENE_CMD_NONE;
		}
		stepMusicSwap();
	}
	if (!swapping() && mPendingCmd != P2_SCENE_CMD_NONE && cmd == P2_SCENE_CMD_NONE) {
		cmd         = mPendingCmd;
		mPendingCmd = P2_SCENE_CMD_NONE;
	}
	// Settings changed on the start menu (live_options 3.3), before the READY logic below.
	if ((mState == ST_Idle || mState == ST_Parked || mState == ST_Failed) && p2_launcher_scene_config_gen() != mConfigGen) {
		applyConfig();
	}

	// One tick per presented frame while the scene plays (the host honours it only when the Frame
	// Rate is not Original). The request lasts one tick, so it is renewed every frame: whatever
	// section follows never inherits it. Everything in the scene moves by dt, so it runs at the same
	// speed at any rate. Back to the retrace pacing when the scene is done.
	if (mState == ST_Idle || mState == ST_Takeoff || mState == ST_Parked) {
		mFreeRun = true;
		p2_host_free_run(1);
	}

	switch (mState) {
	case ST_Idle:
		if (mFramesDrawn > 0 && !mReadySent) {
			mReadySent = true;
			p2_launcher_scene_state(P2_SCENE_READY);
			if (!swapping()) {
				SceneAudio::startMusic(); // else the swap starts the new track when it ends
			}
		}
		if (cmd == P2_SCENE_CMD_PLAY || cmd == P2_SCENE_CMD_SKIP) {
			mDirector.release(mWorld);
			mTakeoff.start(mWorld, cmd == P2_SCENE_CMD_SKIP ? 0 : mWorld.mConfig.takeoff);
			mState = ST_Takeoff;
		}
		break;
	case ST_Takeoff:
		if (cmd == P2_SCENE_CMD_SKIP) {
			mTakeoff.skip(mWorld);
		}
		break;
	case ST_Failed:
	case ST_Parked:
		// Nothing to show: wait for Play, then leave at once (the title follows).
		if (cmd == P2_SCENE_CMD_PLAY || cmd == P2_SCENE_CMD_SKIP) {
			mState        = ST_Done;
			mIsMainActive = false;
		}
		break;
	default:
		break;
	}

	if (mState == ST_Idle || mState == ST_Takeoff) {
		// Particles step at the game's 60 a second while idle; Takeoff::cut moves them to the
		// day-end movies' 30 from the first cut on, with one step on every cut.
		if (mState == ST_Takeoff && mTakeoff.update(mWorld, dt)) {
			mState        = ST_Done;
			mIsMainActive = false;
		}
		updateScene(dt);
	}
	if (mState == ST_Done) {
		endFreeRun();
	}

	BaseHIOSection::doUpdate();
	return mIsMainActive;
}

// The scene's ticks go back to the original retrace pacing and frame time (the title follows).
void Section::endFreeRun()
{
	if (mFreeRun) {
		mFreeRun = false;
		p2_host_free_run(0);
	}
	sys->setFrameRate(1);
}

// One frame of the world: behaviour, camera, then every model's animation, matrices and draw
// entry (calc -> entry -> viewCalc, the title screen's order).
void Section::updateScene(f32 dt)
{
	mWorld.mTime += dt;
	mLightMgr->step(dt);
	if (mDebug != nullptr) {
		mDebug->update(mWorld, mDirector, dt);
	}
	mDirector.update(mWorld, dt);

	mViewport.updateCameraAspect(); // follows the window shape (the aspect work scales it by k)
	mCamera.update(dt, mWorld.mConfig.reduceMotion != 0);
	Matrixf* view = mCamera.getViewMatrix(false);
	j3dSys.setViewMtx(view->mMatrix.mtxView);

	SysShape::Model::setViewCalcModeImm();
	if (mLightMgr != nullptr) {
		// The aerial shot: the fog starts as much further off as the eye has climbed above the site.
		f32 above           = mCamera.mPosition.y - mWorld.mFrame.mOrigin.y - 40.0f;
		mLightMgr->mFogPush = (mTakeoff.aerialShot() && !mTakeoff.skyShot() && above > 0.0f) ? above : 0.0f;
	}
	if (mMap != nullptr) {
		mMap->update(dt);
		mMap->showShafts(!mTakeoff.aerialShot());
		if (!mTakeoff.skyShot()) {
			mMap->entry();
			mMap->viewCalc();
		}
	}
	mAssets.mWater.update(dt);
	if (!mTakeoff.skyShot()) {
		mAssets.mWater.entry();
		mAssets.mWater.viewCalc();
	}
	if (mShip != nullptr) {
		mShip->update(dt);
		mShip->entry();
		mShip->viewCalc();
	}
	for (int i = 0; i < PIKI_ColourCount; i++) {
		if (OnionActor* o = mOnions[i]) {
			o->update(dt);
			o->entry();
			o->viewCalc();
		}
	}
	for (int i = 0; i < ACTOR_Count; i++) {
		if (Actor* a = mActors[i]) {
			a->update(dt, mDebug == nullptr || mDebug->animates(i));
			a->entry();
			a->viewCalc();
		}
	}

	// Shadows, in the view of this frame.
	mShadows.begin(&mCamera);
	for (int i = 0; i < ACTOR_Count; i++) {
		Actor* a = mActors[i];
		if (a == nullptr || !a->mVisible || a->mPos.y > mWorld.mFrame.mOrigin.y + kOffGround) {
			continue;
		}
		const f32* size = i == ACTOR_Olimar ? kOlimarShadow : kPikiShadow;
		mShadows.add(a->mPos, size[0] * a->mScale, size[1], 0.5f, *view);
	}
	if (mShip != nullptr && mShip->mVisible) {
		f32 height = mShip->liftHeight();
		f32 shrink = height <= 0.0f ? 1.0f : (height >= kShipShadowGone ? 0.0f : 1.0f - height / kShipShadowGone);
		mShadows.add(mShip->mPos, kShipShadow[0] * shrink, kShipShadow[1], kShipShadowLift, *view);
	}
	for (int i = 0; i < PIKI_ColourCount; i++) {
		OnionActor* o = mOnions[i];
		if (o == nullptr || !o->mVisible || o->mLift >= kOnionShadowGone) {
			continue;
		}
		f32 shrink = o->mLift <= 0.0f ? 1.0f : 1.0f - o->mLift / kOnionShadowGone;
		mShadows.add(o->mPos, kOnionShadow[0] * shrink, kOnionShadow[1], kOnionShadowLift, *view);
	}

	mEffects.update(mCamera.mPosition, dt);
}

void Section::doDraw(Graphics& gfx)
{
	if (mState == ST_Load || mState == ST_Failed || mState == ST_Parked) {
		return; // the launcher's backdrop covers the screen
	}
	if (mState == ST_Done && mReload) {
		return; // ending for a reload behind the backdrop
	}

	drawSky(gfx);

	Viewport* vp = &mViewport;
	vp->setViewport();
	vp->setProjection();
	gfx.mCurrentViewport = vp;
	mLightMgr->set(gfx);

	j3dSys.drawInit();
	j3dSys.mDrawMode = J3DSys::SYSDRAW_Unk3;
	mOpaBuffer->draw();
	GroundDetail::draw(vp, mWorld.mFrame, mCamera.mPosition, groundDetailStrength());
	mLightMgr->set(gfx);
	Actor* pikmin[PIKI_ColourCount] = { mActors[ACTOR_Red], mActors[ACTOR_Yellow], mActors[ACTOR_Blue] };
	drawLeaves(mAssets, pikmin, PIKI_ColourCount, vp, mLightMgr->mSunDir);
	mLightMgr->set(gfx);
	j3dSys.mDrawMode = J3DSys::SYSDRAW_Unk4;
	mXluBuffer->draw();

	j3dSys.drawInit();
	mShadows.draw(vp);
	vp->setViewport();
	vp->setProjection();
	mEffects.draw(gfx, vp, mLightMgr);

	j3dSys.reinitGX();
	mOpaBuffer->frameInit();
	mXluBuffer->frameInit();

	if (mTakeoff.fadeAlpha() > 0.0f) {
		drawFade(gfx, mTakeoff.fadeAlpha());
	}
	if (mDebug != nullptr) {
		mDebug->draw(gfx, mWorld);
	}
	mFramesDrawn++;
}

// SCENE.md 10.4: the grain over the near ground. MPPM_SCENE_DETAIL overrides the strength (0 = off).
f32 Section::groundDetailStrength() const
{
	static f32 sOverride = -2.0f;
	if (sOverride == -2.0f) {
		const char* env = getenv("MPPM_SCENE_DETAIL");
		sOverride       = env != nullptr ? (f32)atof(env) : -1.0f;
	}
	if (sOverride >= 0.0f) {
		return sOverride;
	}
	return mWorld.mStage != nullptr ? mWorld.mStage->mGroundDetail : 1.0f;
}

// Overworld maps have no sky model; the game shows the fog colour. The sky behind the map is a
// grid of boxes whose corner colours follow the elevation of the view ray through each corner (from
// the camera's basis), so the gradient stays on the horizon whatever the camera does: the fog
// colour at the horizon, mSkyTop at kSkyTopDeg, the zenith colour straight up.
void Section::drawSky(Graphics& gfx)
{
	J2DPerspGraph* graf = &gfx.mPerspGraph;
	graf->setPort();
	// setPort narrows the 2D viewport to the centre 19:14 area on a wide picture, and only a single
	// full-width box is widened back (J2DGrafContext's wide fill). The grid's columns are narrower,
	// so the whole frame's viewport is set here and the grid spans the port's 0..width: every
	// column then lands on its own part of the window, edge to edge.
	const f32 x0 = 0.0f;
	const f32 x1 = sys->getRenderModeObj()->fbWidth;
	const f32 h  = sys->getRenderModeObj()->efbHeight;
	GXSetViewport(0.0f, 0.0f, x1, h, 0.0f, 1.0f);

	// View-space rows of the look matrix are the camera's right, up and back axes in the world.
	const Mtx& m     = mCamera.mLookMatrix.mMatrix.mtxView;
	const f32 tanY   = tanf(mCamera.mViewAngle * 0.5f * 3.14159265f / 180.0f);
	const f32 tanX   = tanY * (mCamera.mAspectRatio > 0.0f ? mCamera.mAspectRatio : 16.0f / 9.0f);
	const f32 upY    = m[1][1];
	const f32 rightY = m[0][1];
	const f32 fwdY   = -m[2][1];

	JUtility::TColor colour[kSkyRows + 1][kSkyCols + 1];
	for (int r = 0; r <= kSkyRows; r++) {
		f32 ny = 1.0f - 2.0f * (f32)r / (f32)kSkyRows; // +1 top .. -1 bottom
		for (int c = 0; c <= kSkyCols; c++) {
			f32 nx = -1.0f + 2.0f * (f32)c / (f32)kSkyCols;
			// The ray through this point: forward + nx * tanX * right + ny * tanY * up (world).
			f32 dx = -m[2][0] + nx * tanX * m[0][0] + ny * tanY * m[1][0];
			f32 dy = fwdY + nx * tanX * rightY + ny * tanY * upY;
			f32 dz = -m[2][2] + nx * tanX * m[0][2] + ny * tanY * m[1][2];
			f32 elev = atan2f(dy, sqrtf(dx * dx + dz * dz)) * 180.0f / 3.14159265f;
			colour[r][c] = skyColour(elev);
		}
	}
	for (int r = 0; r < kSkyRows; r++) {
		f32 y0 = h * (f32)r / (f32)kSkyRows;
		f32 y1 = h * (f32)(r + 1) / (f32)kSkyRows;
		for (int c = 0; c < kSkyCols; c++) {
			f32 bx0 = x0 + (x1 - x0) * (f32)c / (f32)kSkyCols;
			f32 bx1 = x0 + (x1 - x0) * (f32)(c + 1) / (f32)kSkyCols;
			graf->setColor(colour[r][c], colour[r][c + 1], colour[r + 1][c], colour[r + 1][c + 1]);
			JGeometry::TBox2f box(bx0, y0, bx1, y1);
			graf->fillBox(box);
		}
	}
	graf->setPort(); // back to the narrowed viewport for the 2D that follows
}

// The sky colour at `elevDeg` above the horizon (drawSky).
JUtility::TColor Section::skyColour(f32 elevDeg) const
{
	const JUtility::TColor& lo  = mLightMgr->mSkyBottom;
	const JUtility::TColor& mid = mLightMgr->mSkyTop;
	const JUtility::TColor& hi  = mLightMgr->mSkyZenith;
	if (elevDeg <= kSkyBottomDeg) {
		return lo;
	}
	const JUtility::TColor* a = &lo;
	const JUtility::TColor* b = &mid;
	f32 t                     = (elevDeg - kSkyBottomDeg) / (kSkyTopDeg - kSkyBottomDeg);
	if (elevDeg > kSkyTopDeg) {
		a = &mid;
		b = &hi;
		t = (elevDeg - kSkyTopDeg) / (90.0f - kSkyTopDeg);
		t = t > 1.0f ? 1.0f : t;
	}
	return JUtility::TColor((u8)(a->r + (b->r - a->r) * t), (u8)(a->g + (b->g - a->g) * t), (u8)(a->b + (b->b - a->b) * t), 255);
}

// The end fade, in the colour the launcher's flash uses (P2SceneConfig.fadeColor).
void Section::drawFade(Graphics& gfx, f32 alpha)
{
	J2DPerspGraph* graf = &gfx.mPerspGraph;
	graf->setPort();
	graf->setColor(fadeColor(mWorld.mConfig.fadeColor, alpha));
	JGeometry::TBox2f box(p2_wide_min_x(), 0.0f, p2_wide_max_x(), sys->getRenderModeObj()->efbHeight);
	graf->fillBox(box);
}

void Section::doExit()
{
	endFreeRun();
	mEffects.exit();
	SceneAudio::destroy();
	j3dSys.setDrawBuffer(nullptr, J3DSys::SYSDRAW_Opa);
	j3dSys.setDrawBuffer(nullptr, J3DSys::SYSDRAW_Xlu);
	if (mReload) {
		p2_launcher_scene_reloading(); // GameFlow creates the next scene section (H1)
	} else {
		p2_launcher_scene_state(P2_SCENE_DONE);
	}
}

// ---- Live settings (live_options 3.3) ----------------------------------------------------------

// Applies the handed config. Structural changes end the section to be created again behind the
// backdrop; the rest change the running scene. A part that cannot apply during a music swap is
// left for a later frame (mConfigGen only follows once everything applied).
void Section::applyConfig()
{
	unsigned gen = p2_launcher_scene_config_gen();
	P2SceneConfig cfg;
	p2_launcher_scene_config(&cfg);
	bool done = true;

	// 1. A live scene in another area, or one unparked with nothing loaded: a new section. A failed
	// section only retries for another area.
	if (cfg.live && (cfg.area != mLoadedArea || (!mLoaded && mState != ST_Failed))) {
		if (swapping()) {
			return; // after the swap: the section must not end while the DVD thread works
		}
		OSReport("scene: reload for area %d (loaded %d, assets %d)\n", cfg.area, mLoadedArea, mLoaded ? 1 : 0);
		mReload       = true;
		mState        = ST_Done;
		mIsMainActive = false;
		mConfigGen    = gen;
		return;
	}
	// 2. Park or unpark (an area change while parked is only recorded: step 1 runs on unpark).
	if (cfg.live != mWorld.mConfig.live && mState != ST_Failed) {
		if (swapping()) {
			done = false;
		} else {
			mWorld.mConfig.live = cfg.live;
			if (cfg.live) {
				unpark();
			} else {
				park();
			}
		}
	} else if (mState == ST_Failed) {
		mWorld.mConfig.live = cfg.live;
	}

	// 3. Time of day: a short light blend (a snap while nothing is drawn).
	if (cfg.timeOfDay != mWorld.mConfig.timeOfDay) {
		mWorld.mConfig.timeOfDay = cfg.timeOfDay;
		if (mLightMgr != nullptr) {
			mLightMgr->retarget(mAssets.mLightSetting, cfg.timeOfDay, mWorld.mFrame, mState == ST_Idle ? kLightBlend : 0.0f);
		}
	}

	// 4. Music: one swap at a time; a later request is picked up once this one ends.
	if (cfg.music != mWorld.mConfig.music) {
		if (swapping()) {
			done = false;
		} else {
			OSReport("scene: music %d -> %d\n", mWorld.mConfig.music, cfg.music);
			mWorld.mConfig.music = cfg.music;
			mSwapMusic           = cfg.music;
			mSwapPhase           = SWAP_Fading;
			mSwapTime            = 0.0f;
			SceneAudio::fadeOut(kMusicFade);
		}
	}

	// 5. Read at Play or every frame: copied as they are.
	mWorld.mConfig.takeoff      = cfg.takeoff;
	mWorld.mConfig.reduceMotion = cfg.reduceMotion;
	mWorld.mConfig.fadeColor    = cfg.fadeColor;
	if (done) {
		mConfigGen = gen;
	}
}

// Not drawn and silent; the assets stay loaded. The menu's backdrop covers the screen.
void Section::park()
{
	OSReport("scene: park\n");
	SceneAudio::fadeOut(kMusicFade);
	mState       = ST_Parked;
	mReadySent   = false;
	mFramesDrawn = 0;
	// The last updateScene left entries in the draw buffers; nothing draws them now.
	if (mOpaBuffer != nullptr) {
		mOpaBuffer->frameInit();
		mXluBuffer->frameInit();
	}
	p2_launcher_scene_state(P2_SCENE_PARKED);
}

// Back to the idle scene: READY and the music follow the first frame drawn (doUpdate).
void Section::unpark()
{
	OSReport("scene: unpark\n");
	mState        = ST_Idle;
	mClockStarted = false;
	mReadySent    = false;
	mFramesDrawn  = 0;
	// doUpdate runs updateScene for ST_Idle right after this; a second call this frame would enter
	// every model in the draw buffers twice.
}

// Fade the old track out, replace the sound scene on the DVD thread, start the new track if the
// scene is on screen.
void Section::stepMusicSwap()
{
	switch (mSwapPhase) {
	case SWAP_Fading:
		mSwapTime += sys->mDeltaTime;
		if (mSwapTime >= kMusicFade) {
			SceneAudio::destroy();
			mSwapPhase = SWAP_Loading;
			sys->dvdLoadUseCallBack(&mSwapCommand, new Delegate<Section>(this, &Section::loadMusic));
		}
		break;
	case SWAP_Loading:
		if (sys->dvdLoadSyncNoBlock(&mSwapCommand)) {
			mSwapPhase = SWAP_None;
			OSReport("scene: music %d ready\n", mSwapMusic);
			if (mState == ST_Idle && mReadySent) {
				SceneAudio::startMusic();
			}
		}
		break;
	default:
		break;
	}
}

} // namespace LauncherScene

// Hook H1 (GameFlow::createSection): the scene section, in the root menu's child heap.
ISection* p2_launcher_scene_create(JKRHeap* heap)
{
	return new LauncherScene::Section(heap);
}
