#include <libultraship.h>
#include <libultra/gbi.h>
#include <math.h>
#include <vector>
#include "LakituHazard.h"
#include "World.h"
#include "engine/registry/RegisterContent.h"
#include "engine/objects/ChainChomp.h"
#include "engine/objects/Thwomp.h"
#include "engine/objects/Snowman.h"
#include "engine/objects/Crab.h"
#include "engine/objects/Hedgehog.h"
#include "engine/objects/Penguin.h"
#include "engine/objects/MoleGroup.h"
#include "engine/objects/BombKart.h"
#include "engine/objects/CheepCheep.h"
#include "engine/actors/FallingRock.h"
#include "port/interpolation/FrameInterpolation.h"

extern "C" {
#include "macros.h"
#include "main.h"
#include "math_util.h"
#include "math_util_2.h"
#include "update_objects.h"
#include "render_objects.h"
#include "code_80086E70.h"
#include "code_80057C60.h"
#include "defines.h"
#include "sounds.h"
#include "external.h"
#include "objects.h"
#include "waypoints.h"
#include "actor_types.h"
#include <assets/models/common_data.h>
#include <assets/textures/common_data.h>
#include <assets/textures/other_textures.h>
#include <assets/textures/boo_frames.h>
#include <assets/textures/tracks/banshee_boardwalk/banshee_boardwalk_data.h>
#include <assets/models/tracks/luigi_raceway/luigi_raceway_data.h>
// race_mods.c: the hazard diag breadcrumb line (racemods_diag.txt), and racing/collision.c's
// ground probe - the bridges ground their spawns at the waypoint's real floor with it.
void race_mods_diag_line(const char* fmt, ...);
f32 spawn_actor_on_surface(f32 posX, f32 posY, f32 posZ);
// racing/actors.c: the C-actor hazard spawners the spiny lakitu and the balloon bomber feed -
// rolling eggs, trick boxes and the occasional real box. Boxes expect course-data coords
// (pre-multiply x by gTrackDirection to cancel the mirror flip).
void spawn_hazard_prop(s16 actorType, f32 x, f32 y, f32 z);
void spawn_fake_item_box(Vec3f pos);
void spawn_item_box(Vec3f pos);
extern f32 gTrackDirection;
}

size_t OLakituHazard::_count = 0;

// The fishing-Lakitu animation frames (the same set the rescue Lakitu cycles).
static const char* sEvilLakituTextures[] = { gTextureLakituFishing1, gTextureLakituFishing2, gTextureLakituFishing3,
                                             gTextureLakituFishing4 };

// The fishing-Lakitu quad (Lakitu.cpp's fixed_D_0D005F30 layout - the rod hangs off the +x side).
static Vtx sEvilLakituVtx[] = {
    { { { -10, -35, 0 }, 0, { 0, 0 }, { 255, 255, 255, 255 } } },
    { { { 45, -35, 0 }, 0, { 3520, 0 }, { 255, 255, 255, 255 } } },
    { { { 45, 35, 0 }, 0, { 3520, 4480 }, { 255, 255, 255, 255 } } },
    { { { -10, 35, 0 }, 0, { 0, 4480 }, { 255, 255, 255, 255 } } },
};

OLakituHazard::OLakituHazard(s32 targetPlayerId, s32 spinyMode) {
    Player* target;
    u8* tlut;

    Name = "Evil Lakitu";
    ResourceName = "mk:evil_lakitu";
    _idx = _count;
    _count++;
    _target = targetPlayerId;
    _spiny = spinyMode;

    // A free slot straight from the global pool - the per-course index lists (moles, penguins,
    // cheep cheeps...) are never touched, so this is safe on every track.
    find_unused_obj_index(&_objectIndex);
    init_object(_objectIndex, 0);

    tlut = (u8*) LOAD_ASSET_RAW(common_tlut_lakitu_fishing);
    init_texture_object(_objectIndex, tlut, sEvilLakituTextures, 0x38U, (u16) 0x48U);
    // init_texture_object only fills the LISTS (tlutList/textureList) - the draw reads the
    // ACTIVE pair, which a recycled free-pool slot leaves as garbage. func_80073514 is the
    // stock publisher (activeTLUT = tlutList, activeTexture = textureList[index]); without it
    // the billboard rasterized with a junk palette and the lakitu was pure sound, no sprite.
    func_80073514(_objectIndex);
    gObjectList[_objectIndex].vertex = sEvilLakituVtx;
    gObjectList[_objectIndex].sizeScaling = 0.165f; // a hair bigger than the stock 0.15 - menace
    gObjectList[_objectIndex].state = 2;

    target = &gPlayerOne[_target];
    _pos[0] = target->pos[0];
    _pos[1] = target->pos[1] + 140.0f;
    _pos[2] = target->pos[2];

    race_mods_diag_line("evil_lakitu ctor: obj=%d target=%d pos=(%.0f %.0f %.0f)", _objectIndex, _target, _pos[0],
                        _pos[1], _pos[2]);
}

