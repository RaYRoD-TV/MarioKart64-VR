// OpenXR VR support for SpaghettiKart (Mario Kart 64 PC port on libultraship / Fast3D).
//
// Ported from the proven sm64coopdx VR layer. Public interface is platform-agnostic and
// C-linkage so it can be called from libultraship's C++ Fast3D and from the game's C code.
// No OpenXR / GL types leak out of this header.
#ifndef MK64_VR_H
#define MK64_VR_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// VR view modes (CVar gVRViewMode). The eye-matrix builder and the Engine render path branch on this.
typedef enum {
    VR_VIEW_THIRD_PERSON = 0, // game chase cam, life-size stereo (default - the classic racing view)
    VR_VIEW_FIRST_PERSON = 1, // eye pushed forward to the kart, life-size stereo (driver's view)
    VR_VIEW_THEATER      = 2, // flat game frame on a head-locked screen, no stereo (max comfort)
    VR_VIEW_DIORAMA      = 3, // world shrunk to a tabletop miniature anchored in front of you
} VrViewMode;

// --- request / probe ---------------------------------------------------------
// Requested via the --vr CLI flag (or auto-enabled when a headset is detected).
void vr_request_enable(void);
bool vr_is_requested(void);

// Lightweight startup probe: is a VR headset actually connected right now? Creates and tears down a
// throwaway OpenXR instance (no GL context needed) and asks for an HMD system. Lets the same exe
// auto-enable VR when a headset is present and stay flat otherwise.
bool vr_headset_present(void);

// Is an OpenXR session live and actively rendering? Everything VR-specific in the renderer and game
// is gated on this; false == stock flat game.
bool vr_is_active(void);

// The headset's display refresh rate in Hz (from the runtime's predicted display period). Drives the
// render frame-rate target so the headset runs at its native refresh. ~90 until the first frame.
int  vr_display_refresh_hz(void);

// --- per-frame loop ----------------------------------------------------------
// Lazy-boot OpenXR, poll events, xrWaitFrame/xrBeginFrame, locate the per-eye views and build the
// per-eye camera-space -> eye-clip matrices. Call once before the eye loop.
void vr_begin_frame(void);

int  vr_eye_count(void);
int  vr_eye_width(int eye);
int  vr_eye_height(int eye);

// Acquire + bind eye `eye`'s swapchain image (color + shared depth) as the active GL render target
// with the eye-sized viewport. Returns false to skip the eye this frame.
bool vr_begin_eye(int eye);
// Release eye `eye`'s swapchain image and record its projection view for submit.
void vr_end_eye(int eye);

// Camera-space -> eye-clip matrix (16 floats, row-vector: clip = p_cam * M). The renderer composes
// this after the game's world->camera lookAt: P_matrix_vr = lookAt * vr_eye_viewproj(eye).
const float* vr_eye_viewproj(int eye);
// Rotation-only sky view-projection for an eye (16 floats, row-vector). Renders a head-locked sky dome at
// infinity (anchors yaw + pitch). No game lookAt / world-scale is composed onto this.
const float* vr_sky_viewproj(int eye);
// The game pushes its 2D-sky angular half-extent (radians, from camera->fieldOfView) each frame so the cloud
// sprites land at their true world azimuth on the dome and rotate 1:1 with the kart heading (no swim). The
// renderer reads it back via vr_sky_fov_h/v for the per-vertex equiangular sky remap (NDC 1.0 <-> half-angle).
void vr_set_sky_fov(float halfH, float halfV);
float vr_sky_fov_h(void);
float vr_sky_fov_v(void);
// The game pushes the chase camera's yaw each frame (radians). The interpreter subtracts vr_sky_decouple_rad()
// from each 2D-sky sprite vertex's azimuth: in Diorama that's the chase yaw (decouples the clouds from the
// fixed tabletop -> head-stable), in the in-world modes it's 0 (clouds stay world-anchored to the chase cam).
void  vr_set_sky_chase_yaw(float rad);
float vr_sky_decouple_rad(void);

// LIVE render path: the interpreter renders the scene into its managed off-screen fb, then these blit
// that fb's color texture into the OpenXR swapchain image (eye projection layer / head-locked panel).
// Returns false to skip this frame.
bool vr_submit_eye_texture(int eye, unsigned int glTextureId, int w, int h);
bool vr_submit_panel_texture(unsigned int glTextureId, int w, int h);
// Present the desktop window (with the ImGui menu) on the head-locked panel so the menu is usable in
// VR. srcW/srcH = current window pixel size.
bool vr_present_desktop_panel(int srcW, int srcH);

