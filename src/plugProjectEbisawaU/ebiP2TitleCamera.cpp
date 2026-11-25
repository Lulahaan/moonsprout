#include "ebi/title/TTitle.h"
#include "nans.h"

namespace ebi {
namespace title { PC_ONLY(f32 p2TitleIntroFovY(f32 fovY, f32 aspect, f32 height);)
/**
 * @note Address: 0x803C1EBC
 * @note Size: 0x74
 */

void TTitleCameraMgr::update()
{
	mPosition.y += mParms.mYOffset();
	mLookAtRotation.set(0.0f, 0.0f, -1.0f);

	mLookAtPosition = mPosition;
	mLookAtPosition.y -= 1.0f;
	mViewAngle = mParms.mFOVY(); PC_ONLY(mViewAngle = p2TitleIntroFovY(mViewAngle, mAspectRatio, mPosition.y);) // host: no plane edges in the wide intro

	Camera::update();
}

} // namespace title
} // namespace ebi

#if TARGET_PC
#include <math.h>

namespace ebi {
namespace title {

// Host only (docs/launcher/ASPECT.md 7). The intro's camera (cam.bck on the black plane) descends
// from far above onto the flower bed, which was modelled for the 19:14 picture. With Hor+ a wide
// window sees past the bed's left, right and bottom edges while the camera is still high. The
// lens therefore never shows more of the bed's width than the settled title screen does (which
// fits it at every aspect), and never less than the original 19:14 picture: high up (the tree)
// it is Vert- with the original horizontal view, near the end it widens back to full Hor+.
f32 p2TitleIntroFovY(f32 fovY, f32 aspect, f32 height)
{
	const f32 kAuthoredAspect = 19.0f / 14.0f;
	const f32 kSettledHeight  = 939.0f; // the camera's height over the bed once the descent ends
	if (aspect <= kAuthoredAspect || height <= kSettledHeight) {
		return fovY;
	}
	const f32 kRad = 3.14159265f / 180.0f;
	f32 tanV       = tanf(fovY * 0.5f * kRad);
	f32 full       = tanV * aspect;
	f32 authored   = tanV * kAuthoredAspect;
	f32 fit        = full * kSettledHeight / height;
	f32 tanH       = fit < authored ? authored : (fit > full ? full : fit);
	return 2.0f * atanf(tanH / aspect) / kRad;
}

} // namespace title
} // namespace ebi
#endif