void OLakituHazard::Tick() {
    Object* object = &gObjectList[_objectIndex];
    Player* target = &gPlayerOne[_target];
    f32 dx, dy, dz;

    if (!_diagTicked) {
        _diagTicked = true;
        race_mods_diag_line("evil_lakitu first tick: obj=%d state=%d objState=%d pos=(%.0f %.0f %.0f)", _objectIndex,
                            _state, object->state, _pos[0], _pos[1], _pos[2]);
    }
    if (_state == 0) {
        return;
    }
    if (--_life <= 0 || !(target->type & PLAYER_EXISTS) || (target->type & PLAYER_CINEMATIC_MODE)) {
        _state = 0;
        object->state = 0; // stop drawing - he packed up and left
        return;
    }

    dx = target->pos[0] - _pos[0];
    dy = target->pos[1] - _pos[1];
    dz = target->pos[2] - _pos[2];

    switch (_state) {
        case 1: // shadow the target from above, leading slightly into their motion
            _pos[0] += (dx + target->velocity[0] * 8.0f) * 0.08f;
            _pos[2] += (dz + target->velocity[2] * 8.0f) * 0.08f;
            _pos[1] += ((target->pos[1] + 95.0f + sins((u16) (gGlobalTimer * 800)) * 8.0f) - _pos[1]) * 0.1f;
            if (_spiny) {
                // The SMB Lakitu never dives: on a faster cadence he lobs a rolling egg AHEAD
                // of the target's driving line, then keeps looming. Out of eggs = out of here.
                if (++_timer > 105) {
                    _timer = 0;
                    spawn_hazard_prop(ACTOR_YOSHI_EGG, target->pos[0] + target->velocity[0] * 55.0f, target->pos[1],
                                      target->pos[2] + target->velocity[2] * 55.0f);
                    func_800C98B8(object->pos, object->velocity, SOUND_ARG_LOAD(0x19, 0x00, 0x70, 0x18));
                    if (--_eggsLeft <= 0 && _life > 90) {
                        _life = 90; // the last egg is away - linger a beat, then leave
                    }
                }
                break;
            }
            if (++_timer > 75) { // ~2.5s of looming, then the dive (was 5s - too long to ever see)
                _timer = 0;
                if (--_divesLeft < 0) { // out of dives - he leaves instead of pestering (and
                                        // re-playing the dive cue) for his whole lifetime
                    _state = 0;
                    object->state = 0;
                    break;
                }
                _state = 2;
                func_800C98B8(object->pos, object->velocity, SOUND_ARG_LOAD(0x19, 0x00, 0x70, 0x18));
            }
            break;
        case 2: { // dive-bomb the kart - LEAD the target and MATCH its motion so a kart at full
                  // speed can't simply outrun the dive (the old straight-at-current-pos dive
                  // whiffed every time against a moving kart, so the lakitu never landed a hit).
            f32 lx = target->pos[0] + target->velocity[0] * 4.0f; // aim a few ticks ahead
            f32 lz = target->pos[2] + target->velocity[2] * 4.0f;
            f32 ldx = lx - _pos[0];
            f32 ldy = target->pos[1] - _pos[1];
            f32 ldz = lz - _pos[2];
            f32 ld = sqrtf(ldx * ldx + ldy * ldy + ldz * ldz);
            f32 hxz, vgap;
            if (ld > 0.01f) {
                _pos[0] += ldx / ld * 13.0f + target->velocity[0]; // close in AND keep pace
                _pos[1] += ldy / ld * 13.0f;
                _pos[2] += ldz / ld * 13.0f + target->velocity[2];
            }
            // Hit when he's overhead and has descended to kart height, or on a tight 3D pass.
            hxz = sqrtf((target->pos[0] - _pos[0]) * (target->pos[0] - _pos[0]) +
                        (target->pos[2] - _pos[2]) * (target->pos[2] - _pos[2]));
            vgap = _pos[1] - target->pos[1];
            if ((hxz < 28.0f && vgap < 38.0f && vgap > -22.0f) ||
                sqrtf(dx * dx + dy * dy + dz * dz) < 24.0f) {
                // The chomp's wreck: a vertical tumble - unless the target rides a star.
                if (!(target->effects & STAR_EFFECT)) {
                    target->triggers |= VERTICAL_TUMBLE_TRIGGER;
                }
                func_800C98B8(object->pos, object->velocity, SOUND_ARG_LOAD(0x19, 0x01, 0x80, 0x57));
                _timer = 0;
                _state = 3;
            } else if (++_timer > 110) { // whiffed (target teleported / star-dodged) - climb out
                _timer = 0;
                _state = 3;
            }
            break;
        }
        case 3: // climb back to looming height
            _pos[0] += (dx + target->velocity[0] * 8.0f) * 0.05f;
            _pos[2] += (dz + target->velocity[2] * 8.0f) * 0.05f;
            _pos[1] += ((target->pos[1] + 95.0f) - _pos[1]) * 0.06f;
            if (_pos[1] > target->pos[1] + 80.0f) {
                _timer = 0;
                _state = 1;
            }
            break;
    }

    object->pos[0] = _pos[0];
    object->pos[1] = _pos[1];
    object->pos[2] = _pos[2];
    object->textureListIndex = (gGlobalTimer >> 3) & 3; // flutter the frames...
    func_80073514(_objectIndex); // ...and publish them - the ACTIVE texture AND its palette
}

