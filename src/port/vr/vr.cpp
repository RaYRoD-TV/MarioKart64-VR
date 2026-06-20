// OpenXR VR support for SpaghettiKart (Mario Kart 64 PC port on libultraship / Fast3D).
//
// Ported from the proven sm64coopdx VR layer. Boots an OpenXR session bound to the GL context that
// libultraship's OpenGL backend creates (XR_USE_GRAPHICS_API_OPENGL + WGL on Windows), creates stereo
// swapchains, and renders the game's display list ONCE PER EYE into each eye's swapchain image (with a
// depth buffer), injecting a per-eye camera-space -> eye-clip matrix into Fast3D so the two eyes differ
// -> real stereoscopic depth. A head-locked quad carries the 2D HUD; non-gameplay screens go on a flat
// panel.
//
// GL-backend only (OpenXR is bound to WGL); VR forces the FAST3D_SDL_OPENGL backend at startup.

#include "vr.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>

// --- request flag (platform-agnostic, set by the --vr CLI handler) ----------
static bool sRequested = false;
extern "C" void vr_request_enable(void) { sRequested = true; }
extern "C" bool vr_is_requested(void) { return sRequested; }

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <unknwn.h> // IUnknown - referenced by openxr_platform.h's XR_USE_PLATFORM_WIN32 structs

#include <GL/glew.h> // libultraship's GL backend uses GLEW (glewInit); reuse its loaded pointers

#define XR_USE_GRAPHICS_API_OPENGL
#define XR_USE_PLATFORM_WIN32
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

// libultraship console-variable bridge (extern "C") - lets the in-game VR options menu drive the
// tunables below by reading the CVars each frame.
extern "C" float CVarGetFloat(const char* name, float defaultValue);
extern "C" int   CVarGetInteger(const char* name, int defaultValue);

// ---- OpenXR state -----------------------------------------------------------
static XrInstance     sInstance   = XR_NULL_HANDLE;
static XrSystemId     sSystemId   = XR_NULL_SYSTEM_ID;
static XrSession      sSession    = XR_NULL_HANDLE;
static XrSpace        sLocalSpace = XR_NULL_HANDLE; // world-locked reference space
static XrSpace        sViewSpace  = XR_NULL_HANDLE; // VIEW reference space (head-locked) for overlays
static XrSessionState sState      = XR_SESSION_STATE_UNKNOWN;

static bool sBootTried  = false;
static bool sRunning    = false;
static bool sFrameBegun = false;
static bool sViewsValid = false;
static bool sPoseTracked = false;

static XrFrameState            sFrameState;
static uint32_t                sViewCount = 0;
static XrViewConfigurationView sViewConfigs[2];
static XrView                  sViews[2];

typedef struct {
    XrSwapchain handle;
    uint32_t w, h, imgCount;
    XrSwapchainImageOpenGLKHR* images;
} VrSwapchain;
static VrSwapchain sEye[2];

static GLuint   sEyeFbo     = 0; // DRAW FBO: swapchain image as color attachment (blit target)
static GLuint   sEyeDepthRB = 0; // shared depth renderbuffer (legacy direct-render path)
static GLuint   sBlitReadFbo = 0; // READ FBO: the interpreter's managed VR texture is attached here to blit
static XrCompositionLayerProjectionView sProjViews[2];
static uint32_t sEyeImgIdx[2];

static VrSwapchain sHud;                      // HUD/menu swapchain (head-locked / panel)
static GLuint      sOverlayFbo     = 0;
static GLuint      sOverlayDepthRB = 0;
static uint32_t    sHudImgIdx      = 0;
static bool        sHudReady       = false;
static bool        sPanelMode      = false;
static int         sEyesSubmitted  = 0; // eyes blitted into swapchains this frame (gates the proj layer)
static const int   sOverlayW = 1920;
static const int   sOverlayH = 1080;

// --- in-cockpit framing tunables (see vr.h) ----------------------------------
static float sWorldScale = 20.0f;  // game units per meter for THIRD PERSON (Ray-tuned default; lower = bigger)
static float sStereoScale = 0.5f;  // stereo separation strength
static float sHeadScale   = 1.0f;  // 6DoF head-motion amount (1 = full positional in-cockpit)
static float sEyeHeight   = 0.16f; // meters above the head anchor for THIRD PERSON (Ray-tuned default)
static bool  sHorizonLock = true;  // ignore kart pitch/roll (comfortable default)
static bool  sVignette    = true;  // turn/drift/boost comfort vignette
static bool  sCockpit     = true;  // static cockpit reference frame
static float sMenuDist    = 3.2f; // menu/title panel distance (m) - comfortable, not in your face
static float sMenuSize    = 4.2f; // menu/title panel width (m)

// VR view mode (gVRViewMode). 0=Third Person (chase cam, life-size), 1=First Person (eye pushed forward to
// the kart), 2=Theater (flat frame on a head-locked screen), 3=Diorama (world shrunk to a tabletop). The
// eye-matrix builder branches on this; Engine.cpp routes Theater to the flat panel path. Tunables below are
// read from CVars each frame so they apply live.
static int   sViewMode           = 0;       // VrViewMode
static float sFirstPersonForward = 3.00f;   // meters the eye is pushed forward toward the kart (First Person).
                                            // It rides the chase cam's slightly-downward view, so big values
                                            // dive toward the floor - keep it modest; tune in headset.
static float sFPForwardCur       = 0.0f;    // eased First-Person push - see vr_begin_frame (Lakitu start-line fix).
static int   sActionLatch        = 0;       // flip cam: logic frames of pull-back left (latched so quick
                                            // states don't flicker the camera in and out)
static int   sFlipCamOn          = 0;       // gVRFlipCam - ease-back opt-in (default OFF: First Person
                                            // simulates the drama in-place instead of pulling back)
static int   sFPDramaOn          = 1;       // gVRFPDrama - the in-place First Person drama simulation
static int   sRaceFinished       = 0;       // local player crossed the line - kart un-hides post-race
static float sDramaYawRad        = 0.0f;    // EASED kart spin angle (banana / lightning) - First Person
static float sDramaPitchRad      = 0.0f;    // EASED kart tumble flip angle (shells) - rotates the eye
                                            // WITH the kart so the hit is felt from the driver's seat
static float sDramaYawTgt        = 0.0f;    // raw 30Hz angles fed by the game; the eased values above
static float sDramaPitchTgt      = 0.0f;    // chase them every 90Hz frame so the motion is smooth
static int   vr_action_cam_pullback(void);  // defined with the flip-cam plumbing below

static float wrap_pi(float a) {
    while (a > 3.14159265f) a -= 2.0f * 3.14159265f;
    while (a < -3.14159265f) a += 2.0f * 3.14159265f;
    return a;
}
extern "C" int gRaceState;                  // game race phase; == 2 (RACE_STAGING) during the start countdown.
static float sFirstPersonScale   = 20.0f;   // First Person world scale (units/m), its OWN knob (like Diorama)
                                            // so it doesn't shrink Third Person. Lower = bigger world.
static float sFirstPersonEyeHeight = 0.0f;  // First Person eye height (m), its OWN knob (Third Person has its)
static float sThirdPersonDist    = 0.0f;    // Third Person camera distance offset (m): + = further behind the
                                            // kart (and higher, riding the chase angle), - = closer. 0 = stock.
static float sDioramaWorldScale  = 115.0f;  // Diorama world scale (game units/m), INDEPENDENT of the global
                                            // World Scale so tuning the tabletop never touches Third/First
                                            // person. Higher = smaller tabletop. (Ray-tuned balanced default.)
static float sDioramaDist        = 0.30f;   // meters the tabletop sits in front of you (Diorama)
static float sDioramaHeight      = -0.15f;  // meters the tabletop is offset vertically (Diorama; - = below eye)
static float sMenuOpacity        = 0.5f;    // VR menu/HUD panel opacity (1 = opaque, lower = see game through)

// 6DoF damping rest pose (capture after warmup so tracking has settled).
static float sHeadRest[3] = { 0, 0, 0 };
static bool  sHeadRestSet = false;
static int   sHeadWarmup  = 0;
static XrPosef sRenderPose[2];
static XrFovf  sRenderFov[2];

// Cockpit pose fed by the game each frame (world-space, game units + radians).
static float sCockpitPos[3]  = { 0, 0, 0 };
static float sCockpitHeading = 0.0f;
static float sCockpitPitch   = 0.0f;
static float sCockpitRoll    = 0.0f;
static bool  sCockpitValid   = false;
static float sMotionIntensity = 0.0f;

// Per-eye camera-space -> eye-clip matrices (row-vector; clip = p_cam * sEyeVP).
static float sEyeVP[2][4][4];
static float sSkyVP[2][4][4]; // rotation-only sky view-proj (zero translation -> infinity) for the sky dome
// The GAME's 2D-sky angular half-extent (radians), pushed each frame from camera->fieldOfView via
// vr_set_sky_fov. MK64's cloud sprites' screen X is the game's ANGLE-linear projection of their world azimuth
// at THIS fov (NDC 1.0 <-> this half-angle); the interpreter's per-vertex sky remap reads these back via
// vr_sky_fov_h/v to recover each sprite vertex's true azimuth/elevation on the dome. Using any other fov
// makes the clouds rotate at the wrong rate vs the world and swim as the kart turns. Defaults ~ (50 deg H,
// 37.5 deg V) until the game pushes the real fov.
static float sSkyFovH = 0.8727f;
static float sSkyFovV = 0.6545f;
// The chase camera's yaw (radians, same binary->radian convention as the cloud sprites' screen X). In the
// in-world modes (Third/First Person) the rendered world rotates WITH the chase cam, so the cloud sprites
// SHOULD track it (world-anchored - the look the user asked for). In Diorama the world is a fixed tabletop
// that does NOT rotate when the kart turns, so the cloud azimuth must be DECOUPLED from the chase yaw or
// the clouds swim across the stable dome. vr_sky_decouple_rad() returns this value in Diorama, 0 elsewhere;
// the interpreter subtracts it from each sprite vertex's azimuth so Diorama clouds sit head-stable.
static float sSkyChaseYawRad = 0.0f;

// Menu panel world-lock anchor (captured at menu open, yaw-only).
static bool  sPanelAnchorValid = false;
static float sPanelAnchorPos[3] = { 0, 0, 0 };
static float sPanelAnchorQy = 0.0f, sPanelAnchorQw = 1.0f;

// Head-locked HUD plane: the in-game 2D HUD (item box, positions, lap, etc.) is repositioned in the eye
// render onto a view-space quad so it sits at a comfortable distance instead of at your face. Built per
// eye from the symmetric fov; zero eye-offset -> zero disparity (relaxed, far-feeling), head-locked.
static float sHudVP[2][4][4];
static float sHudScale = 0.35f; // fraction of the eye FOV the HUD fills (smaller = tighter, more central)
// HUD LOCK = WORLD (gVRHudWorldLock): yaw-pin the in-race HUD plane to the room direction it was facing
// when enabled, so you can turn your head and look AROUND it instead of it being glued dead-centre. It
// still follows your head POSITION (no drift while driving); only the head's yaw is decoupled.
static float sHudYawAnchor   = 0.0f;   // room yaw the HUD is pinned to (captured on enable)
static bool  sHudYawAnchored = false;
static float sHudYawOffset   = 0.0f;   // yaw applied to the HUD plane this frame (0 = head-locked)
static float sHudDistM = 2.4f;  // HUD plane distance (meters); pushed back so the wide course-name banner
                                // (shares this plane) isn't in your face. Live-tunable via gVRHudDist.

static PFN_xrGetOpenGLGraphicsRequirementsKHR pfnGetGLReq = NULL;

extern "C" const float* vr_hud_viewproj(int eye) {
    return (eye >= 0 && eye < 2) ? &sHudVP[eye][0][0] : NULL;
}

extern "C" bool vr_is_active(void) { return sRunning; }

// Temporary diagnostic logger - writes vr_diag.txt next to the exe while VR runs (capped). Used to capture
// runtime evidence for the cloud-swim + Lakitu-start bugs; remove once both are confirmed fixed.
extern "C" void vr_diag_log(const char* fmt, ...) {
    static FILE* sDiag = nullptr;
    static int   sDiagLines = 0;
    if (!sRunning || sDiagLines > 4000) return;
    if (sDiag == nullptr) {
        sDiag = fopen("vr_diag.txt", "w");
        if (sDiag == nullptr) return;
    }
    va_list ap;
    va_start(ap, fmt);
    vfprintf(sDiag, fmt, ap);
    va_end(ap);
    fputc('\n', sDiag);
    fflush(sDiag);
    sDiagLines++;
}

