// VR sky dome for SpaghettiKart. MK64's sky is a 2-color vertex gradient quad (no texture/panorama), drawn
// in screen-space ortho - which cannot be world-anchored on PITCH in VR (the 2D NDC-shift hack only fakes
// yaw). This rebuilds the sky as a vertex-colored gradient SPHERE drawn with a rotation-only view-projection
// (head orientation, zero translation = infinity), so it stays put on BOTH yaw and pitch. Approach proven in
// sm64coopdx (build_skybox_sphere_vr); textures/UV stripped because MK64's sky is just two gradient colors,
// and head-relative (no camera scroll) because MK64's gradient has no world-anchored features to align to.
#include <libultraship.h>
#include <libultra/gbi.h>
#include <math.h>
#include "mk64.h"
#include "engine/tracks/Track.h"   // SkyboxColours, CM_GetProps()

extern "C" {
#include "common_structs.h"
#include "code_800029B0.h"         // ScreenContext, gScreenOneCtx, Camera (camera->fieldOfView)
}

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

extern "C" s32 gGlobalTimer;
extern "C" float CVarGetFloat(const char* name, float defaultValue);
// Push the game's live 2D-sky angular half-extent (radians) to the VR layer so the cloud/star/snow sprites
// land at their true world azimuth on the dome and rotate with the kart heading instead of swimming.
extern "C" void vr_set_sky_fov(float halfH, float halfV);
// Push the chase camera's yaw so the VR layer can decouple the cloud sprites from the kart heading in
// Diorama (where the tabletop world doesn't rotate). The cloud sprite screen X uses (u16)camera->rot[1] as
// a binary angle (65536 = 2pi), so convert with the same factor.
extern "C" void vr_set_sky_chase_yaw(float rad);
extern "C" float gVRSkyCloudM200; // the (fov+40) the clouds actually used this frame (SkyCloud.cpp)

// Cached dome DL (rebuilt once per game frame; gradient colors can change per track/section).
extern "C" Gfx*         gVrSkyDomeGfx   = nullptr;
extern "C" unsigned int gVrSkyDomeFrame = 0xFFFFFFFFu;

#define DOME_AZ    16   // azimuth segments (around)
#define DOME_BANDS 12   // elevation bands from zenith (+90) to nadir (-90)
#define DOME_QUADS (DOME_AZ * DOME_BANDS)

static Gfx sDomeGfx[DOME_QUADS * 2 + 16];
static Vtx sDomeVtx[DOME_QUADS * 4];

static inline void set_vtx(Vtx* v, float x, float y, float z,
                           unsigned char r, unsigned char g, unsigned char b) {
    v->v.ob[0] = (s16)x; v->v.ob[1] = (s16)y; v->v.ob[2] = (s16)z;
    v->v.flag  = 0;
    v->v.tc[0] = 0; v->v.tc[1] = 0;
    v->v.cn[0] = r; v->v.cn[1] = g; v->v.cn[2] = b; v->v.cn[3] = 255;
}