void OLakituHazard::Draw(s32 cameraId) {
    Object* object = &gObjectList[_objectIndex];
    Camera* camera = &cameras[cameraId];
    s32 width;
    s32 height;

    if (!_diagDrawn) {
        _diagDrawn = true;
        race_mods_diag_line("evil_lakitu first draw: obj=%d state=%d objState=%d cam=%d pos=(%.0f %.0f %.0f)",
                            _objectIndex, _state, object->state, cameraId, _pos[0], _pos[1], _pos[2]);
    }
    if (_state == 0 || object->state == 0) {
        return;
    }
    // The stock Lakitu billboard draw (Lakitu.cpp's opaque branch), aimed at this camera.
    object->orientation[0] = 0;
    object->orientation[1] = func_800418AC(object->pos[0], object->pos[2], camera->pos);
    object->orientation[2] = 0x8000;
    width = object->textureWidth;
    height = object->textureHeight;
    FrameInterpolation_RecordOpenChild("evil_lakitu", TAG_OBJECT((_idx << 5) | cameraId));
    rsp_set_matrix_transformation(object->pos, object->orientation, object->sizeScaling);
    gSPDisplayList(gDisplayListHead++, (Gfx*) D_0D007D78);
    gDPLoadTLUT_pal256(gDisplayListHead++, object->activeTLUT);
    gDPLoadTextureTile(gDisplayListHead++, object->activeTexture, G_IM_FMT_CI, G_IM_SIZ_8b, width, height, 0, 0,
                       width - 1, height - 1, 0, G_TX_NOMIRROR | G_TX_CLAMP, G_TX_NOMIRROR | G_TX_CLAMP, G_TX_NOMASK,
                       G_TX_NOMASK, G_TX_NOLOD, G_TX_NOLOD);
    gSPVertex(gDisplayListHead++, (uintptr_t) object->vertex, 4, 0);
    gSPDisplayList(gDisplayListHead++, (Gfx*) common_rectangle_display);
    FrameInterpolation_RecordCloseChild();
    gSPTexture(gDisplayListHead++, 1, 1, 0, G_TX_RENDERTILE, G_OFF);
}

// --- Boo Haunt --------------------------------------------------------------------------------------

size_t OBooHazard::_count = 0;