extern "C" int  vr_eye_count(void) { return (int)sViewCount; }
extern "C" int  vr_eye_width(int eye)  { return (eye >= 0 && eye < 2) ? (int)sEye[eye].w : 0; }
extern "C" int  vr_eye_height(int eye) { return (eye >= 0 && eye < 2) ? (int)sEye[eye].h : 0; }
extern "C" int  vr_overlay_width(void)  { return sOverlayW; }
extern "C" int  vr_overlay_height(void) { return sOverlayH; }
extern "C" const float* vr_eye_viewproj(int eye) {
    return (eye >= 0 && eye < 2) ? &sEyeVP[eye][0][0] : NULL;
}
extern "C" const float* vr_sky_viewproj(int eye) {
    return (eye >= 0 && eye < 2) ? &sSkyVP[eye][0][0] : NULL;
}
// The game pushes its 2D-sky angular half-extent (radians) each frame, derived from camera->fieldOfView, so
// the interpreter's per-vertex sky remap inverts the same projection that produced the sprites' screen X.
// Without this the clouds rotate at the wrong rate vs the world and swim as the kart turns. Guarded to a
// sane range.
extern "C" void vr_set_sky_fov(float halfH, float halfV) {
    // Clamp (don't reject) the high end: rejecting froze the calibration when gVRSkyYawScale pushed the
    // value past the cap at high fov, silently disconnecting the slider. NaN/garbage still rejected.
    if (halfH > 0.01f) sSkyFovH = (halfH < 1.55f) ? halfH : 1.55f;
    if (halfV > 0.01f) sSkyFovV = (halfV < 1.55f) ? halfV : 1.55f;
}
extern "C" float vr_sky_fov_h(void) { return sSkyFovH; }
extern "C" float vr_sky_fov_v(void) { return sSkyFovV; }
// The game pushes the chase camera's yaw each frame (radians). Stored unconditionally; only consumed in
// Diorama (see sSkyChaseYawRad note).
extern "C" void vr_set_sky_chase_yaw(float rad) { sSkyChaseYawRad = rad; }
// Azimuth to subtract from each 2D-sky sprite vertex: the chase yaw in Diorama (decouple the clouds from the
// non-rotating tabletop world -> head-stable, matching the dome), 0 in the in-world modes (keep the clouds
// world-anchored to the chase cam, which IS the rendered world's rotation there).
extern "C" float vr_sky_decouple_rad(void) { return (sViewMode == VR_VIEW_DIORAMA) ? sSkyChaseYawRad : 0.0f; }

// --- matrix helpers (row-vector convention: clip = v * M, M[row][col]) -------
// Row-vector-convention axis rotations (same convention as the A/V matrices here).
static void mat_rot_y(float m[4][4], float a) {
    float c = cosf(a), s = sinf(a);
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) m[i][j] = (i == j) ? 1.0f : 0.0f;
    m[0][0] = c; m[0][2] = -s;
    m[2][0] = s; m[2][2] = c;
}
static void mat_rot_x(float m[4][4], float a) {
    float c = cosf(a), s = sinf(a);
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) m[i][j] = (i == j) ? 1.0f : 0.0f;
    m[1][1] = c; m[1][2] = s;
    m[2][1] = -s; m[2][2] = c;
}

static void mat_mul(float out[4][4], const float a[4][4], const float b[4][4]) {
    float t[4][4];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            t[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j] + a[i][3] * b[3][j];
    memcpy(out, t, sizeof(t));
}

// OpenXR fov -> projection (row-vector, OpenGL clip z in [-1, 1]).
static void mat_proj_fov(float m[4][4], XrFovf fov, float zn, float zf) {
    float l = tanf(fov.angleLeft), r = tanf(fov.angleRight);
    float dn = tanf(fov.angleDown), up = tanf(fov.angleUp);
    float w = r - l, h = up - dn;
    memset(m, 0, sizeof(float) * 16);
    m[0][0] = 2.0f / w;
    m[1][1] = 2.0f / h;
    m[2][0] = (r + l) / w;
    m[2][1] = (up + dn) / h;
    m[2][2] = -(zf + zn) / (zf - zn);
    m[2][3] = -1.0f;
    m[3][2] = -(2.0f * zf * zn) / (zf - zn);
}

// XrPosef -> world->eye view matrix (row-vector): rigid inverse of the eye's world transform.
static void mat_view_from_pose(float m[4][4], XrPosef pose) {
    float x = pose.orientation.x, y = pose.orientation.y, z = pose.orientation.z, w = pose.orientation.w;
    float Rrv[3][3] = {
        { 1.0f - 2.0f * (y * y + z * z), 2.0f * (x * y + z * w),       2.0f * (x * z - y * w) },
        { 2.0f * (x * y - z * w),        1.0f - 2.0f * (x * x + z * z), 2.0f * (y * z + x * w) },
        { 2.0f * (x * z + y * w),        2.0f * (y * z - x * w),        1.0f - 2.0f * (x * x + y * y) },
    };
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            m[i][j] = Rrv[j][i];
    m[0][3] = m[1][3] = m[2][3] = 0.0f;
    float px = pose.position.x, py = pose.position.y, pz = pose.position.z;
    m[3][0] = -(px * m[0][0] + py * m[1][0] + pz * m[2][0]);
    m[3][1] = -(px * m[0][1] + py * m[1][1] + pz * m[2][1]);
    m[3][2] = -(px * m[0][2] + py * m[1][2] + pz * m[2][2]);
    m[3][3] = 1.0f;
}

// Build sEyeVP[eye] = A (camera-space game units -> meters, with eye-height offset) * V (world->eye,
// meters) * P (eye projection). The renderer composes it after the game's world->camera lookAt:
//   P_matrix_vr = lookAt_game * sEyeVP[eye]
// so a vertex is v_obj * modelview * lookAt * A * V_head * P_eye.
static void vr_build_eye_matrix(int eye) {
    XrPosef pose = sViews[eye].pose;
    float cx = 0.5f * (sViews[0].pose.position.x + sViews[1].pose.position.x);
    float cy = 0.5f * (sViews[0].pose.position.y + sViews[1].pose.position.y);
    float cz = 0.5f * (sViews[0].pose.position.z + sViews[1].pose.position.z);

    // 6DoF damping: scale the head's offset from a captured rest pose by sHeadScale (1 = full).
    if (eye == 0 && !sHeadRestSet && ++sHeadWarmup >= 15) {
        sHeadRest[0] = cx; sHeadRest[1] = cy; sHeadRest[2] = cz; sHeadRestSet = true;
        printf("[VR] 6DoF rest captured at (%.2f, %.2f, %.2f)\n", cx, cy, cz);
    }
    float dcx = cx, dcy = cy, dcz = cz;
    if (sHeadRestSet) {
        dcx = sHeadRest[0] + (cx - sHeadRest[0]) * sHeadScale;
        dcy = sHeadRest[1] + (cy - sHeadRest[1]) * sHeadScale;
        dcz = sHeadRest[2] + (cz - sHeadRest[2]) * sHeadScale;
    }

    // Stereo comfort: keep the IPD offset (scaled by sStereoScale) around the damped center.
    pose.position.x = dcx + (pose.position.x - cx) * sStereoScale;
    pose.position.y = dcy + (pose.position.y - cy) * sStereoScale;
    pose.position.z = dcz + (pose.position.z - cz) * sStereoScale;
    sRenderPose[eye] = pose;

    // A: scale camera-space game units into meters, plus per-mode framing. The game's chase-cam lookAt is
    // composed BEFORE this, so chase-camera space has the eye at the origin looking -Z with the kart ahead.
    //  - Third Person (0): scale + eye-height. Eye sits at the chase cam (the classic racing view).
    //  - First Person (1): also push the eye forward (+Z) toward the kart for a driver's view.
    //  - Diorama   (3): shrink harder (x sDioramaScale) and place the tiny world at a fixed play-space spot
    //                   in front of you (-Z by sDioramaDist, raised by sDioramaHeight) -> a tabletop you
    //                   can lean around (V_head still tracks the head for 6DoF parallax).
    //  - Theater   (2): never reaches here (Engine.cpp renders it flat on the panel).
    float A[4][4] = { { 0 } };
    // Each in-world mode has its OWN scale so tuning one never changes another: Diorama (3) shrinks to a
    // tabletop, First Person (1) uses its own life-feel scale, Third Person (0) uses the global World Scale.
    float effScale = (sViewMode == 3) ? sDioramaWorldScale
                   : (sViewMode == 1) ? sFirstPersonScale
                   : sWorldScale;
    float invS = 1.0f / (effScale < 1.0f ? 1.0f : effScale);
    A[0][0] = invS; A[1][1] = invS; A[2][2] = invS; A[3][3] = 1.0f;
    if (sViewMode == 3) {           // Diorama: shrunk tabletop placed in front of you
        A[3][1] = sDioramaHeight;
        A[3][2] = -sDioramaDist;
    } else if (sViewMode == 1) {    // First Person: own eye height + push the eye toward the kart
        A[3][1] = sFirstPersonEyeHeight;
        A[3][2] = sFPForwardCur; // eased toward the seat; only the drama pullback eases it away
    } else {                        // Third Person: own eye height. The camera DISTANCE is applied game-side in
        A[3][1] = sEyeHeight;       // the chase lookAt (vr_third_person_push_units + GameCamera) so it moves the
    }                               // camera HORIZONTALLY (closer/further), not up/down along the tilted view.


    // Clip planes adapt to the world scale so a big world isn't far-clipped to black.
    float worldHalfM = 10000.0f / sWorldScale;
    float zn = 0.02f;
    float zf = worldHalfM * 3.0f + 5.0f;

    // Symmetrize the fov so reducing eye separation scales all disparities proportionally and keeps
    // infinity at zero disparity (otherwise near + far can't both fuse).
    XrFovf fov = sViews[eye].fov;
    float aH = fmaxf(fabsf(fov.angleLeft), fabsf(fov.angleRight));
    float aV = fmaxf(fabsf(fov.angleUp),   fabsf(fov.angleDown));
    fov.angleLeft = -aH; fov.angleRight = aH;
    fov.angleDown = -aV; fov.angleUp = aV;
    sRenderFov[eye] = fov;

    float V[4][4], P[4][4], AV[4][4];
    mat_view_from_pose(V, pose);
    mat_proj_fov(P, fov, zn, zf);
    // First Person drama simulation: rotate chase-camera space with the kart's spin (yaw) and tumble
    // (pitch) so the driver experiences the hit, exactly like the SM64 port's flip cam rotates the
    // eye with Mario's flips. Skipped while the opt-in pull-back is showing the kart externally.
    if (sViewMode == 1 && sFPDramaOn && (sDramaYawRad != 0.0f || sDramaPitchRad != 0.0f) &&
        !vr_action_cam_pullback()) {
        float Ry[4][4], Rx[4][4], R[4][4], RA[4][4];
        mat_rot_y(Ry, -sDramaYawRad);
        mat_rot_x(Rx, -sDramaPitchRad);
        mat_mul(R, Ry, Rx);
        mat_mul(RA, R, A);
        mat_mul(AV, RA, V);
    } else {
        mat_mul(AV, A, V);
    }
    mat_mul(sEyeVP[eye], AV, P);

    // Rotation-only sky view-proj for the VR sky dome: zero the eye translation so the sphere sits at
    // infinity (no parallax) and tracks ONLY the head's ROTATION -> anchors yaw AND pitch. No world-scale
    // A factor (the dome verts are authored at R=1000 game units, which fit zn=1..zf=5000 here).
    {
        XrPosef skyPose = pose;
        skyPose.position.x = skyPose.position.y = skyPose.position.z = 0.0f;
        float Vsky[4][4], Psky[4][4];
        mat_view_from_pose(Vsky, skyPose);
        mat_proj_fov(Psky, fov, 1.0f, 5000.0f);
        mat_mul(sSkyVP[eye], Vsky, Psky);
        // (MK64's 2D cloud/star/snow sprites are world-anchored by the interpreter's per-vertex equiangular
        // remap onto the dome: it recovers each vertex's azimuth/elevation from its screen NDC using the
        // game-sky fov (vr_sky_fov_h/v) and projects the dome point through this same rotation-only sSkyVP.
        // A flat-plane matrix can't express that mapping - tangent-linear vs the sprites' angle-linear
        // placement made center clouds lead the world ~20% during kart turns.)
    }

    // Head-locked HUD plane: maps the game HUD's ortho output (NDC in [-1,1]) onto a view-space quad at
    // sHudDistM, sized by sHudScale, projected by the same eye fov. No eye offset -> head-locked, zero
    // disparity. The renderer multiplies the game's ortho matrix by this for post-3D 2D (HUD) draws.
    {
        // Use a SHARED symmetric fov across BOTH eyes so sHudVP is identical per eye -> the head-locked
        // HUD has no inter-eye disparity (no cross-eye). (Per-eye fov differs slightly; using it here
        // made the UI a touch cross-eyed.)
        float shAH = 0.0f, shAV = 0.0f;
        for (int k = 0; k < 2; k++) {
            shAH = fmaxf(shAH, fmaxf(fabsf(sViews[k].fov.angleLeft), fabsf(sViews[k].fov.angleRight)));
            shAV = fmaxf(shAV, fmaxf(fabsf(sViews[k].fov.angleUp),   fabsf(sViews[k].fov.angleDown)));
        }
        XrFovf hudFov;
        hudFov.angleLeft = -shAH; hudFov.angleRight = shAH;
        hudFov.angleDown = -shAV; hudFov.angleUp = shAV;
        float D  = (sHudDistM > 0.05f) ? sHudDistM : 2.0f;
        // Size the quad at a FIXED reference distance (not the live D) so changing HUD DISTANCE actually
        // makes the HUD recede and shrink, instead of the plane scaling with distance and looking
        // identical. At the 2.4m default it matches the old framing; farther = smaller, closer = bigger.
        float Dsz = 2.4f;
        float hw = Dsz * tanf(shAH) * sHudScale;
        float hh = Dsz * tanf(shAV) * sHudScale;
        float M[4][4] = { { 0 } };
        M[0][0] = hw; M[1][1] = hh; M[2][2] = 0.0f; M[3][2] = -D; M[3][3] = 1.0f;

        // HUD LOCK=WORLD: eye 0 (built first) updates the once-per-frame yaw decouple; both eyes apply it.
        // Defaults ON (world-locked) - the HUD stays put in the room instead of riding your face.
        if (eye == 0) {
            if (CVarGetInteger("gVRHudWorldLock", 1)) {
                float qy = sViews[0].pose.orientation.y, qw = sViews[0].pose.orientation.w;
                float qn = sqrtf(qy * qy + qw * qw);
                if (qn < 1e-6f) { qy = 0.0f; qw = 1.0f; qn = 1.0f; }
                float headYaw = 2.0f * atan2f(qy / qn, qw / qn);
                if (!sHudYawAnchored) { sHudYawAnchor = headYaw; sHudYawAnchored = true; }
                float d = headYaw - sHudYawAnchor;
                while (d >  3.14159265f) d -= 6.28318531f;
                while (d < -3.14159265f) d += 6.28318531f;
                sHudYawOffset = -d; // counter-rotate the plane by the head's yaw so it stays world-fixed
            } else {
                sHudYawAnchored = false;
                sHudYawOffset = 0.0f;
            }
        }
        float Phud[4][4];
        mat_proj_fov(Phud, hudFov, 0.05f, 100.0f);
        if (sHudYawOffset != 0.0f) {
            float Ry[4][4], Myaw[4][4];
            mat_rot_y(Ry, sHudYawOffset);
            mat_mul(Myaw, M, Ry);              // rotate the view-space quad about the eye -> stays world-yaw-fixed
            mat_mul(sHudVP[eye], Myaw, Phud);
        } else {
            mat_mul(sHudVP[eye], M, Phud);
        }
    }
}

