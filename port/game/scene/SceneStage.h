// Where the start-menu scene takes place (docs/launcher/SCENE.md 3.5, 8.1).
//
// Everything the scene places is given in the *stage frame*: origin at the ship's ground position,
// +Z out of the ship's front (toward its pick-up point `start1`), +X to the ship's left (the
// camera's right when it looks at the ship's front), Y up. The frame is rotated into the area map
// by `azimuthDeg` about the ship. The landing sites are flat, so the ship's Y is the ground height.
#pragma once

#include "types.h"
#include "Vector3.h"
#include "Matrixf.h"

namespace LauncherScene {

// Where an Onion stands (SCENE.md 11.2), stage frame. Red, yellow and blue, as PikminColour.
struct OnionSpot {
	f32 mX, mZ;
	f32 mYawDeg; // the Onion's own yaw (J3D convention, 0 = +Z)
};

// Per-area take-off framing (SCENE.md 11.3), stage frame. Away from Awakening Wood the forest wall
// behind the ship is lower: its rim and the sky (Valley of Repose, Wistful Wild) or a bare white
// fog wall (Perplexing Pool) showed in the shots that look back at the ship.
struct StageShots {
	Vector3f mLiftEye;    // S4, the Onions lift: from high behind them, looking down past them
	Vector3f mLiftTarget; // at the ship, so the ground fills the picture behind them
	f32 mLiftFov;
	f32 mSkyEdgeDeg;      // highest top edge for the shots facing the ship (S3, S5); 0: default
	f32 mOnionsEdgeDeg;   // the same for the shots facing away from it (S2, S2b); 0: default
};

struct StageInfo {
	const char* mName;      // for logs
	const char* mMapFolder; // area model: <folder>/arc.szs
	const char* mAbeFolder; // area colours: <folder>/light.ini
	Vector3f mShipPos;      // from disc:user/Abe/map/<area>/defaultgen.txt
	f32 mShipYawDeg;        // the ship's own yaw from the same file
	f32 mAzimuthDeg;        // stage frame rotation; the ship's yaw unless the view past it is poor
	u16 mAmbientEfx;        // s00_coursein emitter for the area's air (petals, snow...), 0xFFFF = none
	Vector3f mAmbientEfxOffset; // stage frame; the s00 emitters release ~1100-2100 units above
	                            // themselves, so the emitter sits that far below the ground
	const OnionSpot* mOnions;   // three spots for this area, or null for kOnionSpots (SCENE.md 11.2)
	bool mAmbientNearLens;      // the air's particles are big enough to fill the take-off's low
	                            // lenses (Wistful Wild's maple leaves): gone from the S2 cut on
	const StageShots* mShots;   // take-off framing for this area, or null for the defaults
	f32 mGroundDetail = 1.0f;   // strength of the grain over the near ground (SCENE.md 10.4)
	f32 mAmbientSize  = 1.0f;   // size of the air's particles (Wistful Wild's maple leaves are huge)
	f32 mAmbientNear  = 95.0f;  // air particles nearer the eye than this are not drawn
	bool mHidden = false;       // not offered: the area does not hold up from the idle lens (SCENE.md 8.4)
};

// The default Onion layout, the same in every area: red, yellow, blue. Blue and yellow stand where
// defaultgen.txt has them; red is moved from its authored spot (-167, 203), which would fill the
// left of the idle picture, to behind the idle camera.
extern const OnionSpot kOnionSpots[3];
const OnionSpot& onionSpot(const StageInfo& stage, int colour);

// The four overworld areas, indexed like P2SceneConfig.area (0 tutorial, 1 forest, 2 yakushima,
// 3 last). Out-of-range values give the forest.
const StageInfo& getStage(int area);

// Stage frame <-> world.
struct StageFrame {
	void set(const StageInfo& stage);

	Vector3f toWorld(const Vector3f& stagePos) const;  // a point
	Vector3f dirToWorld(const Vector3f& stageDir) const; // a direction (no translation)
	Vector3f toStage(const Vector3f& worldPos) const;
	f32 yawToWorld(f32 stageYaw) const { return stageYaw + mAzimuth; } // radians, J3D convention

	Vector3f mOrigin; // ship ground position in the world
	f32 mAzimuth;     // radians
	f32 mSin, mCos;
};

} // namespace LauncherScene
