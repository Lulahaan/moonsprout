#include "og/Screen/callbackNodes.h"
#include "og/Screen/SunMeter.h"
#include "og/Screen/ogScreen.h"
#include "og/Sound.h"
#include "math.h"
#include "trig.h"

namespace og {
namespace Screen {
/**
 * @note Address: N/A
 * @note Size: 0xB4
 */
CallBack_SunMeter::CallBack_SunMeter()
{
	mTimer            = 0.0f;
	mCurrentTime      = nullptr;
	mStartPane        = nullptr;
	mEndPane          = nullptr;
	mSuniPane         = nullptr;
	mSun1Pane         = nullptr;
	mSun2Pane         = nullptr;
	mHasChimedNoon    = false;
	mHasChimedMorning = false;
	mHasChimedEvening = false;
	mScaleMgr         = new ScaleMgr;
}

/**
 * @note Address: N/A
 * @note Size: 0x108
 */
void CallBack_SunMeter::init(J2DScreen* canvas, f32* time)
{
	mCurrentTime      = time;
	mStartPane        = canvas->search('stat');
	mEndPane          = canvas->search('goal');
	mSuniPane         = canvas->search('suni');
	mSun1Pane         = canvas->search('sun1');
	mSun2Pane         = canvas->search('sun2');
	mHasChimedNoon    = false;
	mHasChimedMorning = false;
	mHasChimedEvening = false;
	mSuniPane->setBasePosition(J2DPOS_Center); PC_ONLY(mWideScreen = static_cast<P2DScreen::Mgr_tuning*>(canvas);)
}

/**
 * @note Address: 0x80307294
 * @note Size: 0x23C
 */
void CallBack_SunMeter::update()
{
	f32 currentTime = *mCurrentTime;
	// Between 0.297 - 0.3, chime
	if (0.297f < currentTime && currentTime < 0.3f && !mHasChimedMorning) {
		mHasChimedMorning = true;
		ogSound->setChime();
		startEffectChime();
	}

	// Between 0.497 - 0.5, chime
	if (0.497f < currentTime && currentTime < 0.5f && !mHasChimedNoon) {
		mHasChimedNoon = true;
		ogSound->setChimeNoon();
		startEffectChime();
	}

	// Between 0.697 - 0.7, chime
	if (0.697f < currentTime && currentTime < 0.7f && !mHasChimedEvening) {
		mHasChimedEvening = true;
		ogSound->setChime();
		startEffectChime();
	}

	mTimer += 1.0f; PC_ONLY(wideSpread();) // PC: spread the track (HUD Edges) before the sun is placed on it

	f32 x0 = mStartPane->getBounds()->i.x;
	f32 x1 = mEndPane->getBounds()->i.x;
	f32 y  = mSuniPane->getBounds()->i.y;
	f32 x  = currentTime * (x1 - x0) + x0;
	mSuniPane->move(x, y);

	mSun1Pane->rotate(mSun1Pane->getWidth() / 2, mSun1Pane->getHeight() / 2, J2DROTATE_Z, -mTimer);

	f32 sinVal = sin(-mTimer * DEG2RAD * PI * 2.0f);
	f32 alpha  = sinVal * 64.0f + 191.0f;

	mSun1Pane->setAlphaFromFloat(alpha);
	mSuniPane->updateScale(mScaleMgr->calc());
}

/**
 * @note Address: 0x803074D0
 * @note Size: 0x34
 */
void CallBack_SunMeter::startEffectChime()
{
	mScaleMgr->up(0.3f, 30.0f, 0.7f, 0.0f);
}

/**
 * @note Address: 0x80307504
 * @note Size: 0x44
 */
SunMeter::SunMeter()
{
	mCurrentTime = 0.0f;
}

/**
 * @note Address: 0x80307548
 * @note Size: 0x1B4
 */
void SunMeter::setCallBack()
{
	setAlphaScreen(this);
	mCallBack = new CallBack_SunMeter();
	mCallBack->init(this, &mCurrentTime);
	addCallBack('suni', mCallBack);
}
#if TARGET_PC
/**
 * PC: with the HUD at the screen edges the captains' gauges move left and the Day bubble moves
 * right (ASPECT 7). The track spreads with them: every static pane moves in proportion to where
 * it sits between the start and goal panes, so the start moves out as far as the left screens,
 * the goal as far as the right ones, and the dots stay evenly spaced. The sun itself follows,
 * placed between the moved start and goal in update().
 */
void CallBack_SunMeter::wideSpread()
{
	if (mWideScreen == nullptr || mWideScreen->mWideAnchor != 2) {
		return;
	}
	if (mWideCount < 0) {
		mWideCount = 0;
		J2DPane* root = mStartPane->getParentPane();
		for (J2DPane* pane = root->getFirstChildPane(); pane != nullptr && mWideCount < 24; pane = pane->getNextChildPane()) {
			if (pane == mSuniPane) {
				continue;
			}
			mWidePanes[mWideCount] = pane;
			mWideBaseX[mWideCount] = pane->getBounds()->i.x;
			mWideCount++;
		}
		mWideStartC = mStartPane->getBounds()->i.x + mStartPane->getWidth() * 0.5f;
		mWideEndC   = mEndPane->getBounds()->i.x + mEndPane->getWidth() * 0.5f;
	}
	const f32 scale  = mWideScreen->mScreenScaleX != 0.0f ? mWideScreen->mScreenScaleX : 1.0f;
	const f32 spread = p2_wide_hud_spread() / scale; // 2D units -> pane units (offset is post-scale)
	if (spread == mWideApplied || mWideEndC == mWideStartC) {
		return;
	}
	mWideApplied = spread;
	for (int i = 0; i < mWideCount; i++) {
		J2DPane* pane = mWidePanes[i];
		const f32 centre = mWideBaseX[i] + pane->getWidth() * 0.5f;
		const f32 t      = 2.0f * (centre - mWideStartC) / (mWideEndC - mWideStartC) - 1.0f;
		pane->move(mWideBaseX[i] + spread * t, pane->getBounds()->i.y);
	}
}
#endif

} // namespace Screen
} // namespace og