// ---- helpers ----------------------------------------------------------------
static bool xrok(XrResult r, const char* what) {
    if (XR_SUCCEEDED(r)) return true;
    char buf[XR_MAX_RESULT_STRING_SIZE] = { 0 };
    if (sInstance != XR_NULL_HANDLE) xrResultToString(sInstance, r, buf);
    else snprintf(buf, sizeof buf, "%d", (int)r);
    printf("[VR] %s failed: %s\n", what, buf);
    return false;
}

// ---- Motion-controller input (OpenXR actions) --------------------------------
// Same shape as the SM64 build's proven layer: one gameplay action set covering both thumbsticks,
// the face buttons, the menu button, stick clicks, triggers, grips, and a haptic output per hand.
// Suggested bindings cover Quest Touch (and the Quest 3 / Pro native Touch Plus profile), Index,
// Reverb G2, WMR wands, Vive wands, and the khr simple fallback. main.c merges this state into the
// first N64 pad so the controllers work everywhere a gamepad does.
static bool     sHasTouchPlus  = false;          // runtime knows the Quest 3 / Pro native profile
static bool     sHasHpMR       = false;          // runtime knows the HP Reverb G2 profile
static XrActionSet sActionSet  = XR_NULL_HANDLE;
static XrAction sActMove       = XR_NULL_HANDLE; // left thumbstick (vector2)
static XrAction sActCam        = XR_NULL_HANDLE; // right thumbstick (vector2)
static XrAction sActBtnA       = XR_NULL_HANDLE;
static XrAction sActBtnB       = XR_NULL_HANDLE;
static XrAction sActBtnX       = XR_NULL_HANDLE;
static XrAction sActBtnY       = XR_NULL_HANDLE;
static XrAction sActMenuBtn    = XR_NULL_HANDLE;
static XrAction sActLStick     = XR_NULL_HANDLE; // thumbstick clicks
static XrAction sActRStick     = XR_NULL_HANDLE;
static XrAction sActLTrigger   = XR_NULL_HANDLE; // analog 0..1, turned digital here with hysteresis
static XrAction sActRTrigger   = XR_NULL_HANDLE;
static XrAction sActLGrip      = XR_NULL_HANDLE;
static XrAction sActRGrip      = XR_NULL_HANDLE;
static XrAction sActHaptic     = XR_NULL_HANDLE; // vibration output, per-hand subactions
static XrPath   sHandPath[2]   = { XR_NULL_PATH, XR_NULL_PATH };
static bool     sInputAttached = false;          // action set attached to the session
static unsigned sCtrlButtons   = 0;              // VR_BTN_* mask, refreshed each xrSyncActions
static float    sCtrlStick[2][2] = {{ 0 }};      // [hand][x,y], +x right +y up
static float    sRumbleAmp     = 0.0f;           // armed rumble amplitude (0 = off)
static XrTime   sRumbleUntil   = 0;              // stop re-arming past this time

static XrAction vr_make_action(XrActionType type, const char* name, const char* localized, bool perHand) {
    XrActionCreateInfo aci = { XR_TYPE_ACTION_CREATE_INFO };
    aci.actionType = type;
    strncpy(aci.actionName, name, XR_MAX_ACTION_NAME_SIZE - 1);
    strncpy(aci.localizedActionName, localized, XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
    if (perHand) { aci.countSubactionPaths = 2; aci.subactionPaths = sHandPath; }
    XrAction a = XR_NULL_HANDLE;
    if (!xrok(xrCreateAction(sActionSet, &aci, &a), name)) { return XR_NULL_HANDLE; }
    return a;
}

struct VrBind { XrAction action; const char* path; };
static void vr_suggest_profile(const char* profilePath, const VrBind* binds, int count) {
    XrPath profile = XR_NULL_PATH;
    if (XR_FAILED(xrStringToPath(sInstance, profilePath, &profile))) { return; }
    XrActionSuggestedBinding sb[32];
    uint32_t n = 0;
    for (int i = 0; i < count && n < 32; i++) {
        if (binds[i].action == XR_NULL_HANDLE) { continue; }
        XrPath p = XR_NULL_PATH;
        if (XR_FAILED(xrStringToPath(sInstance, binds[i].path, &p))) { continue; }
        sb[n].action  = binds[i].action;
        sb[n].binding = p;
        n++;
    }
    if (n == 0) { return; }
    XrInteractionProfileSuggestedBinding spb = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
    spb.interactionProfile = profile;
    spb.suggestedBindings = sb;
    spb.countSuggestedBindings = n;
    // Not fatal if the runtime rejects a profile it doesn't know; the others still apply.
    xrok(xrSuggestInteractionProfileBindings(sInstance, &spb), profilePath);
}

// Create the action set, suggest the per-device bindings and attach to the session. Runs once at
// the end of vr_boot; any failure leaves VR fully functional, just without motion controllers.
static void vr_input_create(void) {
    sInputAttached = false;
    sCtrlButtons = 0;
    memset(sCtrlStick, 0, sizeof(sCtrlStick));
    xrStringToPath(sInstance, "/user/hand/left",  &sHandPath[0]);
    xrStringToPath(sInstance, "/user/hand/right", &sHandPath[1]);

    XrActionSetCreateInfo asci = { XR_TYPE_ACTION_SET_CREATE_INFO };
    strncpy(asci.actionSetName, "gameplay", XR_MAX_ACTION_SET_NAME_SIZE - 1);
    strncpy(asci.localizedActionSetName, "Gameplay", XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE - 1);
    if (!xrok(xrCreateActionSet(sInstance, &asci, &sActionSet), "xrCreateActionSet")) {
        sActionSet = XR_NULL_HANDLE;
        return;
    }

    sActMove     = vr_make_action(XR_ACTION_TYPE_VECTOR2F_INPUT,  "move",          "Steer",               false);
    sActCam      = vr_make_action(XR_ACTION_TYPE_VECTOR2F_INPUT,  "camera",        "Camera",              false);
    sActBtnA     = vr_make_action(XR_ACTION_TYPE_BOOLEAN_INPUT,   "button_a",      "A Button",            false);
    sActBtnB     = vr_make_action(XR_ACTION_TYPE_BOOLEAN_INPUT,   "button_b",      "B Button",            false);
    sActBtnX     = vr_make_action(XR_ACTION_TYPE_BOOLEAN_INPUT,   "button_x",      "X Button",            false);
    sActBtnY     = vr_make_action(XR_ACTION_TYPE_BOOLEAN_INPUT,   "button_y",      "Y Button",            false);
    sActMenuBtn  = vr_make_action(XR_ACTION_TYPE_BOOLEAN_INPUT,   "menu",          "Pause",               false);
    sActLStick   = vr_make_action(XR_ACTION_TYPE_BOOLEAN_INPUT,   "left_stick",    "Left Stick Click",    false);
    sActRStick   = vr_make_action(XR_ACTION_TYPE_BOOLEAN_INPUT,   "right_stick",   "Right Stick Click",   false);
    sActLTrigger = vr_make_action(XR_ACTION_TYPE_FLOAT_INPUT,     "left_trigger",  "Left Trigger",        false);
    sActRTrigger = vr_make_action(XR_ACTION_TYPE_FLOAT_INPUT,     "right_trigger", "Right Trigger",       false);
    sActLGrip    = vr_make_action(XR_ACTION_TYPE_FLOAT_INPUT,     "left_grip",     "Left Grip",           false);
    sActRGrip    = vr_make_action(XR_ACTION_TYPE_FLOAT_INPUT,     "right_grip",    "Right Grip",          false);
    sActHaptic   = vr_make_action(XR_ACTION_TYPE_VIBRATION_OUTPUT,"rumble",        "Rumble",              true);

    // Quest Touch (the right controller's Oculus button is reserved by the system, so it isn't bound).
    const VrBind touch[] = {
        { sActMove,     "/user/hand/left/input/thumbstick" },
        { sActCam,      "/user/hand/right/input/thumbstick" },
        { sActBtnA,     "/user/hand/right/input/a/click" },
        { sActBtnB,     "/user/hand/right/input/b/click" },
        { sActBtnX,     "/user/hand/left/input/x/click" },
        { sActBtnY,     "/user/hand/left/input/y/click" },
        { sActMenuBtn,  "/user/hand/left/input/menu/click" },
        { sActLStick,   "/user/hand/left/input/thumbstick/click" },
        { sActRStick,   "/user/hand/right/input/thumbstick/click" },
        { sActLTrigger, "/user/hand/left/input/trigger/value" },
        { sActRTrigger, "/user/hand/right/input/trigger/value" },
        { sActLGrip,    "/user/hand/left/input/squeeze/value" },
        { sActRGrip,    "/user/hand/right/input/squeeze/value" },
        { sActHaptic,   "/user/hand/left/output/haptic" },
        { sActHaptic,   "/user/hand/right/output/haptic" },
    };
    vr_suggest_profile("/interaction_profiles/oculus/touch_controller", touch, (int)(sizeof(touch) / sizeof(touch[0])));

    // Valve Index: same layout, but A/B exist on both hands and there's no menu button.
    const VrBind index[] = {
        { sActMove,     "/user/hand/left/input/thumbstick" },
        { sActCam,      "/user/hand/right/input/thumbstick" },
        { sActBtnA,     "/user/hand/right/input/a/click" },
        { sActBtnB,     "/user/hand/right/input/b/click" },
        { sActBtnX,     "/user/hand/left/input/a/click" },
        { sActBtnY,     "/user/hand/left/input/b/click" },
        { sActLStick,   "/user/hand/left/input/thumbstick/click" },
        { sActRStick,   "/user/hand/right/input/thumbstick/click" },
        { sActLTrigger, "/user/hand/left/input/trigger/value" },
        { sActRTrigger, "/user/hand/right/input/trigger/value" },
        { sActLGrip,    "/user/hand/left/input/squeeze/value" },
        { sActRGrip,    "/user/hand/right/input/squeeze/value" },
        { sActHaptic,   "/user/hand/left/output/haptic" },
        { sActHaptic,   "/user/hand/right/output/haptic" },
    };
    vr_suggest_profile("/interaction_profiles/valve/index_controller", index, (int)(sizeof(index) / sizeof(index[0])));

    // Quest 3 / Quest Pro native profile: identical layout to Touch, so the same table applies.
    // Suggesting it explicitly matters: with only the older Touch bindings suggested, the runtime
    // auto-translates them onto Touch Plus and that translation can land buttons on the wrong hand.
    if (sHasTouchPlus) {
        vr_suggest_profile("/interaction_profiles/meta/touch_controller_plus", touch, (int)(sizeof(touch) / sizeof(touch[0])));
    }

    // HP Reverb G2: same control set as Touch (a/b right, x/y left, sticks, analog squeeze).
    if (sHasHpMR) {
        vr_suggest_profile("/interaction_profiles/hp/mixed_reality_controller", touch, (int)(sizeof(touch) / sizeof(touch[0])));
    }

    // Windows Mixed Reality wands: sticks and triggers as usual; no face buttons, so the trackpad
    // clicks stand in for A (right) and B (left). Squeeze is a click on these, not analog.
    const VrBind wmr[] = {
        { sActMove,     "/user/hand/left/input/thumbstick" },
        { sActCam,      "/user/hand/right/input/thumbstick" },
        { sActBtnA,     "/user/hand/right/input/trackpad/click" },
        { sActBtnB,     "/user/hand/left/input/trackpad/click" },
        { sActMenuBtn,  "/user/hand/left/input/menu/click" },
        { sActLStick,   "/user/hand/left/input/thumbstick/click" },
        { sActRStick,   "/user/hand/right/input/thumbstick/click" },
        { sActLTrigger, "/user/hand/left/input/trigger/value" },
        { sActRTrigger, "/user/hand/right/input/trigger/value" },
        { sActLGrip,    "/user/hand/left/input/squeeze/click" },
        { sActRGrip,    "/user/hand/right/input/squeeze/click" },
        { sActHaptic,   "/user/hand/left/output/haptic" },
        { sActHaptic,   "/user/hand/right/output/haptic" },
    };
    vr_suggest_profile("/interaction_profiles/microsoft/motion_controller", wmr, (int)(sizeof(wmr) / sizeof(wmr[0])));

    // Vive wands: no sticks at all, so the trackpads steer and drive the camera. Best-effort.
    const VrBind vive[] = {
        { sActMove,     "/user/hand/left/input/trackpad" },
        { sActCam,      "/user/hand/right/input/trackpad" },
        { sActBtnA,     "/user/hand/right/input/trackpad/click" },
        { sActBtnB,     "/user/hand/left/input/trackpad/click" },
        { sActMenuBtn,  "/user/hand/left/input/menu/click" },
        { sActLTrigger, "/user/hand/left/input/trigger/value" },
        { sActRTrigger, "/user/hand/right/input/trigger/value" },
        { sActLGrip,    "/user/hand/left/input/squeeze/click" },
        { sActRGrip,    "/user/hand/right/input/squeeze/click" },
        { sActHaptic,   "/user/hand/left/output/haptic" },
        { sActHaptic,   "/user/hand/right/output/haptic" },
    };
    vr_suggest_profile("/interaction_profiles/htc/vive_controller", vive, (int)(sizeof(vive) / sizeof(vive[0])));

    // Bare-minimum fallback profile every runtime understands (select + menu only).
    const VrBind simple[] = {
        { sActBtnA,    "/user/hand/right/input/select/click" },
        { sActBtnB,    "/user/hand/left/input/select/click" },
        { sActMenuBtn, "/user/hand/left/input/menu/click" },
        { sActHaptic,  "/user/hand/left/output/haptic" },
        { sActHaptic,  "/user/hand/right/output/haptic" },
    };
    vr_suggest_profile("/interaction_profiles/khr/simple_controller", simple, (int)(sizeof(simple) / sizeof(simple[0])));

    XrSessionActionSetsAttachInfo sai = { XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
    sai.countActionSets = 1;
    sai.actionSets = &sActionSet;
    if (!xrok(xrAttachSessionActionSets(sSession, &sai), "xrAttachSessionActionSets")) { return; }
    sInputAttached = true;
    printf("[VR] motion controllers ready (Touch / Touch Plus / Index / G2 / WMR / Vive profiles suggested).\n");
}

// Print which interaction profile the runtime actually bound for each hand. Fires whenever the
// runtime reports a profile change; this line is the first thing to check when a controller
// behaves oddly (wrong hand, dead buttons), since it shows what the runtime matched us to.
static void vr_log_active_profiles(void) {
    if (!sInputAttached || sSession == XR_NULL_HANDLE) { return; }
    static const char* handName[2] = { "left", "right" };
    for (int h = 0; h < 2; h++) {
        char buf[XR_MAX_PATH_LENGTH];
        snprintf(buf, sizeof(buf), "none (not bound)");
        XrInteractionProfileState ips = { XR_TYPE_INTERACTION_PROFILE_STATE };
        if (XR_SUCCEEDED(xrGetCurrentInteractionProfile(sSession, sHandPath[h], &ips))
            && ips.interactionProfile != XR_NULL_PATH) {
            uint32_t len = 0;
            xrPathToString(sInstance, ips.interactionProfile, sizeof(buf), &len, buf);
        }
        printf("[VR] %s controller profile: %s\n", handName[h], buf);
    }
}

static bool vr_action_bool(XrAction a) {
    if (a == XR_NULL_HANDLE) { return false; }
    XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO };
    gi.action = a;
    XrActionStateBoolean st = { XR_TYPE_ACTION_STATE_BOOLEAN };
    return XR_SUCCEEDED(xrGetActionStateBoolean(sSession, &gi, &st)) && st.isActive && st.currentState;
}

static float vr_action_float(XrAction a) {
    if (a == XR_NULL_HANDLE) { return 0.0f; }
    XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO };
    gi.action = a;
    XrActionStateFloat st = { XR_TYPE_ACTION_STATE_FLOAT };
    if (XR_FAILED(xrGetActionStateFloat(sSession, &gi, &st)) || !st.isActive) { return 0.0f; }
    return st.currentState;
}