OBooHazard::OBooHazard(s32 targetPlayerId) {
    Player* target;

    Name = "Boo Haunt";
    ResourceName = "mk:boo_haunt";
    _idx = _count;
    _count++;
    _target = targetPlayerId;

    find_unused_obj_index(&_objectIndex);
    init_object(_objectIndex, 0);
    init_texture_object(_objectIndex, (u8*) d_course_banshee_boardwalk_boo_tlut, gTextureGhosts, 48, (u16) 40);
    func_80073514(_objectIndex); // publish the ACTIVE texture + palette (see the lakitu note)
    gObjectList[_objectIndex].vertex = D_800E4470;
    gObjectList[_objectIndex].sizeScaling = 0.18f;
    gObjectList[_objectIndex].primAlpha = 0;
    gObjectList[_objectIndex].textureListIndex = 0;
    gObjectList[_objectIndex].orientation[0] = 0;
    gObjectList[_objectIndex].orientation[2] = 0x8000;
    gObjectList[_objectIndex].state = 2;

    target = &gPlayerOne[_target];
    _pos[0] = target->pos[0];
    _pos[1] = target->pos[1] + 14.0f;
    _pos[2] = target->pos[2];
    _orbit = (u16) (gGlobalTimer * 977); // start the circle anywhere

    func_800C9060((u8) _target, 0x1900705AU); // the cackle announces him on the haunted seat
    race_mods_diag_line("boo ctor: obj=%d target=%d", _objectIndex, _target);
}

void OBooHazard::Tick() {
    Object* object = &gObjectList[_objectIndex];
    Player* target = &gPlayerOne[_target];
    u16 sector;

    if (!_diagTicked) {
        _diagTicked = true;
        race_mods_diag_line("boo first tick: obj=%d state=%d", _objectIndex, _state);
    }
    if (_state == 0) {
        return;
    }
    if (!(target->type & PLAYER_EXISTS) || (target->type & PLAYER_CINEMATIC_MODE)) {
        _state = 0;
        object->state = 0;
        return;
    }

    // Circle the kart at headlight height - close enough to crowd the view, never touching.
    _orbit += 420; // a full lap around the kart every ~5s
    _pos[0] = target->pos[0] + sins(_orbit) * 42.0f;
    _pos[2] = target->pos[2] + coss(_orbit) * 42.0f;
    _pos[1] = target->pos[1] + 14.0f + sins((u16) (gGlobalTimer * 600)) * 3.0f;

    if (_state == 1) {
        if (object->primAlpha < 0x78) {
            object->primAlpha += 4; // materialize
        }
        if (--_life <= 0) {
            _state = 2;
            func_800C9060((u8) _target, 0x1900705AU); // the parting cackle
        }
    } else {
        object->primAlpha -= 4; // fade back out
        if (object->primAlpha <= 0) {
            object->primAlpha = 0;
            _state = 0;
            object->state = 0;
        }
    }

    // The stock boo's 36-sector sprite mapper (Boos.cpp func_8007C4A4): front half plays the
    // frames straight, the back half mirrors them. Facing rides the orbit so he turns as he
    // circles.
    sector = (u16) (((u32) (u16) (_orbit + 0x8000) * 0x24) >> 16);
    if (sector < 0x13) {
        clear_object_flag(_objectIndex, 0x80);
        object->textureListIndex = sector;
        object->vertex = D_800E4470;
    } else {
        set_object_flag(_objectIndex, 0x80);
        object->textureListIndex = 0x24 - sector;
        object->vertex = D_800E44B0;
    }
    func_80073514(_objectIndex); // publish the frame - the draw reads the ACTIVE pair

    object->pos[0] = _pos[0];
    object->pos[1] = _pos[1];
    object->pos[2] = _pos[2];
}

void OBooHazard::Draw(s32 cameraId) {
    Object* object = &gObjectList[_objectIndex];
    Camera* camera = &camera1[cameraId];

    if (_state == 0 || object->state == 0 || object->primAlpha <= 0) {
        return;
    }
    object->orientation[1] = func_800418AC(object->pos[0], object->pos[2], camera->pos);
    FrameInterpolation_RecordOpenChild("boo_haunt", TAG_OBJECT((_idx << 5) | cameraId));
    func_800484BC(object->pos, object->orientation, object->sizeScaling, object->primAlpha,
                  (u8*) object->activeTLUT, (u8*) object->activeTexture, object->vertex, 0x30, 0x28, 0x30, 0x28);
    FrameInterpolation_RecordCloseChild();
}