// Stable VR menu render: bind + clear a private offscreen FBO (caller renders ImGui into it), then mirror
// it to the desktop window and present the stable texture on the head-locked panel. Avoids the GL_BACK
// double-buffer flicker. w/h = window pixel size.
void vr_menu_render_begin(int w, int h);
void vr_menu_render_present(int w, int h);
// Mirror the menu onto the desktop window - call ONCE after the eye loop (avoids flatscreen flicker).
void vr_menu_mirror_desktop(int w, int h);
// Mirror the rendered VR game frame (managed fb texture: last eye / panel) onto the desktop window when the
// MENU IS CLOSED, so the flat window shows the game instead of flickering stale back-buffers. Call ONCE per
// frame after the eye loop, before the engine SwapBuffers. srcW/srcH = managed fb size; dstW/dstH = window.
void vr_mirror_game_desktop(unsigned int glTex, int srcW, int srcH, int dstW, int dstH);
// Apply gVRMenuOpacity to the VR menu panel (alpha into the menu texture). Call after the menu's ImGui has
// rendered into the FBO and before the panel is presented. No-op at full opacity. Desktop mirror unaffected.
void vr_menu_apply_opacity(void);

// Head-locked HUD/menu overlay (its own swapchain layer). Mirrors the SM64 build.
int  vr_overlay_width(void);
int  vr_overlay_height(void);
bool vr_begin_overlay(bool sky); // bind the HUD (sky currently unused; reserved)
void vr_end_overlay(bool sky);

// FLATSCREEN-ON-A-PANEL: for non-gameplay screens (menus), render the flat frame once into the panel
// swapchain and present it on one large head-locked quad.
void vr_set_panel_mode(bool on);
bool vr_begin_panel(void);
void vr_end_panel(void);

// xrEndFrame with the stereo projection layer (+ HUD quad / panel).
void vr_submit(void);

// --- live-tunable comfort / framing knobs (in-game VR options panel) ---------
// VR view mode (Theater / Third Person / First Person / Diorama). See VrViewMode. Read from gVRViewMode each
// frame; Engine routes Theater to the flat panel path, the others to the per-eye stereo path.
int  vr_get_view_mode(void);     void vr_set_view_mode(int mode);
// True while switching INTO First Person is blocked (pre-race course-open intro, unless FP was already the
// mode at race start). The view-mode cycles skip First Person while this holds; other modes switch freely.
bool vr_fp_switch_locked(void);
// Game units to push the chase camera back (horizontal kart->camera direction) for the Third Person VR
// distance knob. The game lookAt build adds this so distance reads as closer/further. 0 outside Third Person VR.
float vr_third_person_push_units(void);
// Game units the First Person eye currently sits ahead of the chase camera (the CAM DISTANCE push,
// eased). The kart shadow rides this so it stays under the player's seat. 0 outside First Person.
float vr_fp_forward_game_units(void);
// Game units per meter: how big the world feels. Bigger = world appears smaller.
float vr_get_world_scale(void);  void vr_set_world_scale(float v);
// Stereo separation strength (0 = mono, 1 = full IPD). Lower is gentler / less cross-eye.
float vr_get_stereo(void);       void vr_set_stereo(float v);
// 6DoF head-motion amount (0 = orientation only / locked translation, 1 = full positional).
float vr_get_head_scale(void);   void vr_set_head_scale(float v);
// Eye height offset above the kart's head anchor (meters).
float vr_get_eye_height(void);   void vr_set_eye_height(float v);
// Comfort: horizon lock (ignore kart pitch/roll on ramps), turn/drift/boost vignette.
bool  vr_get_horizon_lock(void); void vr_set_horizon_lock(bool on);
bool  vr_get_vignette(void);     void vr_set_vignette(bool on);
// Static cockpit frame (head-locked reference mesh - the big anti-nausea trick).
bool  vr_get_cockpit(void);      void vr_set_cockpit(bool on);
// Head-locked menu panel placement.
float vr_get_menu_dist(void);    void vr_set_menu_dist(float v);
float vr_get_menu_size(void);    void vr_set_menu_size(float v);
// In-game 2D HUD (item box / positions / lap): repositioned onto a head-locked plane in the eye render.
float vr_get_hud_scale(void);    void vr_set_hud_scale(float v); // fraction of FOV the HUD fills
float vr_get_hud_dist(void);     void vr_set_hud_dist(float v);  // HUD plane distance (meters)
// Head-locked HUD view-projection for an eye (16 floats, row-vector). The renderer multiplies the game's
// 2D ortho matrix by this for post-3D HUD draws so the HUD sits at a comfortable distance.
const float* vr_hud_viewproj(int eye);
// Full-FOV head-locked view-projection for an eye (16 floats, row-vector). The renderer multiplies a
// screen-space 2D ortho matrix by this (pre-3D intro overlays / dome-less 2D sky) so that 2D fills the view
// head-locked instead of being emitted raw - raw screen-space 2D doubles under the per-eye asymmetric submit.
const float* vr_full2d_viewproj(int eye);

