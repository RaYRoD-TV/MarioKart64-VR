#include <libultraship.h>
#include <libultra/gbi.h>
#include "SkyCloud.h"
#include <vector>
#include "engine/tracks/Track.h"
#include "engine/World.h"

#include "port/Engine.h"
#include "port/Game.h"
#include "port/interpolation/FrameInterpolation.h"

extern "C" {
#include "update_objects.h"
#include "code_80057C60.h"
#include "code_8006E9C0.h"
#include "code_800029B0.h" // gScreenOneCtx - gates the gVRSkyCloudM200 publish to the rendered screen
#include "assets/models/common_data.h"
#include "math_util_2.h"
#include "render_objects.h"
}

size_t SkyCloud::_count = 0;

SkyCloud::SkyCloud(ScreenContext* screen, u16 cloudVariant, u16 posY, u16 rotY, u16 scalePercent) : SkyActor(screen) {
    _idx = _count;
    mScreen = screen;
    mCloudVariant = cloudVariant;
    mY = posY;
    mRotY = rotY;
    mScale = (f32) scalePercent / 100.0;
    mTextureWidth = 64;
    mTextureHeight = 32;

    // Stock
    if (GameEngine_ResourceGetTexTypeByName((const char*)CM_GetProps()->CloudTexture) != 1) {
        mTexture = ((u8*) LOAD_ASSET_RAW(CM_GetProps()->CloudTexture)) + (cloudVariant * 1024);
        mVtx = (Vtx*)D_0D005FB0;
    } else { // Texture pack
        mTexture = CM_GetProps()->CloudTexture;

        if ((strcmp((const char*)CM_GetProps()->CloudTexture, gTextureExhaust3) == 0) ||
           (strcmp((const char*)CM_GetProps()->CloudTexture, gTextureExhaust4) == 0) ||
           (strcmp((const char*)CM_GetProps()->CloudTexture, gTextureExhaust5) == 0)) {
            mVtx = cloudvtx[cloudVariant];
        } else if (strcmp((const char*)CM_GetProps()->CloudTexture, gTextureExhaust0) == 0 ||
            strcmp((const char*)CM_GetProps()->CloudTexture, gTextureExhaust1) == 0 ||
            strcmp((const char*)CM_GetProps()->CloudTexture, gTextureExhaust2) == 0) {
            mVtx = cloudvtx2[cloudVariant];
        } else {
            mVtx = cloudvtx2[cloudVariant];
        }
    }

    _count += 1;
}

// VR: the exact (fov+40) value the clouds used for THIS frame's sprite placement. The VR sky-dome mapping must
// be calibrated with the SAME value - the camera fieldOfView animates with speed/boost (changes while driving/
// turning), so re-reading it at a different point in the frame produces a mismatched mapping = clouds swim.
extern "C" float gVRSkyCloudM200;
float gVRSkyCloudM200 = 0.0f;
extern "C" void vr_diag_log(const char* fmt, ...);
extern "C" s32 gGlobalTimer;

void SkyCloud::Tick() { // func_800788F8
    s16 cameraRot;

    s16 mUnk200 = mScreen->camera->fieldOfView + 40.0f;
    // Publish for the VR sky mapping - it must match the placement of the sprites the player SEES. Gated to
    // the rendered screen: single-player init builds a SECOND, never-drawn cloud batch on gScreenTwoCtx
    // (init_hud_one_player calls func_8007055C for both contexts), and since TickSkyActors ticks every batch
    // in order, the zombie screen-two batch (camera frozen at fov 80 -> m200 120) used to overwrite this
    // value LAST every frame - calibrating the dome remap for m200=120 while the visible sprites were placed
    // with m200=80, which made the clouds rotate at ~2.5x the world rate during kart turns.
    if (mScreen == gScreenOneCtx) {
        gVRSkyCloudM200 = (float) mUnk200;
    }
    mUnk208 = ((mUnk200 / 2) * 0xB6) + 0x71C;
    mUnk210 = (-(mUnk200 / 2) * 0xB6) - 0x71C;
    mUnk1E8 = 1.7578125 / mUnk200;
    mUnk218 = SCREEN_WIDTH / 2;

    // Adjustable culling factor
    const float cullingFactor = OTRGetAspectRatio();

    // Calculate the cloud's rotation relative to the camera
    cameraRot = (u16)mScreen->camera->rot[1] + (u16)mRotY;
    // Adjust bounds based on the culling factor
    s16 adjustedLowerBound = (s16) (mUnk210 * cullingFactor);
    s16 adjustedUpperBound = (s16) (mUnk208 * cullingFactor);

    // Check if the object is within the adjusted bounds
    if ((cameraRot >= adjustedLowerBound) && (adjustedUpperBound >= cameraRot)) {
        // Calculate and update the object's X position
        // 160 (SCREEN_WIDTH / 2) + (D_8018D1E8 * cameraRot);
        // Grab center of screen, scale by fov factor, offset based on camera rotation
        mX = mUnk218 + (mUnk1E8 * cameraRot);

        // Mark the object as visible
        mVisible = true;
    } else {
        // If outside the bounds, mark the object as not visible
        mVisible = false;
    }

    // TEMP diagnostic (throttled, cloud 0 only): the yaw the sprites scroll with (camera->rot[1]) vs the yaw
    // of the camera's ACTUAL look direction (pos->lookAt). If these diverge while turning, that delta is the
    // cloud swim. Remove once confirmed fixed. (mX is int32_t - cast for the float varargs slot, the uncast
    // int previously printed as a garbage 0.0.)
    {
        if (_idx == 0 && (gGlobalTimer & 15) == 0) {
            float dx = mScreen->camera->lookAt[0] - mScreen->camera->pos[0];
            float dz = mScreen->camera->lookAt[2] - mScreen->camera->pos[2];
            float lookYawDeg = atan2f(dx, dz) * 57.29578f;
            float rot1Deg    = (float) (u16) mScreen->camera->rot[1] * (360.0f / 65536.0f);
            vr_diag_log("[cloud] t=%d rot1=%.1fdeg lookYaw=%.1fdeg m200=%d mX=%.1f fov=%.1f vis=%d",
                        gGlobalTimer, rot1Deg, lookYawDeg, (s32) mUnk200, (float) mX,
                        mScreen->camera->fieldOfView, mVisible ? 1 : 0);
        }
    }
}

void SkyCloud::Draw(ScreenContext* screen, s32 arg0) { // render_clouds
   // Object* object = &gObjectList[_objectIndex];
    s32 posY = arg0 - mY;
    func_8004B6C4(255, 255, 255);
    // Skip drawing the object this frame if it warped to the other side of the screen
    if ((fabs(mX - mOldX) > SCREEN_WIDTH / 2) || (fabs(posY - mOldY) > SCREEN_HEIGHT / 2)) {
        mOldX = mX;
        mOldY = posY;
        return;
    }
    if (mVisible) {
        FrameInterpolation_RecordOpenChild("render_clouds", TAG_CLOUDS((_idx << 4) | (mScreen - gScreenContexts)));

        if (D_8018D228 != mCloudVariant) {
            D_8018D228 = mCloudVariant;
            func_80044DA0(mTexture, mTextureWidth, mTextureHeight);
        }
        func_80042330_unchanged(mX, posY, 0, mScale);
        gSPVertex(gDisplayListHead++, (uintptr_t)mVtx, 4, 0);
        gSPDisplayList(gDisplayListHead++, (Gfx*)common_rectangle_display);

        FrameInterpolation_RecordCloseChild();
    }
    mOldX = mX;
    mOldY = posY;
}