// --- Bob-omb Balloon --------------------------------------------------------------------------------

size_t OBalloonBomber::_count = 0;

OBalloonBomber::OBalloonBomber(f32 x, f32 y, f32 z, s32 wpIdx) {
    f32 ground;

    Name = "Bob-omb Balloon";
    ResourceName = "mk:balloon_bomber";
    _idx = _count;
    _count++;

    find_unused_obj_index(&_objectIndex);
    init_object(_objectIndex, 0);
    gObjectList[_objectIndex].state = 2;
    gObjectList[_objectIndex].sizeScaling = 1.0f;
    gObjectList[_objectIndex].textureListIndex = 0;
    set_obj_direction_angle(_objectIndex, 0U, 0U, 0U);

    ground = spawn_actor_on_surface(x, y + 200.0f, z);
    if (ground <= -2999.0f) {
        ground = y;
    }
    _progress = (f32) wpIdx;
    _pos[0] = x;
    _pos[1] = ground + 240.0f;
    _pos[2] = z;

    race_mods_diag_line("balloon_bomber ctor: obj=%d wp=%d pos=(%.0f %.0f %.0f)", _objectIndex, wpIdx, _pos[0],
                        _pos[1], _pos[2]);
}

void OBalloonBomber::Tick() {
    Object* object = &gObjectList[_objectIndex];
    TrackPathPoint* path = gTrackPaths[0];

    if (object->state == 0 || path == NULL || gSelectedPathCount == 0) {
        return;
    }
    if (--_life <= 0) {
        _pos[1] += 1.6f; // job done - up and away
        if (_life < -240) {
            object->state = 0;
        }
    } else {
        TrackPathPoint* wp;
        f32 targetY;
        _progress += 0.045f; // a slow patrol down the racing line
        while (_progress >= (f32) gSelectedPathCount) {
            _progress -= (f32) gSelectedPathCount;
        }
        wp = &path[(s32) _progress];
        _pos[0] += ((f32) wp->x - _pos[0]) * 0.02f;
        _pos[2] += ((f32) wp->z - _pos[2]) * 0.02f;
        targetY = (f32) wp->y + 240.0f + sins((u16) (gGlobalTimer * 350)) * 10.0f;
        _pos[1] += (targetY - _pos[1]) * 0.02f;

        if (--_dropTimer <= 0) {
            f32 ground = spawn_actor_on_surface(_pos[0], _pos[1], _pos[2]);
            _dropTimer = 150; // a payload every 5s
            if (ground > -2999.0f) {
                Vec3f hp;
                hp[0] = _pos[0] * gTrackDirection; // boxes expect course-data coords
                hp[1] = ground + 4.0f;
                hp[2] = _pos[2];
                if ((++_drops % 6) == 0) {
                    spawn_item_box(hp); // the occasional real one keeps the bait honest
                } else {
                    spawn_fake_item_box(hp);
                }
            }
        }
    }
    object->direction_angle[1] += 0x100; // the stock balloon's lazy spin
    object->pos[0] = _pos[0];
    object->pos[1] = _pos[1];
    object->pos[2] = _pos[2];
}

void OBalloonBomber::Draw(s32 cameraId) {
    Object* object = &gObjectList[_objectIndex];

    if (object->state == 0) {
        return;
    }
    if (!_diagDrawn) {
        _diagDrawn = true;
        race_mods_diag_line("balloon_bomber first draw: obj=%d pos=(%.0f %.0f %.0f)", _objectIndex, _pos[0], _pos[1],
                            _pos[2]);
    }
    FrameInterpolation_RecordOpenChild("balloon_bomber", TAG_OBJECT((_idx << 5) | cameraId));
    func_80043328(object->pos, (u16*) object->direction_angle, object->sizeScaling,
                  (Gfx*) d_course_luigi_raceway_dl_F960);
    gSPDisplayList(gDisplayListHead++, (Gfx*) d_course_luigi_raceway_dl_F650);
    FrameInterpolation_RecordCloseChild();
}

// --- Fog Bank ---------------------------------------------------------------------------------------

size_t OFogBank::_count = 0;