extern "C" Gfx* build_sky_dome_vr(void) {
    // Push the game's 2D-sky angular half-extent to the VR layer so the cloud/star/snow sprites land at their
    // true world azimuth on the dome (and rotate 1:1 with the kart heading instead of swimming). The sprites'
    // screen X = SCREEN_WIDTH/2 + (1.7578125/mUnk200)*cameraRot with mUnk200 = (s16)(fieldOfView + 40) and
    // cameraRot a u16 binary angle, so the screen-edge azimuth = mUnk200/2 degrees. Vertical is 4:3-
    // proportional. gVRSkyYawScale / gVRSkyPitchScale (default 1.0) fine-tune the rate live (no rebuild).
    if (gScreenOneCtx != nullptr && gScreenOneCtx->camera != nullptr) {
        // Use the EXACT (fov+40) the clouds used when they placed their sprites this frame (published by
        // SkyCloud::Tick). The camera fieldOfView animates with speed/boost while driving, so re-reading it
        // here (a different point in the frame, possibly a different camera after look-behind swaps) gives a
        // mismatched calibration and the clouds swim. Fall back to the live read when no cloud ticked yet.
        const float m200 = (gVRSkyCloudM200 > 0.0f)
                               ? gVRSkyCloudM200
                               : (float)(s16)(gScreenOneCtx->camera->fieldOfView + 40.0f);
        const float azH  = m200 * 0.5f * (float)(M_PI / 180.0);   // screen-edge azimuth (rad)
        const float azV  = azH * (240.0f / 320.0f);               // 4:3-proportional vertical
        vr_set_sky_fov(azH * CVarGetFloat("gVRSkyYawScale", 1.0f),
                       azV * CVarGetFloat("gVRSkyPitchScale", 1.0f));
        // Same camera the cloud sprites read; (u16) binary angle -> radians (65536 == 2*pi). Consumed only
        // in Diorama to decouple the clouds from the kart heading (vr_sky_decouple_rad).
        vr_set_sky_chase_yaw((float)(unsigned short)gScreenOneCtx->camera->rot[1] * (float)(2.0 * M_PI / 65536.0));
    }

    SkyboxColours* prop = (SkyboxColours*)&CM_GetProps()->Skybox;
    // Gradient endpoints, same source as the flat sky quad: zenith = TopLeft, horizon = BottomLeft, and the
    // lower hemisphere fades to the floor color.
    const unsigned char zr = prop->TopLeft.r,         zg = prop->TopLeft.g,         zb = prop->TopLeft.b;
    const unsigned char hr = prop->BottomLeft.r,      hg = prop->BottomLeft.g,      hb = prop->BottomLeft.b;
    const unsigned char fr = prop->FloorBottomLeft.r, fg = prop->FloorBottomLeft.g, fb = prop->FloorBottomLeft.b;

    const float R = 1000.0f; // game units; rotation-only VP puts it at infinity, fits zn=1..zf=5000.
    Gfx* g = sDomeGfx;
    gDPPipeSync(g++);
    gDPSetCycleType(g++, G_CYC_1CYCLE);
    gDPSetTextureLUT(g++, G_TT_NONE);
    gSPTexture(g++, 0, 0, 0, G_TX_RENDERTILE, G_OFF);               // no texturing - vertex colors only
    gDPSetCombineMode(g++, G_CC_SHADE, G_CC_SHADE);                 // output = vertex (shade) color, opaque
    gDPSetRenderMode(g++, G_RM_OPA_SURF, G_RM_OPA_SURF2);
    // Clear depth/lighting/cull/fog/texgen; SET G_SHADE | G_SHADING_SMOOTH so the vertex colors are used
    // (without G_SHADE the SHADE combine outputs black - the flat sky gets this from init_rdp()).
    gSPClearGeometryMode(g++, G_ZBUFFER | G_LIGHTING | G_CULL_BOTH | G_FOG | G_TEXTURE_GEN);
    gSPSetGeometryMode(g++, G_SHADE | G_SHADING_SMOOTH);

    int vi = 0;
    for (int b = 0; b < DOME_BANDS; b++) {
        const float el0 = (90.0f - (float)b       * (180.0f / DOME_BANDS)) * (float)(M_PI / 180.0);
        const float el1 = (90.0f - (float)(b + 1) * (180.0f / DOME_BANDS)) * (float)(M_PI / 180.0);
        unsigned char cTop[3], cBot[3];
        for (int p = 0; p < 2; p++) {
            const float eDeg = (p == 0 ? el0 : el1) * (float)(180.0 / M_PI); // +90 zenith .. -90 nadir
            unsigned char* o = (p == 0) ? cTop : cBot;
            if (eDeg >= 0.0f) {                 // upper hemisphere: zenith -> horizon
                const float t = 1.0f - eDeg / 90.0f;
                o[0] = (unsigned char)(zr + (hr - zr) * t);
                o[1] = (unsigned char)(zg + (hg - zg) * t);
                o[2] = (unsigned char)(zb + (hb - zb) * t);
            } else {                            // lower hemisphere: horizon -> floor
                const float t = -eDeg / 90.0f;
                o[0] = (unsigned char)(hr + (fr - hr) * t);
                o[1] = (unsigned char)(hg + (fg - hg) * t);
                o[2] = (unsigned char)(hb + (fb - hb) * t);
            }
        }
        for (int a = 0; a < DOME_AZ; a++) {
            const float az0 = (float)a       / DOME_AZ * 2.0f * (float)M_PI;
            const float az1 = (float)(a + 1) / DOME_AZ * 2.0f * (float)M_PI;
            Vtx* v = &sDomeVtx[vi];
            set_vtx(&v[0], R * cosf(el0) * sinf(az0), R * sinf(el0), -R * cosf(el0) * cosf(az0), cTop[0], cTop[1], cTop[2]);
            set_vtx(&v[1], R * cosf(el1) * sinf(az0), R * sinf(el1), -R * cosf(el1) * cosf(az0), cBot[0], cBot[1], cBot[2]);
            set_vtx(&v[2], R * cosf(el1) * sinf(az1), R * sinf(el1), -R * cosf(el1) * cosf(az1), cBot[0], cBot[1], cBot[2]);
            set_vtx(&v[3], R * cosf(el0) * sinf(az1), R * sinf(el0), -R * cosf(el0) * cosf(az1), cTop[0], cTop[1], cTop[2]);
            gSPVertex(g++, (uintptr_t)v, 4, 0);
            gSP2Triangles(g++, 0, 1, 2, 0, 0, 2, 3, 0);
            vi += 4;
        }
    }
    gSPEndDisplayList(g);
    gVrSkyDomeFrame = (unsigned int)gGlobalTimer;
    gVrSkyDomeGfx   = sDomeGfx;
    return sDomeGfx;
}

// True when the dome should replace the flat 2D sky (CVar on + a dome built). The interpreter reads this to
// suppress the flat sky quad and disable the legacy NDC-shift.
extern "C" bool vr_sky_dome_active(void) {
    return (CVarGetFloat("gVRSkyDome", 1.0f) != 0.0f) && (gVrSkyDomeGfx != nullptr);
}