static void vr_action_vec2(XrAction a, float out[2]) {
    out[0] = out[1] = 0.0f;
    if (a == XR_NULL_HANDLE) { return; }
    XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO };
    gi.action = a;
    XrActionStateVector2f st = { XR_TYPE_ACTION_STATE_VECTOR2F };
    if (XR_FAILED(xrGetActionStateVector2f(sSession, &gi, &st)) || !st.isActive) { return; }
    out[0] = st.currentState.x;
    out[1] = st.currentState.y;
}

// Analog trigger/grip to a digital button with hysteresis: press past 60%, release under 40%,
// so a finger resting lightly on the trigger can't flicker the bound action.
static bool vr_analog_latch(float v, bool held) {
    return held ? (v > 0.4f) : (v > 0.6f);
}

// Pull fresh controller state from the runtime. Once per begun frame, from vr_begin_frame.
static void vr_input_sync(void) {
    if (!sInputAttached) { return; }
    XrActiveActionSet active = { sActionSet, XR_NULL_PATH };
    XrActionsSyncInfo si = { XR_TYPE_ACTIONS_SYNC_INFO };
    si.countActiveActionSets = 1;
    si.activeActionSets = &active;
    // XR_SESSION_NOT_FOCUSED is a success code that means "no input for you" (headset off, runtime
    // menu up). Release everything so a button held at that moment can't stay stuck down, and
    // disarm rumble so the controllers don't keep buzzing in their holders.
    if (xrSyncActions(sSession, &si) != XR_SUCCESS) {
        sCtrlButtons = 0;
        memset(sCtrlStick, 0, sizeof(sCtrlStick));
        sRumbleAmp = 0.0f;
        return;
    }
    unsigned b = 0;
    if (vr_action_bool(sActBtnA))   { b |= VR_BTN_A; }
    if (vr_action_bool(sActBtnB))   { b |= VR_BTN_B; }
    if (vr_action_bool(sActBtnX))   { b |= VR_BTN_X; }
    if (vr_action_bool(sActBtnY))   { b |= VR_BTN_Y; }
    if (vr_action_bool(sActMenuBtn)){ b |= VR_BTN_MENU; }
    if (vr_action_bool(sActLStick)) { b |= VR_BTN_LSTICK; }
    if (vr_action_bool(sActRStick)) { b |= VR_BTN_RSTICK; }
    if (vr_analog_latch(vr_action_float(sActLTrigger), sCtrlButtons & VR_BTN_LTRIGGER)) { b |= VR_BTN_LTRIGGER; }
    if (vr_analog_latch(vr_action_float(sActRTrigger), sCtrlButtons & VR_BTN_RTRIGGER)) { b |= VR_BTN_RTRIGGER; }
    if (vr_analog_latch(vr_action_float(sActLGrip),    sCtrlButtons & VR_BTN_LGRIP))    { b |= VR_BTN_LGRIP; }
    if (vr_analog_latch(vr_action_float(sActRGrip),    sCtrlButtons & VR_BTN_RGRIP))    { b |= VR_BTN_RGRIP; }
    sCtrlButtons = b;
    vr_action_vec2(sActMove, sCtrlStick[0]);
    vr_action_vec2(sActCam,  sCtrlStick[1]);

    // Haptics: while armed, re-arm a SHORT burst every frame instead of ever submitting one long
    // vibration. Runtimes don't all honor stop requests promptly (or at all, over wireless) - a
    // long one-shot buzz that misses its stop can never be cancelled. Short bursts die on their
    // own right after the last re-arm, so a lost stop can't strand the motor.
    if (sRumbleAmp > 0.0f && sActHaptic != XR_NULL_HANDLE
        && sFrameState.predictedDisplayTime < sRumbleUntil) {
        XrHapticVibration vib = { XR_TYPE_HAPTIC_VIBRATION };
        vib.duration  = 60000000; // 60 ms: outlasts one frame, dies fast once re-arming stops
        vib.frequency = XR_FREQUENCY_UNSPECIFIED;
        vib.amplitude = sRumbleAmp;
        for (int h = 0; h < 2; h++) {
            XrHapticActionInfo hai = { XR_TYPE_HAPTIC_ACTION_INFO };
            hai.action = sActHaptic;
            hai.subactionPath = sHandPath[h];
            xrApplyHapticFeedback(sSession, &hai, (const XrHapticBaseHeader*)&vib);
        }
    }
}

extern "C" bool vr_controllers_active(void) {
    return sInputAttached && sRunning && sState == XR_SESSION_STATE_FOCUSED;
}

extern "C" unsigned vr_controller_buttons(void) {
    return vr_controllers_active() ? sCtrlButtons : 0;
}

extern "C" void vr_controller_stick(int hand, float out[2]) {
    if (!vr_controllers_active() || hand < 0 || hand > 1) { out[0] = out[1] = 0.0f; return; }
    out[0] = sCtrlStick[hand][0];
    out[1] = sCtrlStick[hand][1];
}

// Arm the rumble. No vibration is submitted here: vr_input_sync re-arms a short burst each
// frame while armed, so a runtime that mishandles stop requests can't strand the motor on.
extern "C" void vr_controller_rumble(float strength, float seconds) {
    if (!vr_controllers_active() || sActHaptic == XR_NULL_HANDLE) { return; }
    if (strength < 0.0f) { strength = 0.0f; }
    if (strength > 1.0f) { strength = 1.0f; }
    sRumbleAmp   = strength;
    sRumbleUntil = sFrameState.predictedDisplayTime + (XrTime)(seconds * 1e9);
}

extern "C" void vr_controller_rumble_stop(void) {
    sRumbleAmp = 0.0f;
    sRumbleUntil = 0;
}

extern "C" float vr_head_yaw_rad(void) {
    if (!sRunning || !sViewsValid) return 0.0f;
    XrQuaternionf q = sViews[0].pose.orientation;
    float fx = -2.0f * (q.x * q.z + q.w * q.y);
    float fz = -(1.0f - 2.0f * (q.x * q.x + q.y * q.y));
    return atan2f(fx, -fz);
}
extern "C" float vr_head_pitch_rad(void) {
    if (!sRunning || !sViewsValid) return 0.0f;
    XrQuaternionf q = sViews[0].pose.orientation;
    float fy = -2.0f * (q.y * q.z - q.w * q.x);
    if (fy >  1.0f) fy =  1.0f;
    if (fy < -1.0f) fy = -1.0f;
    return asinf(fy);
}
extern "C" short vr_item_throw_yaw_offset(int isHumanPlayer) {
    if (!isHumanPlayer || !sRunning || sViewMode != VR_VIEW_FIRST_PERSON) {
        return 0;
    }
    // Game binary yaw turns the opposite way to the headset's yaw (the cloud anchor work pinned
    // both conventions down), hence the negation. 0x10000 binary units = a full turn.
    return (short) (int) (-vr_head_yaw_rad() * (65536.0f / (2.0f * 3.14159265f)));
}
// Flip cam: the game reports once per logic frame whether the player kart is in a dramatic state
// (spinout, tumble, lightning, squish, big air, Lakitu rescue) and whether it has finished the race.
// With the flip cam opted in, First Person eases the eye back to the chase framing so you SEE your
// kart take the hit. Finishing always un-hides the kart for the post-race camera.
extern "C" void vr_set_action_cam(int dramatic, int finished) {
    if (dramatic) {
        sActionLatch = 40; // ~1.3 s hold past the last dramatic frame
        // Feel the hit in the hands too: re-armed every logic frame while the drama plays, so the
        // buzz dies on its own the moment the kart recovers.
        vr_controller_rumble(0.6f, 0.12f);
    } else if (sActionLatch > 0) {
        sActionLatch--;
    }
    sRaceFinished = finished;
}
// The kart un-hide: flip-cam pull-backs and the post-race sequence both show the player kart.
extern "C" int vr_action_cam_active(void) {
    if (!sRunning || sViewMode != VR_VIEW_FIRST_PERSON) {
        return 0;
    }
    return (sFlipCamOn && sActionLatch > 0) || sRaceFinished;
}
// The camera pull-back: opt-in only. Without it First Person stays in the kart through everything.
static int vr_action_cam_pullback(void) {
    return sFlipCamOn && sRunning && sViewMode == VR_VIEW_FIRST_PERSON && (sActionLatch > 0 || sRaceFinished);
}
// The kart's current spin/tumble angles, fed once per logic frame so First Person can SIMULATE the
// hit in-place (the default; the flip-cam pull-back replaces this when opted in). Binary angles,
// 0x10000 = a full turn. Only the TARGETS update here; the eased values chase them per render
// frame in vr_begin_frame so the 30Hz game steps don't read as judder at the headset rate.
extern "C" void vr_set_drama_rot(short yawBinary, short pitchBinary) {
    sDramaYawTgt   = (float) yawBinary * (2.0f * 3.14159265f / 65536.0f);
    sDramaPitchTgt = (float) pitchBinary * (2.0f * 3.14159265f / 65536.0f);
}
extern "C" int vr_display_refresh_hz(void) {
    if (sRunning && sFrameState.predictedDisplayPeriod > 0) {
        double hz = 1.0e9 / (double)sFrameState.predictedDisplayPeriod;
        if (hz >= 30.0 && hz <= 1000.0) return (int)(hz + 0.5);
    }
    return 90; // sane default until the first frame's display period is known
}