OFogBank::OFogBank(f32 x, f32 y, f32 z, f32 fx, f32 fz, f32 px, f32 pz) {
    s32 j;

    Name = "Fog Bank";
    ResourceName = "mk:fog_bank";
    _idx = _count;
    _count++;

    // A 3x3 patch of puffs spanning the line: three across, three along, jittered so it reads
    // as weather instead of a grid.
    for (j = 0; j < kPuffs; j++) {
        f32 a = (f32) (j % 3) - 1.0f;
        f32 b = (f32) (j / 3) - 1.0f;
        _puffs[j][0] = x + px * a * 85.0f + fx * b * 110.0f + (f32) ((s32) random_int(40) - 20);
        _puffs[j][1] = y + 16.0f + (f32) random_int(22);
        _puffs[j][2] = z + pz * a * 85.0f + fz * b * 110.0f + (f32) ((s32) random_int(40) - 20);
        _scale[j] = 7.0f + 0.16f * (f32) random_int(25);
    }
    race_mods_diag_line("fog_bank ctor: pos=(%.0f %.0f %.0f)", x, y, z);
}

void OFogBank::Tick() {
    if (_age < _life) {
        _age++;
    }
    if (_age < 90) { // roll in...
        _alpha = _age * 150 / 90;
    } else if (_age > _life - 120) { // ...thin out...
        _alpha = (_life - _age) * 150 / 120;
        if (_alpha < 0) {
            _alpha = 0;
        }
    } else { // ...sit heavy in between
        _alpha = 150;
    }
}

void OFogBank::Draw(s32 cameraId) {
    Vec3s rot = { 0, 0, 0 };
    s32 j;

    if (_alpha <= 0) {
        return;
    }
    for (j = 0; j < kPuffs; j++) {
        rot[1] = func_800418AC(_puffs[j][0], _puffs[j][2], camera1[cameraId].pos); // face the camera
        FrameInterpolation_RecordOpenChild("fog_puff", TAG_OBJECT((_idx << 9) | (j << 5) | cameraId));
        func_800652D4(_puffs[j], rot, _scale[j]);
        gSPDisplayList(gDisplayListHead++, (Gfx*) D_0D008DB8);
        gDPLoadTextureBlock(gDisplayListHead++, common_texture_particle_smoke[(j + (gGlobalTimer >> 4)) & 3],
                            G_IM_FMT_I, G_IM_SIZ_8b, 32, 32, 0, G_TX_NOMIRROR | G_TX_WRAP, G_TX_NOMIRROR | G_TX_WRAP,
                            G_TX_NOMASK, G_TX_NOMASK, G_TX_NOLOD, G_TX_NOLOD);
        func_8004B72C(216, 216, 224, 150, 150, 160, _alpha);
        gDPSetAlphaCompare(gDisplayListHead++, G_AC_DITHER);
        gSPDisplayList(gDisplayListHead++, (Gfx*) D_0D008E48);
        FrameInterpolation_RecordCloseChild();
    }
    gSPTexture(gDisplayListHead++, 1, 1, 0, G_TX_RENDERTILE, G_OFF);
}

// --- C bridges for race_mods.c's track-hazard waves ------------------------------------------------
// The hazard table lives in C; these hand the heavy engine objects over the language fence.

extern "C" void race_mods_spawn_evil_lakitu(int targetPlayerId) {
    race_mods_diag_line("bridge: spawn evil_lakitu target=%d", targetPlayerId);
    GetWorld()->AddObject(std::make_unique<OLakituHazard>((s32) targetPlayerId));
}

extern "C" void race_mods_spawn_chomp(int slotIdx) {
    race_mods_diag_line("bridge: spawn chomp slot=%d objIdx=%d pathCount=%d", slotIdx, indexObjectList2[slotIdx],
                        gSelectedPathCount);
    GetWorld()->AddObject(std::make_unique<OChainChomp>((size_t) slotIdx));
}

// Prop-swap chomp (the PROPS row's takeover): same slot discipline as the hazard chomp, but the
// start waypoint is handed in so the pack spreads evenly along the racing line.
extern "C" void race_mods_spawn_chomp_at(int slotIdx, int wpIdx) {
    race_mods_diag_line("bridge: spawn chomp slot=%d wp=%d pathCount=%d", slotIdx, wpIdx, gSelectedPathCount);
    GetWorld()->AddObject(std::make_unique<OChainChomp>((size_t) slotIdx, (s32) wpIdx));
}

