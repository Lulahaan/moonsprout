// Stage table for the start-menu scene (docs/launcher/SCENE.md 3.5, 8.1).
#include "SceneStage.h"

#include <math.h>
#include <stdlib.h>

namespace LauncherScene {
namespace {

// The take-off's S4 from high behind the Onions (checked at 16:9 and 21:9 in the three areas), and
// lower top edges for the shots whose backdrop rim is low (SCENE.md 11.3): in Valley of Repose the
// wall facing away from the ship (S2, S2b) is bare white snow under a blue sky, in Wistful Wild the
// backdrop's corner and the sky above it showed in S2, S3 and S5.
const StageShots kOpenShots   = { Vector3f(-120.0f, 260.0f, 820.0f), Vector3f(0.0f, 60.0f, 300.0f), 46.0f, 24.0f, 0.0f };
const StageShots kValleyShots = { Vector3f(-120.0f, 260.0f, 820.0f), Vector3f(0.0f, 60.0f, 300.0f), 46.0f, 24.0f, 14.0f };
const StageShots kWildShots   = { Vector3f(-120.0f, 260.0f, 820.0f), Vector3f(0.0f, 60.0f, 300.0f), 46.0f, 19.0f, 15.0f };

// Ship positions and yaws from disc:user/Abe/map/<area>/defaultgen.txt (the `onyn` entry with
// onyon index 4). Ambient emitters are the s00_coursein ids per area (SCENE.md 3.4). The trailing
// fields: ground grain strength, air particle size and near cull, hidden (SceneStage.h).
const StageInfo sStages[] = {
	{ "Valley of Repose", "/user/Kando/map/tutorial", "/user/Abe/map/tutorial", Vector3f(-137.643f, 0.0f, 3033.014f), -121.22f,
	  -121.22f, 14, Vector3f(10.0f, -1060.0f, 290.0f), nullptr, false, &kValleyShots, 0.7f },
	{ "Awakening Wood", "/user/Kando/map/forest", "/user/Abe/map/forest", Vector3f(638.355f, -70.0f, 2639.886f), -91.22f, -91.22f, 13,
	  Vector3f(10.0f, -1060.0f, 290.0f), nullptr },
	// Perplexing Pool: the ship stands in a walled concrete pit; from 11 units up the picture is
	// bare concrete, its wall and the area's pale backdrop at every heading. Hidden until it gets
	// its own idle framing.
	{ "Perplexing Pool", "/user/Kando/map/yakushima", "/user/Abe/map/yakushima", Vector3f(-331.455f, 80.0f, 1228.324f), -171.525f,
	  -171.525f, 0xFFFF, Vector3f(10.0f, -1060.0f, 290.0f), nullptr, false, &kOpenShots, 1.0f, 1.0f, 95.0f, true },
	{ "Wistful Wild", "/user/Kando/map/last", "/user/Abe/map/last", Vector3f(-3130.88f, 0.0f, -1043.1f), -41.22f, -41.22f, 15,
	  Vector3f(10.0f, -1060.0f, 290.0f), nullptr, true, &kWildShots, 1.0f, 0.45f, 150.0f },
};

const f32 kDegToRad = 3.14159265f / 180.0f;

} // namespace

// SCENE.md 11.2. The idle eye stands at (20, 11, 354), inside the landing circle: yellow is behind
// it, blue beside it, red behind and to its left, so no Onion part is in the idle picture at 4:3,
// 16:9 or 21:9. The yaws keep every foot off the cast's runs and away from the lens.
const OnionSpot kOnionSpots[3] = {
	{ -215.0f, 450.0f, -56.0f }, // red (authored at (-167, 203))
	{ 19.0f, 513.0f, 180.0f },   // yellow (defaultgen.txt: 184.1)
	{ 166.0f, 268.0f, -90.0f },  // blue (defaultgen.txt: -93.8)
};

const OnionSpot& onionSpot(const StageInfo& stage, int colour)
{
	if (colour < 0 || colour > 2) {
		colour = 0;
	}
	return stage.mOnions != nullptr ? stage.mOnions[colour] : kOnionSpots[colour];
}

const StageInfo& getStage(int area)
{
	if (area < 0 || area >= (int)(sizeof(sStages) / sizeof(sStages[0]))) {
		area = 1;
	}
	// A hidden area (reached only through Follow the Season, or a hand-edited config) shows
	// Awakening Wood. Test switch: MPPM_SCENE_ALL_AREAS=1 shows it anyway.
	static const bool sAll = getenv("MPPM_SCENE_ALL_AREAS") != nullptr;
	if (sStages[area].mHidden && !sAll) {
		area = 1;
	}
	return sStages[area];
}

void StageFrame::set(const StageInfo& stage)
{
	mOrigin  = stage.mShipPos;
	mAzimuth = stage.mAzimuthDeg * kDegToRad;
	// Test switch: MPPM_SCENE_AZIMUTH=<deg> turns the staging that much further round the ship.
	if (const char* env = getenv("MPPM_SCENE_AZIMUTH")) {
		mAzimuth += (f32)atof(env) * kDegToRad;
	}
	mSin     = sinf(mAzimuth);
	mCos     = cosf(mAzimuth);
}

// Rotation about Y by the azimuth in the J3D sense (local +Z ends up at (sin a, 0, cos a)), which
// is what Matrixf::makeSRT does with a rotation of (0, a, 0).
Vector3f StageFrame::dirToWorld(const Vector3f& d) const
{
	return Vector3f(mCos * d.x + mSin * d.z, d.y, -mSin * d.x + mCos * d.z);
}

Vector3f StageFrame::toWorld(const Vector3f& p) const
{
	Vector3f d = dirToWorld(p);
	return Vector3f(mOrigin.x + d.x, mOrigin.y + d.y, mOrigin.z + d.z);
}

Vector3f StageFrame::toStage(const Vector3f& w) const
{
	f32 x = w.x - mOrigin.x;
	f32 z = w.z - mOrigin.z;
	return Vector3f(mCos * x - mSin * z, w.y - mOrigin.y, mSin * x + mCos * z);
}

} // namespace LauncherScene