static int64_t vr_choose_swapchain_format(void) {
    uint32_t n = 0;
    if (!XR_SUCCEEDED(xrEnumerateSwapchainFormats(sSession, 0, &n, NULL)) || n == 0)
        return GL_SRGB8_ALPHA8;
    int64_t* fmts = (int64_t*)calloc(n, sizeof(int64_t));
    xrEnumerateSwapchainFormats(sSession, n, &n, fmts);
    const int64_t prefs[] = { GL_SRGB8_ALPHA8, GL_RGBA8 };
    int64_t chosen = fmts[0];
    bool found = false;
    for (uint32_t p = 0; p < 2 && !found; p++)
        for (uint32_t i = 0; i < n; i++)
            if (fmts[i] == prefs[p]) { chosen = prefs[p]; found = true; break; }
    free(fmts);
    printf("[VR] swapchain format: 0x%llx\n", (unsigned long long)chosen);
    return chosen;
}

// Lightweight startup probe: is a VR headset connected right now?
extern "C" bool vr_headset_present(void) {
    const char* exts[1] = { XR_KHR_OPENGL_ENABLE_EXTENSION_NAME };
    XrInstanceCreateInfo ici = { XR_TYPE_INSTANCE_CREATE_INFO };
    ici.enabledExtensionCount = 1;
    ici.enabledExtensionNames = exts;
    strncpy(ici.applicationInfo.applicationName, "SpaghettiKart", XR_MAX_APPLICATION_NAME_SIZE - 1);
    ici.applicationInfo.apiVersion = XR_API_VERSION_1_0; // VirtualDesktopXR etc. are OpenXR 1.0
    XrInstance inst = XR_NULL_HANDLE;
    if (XR_FAILED(xrCreateInstance(&ici, &inst)) || inst == XR_NULL_HANDLE) return false;
    XrSystemGetInfo sgi = { XR_TYPE_SYSTEM_GET_INFO };
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId sys = XR_NULL_SYSTEM_ID;
    XrResult r = xrGetSystem(inst, &sgi, &sys);
    xrDestroyInstance(inst);
    return XR_SUCCEEDED(r) && sys != XR_NULL_SYSTEM_ID;
}

static void vr_boot(void) {
    setvbuf(stdout, NULL, _IONBF, 0); // unbuffered so [VR] logs flush immediately (visible even if killed)
    printf("[VR] booting OpenXR...\n");
    // Probe the optional controller-profile extensions before creating the instance: the Quest 3 /
    // Pro native Touch Plus profile and the HP Reverb G2 profile only exist behind extensions, and
    // suggesting the native profile avoids the runtime's auto-translation (which can land buttons
    // on the wrong hand).
    sHasTouchPlus = false;
    sHasHpMR = false;
    {
        uint32_t ec = 0;
        if (XR_SUCCEEDED(xrEnumerateInstanceExtensionProperties(NULL, 0, &ec, NULL)) && ec > 0) {
            XrExtensionProperties* ep = (XrExtensionProperties*)calloc(ec, sizeof(XrExtensionProperties));
            for (uint32_t i = 0; i < ec; i++) ep[i].type = XR_TYPE_EXTENSION_PROPERTIES;
            if (XR_SUCCEEDED(xrEnumerateInstanceExtensionProperties(NULL, ec, &ec, ep))) {
                for (uint32_t i = 0; i < ec; i++) {
                    if (strcmp(ep[i].extensionName, "XR_META_touch_controller_plus")      == 0) sHasTouchPlus = true;
                    if (strcmp(ep[i].extensionName, "XR_EXT_hp_mixed_reality_controller") == 0) sHasHpMR = true;
                }
            }
            free(ep);
        }
    }
    const char* exts[3];
    uint32_t nexts = 0;
    exts[nexts++] = XR_KHR_OPENGL_ENABLE_EXTENSION_NAME;
    if (sHasTouchPlus) exts[nexts++] = "XR_META_touch_controller_plus";
    if (sHasHpMR)      exts[nexts++] = "XR_EXT_hp_mixed_reality_controller";
    XrInstanceCreateInfo ici = { XR_TYPE_INSTANCE_CREATE_INFO };
    ici.enabledExtensionCount = nexts;
    ici.enabledExtensionNames = exts;
    strncpy(ici.applicationInfo.applicationName, "SpaghettiKart", XR_MAX_APPLICATION_NAME_SIZE - 1);
    ici.applicationInfo.applicationVersion = 1;
    strncpy(ici.applicationInfo.engineName, "fast3d", XR_MAX_ENGINE_NAME_SIZE - 1);
    ici.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    if (!xrok(xrCreateInstance(&ici, &sInstance), "xrCreateInstance")) { vr_shutdown(); return; }

    XrInstanceProperties props = { XR_TYPE_INSTANCE_PROPERTIES };
    if (XR_SUCCEEDED(xrGetInstanceProperties(sInstance, &props)))
        printf("[VR] runtime: %s\n", props.runtimeName);

    XrSystemGetInfo sgi = { XR_TYPE_SYSTEM_GET_INFO };
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (!xrok(xrGetSystem(sInstance, &sgi, &sSystemId), "xrGetSystem")) { vr_shutdown(); return; }

    if (!xrok(xrGetInstanceProcAddr(sInstance, "xrGetOpenGLGraphicsRequirementsKHR",
            (PFN_xrVoidFunction*)&pfnGetGLReq), "get xrGetOpenGLGraphicsRequirementsKHR")) { vr_shutdown(); return; }
    XrGraphicsRequirementsOpenGLKHR glReq = { XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR };
    if (!xrok(pfnGetGLReq(sInstance, sSystemId, &glReq), "xrGetOpenGLGraphicsRequirementsKHR")) { vr_shutdown(); return; }

    HDC   hdc  = wglGetCurrentDC();
    HGLRC glrc = wglGetCurrentContext();
    if (!hdc || !glrc) { printf("[VR] no current WGL context - is the OpenGL backend active? (VR needs it)\n"); vr_shutdown(); return; }

    XrGraphicsBindingOpenGLWin32KHR gb = { XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR };
    gb.hDC = hdc;
    gb.hGLRC = glrc;
    XrSessionCreateInfo sci = { XR_TYPE_SESSION_CREATE_INFO };
    sci.next = &gb;
    sci.systemId = sSystemId;
    if (!xrok(xrCreateSession(sInstance, &sci, &sSession), "xrCreateSession")) { vr_shutdown(); return; }

    XrReferenceSpaceCreateInfo rsci = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rsci.poseInReferenceSpace.orientation.w = 1.0f;
    if (!xrok(xrCreateReferenceSpace(sSession, &rsci, &sLocalSpace), "xrCreateReferenceSpace")) { vr_shutdown(); return; }

    XrViewConfigurationType vct = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    uint32_t n = 0;
    if (!xrok(xrEnumerateViewConfigurationViews(sInstance, sSystemId, vct, 0, &n, NULL), "enum view count")) { vr_shutdown(); return; }
    if (n > 2) n = 2;
    for (uint32_t i = 0; i < n; i++) sViewConfigs[i].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
    if (!xrok(xrEnumerateViewConfigurationViews(sInstance, sSystemId, vct, n, &n, sViewConfigs), "enum views")) { vr_shutdown(); return; }
    sViewCount = n;
    printf("[VR] %u eyes, %ux%u per eye recommended\n", n,
        sViewConfigs[0].recommendedImageRectWidth, sViewConfigs[0].recommendedImageRectHeight);

    int64_t fmt = vr_choose_swapchain_format();
    for (uint32_t e = 0; e < sViewCount; e++) {
        XrSwapchainCreateInfo scci = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
        scci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        scci.format = fmt;
        scci.sampleCount = 1;
        scci.faceCount = 1; scci.arraySize = 1; scci.mipCount = 1;
        // Try the recommended size, then fall back if GPU memory is tight (so VR still boots).
        // Start below the runtime's (often supersampled) recommended size: rendering the full scene
        // twice per eye per interpolation step is heavy, and falling behind the headset frame budget
        // slows the game tick + underruns the audio (crackling). 0.8x trades a little sharpness for the
        // headroom to keep pace; falls back further under GPU-memory pressure.
        static const float scEyeScales[] = { 0.8f, 0.7f, 0.6f, 0.5f, 0.4f };
        XrResult scRes = XR_ERROR_RUNTIME_FAILURE;
        for (int s = 0; s < (int)(sizeof(scEyeScales) / sizeof(scEyeScales[0])); s++) {
            scci.width  = (uint32_t)(sViewConfigs[e].recommendedImageRectWidth  * scEyeScales[s]);
            scci.height = (uint32_t)(sViewConfigs[e].recommendedImageRectHeight * scEyeScales[s]);
            scRes = xrCreateSwapchain(sSession, &scci, &sEye[e].handle);
            if (XR_SUCCEEDED(scRes)) {
                if (scEyeScales[s] < 1.0f)
                    printf("[VR] eye %u swapchain fell back to %ux%u\n", e, scci.width, scci.height);
                break;
            }
        }
        if (!XR_SUCCEEDED(scRes)) {
            printf("[VR] xrCreateSwapchain failed at every size - VR disabled.\n");
            vr_shutdown(); return;
        }
        sEye[e].w = scci.width;
        sEye[e].h = scci.height;

        uint32_t imgN = 0;
        xrEnumerateSwapchainImages(sEye[e].handle, 0, &imgN, NULL);
        sEye[e].images = (XrSwapchainImageOpenGLKHR*)calloc(imgN, sizeof(XrSwapchainImageOpenGLKHR));
        for (uint32_t i = 0; i < imgN; i++) sEye[e].images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR;
        xrEnumerateSwapchainImages(sEye[e].handle, imgN, &imgN, (XrSwapchainImageBaseHeader*)sEye[e].images);
        sEye[e].imgCount = imgN;
    }

    glGenFramebuffers(1, &sEyeFbo);
    glGenFramebuffers(1, &sBlitReadFbo);
    glGenRenderbuffers(1, &sEyeDepthRB);
    glBindRenderbuffer(GL_RENDERBUFFER, sEyeDepthRB);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, (GLsizei)sEye[0].w, (GLsizei)sEye[0].h);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);

    XrReferenceSpaceCreateInfo vrci = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    vrci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    vrci.poseInReferenceSpace.orientation.w = 1.0f;
    xrok(xrCreateReferenceSpace(sSession, &vrci, &sViewSpace), "xrCreateReferenceSpace(VIEW)");

    XrSwapchainCreateInfo oci = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
    oci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    oci.format = fmt;
    oci.sampleCount = 1;
    oci.width  = sHud.w = (uint32_t)sOverlayW;
    oci.height = sHud.h = (uint32_t)sOverlayH;
    oci.faceCount = 1; oci.arraySize = 1; oci.mipCount = 1;
    if (xrok(xrCreateSwapchain(sSession, &oci, &sHud.handle), "xrCreateSwapchain(hud)")) {
        uint32_t hn = 0;
        xrEnumerateSwapchainImages(sHud.handle, 0, &hn, NULL);
        sHud.images = (XrSwapchainImageOpenGLKHR*)calloc(hn, sizeof(XrSwapchainImageOpenGLKHR));
        for (uint32_t i = 0; i < hn; i++) sHud.images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR;
        xrEnumerateSwapchainImages(sHud.handle, hn, &hn, (XrSwapchainImageBaseHeader*)sHud.images);
        sHud.imgCount = hn;
        glGenFramebuffers(1, &sOverlayFbo);
        glGenRenderbuffers(1, &sOverlayDepthRB);
        glBindRenderbuffer(GL_RENDERBUFFER, sOverlayDepthRB);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, (GLsizei)sOverlayW, (GLsizei)sOverlayH);
        glBindRenderbuffer(GL_RENDERBUFFER, 0);
    }

    // Motion controllers: action set + per-device suggested bindings. Failure here is non-fatal
    // (VR keeps rendering, the gamepad keeps working).
    vr_input_create();

    printf("[VR] OpenXR ready; waiting for session to start.\n");
}