extern "C" void race_mods_spawn_thwomp(float x, float y, float z, int behaviour, int primAlpha) {
    // The stock thwomp inits assume Bowser's Castle: floors at y=0 (origin_pos[1] = 0) and
    // course-section ids 6/8/10 gating visibility + crush collision. A hazard thwomp lands on
    // ANY course, so the bridge probes the real floor at the waypoint and flags HazardMode -
    // the inits ground to GroundY and SetVisibility skips the section band (frustum cull only).
    // primAlpha doubles as a per-behaviour config knob (slide speed 0/1, slam timer 0-3) - the
    // wave table picks it per entry.
    f32 ground = spawn_actor_on_surface(x, y + 200.0f, z);
    if (ground <= -2999.0f) {
        ground = y; // probe found void - trust the waypoint's own height
    }
    OThwomp* t =
        OThwomp::Spawn((s16) x, (s16) z, (s16) ((gGlobalTimer & 3) * 0x4000), 1.0f, (s16) behaviour, (s16) primAlpha);
    if (t != nullptr) {
        t->HazardMode = true;
        t->GroundY = ground;
    }
    race_mods_diag_line("bridge: spawn thwomp pos=(%.0f %.0f %.0f) behaviour=%d alpha=%d ground=%.0f ok=%d", x, y, z,
                        behaviour, primAlpha, ground, t != nullptr ? 1 : 0);
}

extern "C" void race_mods_spawn_boulder(float x, float y, float z) {
    // The real bouncing boulder (the C-actor ACTOR_FALLING_ROCK's render moved to C++ and the
    // old hazard spawn went invisible) - dropped from the sky onto the racing line.
    race_mods_diag_line("bridge: spawn boulder pos=(%.0f %.0f %.0f)", x, y, z);
    AFallingRock::Spawn(FVector(x, y + 350.0f, z), 150);
}

// Probe the floor under a waypoint; the waypoint's own height when the probe finds only void.
static f32 hazard_ground(f32 x, f32 y, f32 z) {
    f32 ground = spawn_actor_on_surface(x, y + 80.0f, z);
    return (ground <= -2999.0f) ? y : ground;
}

extern "C" void race_mods_spawn_snowman(float x, float y, float z) {
    race_mods_diag_line("bridge: spawn snowman pos=(%.0f %.0f %.0f)", x, y, z);
    OSnowman::Spawn(FVector(x, hazard_ground(x, y, z), z));
}

extern "C" void race_mods_spawn_crab(float x1, float z1, float x2, float z2) {
    race_mods_diag_line("bridge: spawn crab patrol=(%.0f %.0f)-(%.0f %.0f)", x1, z1, x2, z2);
    OCrab::Spawn(FVector2D(x1, z1), FVector2D(x2, z2)); // grounds itself every tick
}

extern "C" void race_mods_spawn_hedgehog(float x, float y, float z, float px, float pz) {
    race_mods_diag_line("bridge: spawn hedgehog pos=(%.0f %.0f %.0f)", x, y, z);
    OHedgehog::Spawn(FVector(x, hazard_ground(x, y, z), z), FVector2D(px, pz), 0);
}

extern "C" void race_mods_spawn_penguin(float x, float y, float z, float diameter) {
    race_mods_diag_line("bridge: spawn penguin pos=(%.0f %.0f %.0f) d=%.0f", x, y, z, diameter);
    // A CIRCLE penguin reads this "direction" as the angular STEP it advances around its circle
    // each tick (the original Sherbet adults use ~0x150). The old gGlobalTimer*4047 value was a
    // huge, frame-dependent step, so the chick teleported to a new spot on its circle every frame -
    // that was the "glitching all over the place". Keep it small; nudge it by the diameter so a
    // wave of three doesn't orbit in perfect lockstep.
    u16 circleStep = (u16) (0x140 + ((s32) diameter & 0x3F));
    OPenguin::Spawn(FVector(x, hazard_ground(x, y, z), z), circleStep, 0, diameter,
                    OPenguin::PenguinType::CHICK, OPenguin::Behaviour::CIRCLE);
}

