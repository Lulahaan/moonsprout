// Widescreen state shared by host and game code (docs/launcher/ASPECT.md 3 and 5).
//
// Model: the game is laid out for W0 x H0 = 608 x 448 (aspect A0 = 19:14). When the content is
// wider, k = contentAspect / A0 (clamped to 1..3.6). The 3D camera widens by k; 2D viewports are
// remapped about the centre X = 304 so 2D keeps its shape (Hor+). k = 1 makes every function here
// the identity, which is the original picture.
//
// Plain C, included by game files under #if TARGET_PC. Implemented in port/sdk/wide.cpp, which
// never reads settings: mppm::video::apply() (port/launcher/video.cpp) pushes them in with
// p2_wide_configure.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Once per frame, right after aurora_begin_frame: samples the content size (AuroraGetRenderSize).
void p2_wide_update(void);
// Pushed by mppm::video::apply(). stretch: aspect mode Stretch (k forced to 1).
// hudLayout: 0 centered, 1 edges, 2 stretched. hudMaxAspect: limit for edge anchoring, 0 = unlimited.
void p2_wide_configure(int stretch, int hudLayout, float hudMaxAspect);
float p2_wide_k(void);           // 1.0 when off
float p2_wide_2d_k(void);        // k, or 1.0 when hudLayout is "stretched" or inside a raw scope
float p2_wide_map_x(float x);    // 304 + (x - 304) / p2_wide_2d_k()
float p2_wide_min_x(void);       // 304 * (1 - k2d): leftmost visible 2D X
float p2_wide_max_x(void);       // 304 * (1 + k2d): rightmost visible 2D X
// GXSetScissor for a rectangle in 2D (pane) coordinates: X edges are remapped like the viewport,
// edges at the frame border (<= 0, >= 608) stay there. Game code: PC_SWITCH(p2_wide_scissor, GXSetScissor)(...).
void p2_wide_scissor(unsigned int x, unsigned int y, unsigned int width, unsigned int height);
void p2_wide_raw_begin(void);    // nestable: 2D remap off (capture passes)
void p2_wide_raw_end(void);
void p2_wide_scene_original(int on); // nestable: the current scene wants Original; applied next frame
int p2_wide_scene_original_active(void); // non-zero while any scene_original(1) is outstanding
float p2_wide_hud_shift(int anchor); // anchor -1 left, 0 centre, +1 right -> X offset in 2D units
// For a spread screen (anchor 2, the sun meter): how far its outermost panes move out on each side,
// in 2D units (the shift of a right-anchored screen); 0 unless the HUD layout is Edges.
float p2_wide_hud_spread(void);
// HUD anchor of a HUD screen by its .blo file name (-1 left, 0 centre, +1 right, 2 spread); unknown
// names are centre. MPPM_HUD_ANCHORS=name:L,name:R,name:C,name:S overrides the table (tuning
// without a rebuild).
int p2_wide_anchor_for_blo(const char* name);
// Nestable "wide canvas" scope (ASPECT 7): 2D drawn inside may reach the window edges
// (p2_wide_min_x()..p2_wide_max_x()) instead of being clipped to the centre 608 units. Same
// placement as the viewport remap, but done with a full-width viewport and a projection squeezed
// by 1/k, so content shifted outside 0..608 (edge-anchored HUD screens, their 2D effects) still
// shows. begin converts the current GX viewport and projection; end restores them.
void p2_wide_canvas_begin(void);
void p2_wide_canvas_end(void);
int p2_wide_canvas_active(void);
// Inside a canvas scope: J2DGrafContext::setPort hands its unmapped port X range here (and sets
// that as the viewport), and the 2D graphs load their projection through p2_wide_set_projection,
// which squeezes it to match. Outside a scope p2_wide_set_projection is GXSetProjection.
void p2_wide_canvas_port(float x0, float x1);
void p2_wide_set_projection(const void* mtx44, int type);
// The free J2DFillBox helpers draw through a bare J2DOrthoGraph (no port bounds), in whatever 2D
// port the caller set: a fade over the whole 608 frame (ResetManager, DVD error screen) would
// cover only the centre 19:14 column. begin: when the box spans 0..608 and the current viewport
// is a full-width remapped 2D port, widen the viewport to the whole window (same projection);
// returns non-zero when it did, and end restores the viewport.
int p2_wide_fill_begin(float x0, float x1);
// Title screen backdrop (ebi::title::TMapBase, TBGEnemyBase): the ground models are authored to
// fill the picture up to about k = 1.45; wider than that their hard edges show. Returns the
// horizontal scale (>= 1) that makes them fill the window, 1 up to that width.
float p2_wide_backdrop_scale(void);
void p2_wide_fill_end(void);
// Full-width backdrop pictures (ASPECT 8, phase 3): J2DPicture / J2DPictureEx::drawTexCoord pass
// their position matrix (graf * pane), the quad's X edges and the S coordinates at those edges
// (bottom x0, bottom x1, top x0, top x1). A quad that covers the 608-wide frame (to within 3% of
// each edge) without rotation is extended to the window edges with its S coordinates extrapolated (the centre stays
// pixel-identical; the sides continue the texture by its wrap mode) inside a wide canvas, when its
// texture repeats (texWrapS = GX_REPEAT / GX_MIRROR) or is at most 64 texels wide (a stretched
// gradient or band; texWidth 0 = untextured). Other (clamped picture) quads keep their size and
// only show what they already draw past the frame.
// Returns non-zero when it did; p2_wide_backdrop_end then restores the GX state.
int p2_wide_backdrop_begin(const void* posMtx34, float* x0, float* x1, short* s00, short* s10, short* s01, short* s11, int texWidth, int texWrapS);
void p2_wide_backdrop_end(void);
// Host only: called from p2_wide_update when the scene-original state flips, before k is sampled,
// so mppm::video can re-lock the content shape for the same frame.
void p2_wide_set_change_hook(void (*hook)(void));

#ifdef __cplusplus
}
#endif