static void vr_poll_events(void) {
    XrEventDataBuffer ev = { XR_TYPE_EVENT_DATA_BUFFER };
    while (xrPollEvent(sInstance, &ev) == XR_SUCCESS) {
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            const XrEventDataSessionStateChanged* e = (const XrEventDataSessionStateChanged*)&ev;
            sState = e->state;
            if (e->state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo sbi = { XR_TYPE_SESSION_BEGIN_INFO };
                sbi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                if (xrok(xrBeginSession(sSession, &sbi), "xrBeginSession")) {
                    sRunning = true;
                    printf("[VR] session running.\n");
                }
            } else if (e->state == XR_SESSION_STATE_STOPPING) {
                xrEndSession(sSession);
                sRunning = false;
                printf("[VR] session stopped.\n");
            }
        } else if (ev.type == XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED) {
            vr_log_active_profiles(); // which controller profile each hand actually bound to
        }
        ev.type = XR_TYPE_EVENT_DATA_BUFFER;
    }
}

// --- in-game VR menu accessors ----------------------------------------------
extern "C" float vr_get_world_scale(void)  { return sWorldScale; }
extern "C" void  vr_set_world_scale(float v){ sWorldScale = (v < 1.0f) ? 1.0f : v; }
extern "C" float vr_get_stereo(void)        { return sStereoScale; }
extern "C" void  vr_set_stereo(float v)     { sStereoScale = (v < 0.0f) ? 0.0f : v; }
extern "C" float vr_get_head_scale(void)    { return sHeadScale; }
extern "C" void  vr_set_head_scale(float v) { sHeadScale = (v < 0.0f) ? 0.0f : (v > 1.5f ? 1.5f : v); }
extern "C" float vr_get_eye_height(void)    { return sEyeHeight; }
extern "C" void  vr_set_eye_height(float v) { sEyeHeight = v; }
extern "C" bool  vr_get_horizon_lock(void)  { return sHorizonLock; }
extern "C" void  vr_set_horizon_lock(bool on){ sHorizonLock = on; }
extern "C" bool  vr_get_vignette(void)      { return sVignette; }
extern "C" void  vr_set_vignette(bool on)   { sVignette = on; }
extern "C" bool  vr_get_cockpit(void)       { return sCockpit; }
extern "C" void  vr_set_cockpit(bool on)    { sCockpit = on; }
extern "C" float vr_get_menu_dist(void)     { return sMenuDist; }
extern "C" void  vr_set_menu_dist(float v)  { sMenuDist = v; }
extern "C" float vr_get_menu_size(void)     { return sMenuSize; }
extern "C" void  vr_set_menu_size(float v)  { sMenuSize = v; }
extern "C" float vr_get_hud_scale(void)     { return sHudScale; }
extern "C" void  vr_set_hud_scale(float v)  { sHudScale = (v < 0.1f) ? 0.1f : (v > 1.5f ? 1.5f : v); }
extern "C" float vr_get_hud_dist(void)      { return sHudDistM; }
extern "C" void  vr_set_hud_dist(float v)   { sHudDistM = (v < 0.3f) ? 0.3f : (v > 20.0f ? 20.0f : v); }
// Read the live CVar (not the cached sViewMode) so Engine's per-frame stereo/Theater gate matches the
// eye-matrix build on the SAME frame - avoids a one-frame mismatched render when switching modes. Clamped.
extern "C" int   vr_get_view_mode(void)     { int m = CVarGetInteger("gVRViewMode", sViewMode); return (m < 0) ? 0 : (m > 3 ? 3 : m); }
extern "C" void  vr_set_view_mode(int m)    { sViewMode = (m < 0) ? 0 : (m > 3 ? 3 : m); }
// How far (GAME units) to push the chase camera back along the horizontal kart->camera direction in Third
// Person VR. The game (GameCamera::SetViewProjection) moves the eye by this so distance reads as closer/
// further (horizontal), not up/down. Converts the meters slider via the Third Person world scale. 0 unless
// VR is live AND in Third Person, so flat play and the other modes are untouched.
extern "C" float vr_third_person_push_units(void) {
    if (!sRunning || sViewMode != 0) return 0.0f;
    return sThirdPersonDist * (sWorldScale < 1.0f ? 1.0f : sWorldScale);
}
// How far (GAME units) the First Person eye currently sits ahead of the chase camera along the
// camera->kart ray. The kart shadow rides this so it stays under the player's seat at any CAM
// DISTANCE instead of hanging back at the kart origin. 0 outside First Person (and pre-race,
// where the forward push eases to 0 anyway).
extern "C" float vr_fp_forward_game_units(void) {
    if (!sRunning || vr_get_view_mode() != VR_VIEW_FIRST_PERSON) return 0.0f;
    return sFPForwardCur * (sFirstPersonScale < 1.0f ? 1.0f : sFirstPersonScale);
}
extern "C" void  vr_reset_defaults(void) {
    sWorldScale = 100.0f; sStereoScale = 0.5f; sHeadScale = 1.0f; sEyeHeight = 0.0f;
    sHorizonLock = true; sVignette = true; sCockpit = true;
    sMenuDist = 3.0f; sMenuSize = 4.8f;
    sHeadRestSet = false; sHeadWarmup = 0; sPanelAnchorValid = false;
    printf("[VR] reset to defaults.\n");
}

extern "C" void vr_set_cockpit_pose(const float headPosWorld[3], float headingRad, float pitchRad, float rollRad) {
    sCockpitPos[0] = headPosWorld[0]; sCockpitPos[1] = headPosWorld[1]; sCockpitPos[2] = headPosWorld[2];
    sCockpitHeading = headingRad; sCockpitPitch = pitchRad; sCockpitRoll = rollRad;
    sCockpitValid = true;
}
extern "C" void vr_set_motion_intensity(float v) { sMotionIntensity = v < 0 ? 0 : (v > 1 ? 1 : v); }

extern "C" void vr_begin_frame(void) {
    if (!sRequested) return;
    if (!sBootTried) { sBootTried = true; vr_boot(); }
    if (sSession == XR_NULL_HANDLE) return;

    vr_poll_events();

    // Live VR tunables from the in-game Enhancements > VR menu (CVars). Read each frame so slider
    // changes apply instantly. Default = the current value, so an unset CVar keeps the built-in default.
    sWorldScale  = CVarGetFloat("gVRWorldScale", sWorldScale);
    sStereoScale = CVarGetFloat("gVRStereo",     sStereoScale);
    sEyeHeight   = CVarGetFloat("gVREyeHeight",   sEyeHeight);
    sHudScale    = CVarGetFloat("gVRHudScale",    sHudScale);
    sHudDistM    = CVarGetFloat("gVRHudDist",     sHudDistM);
    sMenuDist    = CVarGetFloat("gVRMenuDist",    sMenuDist);
    sMenuSize    = CVarGetFloat("gVRMenuSize",    sMenuSize);
    sViewMode           = CVarGetInteger("gVRViewMode",      sViewMode);
    if (sViewMode < 0) sViewMode = 0; else if (sViewMode > 3) sViewMode = 3;
    sFirstPersonForward   = CVarGetFloat("gVRFirstPersonFwd",       sFirstPersonForward);
    // First Person sits AT the driver's seat from the very first frame - pre-race included. (An
    // earlier cut held the push at 0 through the countdown so Lakitu's start light stayed in
    // view, but that read as "my body starts way out in front of me" - the eye trailed the kart
    // until the ease caught up after GO. Reverted by request.) The only thing that still pulls
    // the eye back is the action-cam pullback (drama / finish), with the quick ease out.
    // sFPForwardCur is what the eye matrix uses (A[3][2]).
    {
        sFlipCamOn = CVarGetInteger("gVRFlipCam", 0);
        sFPDramaOn = CVarGetInteger("gVRFPDrama", 1);
        float fpTarget = !vr_action_cam_pullback() ? sFirstPersonForward : 0.0f;
        if (fpTarget < sFPForwardCur) { sFPForwardCur += (fpTarget - sFPForwardCur) * 0.35f; } // quick ease out
        else { sFPForwardCur += (fpTarget - sFPForwardCur) * 0.12f; }                          // gentle ease back

        // Smooth the First Person drama rotation at the headset rate. The game's spin/tumble angles
        // step once per 30Hz logic tick; chasing them on the shortest angular path here turns the
        // held-then-jump staircase into continuous motion, and softens both onset and recovery. The
        // chase is deliberately lazy (low rate + a hard per-frame cap of ~260 deg/s) so a full spin
        // reads as a smooth sweep rather than a violent snap.
        float dYaw = wrap_pi(sDramaYawTgt - sDramaYawRad) * 0.10f;
        float dPitch = wrap_pi(sDramaPitchTgt - sDramaPitchRad) * 0.10f;
        if (dYaw > 0.05f) { dYaw = 0.05f; } else if (dYaw < -0.05f) { dYaw = -0.05f; }
        if (dPitch > 0.05f) { dPitch = 0.05f; } else if (dPitch < -0.05f) { dPitch = -0.05f; }
        sDramaYawRad += dYaw;
        sDramaPitchRad += dPitch;
        if (sDramaYawTgt == 0.0f && fabsf(sDramaYawRad) < 0.003f) { sDramaYawRad = 0.0f; }
        if (sDramaPitchTgt == 0.0f && fabsf(sDramaPitchRad) < 0.003f) { sDramaPitchRad = 0.0f; }
    }
    sFirstPersonScale     = CVarGetFloat("gVRFirstPersonScale",     sFirstPersonScale);
    sFirstPersonEyeHeight = CVarGetFloat("gVRFirstPersonEyeHeight", sFirstPersonEyeHeight);
    sThirdPersonDist      = CVarGetFloat("gVRThirdPersonDist",      sThirdPersonDist);
    sDioramaWorldScale    = CVarGetFloat("gVRDioramaWorldScale",    sDioramaWorldScale);
    sDioramaDist        = CVarGetFloat("gVRDioramaDist",      sDioramaDist);
    sDioramaHeight      = CVarGetFloat("gVRDioramaHeight",    sDioramaHeight);
    sMenuOpacity        = CVarGetFloat("gVRMenuOpacity",      sMenuOpacity);

    sViewsValid = false;
    sPoseTracked = false;
    sHudReady = false;
    sPanelMode = false;
    sEyesSubmitted = 0;
    if (!sRunning) return;

    XrFrameWaitInfo fwi = { XR_TYPE_FRAME_WAIT_INFO };
    sFrameState.type = XR_TYPE_FRAME_STATE;
    sFrameState.next = NULL;
    if (!xrok(xrWaitFrame(sSession, &fwi, &sFrameState), "xrWaitFrame")) return;

    XrFrameBeginInfo fbi = { XR_TYPE_FRAME_BEGIN_INFO };
    if (!xrok(xrBeginFrame(sSession, &fbi), "xrBeginFrame")) return;
    sFrameBegun = true;

    // Fresh motion-controller state for this frame (and the haptic re-arm) - after xrWaitFrame so
    // the rumble timing reads the new predicted display time.
    vr_input_sync();

    if (sFrameState.shouldRender) {
        XrViewLocateInfo vli = { XR_TYPE_VIEW_LOCATE_INFO };
        vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        vli.displayTime = sFrameState.predictedDisplayTime;
        vli.space = sLocalSpace;
        XrViewState vs = { XR_TYPE_VIEW_STATE };
        sViews[0].type = XR_TYPE_VIEW;
        sViews[1].type = XR_TYPE_VIEW;
        uint32_t got = 0;
        if (XR_SUCCEEDED(xrLocateViews(sSession, &vli, &vs, 2, &got, sViews)) && got == 2) {
            sViewsValid = true;
            sPoseTracked = (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0
                        && (vs.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT)    != 0;
            vr_build_eye_matrix(0);
            vr_build_eye_matrix(1);
        }
    }
}

extern "C" bool vr_begin_eye(int eye) {
    if (!sRunning || !sFrameBegun || !sFrameState.shouldRender || !sViewsValid) return false;
    if (eye < 0 || eye >= (int)sViewCount || sEyeFbo == 0) return false;

    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    if (!XR_SUCCEEDED(xrAcquireSwapchainImage(sEye[eye].handle, &ai, &idx))) return false;
    XrSwapchainImageWaitInfo swi = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    swi.timeout = XR_INFINITE_DURATION;
    xrWaitSwapchainImage(sEye[eye].handle, &swi);
    sEyeImgIdx[eye] = idx;

    glBindFramebuffer(GL_FRAMEBUFFER, sEyeFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sEye[eye].images[idx].image, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, sEyeDepthRB);
    glViewport(0, 0, (GLsizei)sEye[eye].w, (GLsizei)sEye[eye].h);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    return true;
}

extern "C" void vr_end_eye(int eye) {
    if (eye < 0 || eye >= (int)sViewCount) return;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    xrReleaseSwapchainImage(sEye[eye].handle, &ri);

    memset(&sProjViews[eye], 0, sizeof(sProjViews[eye]));
    sProjViews[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
    sProjViews[eye].pose = sRenderPose[eye];
    sProjViews[eye].fov  = sRenderFov[eye];
    sProjViews[eye].subImage.swapchain = sEye[eye].handle;
    sProjViews[eye].subImage.imageRect.offset.x = 0;
    sProjViews[eye].subImage.imageRect.offset.y = 0;
    sProjViews[eye].subImage.imageRect.extent.width  = (int32_t)sEye[eye].w;
    sProjViews[eye].subImage.imageRect.extent.height = (int32_t)sEye[eye].h;
}

extern "C" bool vr_begin_overlay(bool sky) {
    (void)sky;
    if (!sRunning || !sFrameBegun || !sFrameState.shouldRender) return false;
    if (sOverlayFbo == 0 || sHud.handle == XR_NULL_HANDLE) return false;
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    if (!XR_SUCCEEDED(xrAcquireSwapchainImage(sHud.handle, &ai, &idx))) return false;
    XrSwapchainImageWaitInfo swi = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    swi.timeout = XR_INFINITE_DURATION;
    xrWaitSwapchainImage(sHud.handle, &swi);
    sHudImgIdx = idx;

    glBindFramebuffer(GL_FRAMEBUFFER, sOverlayFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sHud.images[idx].image, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, sOverlayDepthRB);
    glViewport(0, 0, sOverlayW, sOverlayH);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    return true;
}

extern "C" void vr_end_overlay(bool sky) {
    (void)sky;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    xrReleaseSwapchainImage(sHud.handle, &ri);
    sHudReady = true;
}

extern "C" void vr_set_panel_mode(bool on) { sPanelMode = on; }
extern "C" bool vr_begin_panel(void) { sPanelMode = true; return vr_begin_overlay(false); }
extern "C" void vr_end_panel(void) { vr_end_overlay(false); }

// Blit a rendered texture (from the interpreter's managed VR fb, sized srcW x srcH) into the given
// swapchain image, flipped vertically. The managed fb is rendered invert-Y=true like mGameFb; the
// desktop present (CopyFramebuffer) flips it to the screen because screen invertY=false, so the
// OpenXR swapchain (same bottom-left GL convention as the screen) needs the same flip - otherwise the
// image is upside down. The flip is whole-framebuffer, so it applies uniformly to both the stereo eyes
// and the 2D menu panel. sEyeFbo is reused as a scratch DRAW fbo for the swapchain image.
static bool vr_blit_into(VrSwapchain* sc, unsigned int glTex, int srcW, int srcH, uint32_t* outIdx) {
    if (sc->handle == XR_NULL_HANDLE) return false;
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    if (!XR_SUCCEEDED(xrAcquireSwapchainImage(sc->handle, &ai, &idx))) return false;
    XrSwapchainImageWaitInfo swi = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    swi.timeout = XR_INFINITE_DURATION;
    xrWaitSwapchainImage(sc->handle, &swi);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, sBlitReadFbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, (GLuint)glTex, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, sEyeFbo);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sc->images[idx].image, 0);
    glDisable(GL_SCISSOR_TEST);
    GLenum filt = (srcW == (int)sc->w && srcH == (int)sc->h) ? GL_NEAREST : GL_LINEAR;
    // Flip vertically: src bottom->dst top, src top->dst bottom (dst Y0/Y1 swapped).
    glBlitFramebuffer(0, 0, srcW, srcH, 0, (int)sc->h, (int)sc->w, 0, GL_COLOR_BUFFER_BIT, filt);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);

    XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    xrReleaseSwapchainImage(sc->handle, &ri);
    *outIdx = idx;
    return true;
}

