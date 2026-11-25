// Ground detail for the start-menu scene (docs/launcher/SCENE.md 10.4).
//
// The area maps' ground textures are 256 texels across tens of units of ground and carry no mips:
// the gameplay camera sees them from 400+ units away, the idle lens from 11 units up, where every
// texel spans dozens of pixels and the ground in front of the cast smears into soft streaks. This
// lays a fine, tiling grain over the ground near the eye: a flat grid at the landing site's height,
// depth-tested against the drawn map, blended as 2 x source x destination (mid grey changes
// nothing), its strength falling off with distance from the eye, so the far ground is untouched.
#pragma once

#include "types.h"
#include "Vector3.h"

struct Viewport;

namespace LauncherScene {

struct StageFrame;

struct GroundDetail {
	// `eye` world position of the camera; `strength` 0 (off) .. 1. Call after the opaque map is
	// drawn and before anything translucent.
	static void draw(Viewport* vp, const StageFrame& frame, const Vector3f& eye, f32 strength);
};

} // namespace LauncherScene
