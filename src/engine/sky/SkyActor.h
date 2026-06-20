#pragma once

#include <libultraship.h>
#include "engine/SpawnParams.h"
#include "engine/CoreMath.h"

extern "C" {
#include "common_structs.h"
#include "code_800029B0.h"
}

/**
 * SkyActor base class
 *
 *
 */
class SkyActor {
public:
    SkyActor(ScreenContext* screen) {
        mScreen = screen;
    };
    SkyActor(ScreenContext* screen, u16 cloudVariant, u16 posY, u16 rotY, u16 scalePercent) {};
    virtual ~SkyActor() {};

    virtual void Draw(ScreenContext* ctx, s32 arg0) {};
    virtual void Tick() {};
    ScreenContext* mScreen;
protected:
    // Every member defaults to a safe value: subclasses fill these from their Tick state machines,
    // but the renderer can call Draw before the first Tick runs (the VR path renders several frames
    // during race setup). Uninitialized values here crashed Frappe Snowland - a garbage
    // mTextureWidth divided by zero inside gDPLoadBlock, a garbage mTexture access-violated.
    f32 mScale = 1.0f;
    u16 mCloudVariant = 0;
    u8* mTexture = nullptr;
    s32 mTextureWidth = 0;
    s32 mTextureHeight = 0;
    bool mVisible = false;
    Vtx* mVtx = nullptr;
    int32_t mX = 0;
    int32_t mY = 0;
    int32_t mRotY = 0;
    int32_t mOldX = 0;
    int32_t mOldY = 0;

    s16 mUnk208 = 0;
    s16 mUnk210 = 0;
    f32 mUnk1E8 = 0.0f;
    s16 mUnk218 = 0;
};