extern "C" bool vr_submit_eye_texture(int eye, unsigned int glTex, int w, int h) {
    if (!sRunning || !sFrameBegun || !sFrameState.shouldRender || !sViewsValid) return false;
    if (eye < 0 || eye >= (int)sViewCount || sEyeFbo == 0 || glTex == 0) return false;
    uint32_t idx = 0;
    if (!vr_blit_into(&sEye[eye], glTex, w, h, &idx)) return false;
    sEyeImgIdx[eye] = idx;
    sEyesSubmitted++;
    memset(&sProjViews[eye], 0, sizeof(sProjViews[eye]));
    sProjViews[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
    sProjViews[eye].pose = sRenderPose[eye];
    sProjViews[eye].fov  = sRenderFov[eye];
    sProjViews[eye].subImage.swapchain = sEye[eye].handle;
    sProjViews[eye].subImage.imageRect.offset.x = 0;
    sProjViews[eye].subImage.imageRect.offset.y = 0;
    sProjViews[eye].subImage.imageRect.extent.width  = (int32_t)sEye[eye].w;
    sProjViews[eye].subImage.imageRect.extent.height = (int32_t)sEye[eye].h;
    return true;
}

extern "C" bool vr_submit_panel_texture(unsigned int glTex, int w, int h) {
    if (!sRunning || !sFrameBegun || !sFrameState.shouldRender) return false;
    if (sEyeFbo == 0 || glTex == 0) return false;
    uint32_t idx = 0;
    if (!vr_blit_into(&sHud, glTex, w, h, &idx)) return false;
    sHudImgIdx = idx;
    sHudReady = true;
    sPanelMode = true;
    return true;
}

// Present the desktop default framebuffer (window back buffer, with the ImGui menu drawn into it) on
// the head-locked panel, so the in-game menu is usable inside the headset. srcW/srcH = window pixel
// size (the region of fb0 to copy). fb0 is invert-Y=false (unlike the managed fb), so a STRAIGHT blit
// (no vertical flip) lands upright in the swapchain.
extern "C" bool vr_present_desktop_panel(int srcW, int srcH) {
    if (!sRunning || !sFrameBegun || !sFrameState.shouldRender) return false;
    if (sEyeFbo == 0 || sHud.handle == XR_NULL_HANDLE) return false;
    if (srcW <= 0 || srcH <= 0) { srcW = sOverlayW; srcH = sOverlayH; }
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    if (!XR_SUCCEEDED(xrAcquireSwapchainImage(sHud.handle, &ai, &idx))) return false;
    XrSwapchainImageWaitInfo swi = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    swi.timeout = XR_INFINITE_DURATION;
    xrWaitSwapchainImage(sHud.handle, &swi);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glReadBuffer(GL_BACK);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, sEyeFbo);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sHud.images[idx].image, 0);
    glDisable(GL_SCISSOR_TEST);
    glBlitFramebuffer(0, 0, srcW, srcH, 0, 0, (int)sHud.w, (int)sHud.h, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);

    XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    xrReleaseSwapchainImage(sHud.handle, &ri);
    sHudImgIdx = idx;
    sHudReady = true;
    sPanelMode = true;
    return true;
}

// ---- VR menu: render ImGui into a dedicated, cleared offscreen FBO, then present that STABLE texture on
// the head-locked panel. vr_present_desktop_panel read GL_BACK of the double-buffered window, which
// alternates 2-frame-stale content -> flicker. A private FBO removes the double-buffer dependency. ----
static GLuint sMenuFbo = 0, sMenuTex = 0, sMenuDepth = 0;
static int    sMenuW = 0, sMenuH = 0;