extern "C" void race_mods_spawn_mole_ambush(float x, float y, float z) {
    // A cluster popping out of the ground around the waypoint. OMoleGroup multiplies each
    // stored x by xOrientation when a mole pops, and these coords are already runtime space -
    // pre-multiply so the two flips cancel (the same dance the box spawners do with the
    // mirror). tickRate = group size so every mole in the ambush ticks each frame.
    f32 ground = hazard_ground(x, y, z);
    std::vector<FVector> spawns;
    spawns.push_back(FVector(x * xOrientation, ground, z));
    spawns.push_back(FVector((x + 42.0f) * xOrientation, ground, z + 30.0f));
    spawns.push_back(FVector((x - 38.0f) * xOrientation, ground, z + 36.0f));
    spawns.push_back(FVector((x + 26.0f) * xOrientation, ground, z - 42.0f));
    race_mods_diag_line("bridge: spawn mole_ambush pos=(%.0f %.0f %.0f)", x, ground, z);
    GetWorld()->AddObject(std::make_unique<OMoleGroup>(spawns, spawns.size()));
}

extern "C" void race_mods_spawn_bomb_kart(int pathPoint, int behaviour) {
    race_mods_diag_line("bridge: spawn bomb_kart wp=%d behaviour=%d", pathPoint, behaviour);
    OBombKart::Spawn((uint32_t) 0, (uint32_t) pathPoint, (uint16_t) behaviour, 1.0f);
}

extern "C" void race_mods_spawn_cheep_school(int slotIdx, float x, float y, float z, int wpIdx) {
    TrackPathPoint* path = gTrackPaths[0];
    s32 start = wpIdx - 4;
    s32 end = wpIdx + 16;
    OCheepCheep* cheep;

    if (path == NULL || gSelectedPathCount < 24) {
        return;
    }
    // The activation span is strict (start < point < end) and unwrapped - clamp it inside the
    // path so a school rolled near the start line still arms.
    if (start < 1) {
        start = 1;
        end = 21;
    }
    if (end > gSelectedPathCount - 2) {
        end = gSelectedPathCount - 2;
        start = (end - 20 < 1) ? 1 : end - 20;
    }
    // Spawn just under the waypoint so the arc breaks UP across the line, leaping along the
    // sideways basis (the stock yaw is Banshee's lake orientation).
    cheep = OCheepCheep::Spawn(FVector(x, y - 60.0f, z), OCheepCheep::Behaviour::RACE,
                               IPathSpan(start, end));
    if (cheep != nullptr) {
        f32 dx = (f32) (path[(wpIdx + 3) % gSelectedPathCount].x - path[wpIdx].x);
        f32 dz = (f32) (path[(wpIdx + 3) % gSelectedPathCount].z - path[wpIdx].z);
        cheep->Slot = slotIdx;
        cheep->HazardLeap = true;
        // Binary yaw of the line's sideways basis: displacement convention is
        // (sins(yaw), coss(yaw)), so yaw = atan2(x, z) of the perp vector (dz, -dx).
        cheep->LeapYaw = (s16) (s32) (atan2f(dz, -dx) * (32768.0f / 3.14159265f));
        // The slot may hold a stale object from this course's own kit - park it at state 0 so
        // the school arms cleanly from the activation span.
        func_80072428(indexObjectList2[slotIdx]);
    }
    race_mods_diag_line("bridge: spawn cheep slot=%d span=%d-%d ok=%d", slotIdx, start, end,
                        cheep != nullptr ? 1 : 0);
}

extern "C" void race_mods_spawn_boo(int targetPlayerId) {
    race_mods_diag_line("bridge: spawn boo target=%d", targetPlayerId);
    GetWorld()->AddObject(std::make_unique<OBooHazard>((s32) targetPlayerId));
}

extern "C" void race_mods_spawn_spiny_lakitu(int targetPlayerId) {
    race_mods_diag_line("bridge: spawn spiny_lakitu target=%d", targetPlayerId);
    GetWorld()->AddObject(std::make_unique<OLakituHazard>((s32) targetPlayerId, 1));
}

extern "C" void race_mods_spawn_balloon_bomber(float x, float y, float z, int wpIdx) {
    GetWorld()->AddObject(std::make_unique<OBalloonBomber>(x, y, z, (s32) wpIdx));
}

extern "C" void race_mods_spawn_fog_bank(float x, float y, float z, float fx, float fz, float px, float pz) {
    GetWorld()->AddObject(std::make_unique<OFogBank>(x, y, z, fx, fz, px, pz));
}
