#pragma once

#include <libultraship.h>
#include "engine/objects/Object.h"

extern "C" {
#include "common_structs.h"
#include "camera.h"
#include "objects.h"
}

/**
 * Evil Lakitu - a track hazard (race_mods.c), not part of any course's stock object set.
 *
 * A rogue Lakitu shadows one racer from above, then dive-bombs them on a timer. Contact lands
 * the same vertical-tumble wreck the chain chomp deals; star carriers shrug it off. He gives up
 * and leaves after his lifetime runs out or his target finishes. Draws as the fishing-Lakitu
 * billboard (the rod reads as "coming for you"), claiming its own free object slot so it never
 * collides with a course's stock object lists.
 *
 * spinyMode = 1 is the classic SMB Lakitu instead: he never dives - he lobs rolling (yoshi)
 * eggs AHEAD of the target's driving line, four of them, then packs up.
 */
class OLakituHazard : public OObject {
public:
    explicit OLakituHazard(s32 targetPlayerId, s32 spinyMode = 0);

    ~OLakituHazard() {
        _count--;
    }

    static size_t GetCount() {
        return _count;
    }

    virtual void Tick() override;
    virtual void Draw(s32 cameraId) override;

private:
    static size_t _count;
    size_t _idx;
    s32 _objectIndex = -1;
    s32 _target = 0;
    s32 _timer = 0;
    s32 _life = 1500; // ~50s at the 30Hz tick, then he leaves
    s32 _state = 1;   // 1 shadow the target / 2 dive / 3 climb away / 0 done
    s32 _spiny = 0;   // 1 = egg lobber (no dives)
    s32 _eggsLeft = 4;
    s32 _divesLeft = 4; // the dive-bomber's budget - then he leaves (caps the recurring sound too)
    f32 _pos[3] = { 0.0f, 0.0f, 0.0f };
    bool _diagTicked = false; // one-shot racemods_diag.txt breadcrumbs - spawn/tick/draw chain
    bool _diagDrawn = false;
};

/**
 * Boo Haunt - a track hazard (race_mods.c). A boo materializes beside one racer and circles
 * them, cackling, then fades back out. Pure menace for now: the item STEAL is deferred until
 * the true held-item storage is located (common_structs' currentItemCopy is only a synced
 * copy). Free object slot; boo frames + Banshee tlut come through the resource archive, so it
 * haunts every course.
 */
class OBooHazard : public OObject {
public:
    explicit OBooHazard(s32 targetPlayerId);

    ~OBooHazard() {
        _count--;
    }

    virtual void Tick() override;
    virtual void Draw(s32 cameraId) override;

private:
    static size_t _count;
    size_t _idx;
    s32 _objectIndex = -1;
    s32 _target = 0;
    s32 _life = 360; // ~12s of haunting
    s32 _state = 1;  // 1 fade in + circle / 2 fade out / 0 done
    u16 _orbit = 0;  // orbit angle around the target, binary
    f32 _pos[3] = { 0.0f, 0.0f, 0.0f };
    bool _diagTicked = false;
};

/**
 * Bob-omb Balloon - a track hazard (race_mods.c). The Luigi Raceway hot-air balloon drifts
 * along the racing line dropping trick item boxes (the odd REAL one keeps the bait honest),
 * then floats away. The balloon model rides the resource archive, so it flies anywhere; the
 * stock OHotAirBalloon stays untouched (it is gated to Luigi Raceway and owns the item-box
 * actor there).
 */
class OBalloonBomber : public OObject {
public:
    explicit OBalloonBomber(f32 x, f32 y, f32 z, s32 wpIdx);

    ~OBalloonBomber() {
        _count--;
    }

    virtual void Tick() override;
    virtual void Draw(s32 cameraId) override;

private:
    static size_t _count;
    size_t _idx;
    s32 _objectIndex = -1;
    f32 _progress = 0.0f; // waypoint-space drift position along the racing line
    s32 _life = 1800;     // ~60s over the track, then he drifts up and away
    s32 _dropTimer = 90;
    s32 _drops = 0;
    f32 _pos[3] = { 0.0f, 0.0f, 0.0f };
    bool _diagDrawn = false;
};

/**
 * Fog Bank - a track hazard (race_mods.c). A drive-through patch of big smoke billboards
 * parked across the racing line: zero collision, no render-state or fog-register tricks (VR
 * safety) - pure visibility nerves. Fades in, sits a while, fades out.
 */
class OFogBank : public OObject {
public:
    explicit OFogBank(f32 x, f32 y, f32 z, f32 fx, f32 fz, f32 px, f32 pz);

    ~OFogBank() {
        _count--;
    }

    virtual void Tick() override;
    virtual void Draw(s32 cameraId) override;

private:
    static const s32 kPuffs = 9;
    static size_t _count;
    size_t _idx;
    s32 _life = 900; // ~30s
    s32 _age = 0;
    s32 _alpha = 0;
    f32 _puffs[9][3];
    f32 _scale[9];
};