// Bind + clear the menu FBO and leave it bound so the caller's ImGui render lands inside it.
extern "C" void vr_menu_render_begin(int w, int h) {
    if (w <= 0) w = 1;
    if (h <= 0) h = 1;
    if (sMenuFbo == 0) {
        glGenFramebuffers(1, &sMenuFbo);
        glGenTextures(1, &sMenuTex);
        glGenRenderbuffers(1, &sMenuDepth);
    }
    if (w != sMenuW || h != sMenuH) {
        glBindTexture(GL_TEXTURE_2D, sMenuTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glBindRenderbuffer(GL_RENDERBUFFER, sMenuDepth);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
        glBindFramebuffer(GL_FRAMEBUFFER, sMenuFbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sMenuTex, 0);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, sMenuDepth);
        sMenuW = w;
        sMenuH = h;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, sMenuFbo);
    glViewport(0, 0, w, h);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

// Make the VR menu panel see-through by writing a uniform alpha (gVRMenuOpacity) into the menu texture, then
// the head-locked quad layer (which blends on source alpha when a game is rendered behind it - see vr_submit)
// shows the race through the menu. Call AFTER the menu's ImGui has rendered into the FBO and BEFORE the panel
// blit. Alpha-only clear leaves the RGB (the menu image) untouched, so the desktop mirror still shows the
// full-color menu. No-op at full opacity. Floored at 0.3 so the menu can never become unreadable.
extern "C" void vr_menu_apply_opacity(void) {
    if (sMenuFbo == 0) return;
    // Fresh CVar read: the cached sMenuOpacity refreshes in vr_begin_frame, which runs AFTER this call in
    // the menu loop - using the cache would write last frame's alpha against this frame's blend gate.
    float op = CVarGetFloat("gVRMenuOpacity", sMenuOpacity);
    if (op < 0.3f) op = 0.3f;
    if (op > 0.999f) return; // fully opaque: keep the FBO's cleared alpha (1.0) as-is
    glBindFramebuffer(GL_FRAMEBUFFER, sMenuFbo);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
    glClearColor(0.0f, 0.0f, 0.0f, op);
    glClear(GL_COLOR_BUFFER_BIT);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
}

// Mirror the menu texture onto the desktop window (fb0). Call ONCE per frame AFTER the eye loop - the eye
// renders call mRapi->StartFrame() which churns fb0, so mirroring per-step (interleaved with the eyes) made
// the flatscreen flicker. One mirror right before the engine's SwapBuffers keeps the desktop steady.
extern "C" void vr_menu_mirror_desktop(int w, int h) {
    if (w <= 0) w = 1;
    if (h <= 0) h = 1;
    if (sMenuTex == 0) return;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, sBlitReadFbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sMenuTex, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glBlitFramebuffer(0, 0, sMenuW, sMenuH, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
}

// Mirror the rendered VR game frame (the interpreter's managed fb texture - last eye, or the panel render)
// onto the desktop window (fb0) when the MENU IS CLOSED. Without this, the flat window's double buffer just
// swaps between stale back-buffers each frame (the game renders into the OpenXR swapchains, never fb0) and
// flickers. Call ONCE per frame AFTER the eye loop, right before the engine's SwapBuffers - mirroring
// per-step fights the eye renders churning fb0 (same reason as vr_menu_mirror_desktop). The managed fb is
// invert-Y=true (bottom-left origin) like the eyes, so flip vertically into fb0 (top-left) - identical flip
// to vr_blit_into. srcW/srcH = the managed fb's rendered size (eye or panel); dstW/dstH = window pixels.
extern "C" void vr_mirror_game_desktop(unsigned int glTex, int srcW, int srcH, int dstW, int dstH) {
    if (glTex == 0 || srcW <= 0 || srcH <= 0) return;
    if (dstW <= 0) dstW = 1;
    if (dstH <= 0) dstH = 1;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, sBlitReadFbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, (GLuint)glTex, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    // Flip vertically (src bottom -> dst top): dst Y runs dstH..0, like vr_blit_into's swapchain blit.
    glBlitFramebuffer(0, 0, srcW, srcH, 0, dstH, dstW, 0, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
}

// ImGui has rendered into the menu FBO. Present the stable texture on the head-locked panel (per step). The
// desktop mirror is done separately, once after the loop (vr_menu_mirror_desktop).
extern "C" void vr_menu_render_present(int w, int h) {
    if (w <= 0) w = 1;
    if (h <= 0) h = 1;
    glBindFramebuffer(GL_FRAMEBUFFER, 0); // unbind the menu FBO (gui->EndDraw left it bound)

    if (!sRunning || !sFrameBegun || !sFrameState.shouldRender) return;
    if (sEyeFbo == 0 || sHud.handle == XR_NULL_HANDLE || sMenuTex == 0) return;
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    if (!XR_SUCCEEDED(xrAcquireSwapchainImage(sHud.handle, &ai, &idx))) return;
    XrSwapchainImageWaitInfo swi = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    swi.timeout = XR_INFINITE_DURATION;
    xrWaitSwapchainImage(sHud.handle, &swi);
    // Straight blit (no vertical flip): sMenuTex is top-left origin like fb0, matching vr_present_desktop_panel.
    glBindFramebuffer(GL_READ_FRAMEBUFFER, sBlitReadFbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sMenuTex, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, sEyeFbo);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sHud.images[idx].image, 0);
    glDisable(GL_SCISSOR_TEST);
    glBlitFramebuffer(0, 0, w, h, 0, 0, (int)sHud.w, (int)sHud.h, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    xrReleaseSwapchainImage(sHud.handle, &ri);
    sHudImgIdx = idx;
    sHudReady = true;
    sPanelMode = true;
}

extern "C" void vr_submit(void) {
    if (!sRunning || !sFrameBegun) return;
    sFrameBegun = false;

    if (!sPanelMode) sPanelAnchorValid = false;

    XrCompositionLayerProjection proj    = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
    XrCompositionLayerQuad       hudQuad = { XR_TYPE_COMPOSITION_LAYER_QUAD };
    const XrCompositionLayerFlags kBlend = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT
                                         | XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
    const XrCompositionLayerBaseHeader* layers[2];
    uint32_t layerCount = 0;

    // The 3D game projection layer is shown whenever both eyes were rendered this frame - INCLUDING when
    // the menu is open over a live race, so the world stays visible behind the menu (it no longer blacks
    // out the game).
    const bool haveEyes = (sFrameState.shouldRender && sViewsValid && sViewCount == 2 && sEyesSubmitted == 2);
    if (haveEyes) {
        proj.layerFlags = 0;
        proj.space = sLocalSpace;
        proj.viewCount = 2;
        proj.views = sProjViews;
        layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&proj;
    }

    if (sHudReady) {
        if (sPanelMode) {
            // Menu / non-gameplay flat panel: world-locked quad anchored in front of you. Drawn ON TOP of the
            // game projection layer when one exists, so the menu floats over the live world. When there IS a
            // game behind it (haveEyes) and the user dialed the menu see-through (gVRMenuOpacity < 1), blend on
            // the panel's source alpha (vr_menu_apply_opacity wrote that alpha) so the race shows through;
            // otherwise keep it an opaque virtual screen (Theater / title have nothing behind to blend with).
            hudQuad.layerFlags = (haveEyes && sMenuOpacity < 0.999f) ? kBlend : 0;
            hudQuad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            hudQuad.subImage.swapchain = sHud.handle;
            hudQuad.subImage.imageRect.offset.x = 0;
            hudQuad.subImage.imageRect.offset.y = 0;
            hudQuad.subImage.imageRect.extent.width  = (int32_t)sHud.w;
            hudQuad.subImage.imageRect.extent.height = (int32_t)sHud.h;
            hudQuad.size.width  = sMenuSize;
            hudQuad.size.height = sMenuSize * (float)sHud.h / (float)sHud.w;
            if (sLocalSpace != XR_NULL_HANDLE && sViewsValid) {
                float cqy = sViews[0].pose.orientation.y, cqw = sViews[0].pose.orientation.w;
                float cn = sqrtf(cqy * cqy + cqw * cqw);
                if (cn < 1e-6f) { cqy = 0.0f; cqw = 1.0f; cn = 1.0f; }
                cqy /= cn; cqw /= cn;
                float hfx = -2.0f * cqw * cqy, hfz = -(1.0f - 2.0f * cqy * cqy);
                float afx = -2.0f * sPanelAnchorQw * sPanelAnchorQy, afz = -(1.0f - 2.0f * sPanelAnchorQy * sPanelAnchorQy);
                float dot = hfx * afx + hfz * afz;
                (void) dot;
                // World-lock the panel where it opened so you can look around it. The enclosing branch
                // already requires valid located views, so DON'T gate the anchor on sPoseTracked (some
                // runtimes never report POSITION_VALID, which left the anchor invalid -> re-anchoring every
                // frame -> the panel stayed glued to the face). Re-anchors only on (re)open.
                bool reAnchor = !sPanelAnchorValid;
                if (reAnchor) {
                    sPanelAnchorPos[0] = 0.5f * (sViews[0].pose.position.x + sViews[1].pose.position.x);
                    sPanelAnchorPos[1] = 0.5f * (sViews[0].pose.position.y + sViews[1].pose.position.y);
                    sPanelAnchorPos[2] = 0.5f * (sViews[0].pose.position.z + sViews[1].pose.position.z);
                    sPanelAnchorQy = cqy; sPanelAnchorQw = cqw;
                    sPanelAnchorValid = true;
                }
                float qy = sPanelAnchorQy, qw = sPanelAnchorQw;
                float fwdx = -2.0f * qw * qy;
                float fwdz = -(1.0f - 2.0f * qy * qy);
                hudQuad.space = sLocalSpace;
                hudQuad.pose.orientation.x = 0.0f; hudQuad.pose.orientation.y = qy;
                hudQuad.pose.orientation.z = 0.0f; hudQuad.pose.orientation.w = qw;
                hudQuad.pose.position.x = sPanelAnchorPos[0] + sMenuDist * fwdx;
                hudQuad.pose.position.y = sPanelAnchorPos[1];
                hudQuad.pose.position.z = sPanelAnchorPos[2] + sMenuDist * fwdz;
                layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&hudQuad;
            }
        } else if (sViewSpace != XR_NULL_HANDLE) {
            // Head-locked alpha-blended HUD quad (legacy overlay path).
            hudQuad.layerFlags = kBlend;
            hudQuad.space = sViewSpace;
            hudQuad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            hudQuad.subImage.swapchain = sHud.handle;
            hudQuad.subImage.imageRect.extent.width  = (int32_t)sHud.w;
            hudQuad.subImage.imageRect.extent.height = (int32_t)sHud.h;
            hudQuad.pose.orientation.w = 1.0f;
            hudQuad.pose.position.z = -2.0f;
            hudQuad.size.width  = 2.4f;
            hudQuad.size.height = 2.4f * (float)sOverlayH / (float)sOverlayW;
            layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&hudQuad;
        }
    }

    XrFrameEndInfo fei = { XR_TYPE_FRAME_END_INFO };
    fei.displayTime = sFrameState.predictedDisplayTime;
    fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    fei.layerCount = layerCount;
    fei.layers = layerCount ? layers : NULL;
    xrok(xrEndFrame(sSession, &fei), "xrEndFrame");
}

extern "C" void vr_shutdown(void) {
    if (sEyeFbo)     { glDeleteFramebuffers(1, &sEyeFbo); sEyeFbo = 0; }
    if (sBlitReadFbo){ glDeleteFramebuffers(1, &sBlitReadFbo); sBlitReadFbo = 0; }
    if (sEyeDepthRB) { glDeleteRenderbuffers(1, &sEyeDepthRB); sEyeDepthRB = 0; }
    if (sOverlayFbo)     { glDeleteFramebuffers(1, &sOverlayFbo); sOverlayFbo = 0; }
    if (sOverlayDepthRB) { glDeleteRenderbuffers(1, &sOverlayDepthRB); sOverlayDepthRB = 0; }
    if (sHud.handle != XR_NULL_HANDLE) { xrDestroySwapchain(sHud.handle); sHud.handle = XR_NULL_HANDLE; }
    if (sHud.images) { free(sHud.images); sHud.images = NULL; }
    if (sViewSpace != XR_NULL_HANDLE) { xrDestroySpace(sViewSpace); sViewSpace = XR_NULL_HANDLE; }
    for (int e = 0; e < 2; e++) {
        if (sEye[e].handle != XR_NULL_HANDLE) { xrDestroySwapchain(sEye[e].handle); sEye[e].handle = XR_NULL_HANDLE; }
        if (sEye[e].images) { free(sEye[e].images); sEye[e].images = NULL; }
        sEye[e].imgCount = 0;
    }
    if (sLocalSpace != XR_NULL_HANDLE) { xrDestroySpace(sLocalSpace); sLocalSpace = XR_NULL_HANDLE; }
    if (sSession    != XR_NULL_HANDLE) { xrDestroySession(sSession);  sSession    = XR_NULL_HANDLE; }
    if (sInstance   != XR_NULL_HANDLE) { xrDestroyInstance(sInstance); sInstance   = XR_NULL_HANDLE; }
    sRunning = false;
    sFrameBegun = false;
    sViewCount = 0;
}

#else // !_WIN32 - VR is Windows/WGL-only for now; stub out elsewhere.

extern "C" bool vr_is_active(void) { return false; }
extern "C" void vr_diag_log(const char* fmt, ...) { (void)fmt; }
extern "C" int  vr_display_refresh_hz(void) { return 0; }
extern "C" bool vr_headset_present(void) { return false; }
extern "C" void vr_begin_frame(void) {}
extern "C" int  vr_eye_count(void) { return 0; }
extern "C" int  vr_eye_width(int e)  { (void)e; return 0; }
extern "C" int  vr_eye_height(int e) { (void)e; return 0; }
extern "C" bool vr_begin_eye(int e) { (void)e; return false; }
extern "C" void vr_end_eye(int e) { (void)e; }
extern "C" bool vr_submit_eye_texture(int e, unsigned int t, int w, int h) { (void)e; (void)t; (void)w; (void)h; return false; }
extern "C" bool vr_submit_panel_texture(unsigned int t, int w, int h) { (void)t; (void)w; (void)h; return false; }
extern "C" bool vr_present_desktop_panel(int w, int h) { (void)w; (void)h; return false; }
extern "C" void vr_menu_render_begin(int w, int h) { (void)w; (void)h; }
extern "C" void vr_menu_render_present(int w, int h) { (void)w; (void)h; }
extern "C" void vr_menu_mirror_desktop(int w, int h) { (void)w; (void)h; }
extern "C" void vr_menu_apply_opacity(void) {}
extern "C" void vr_mirror_game_desktop(unsigned int t, int sw, int sh, int dw, int dh) { (void)t; (void)sw; (void)sh; (void)dw; (void)dh; }
extern "C" const float* vr_eye_viewproj(int e) { (void)e; return 0; }
extern "C" const float* vr_sky_viewproj(int e) { (void)e; return 0; }
extern "C" void vr_set_sky_fov(float a, float b) { (void)a; (void)b; }
extern "C" float vr_sky_fov_h(void) { return 0.0f; }
extern "C" float vr_sky_fov_v(void) { return 0.0f; }
extern "C" void vr_set_sky_chase_yaw(float r) { (void)r; }
extern "C" float vr_sky_decouple_rad(void) { return 0.0f; }
extern "C" int  vr_overlay_width(void) { return 0; }
extern "C" int  vr_overlay_height(void) { return 0; }
extern "C" bool vr_begin_overlay(bool s) { (void)s; return false; }
extern "C" void vr_end_overlay(bool s) { (void)s; }
extern "C" void vr_set_panel_mode(bool on) { (void)on; }
extern "C" bool vr_begin_panel(void) { return false; }
extern "C" void vr_end_panel(void) {}
extern "C" void vr_submit(void) {}
extern "C" float vr_get_world_scale(void) { return 0; } extern "C" void vr_set_world_scale(float v) { (void)v; }
extern "C" float vr_get_stereo(void) { return 0; }      extern "C" void vr_set_stereo(float v) { (void)v; }
extern "C" float vr_get_head_scale(void) { return 0; }  extern "C" void vr_set_head_scale(float v) { (void)v; }
extern "C" float vr_get_eye_height(void) { return 0; }  extern "C" void vr_set_eye_height(float v) { (void)v; }
extern "C" bool  vr_get_horizon_lock(void) { return false; } extern "C" void vr_set_horizon_lock(bool on) { (void)on; }
extern "C" bool  vr_get_vignette(void) { return false; }     extern "C" void vr_set_vignette(bool on) { (void)on; }
extern "C" bool  vr_get_cockpit(void) { return false; }      extern "C" void vr_set_cockpit(bool on) { (void)on; }
extern "C" float vr_get_menu_dist(void) { return 0; }   extern "C" void vr_set_menu_dist(float v) { (void)v; }
extern "C" float vr_get_menu_size(void) { return 0; }   extern "C" void vr_set_menu_size(float v) { (void)v; }
extern "C" float vr_get_hud_scale(void) { return 0; }   extern "C" void vr_set_hud_scale(float v) { (void)v; }
extern "C" float vr_get_hud_dist(void)  { return 0; }   extern "C" void vr_set_hud_dist(float v) { (void)v; }
extern "C" int   vr_get_view_mode(void) { return 0; }   extern "C" void vr_set_view_mode(int m) { (void)m; }
extern "C" float vr_third_person_push_units(void) { return 0.0f; }
extern "C" float vr_fp_forward_game_units(void) { return 0.0f; }
extern "C" const float* vr_hud_viewproj(int e) { (void)e; return 0; }
extern "C" void  vr_reset_defaults(void) {}
extern "C" void  vr_set_cockpit_pose(const float p[3], float h, float pi, float r) { (void)p; (void)h; (void)pi; (void)r; }
extern "C" void  vr_set_motion_intensity(float v) { (void)v; }
extern "C" float vr_head_yaw_rad(void) { return 0; }
extern "C" float vr_head_pitch_rad(void) { return 0; }
extern "C" short vr_item_throw_yaw_offset(int isHumanPlayer) { return 0; }
extern "C" void  vr_set_action_cam(int dramatic, int finished) { (void) dramatic; (void) finished; }
extern "C" void  vr_set_drama_rot(short yawBinary, short pitchBinary) { (void) yawBinary; (void) pitchBinary; }
extern "C" int   vr_action_cam_active(void) { return 0; }
extern "C" bool  vr_controllers_active(void) { return false; }
extern "C" unsigned vr_controller_buttons(void) { return 0; }
extern "C" void  vr_controller_stick(int hand, float out[2]) { (void) hand; out[0] = out[1] = 0.0f; }
extern "C" void  vr_controller_rumble(float strength, float seconds) { (void) strength; (void) seconds; }
extern "C" void  vr_controller_rumble_stop(void) {}
extern "C" void  vr_shutdown(void) {}

#endif