void  vr_reset_defaults(void);

// --- viewpoint feed (game -> VR) ---------------------------------------------
// The game writes the local kart's head pose each frame (world-space, game units + radians) so the
// in-cockpit eye sits at the driver's head. heading is the kart yaw in radians (0 = +X N64 convention
// resolved inside). pitch/roll are the kart body tilt (used only when horizon lock is OFF).
void vr_set_cockpit_pose(const float headPosWorld[3], float headingRad, float pitchRad, float rollRad);
// Per-frame turn/drift/boost intensity in [0,1] to drive the comfort vignette.
void vr_set_motion_intensity(float v);

// Head orientation offsets (radians) from facing forward - exposed for any world-locking the game
// wants to do (e.g. anchoring the cockpit frame).
float vr_head_yaw_rad(void);
float vr_head_pitch_rad(void);

// Yaw to add to a thrown item's launch direction (binary angle units, 0x10000 = a full turn) so
// shells and bananas fly where the player is LOOKING. Nonzero only for the human player in First
// Person mode; everything else throws straight ahead like stock.
short vr_item_throw_yaw_offset(int isHumanPlayer);

// First Person drama handling. The game feeds once per logic frame: whether the player kart is in a
// dramatic state, whether it has finished the race, and the kart's current spin/tumble angles
// (binary, 0x10000 = a full turn). Default behavior SIMULATES the hit from the driver's seat by
// rotating the eye with the kart; the opt-in flip cam (gVRFlipCam) instead pulls the eye back to
// the chase framing so the kart is watched externally. Finishing always un-hides the kart.
void vr_set_action_cam(int dramatic, int finished);
void vr_set_drama_rot(short yawBinary, short pitchBinary);
int vr_action_cam_active(void);

// --- motion controllers (OpenXR actions) --------------------------------------
// One gameplay action set: both thumbsticks, face buttons, menu button, stick clicks, triggers,
// grips, and rumble. Suggested bindings cover Quest Touch (incl. the Quest 3 / Pro Touch Plus
// profile), Valve Index, HP Reverb G2, WMR wands, Vive wands and the khr simple fallback.
// main.c merges this state into the first N64 pad ahead of its edge detection, so the controllers
// drive gameplay and every menu exactly like a gamepad.
enum {
    VR_BTN_A        = (1 <<  0), // right controller A
    VR_BTN_B        = (1 <<  1), // right controller B
    VR_BTN_X        = (1 <<  2), // left controller X
    VR_BTN_Y        = (1 <<  3), // left controller Y
    VR_BTN_MENU     = (1 <<  4), // left controller menu button
    VR_BTN_LSTICK   = (1 <<  5), // left thumbstick click
    VR_BTN_RSTICK   = (1 <<  6), // right thumbstick click
    VR_BTN_LTRIGGER = (1 <<  7), // left index trigger (analog, latched digital with hysteresis)
    VR_BTN_RTRIGGER = (1 <<  8), // right index trigger
    VR_BTN_LGRIP    = (1 <<  9), // left grip squeeze
    VR_BTN_RGRIP    = (1 << 10), // right grip squeeze
};
// True while the session is focused and the action set is attached (controllers deliver input).
bool vr_controllers_active(void);
// VR_BTN_* mask of currently-held controls (0 when inactive).
unsigned vr_controller_buttons(void);
// Thumbstick state, -1..1 with +x right and +y up. hand: 0 = left, 1 = right.
void vr_controller_stick(int hand, float out[2]);
// Arm the rumble on both hands: short bursts re-arm each frame while armed, so a runtime that
// drops a stop request can't strand the motors buzzing.
void vr_controller_rumble(float strength, float seconds);
void vr_controller_rumble_stop(void);

void vr_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif // MK64_VR_H
