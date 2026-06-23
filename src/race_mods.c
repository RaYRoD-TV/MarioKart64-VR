// Runtime race mutators (see race_mods.h). Configured on the race setup screen via CVars:
// gRaceKnockout, gSpeedTurboLaps, gKartSize, gItemRain, gLapsOverride. Everything keys off the
// leader's lap watermark and player state that the game already maintains; nothing here ticks
// outside GRAND_PRIX / VERSUS races.
#include <libultraship.h>
#include <libultra/types.h>
#include <stdio.h>
#include <stdarg.h>
#include <libc/math.h>
#include <macros.h>
#include <mk64.h>
#include <common_structs.h>
#include <defines.h>
#include "main.h"
#include "kart_attributes.h"
#include "effects.h"
#include "waypoints.h"
#include <sounds.h>
#include <actor_types.h>
#include "race_mods.h"

extern f32 gTrackDirection; // -1 in mirror mode - spawners flip x by it for course-data coords
extern s16 gCurrentCourseId; // code_800029B0.c - the TRACKS enum id; custom courses sit outside it

extern int   CVarGetInteger(const char* name, int defaultValue);
extern void  CVarSetInteger(const char* name, int value);
extern float CVarGetFloat(const char* name, float defaultValue);
extern void  CVarSetFloat(const char* name, float value);
// racing/actors.c: an item box that drops from the sky and settles at float height (item rain).
extern void spawn_falling_item_box(f32 x, f32 y, f32 z);
// racing/actors.c: track hazards - a fake item box (trap) and a normal item box (reward). Both
// expect course-data coords and flip x by gTrackDirection internally, so waypoint positions
// (already runtime space) pre-multiply x by gTrackDirection to cancel the flip.
extern void spawn_fake_item_box(Vec3f pos);
extern void spawn_item_box(Vec3f pos);
// racing/actors.c: the treasure-hunt prize box - a normal item box whose actor index comes back,
// so the renderer can draw it giant and the minimap can mark it.
extern s32 spawn_treasure_item_box(Vec3f pos);
// racing/actors.c: cross-course props - piranha plants, rolling yoshi eggs, falling rocks.
extern void spawn_hazard_prop(s16 actorType, f32 x, f32 y, f32 z);
// racing/actors.c: nearest live item box to a point (battle rivals drive to these to gather weapons).
extern s32 find_nearest_item_box(f32 fromX, f32 fromZ, f32* outX, f32* outZ);
// racing/actors.c: a kiwano fruit (the DK Jungle pest) ambushing one human racer, and a loose red
// shell that hunts whoever leads the race.
extern void spawn_hazard_kiwano(s32 targetPlayerId);
extern void spawn_hazard_red_shell(f32 x, f32 y, f32 z, f32 dirX, f32 dirZ);
// engine/objects/LakituHazard.cpp: the heavy engine-object hazards, bridged over the language
// fence - a rogue Lakitu that dive-bombs one racer, a chain chomp patrolling the racing line
// (any course - the path follower wraps), a thwomp, and the real bouncing boulder (the old
// C-actor falling rock's renderer moved to C++ and its hazard spawn went invisible).
extern void race_mods_spawn_evil_lakitu(int targetPlayerId);
extern void race_mods_spawn_chomp(int slotIdx);
extern void race_mods_spawn_chomp_at(int slotIdx, int wpIdx);
extern void race_mods_spawn_thwomp(float x, float y, float z, int behaviour, int primAlpha);
extern void race_mods_spawn_boulder(float x, float y, float z);
// engine/objects/LakituHazard.cpp: the rest of the hazard menagerie, bridged the same way. The
// critters reuse stock course-object classes - their models and textures load through the
// resource archive by name, so a Frappe snowman or a Sherbet penguin renders on every course.
extern void race_mods_spawn_snowman(float x, float y, float z);
extern void race_mods_spawn_crab(float x1, float z1, float x2, float z2);
extern void race_mods_spawn_hedgehog(float x, float y, float z, float px, float pz);
extern void race_mods_spawn_penguin(float x, float y, float z, float diameter);
extern void race_mods_spawn_mole_ambush(float x, float y, float z);
extern void race_mods_spawn_bomb_kart(int pathPoint, int behaviour);
extern void race_mods_spawn_cheep_school(int slotIdx, float x, float y, float z, int wpIdx);
extern void race_mods_spawn_boo(int targetPlayerId);
extern void race_mods_spawn_spiny_lakitu(int targetPlayerId);
extern void race_mods_spawn_balloon_bomber(float x, float y, float z, int wpIdx);
extern void race_mods_spawn_fog_bank(float x, float y, float z, float fx, float fz, float px, float pz);
// code_80057C60.c: the battle balloon kit, reused by the BALLOONS race mode.
extern void init_all_player_balloons(Player* player, s8 playerIndex);
extern void pop_player_balloon(Player* player, s8 playerIndex);
extern s16 gPlayerBalloonCount[];
// The d-pad-up view-mode hotkey reads these: live controller edges, the pause state (the pause
// overlay owns the d-pad while it's up - gIsGamePaused is u16, extern it as unsigned short,
// NOT int), and whether VR owns the view (VR cycles its own four modes).
extern struct Controller gControllers[];
extern unsigned short gIsGamePaused;
extern bool vr_is_active(void);
// render_player.c: the star rainbow only cycles while gCourseTimer sits within 8s of this start
// time (the star ITEM writes it once). Infected carriers refresh it every tick so the flash is
// permanent - the STAR_EFFECT flag alone renders nothing.
extern s32 gPlayerStarEffectStartTime[];
// code_80005FD0.c: the live rank tables (rank slot -> player id, player id -> rank slot), their
// duplicate pair (the second sort, no cinematic gate - some displays read these instead), and
// the authoritative lap-CROSSING counter that gPlayers[].lapCount follows.
extern s16 gGPCurrentRacePlayerIdByRank[];
extern s32 gGPCurrentRaceRankByPlayerId[];
extern s16 D_80164378[];
extern s32 gGPCurrentRaceRankByPlayerIdDup[];
extern s32 gLapCountByPlayerId[];
// racing/math_util.c: rotate a velocity/offset vector around Y by a binary angle, plus the
// heading basis the treasure placement scatters candidates with, and the binary yaw between
// two points (the CPU stuck-rescue points the kart down the path with it).
extern void func_802B64C4(Vec3f vec, s16 yawBinary);
extern f32 sins(u16 angle);
extern f32 coss(u16 angle);
extern s32 get_angle_between_two_vectors(Vec3f from, Vec3f to);
// racing/collision.c: the ground probe treasure placement validates "reachable" with - finds
// the surface height below a point, or -3000 when there is nothing down there but void.
extern f32 spawn_actor_on_surface(f32 posX, f32 posY, f32 posZ);
// port/vr: First Person drama - dramatic/finished state plus the kart's live spin/tumble angles.
extern void vr_set_action_cam(int dramatic, int finished);
extern void vr_set_drama_rot(short yawBinary, short pitchBinary);

#define TURBO_BASE_CC 150.0f
#define TURBO_STEP_CC 100.0f
#define TURBO_MAX_CC 900.0f
#define KO_SIZE 0.45f
#define KO_SPEED_PENALTY 0.55f
#define RAIN_PERIOD 270      // 9s at the 30Hz logic tick
#define RAIN_MAX_DROPS 12    // spawns per race, so the actor pool never starves of slots
#define HAZARD_PERIOD 540    // a fresh hazard wave every 18s
#define HAZARD_MAX_WAVES 8   // waves per race (3 boxes each), same actor-pool protection
#define HAZARD_MAX_CHOMPS 3  // persistent heavies are capped per race - chomps also burn
#define HAZARD_MAX_THWOMPS 4 // indexObjectList2 slots (8+) and must stay under the list's 32
#define HAZARD_MAX_LAKITUS 1 // only ONE evil-lakitu dive-bomber per race - a per-race cap of 1 also
                             // guarantees you never face two at once (the rare-gate keeps it rare)
// One bit per hazard family, for the LEVEL-THEMED hazards mode (gTrackHazards 1): every stock
// course gets a hand-picked "believable" menu below, and a rolled hazard outside it re-rolls.
// WILD mode (gTrackHazards 2) allows everything everywhere.
#define HZ_BANANA (1u << 0)
#define HZ_SHELL (1u << 1)
#define HZ_PIRANHA (1u << 2)
#define HZ_EGG (1u << 3)
#define HZ_BOULDER (1u << 4)
#define HZ_MOLE (1u << 5)
#define HZ_SNOWMAN (1u << 6)
#define HZ_CHEEP (1u << 7)
#define HZ_CRAB (1u << 8)
#define HZ_HOG (1u << 9)
#define HZ_PENGUIN (1u << 10)
#define HZ_BOMBKART (1u << 11)
#define HZ_CHOMP (1u << 12)
#define HZ_THWOMP (1u << 13)
#define HZ_LAKITU (1u << 14) // dive-bomber and spiny-egg lobber together
#define HZ_BOO (1u << 15)
#define HZ_BALLOON (1u << 16)
#define HZ_FOG (1u << 17)
#define HZ_LEADROCK (1u << 18) // the leader boulder strike
#define HZ_BOXES (1u << 19)
#define HZ_STORM (1u << 20)  // systemic: lightning storm
#define HZ_GUST (1u << 21)   // systemic: crosswind gust
#define HZ_KIWANO (1u << 22) // systemic: the off-road ambush
#define HZ_ALL 0xFFFFFFFFu

// Item junk is universal - it falls off karts on every course in the galaxy.
#define HZ_BASE (HZ_BANANA | HZ_SHELL | HZ_BOXES)

// The wider menagerie is capped the same way - every persistent critter burns a free object
// slot for the rest of the race, and the pool also feeds shells, items and effects.
#define HAZARD_MAX_BOOS 2
#define HAZARD_MAX_BOMBERS 1 // the bob-omb balloon - one is an event, two is noise
#define HAZARD_MAX_FOG 2
#define HAZARD_MAX_BOMBKARTS 3
#define HAZARD_MAX_MOLES 2   // mole GROUPS (a group is ~4 moles + a shared dirt pool)
#define HAZARD_MAX_SNOWMEN 4
#define HAZARD_MAX_CHEEPS 3  // indexObjectList2 slots 11+ (the chomps own 8-10)
#define HAZARD_MAX_CRABS 4
#define HAZARD_MAX_HOGS 4
#define HAZARD_MAX_PENGUIN_WAVES 2 // a crossing is 3 penguins - two crossings is plenty

// PROP SWAP (gPropSwap, the setup screen's PROP SWAP row): courses with signature movers -
// turnpike traffic, kalimari trains, frappe snowmen, choco falling rocks, rainbow chomps - skip
// them at BeginPlay (race_mods_prop_swap_live) and register how many stood down; the replacement
// menagerie spawns evenly along the racing line at the staging one-shot (prop_swap_spawn).
// Chomp slots sit ABOVE the hazard families: hazard chomps own 8-10, hazard cheeps 11-13, so the
// takeover pack starts at 14 and the two systems never fight over indexObjectList2 (32 entries).
#define PROP_SWAP_CHOMP_SLOT_BASE 14
#define PROP_SWAP_MAX_CHOMPS 12
#define PROP_SWAP_MAX_BOMBKARTS 10
#define PROP_SWAP_MAX_SNOWMEN 16
#define PROP_SWAP_MAX_PENGUINS 12
static s32 sPropSwapSkipped; // movers the course skipped this load - consumed by prop_swap_spawn

static s32 sKO[NUM_PLAYERS];
static s32 sSlow[NUM_PLAYERS]; // the SLOWED balloon-out consequence: full size, knockout's speed cut
static s32 sMaxLapSeen; // leader's lap watermark - knockout and turbo both fire on its rise
static s32 sPreRace = 1;
static s32 sRainTimer;
static s32 sRainDrops;
static s32 sHazardTimer;
static s32 sHazardWaves;
static s32 sHazardChomps;  // persistent-heavy counts, against the HAZARD_MAX_* caps
static s32 sHazardThwomps;
static s32 sHazardLakitus;
static s32 sHazardBoos;
static s32 sHazardBombers;
static s32 sHazardFog;
static s32 sHazardBombKarts;
static s32 sHazardMoles;
static s32 sHazardSnowmen;
static s32 sHazardCheeps;
static s32 sHazardCrabs;
static s32 sHazardHogs;
static s32 sHazardPenguinWaves;
static s32 sOffroadTicks[NUM_PLAYERS];   // consecutive ticks a human has spent off the road
static s32 sKiwanoCooldown[NUM_PLAYERS]; // per-player grace after a kiwano ambush
static s32 sLeaderStrikeTicks;           // >0: a warned boulder is inbound on the race leader
static s32 sGustTimer;                   // counts up to the next crosswind gust
static s32 sGustNext;                    // when the next gust arrives
static s32 sGustTicks;                   // remaining gust duration
static s32 sGustStart;                   // first waypoint of the gust zone
static f32 sGustVecX;                    // the sideways shove, world units per tick (gentle - VR)
static f32 sGustVecZ;
// CPU stuck watchdog: high speed classes can pin a CPU against a wall its waypoint steering
// can't solve - it grinds there for the rest of the race. The watchdog spots a CPU that has
// barely moved for a stretch and sets it back on the racing line. Always on (it costs nothing
// while everyone drives), every race mode.
static f32 sCpuLastX[NUM_PLAYERS];
static f32 sCpuLastZ[NUM_PLAYERS];
static s32 sCpuStuckTicks[NUM_PLAYERS];
static s32 sCpuRescueCool[NUM_PLAYERS];

// Infected-mode state lives up here so apply_cc_to_players can re-apply the carrier speed
// factors after a turbo-lap recompute wipes topSpeed (the infected logic itself is further down).
#define INFECTED_SPEED_BONUS 1.08f
#define INFECTED_STAR_SURGE 1.25f // a used star item buys a real hunting surge on top
static s32 sInfected[NUM_PLAYERS];
static s32 sInfectStarSurge[NUM_PLAYERS]; // ticks left on a carrier's star-item speed surge

static s32 player_exists(s32 i) {
    return (gPlayers[i].type & PLAYER_EXISTS) != 0;
}

// Append one line to the race diag file (racemods_diag.txt, next to the exe - the same file the
// race-start line goes to). The heavy-hazard spawn bridges and each heavy's first tick/draw log
// through this, so ONE hazards-on race localizes where a chain breaks: spawned at all -> ticking
// -> drawn -> where. Cheap by construction - every caller is a one-shot or a spawn edge.
void race_mods_diag_line(const char* fmt, ...) {
    va_list ap;
    FILE* df = fopen("racemods_diag.txt", "a");
    if (df == NULL) {
        return;
    }
    va_start(ap, fmt);
    vfprintf(df, fmt, ap);
    va_end(ap);
    fputc('\n', df);
    fclose(df);
}

// The HUD's rank ladder and minimap need the same EXISTS test the mutators use - a culled duel
// seat (type zeroed, sunk below the track) must never draw a ghost portrait or dot.
int race_mods_player_exists(int i) {
    return i >= 0 && i < NUM_PLAYERS && player_exists(i);
}

// True while kart i's tires sit on slow terrain - the kiwano ambush and the lightning storm
// both target shortcutters with it. Airborne (surfaceType 0) deliberately reads as ON the road.
static s32 player_is_offroad(s32 i) {
    switch (gPlayers[i].surfaceType) {
        case SAND_OFFROAD:
        case GRASS:
        case SNOW_OFFROAD:
        case CLIFF:
        case DIRT_OFFROAD:
            return 1;
    }
    return 0;
}

// A random human still racing, for the targeted hazards (evil lakitu, spiny lakitu, boo). -1
// when everyone's finished - callers drop a banana instead so the wave never fizzles silently.
static s32 pick_random_human_target(void) {
    s32 humans[NUM_PLAYERS];
    s32 nh = 0;
    s32 h;
    for (h = 0; h < NUM_PLAYERS; h++) {
        if (player_exists(h) && (gPlayers[h].type & PLAYER_HUMAN) &&
            gPlayers[h].lapCount < race_mods_total_laps()) {
            humans[nh++] = h;
        }
    }
    if (nh == 0) {
        return -1;
    }
    return humans[random_int((u16) (nh - 1))];
}

int race_mods_total_laps(void) {
    // A treasure hunt runs until the box is found - a lap total must never end it, so it pins
    // NO LIMIT regardless of the LAPS row (which the setup screen locks to match). Balloon
    // elimination deliberately does NOT pin: a lap count plus eliminations makes a fun race
    // (finish line or last-one-standing, whichever comes first).
    if (CVarGetInteger("gRaceMode", 0) == 4) {
        return 100;
    }
    // No mode gate here on purpose: every non-CUSTOM menu pick (stock CC, TT, Battle) zeroes the
    // override via race_mods_reset_to_stock, and the HUD init reads this during course load.
    // 100 is the NO LIMIT sentinel - the finish condition simply never arrives.
    s32 laps = CVarGetInteger("gLapsOverride", 0);
    if (laps == 100) {
        return 100;
    }
    return (laps >= 1 && laps <= 9) ? laps : 3;
}

// LOW GRAVITY mode (gLowGravity): the factor player_controller.c multiplies the finalized kart
// gravity by, so karts hop higher, catch big air off ramps, and drift down slowly. 1.0 = normal.
// GP/VS races only, never attract demos.
float race_mods_gravity_scale(void) {
    if (!CVarGetInteger("gLowGravity", 0) || gDemoMode != 0 ||
        (gModeSelection != GRAND_PRIX && gModeSelection != VERSUS)) {
        return 1.0f;
    }
    return 0.5f;
}

// The CC -> speed-field mapping spawn_players uses: linear through the CC_50 and CC_150 anchors.
static f32 cc_lerp(f32 v50, f32 v150, f32 cc) {
    f32 a = (v150 - v50) / 100.0f;
    return a * cc + (v50 - a * 50.0f);
}

#define TURBO_CPU_CAP_CC 300.0f // CPUs escalate too, but stop here - past this their pathing breaks

static void apply_cc_to_players(f32 cc) {
    s32 i;
    for (i = 0; i < NUM_PLAYERS; i++) {
        s32 ch;
        f32 useCc = cc;
        if (!player_exists(i)) {
            continue;
        }
        // CPUs ride the escalation up to a cap: their waypoint steering stays sane through 300cc
        // but misses corners wholesale beyond it. Humans escalate without limit.
        if ((gPlayers[i].type & PLAYER_HUMAN) == 0 && useCc > TURBO_CPU_CAP_CC) {
            useCc = TURBO_CPU_CAP_CC;
        }
        ch = gPlayers[i].characterId;
        gPlayers[i].unk_084 = cc_lerp(D_800E2400[CC_50][ch], D_800E2400[CC_150][ch], useCc);
        gPlayers[i].unk_088 = cc_lerp(D_800E24B4[CC_50][ch], D_800E24B4[CC_150][ch], useCc);
        gPlayers[i].unk_210 = cc_lerp(D_800E2568[CC_50][ch], D_800E2568[CC_150][ch], useCc);
        gPlayers[i].topSpeed = cc_lerp(gTopSpeedTable[CC_50][ch], gTopSpeedTable[CC_150][ch], useCc);
        if (sKO[i] || sSlow[i]) {
            gPlayers[i].topSpeed *= KO_SPEED_PENALTY; // keep punished karts slow through escalations
        }
        if (sInfected[i] && CVarGetInteger("gRaceMode", 0) == 5) {
            // The recompute just wiped the carrier perks - put them back so a turbo escalation
            // doesn't silently cure the outbreak's speed edge.
            gPlayers[i].topSpeed *= INFECTED_SPEED_BONUS;
            if (sInfectStarSurge[i] > 0) {
                gPlayers[i].topSpeed *= INFECTED_STAR_SURGE;
            }
        }
    }
}

// Knockout strikes the kart that crosses the lap line LAST: once every other still-racing kart has
// reached the current lap, whoever is left on the old one (the human included) takes the lightning.
static void knockout_check(void) {
    s32 totalLaps = race_mods_total_laps();
    s32 racing = 0;
    s32 behind = 0;
    s32 lastIdx = -1;
    s32 i;

    if (sMaxLapSeen < 1 || sMaxLapSeen >= totalLaps) {
        return; // no strikes before the field reaches lap 2, none once the leader can finish
    }
    for (i = 0; i < NUM_PLAYERS; i++) {
        if (!player_exists(i) || sKO[i] || gPlayers[i].lapCount >= totalLaps) {
            continue;
        }
        racing++;
        if (gPlayers[i].lapCount < sMaxLapSeen) {
            behind++;
            lastIdx = i;
        }
    }
    if (racing > 2 && behind == 1) {
        sKO[lastIdx] = 1;
        // The real strike: sfx + spin + shrink animation + the 0.6x propulsion hit. The per-frame
        // squash below keeps them tiny after the effect would normally time out and regrow.
        trigger_lightning_strike(&gPlayers[lastIdx], (s8) lastIdx);
        gPlayers[lastIdx].topSpeed *= KO_SPEED_PENALTY;
    }
}

// --- race modes (gRaceMode: 0 normal / 1 knockout / 2 balloons / 3 tag / 4 treasure hunt) ---------

extern void play_sound2(s32 soundBits);

static s32 sTagIt = -1;
static s32 sTagCooldown;
static s32 sTagLive;                 // per-tick "mode 3 race running" flag - the place-HUD hide reads it
static s32 sTagItTicks[NUM_PLAYERS]; // cumulative ticks each seat has been IT - the tag scoreboard
static s32 sTagLoser = -1;           // the single tag LOSER (most time IT), frozen when the human finishes
static s32 sTreasureActive;
static s32 sTreasureHuntLive;      // per-tick "mode 4 race running" flag - the place-HUD hide reads it
static s32 sTreasureActorIdx = -1; // the prize box's actor slot, for the giant render
static f32 sTreasurePos[3];
static s32 sTreasureWinner = -1; // who claimed the prize - the banner + the rank hold read this
static s32 sTreasureEndTimer;    // grace after the find, then the whole field force-finishes
static s32 sTreasureEndFired;

// --- duel spawn filter ----------------------------------------------------------------------------
// Driven from spawn_players_gp_one_player so a duel is a two-kart race from the very first intro
// frame: the HUD, the fly-by and the countdown only ever know the pair that actually races. The old
// shape - eight karts on the grid that vanished at the green light - read as a bug, because it was.

extern s16 chooseCPUPlayers[]; // spawn_players.c - the 7 CPU characters players 1..7 spawn with
extern s8 gCharacterSelections[]; // menus.c - [0] is the human's character

static s32 sDuelSpawnFiltered; // set when the spawn-time filter culled this race's field

// Cleared at the top of every spawn pass; a duel spawn sets it back (keeps the fallback honest).
void race_mods_duel_spawn_reset(void) {
    sDuelSpawnFiltered = 0;
}

// Prepares a 1P VERSUS duel spawn and returns the rival's player index: always 1 (gPlayerTwo,
// the only CPU seat whose spawn reads one grid slot for both coordinates - the others cross-pair
// slots), or -1 when this spawn isn't a duel. The rival picked on the character screen is swapped
// into that seat. gVersusDuel is set by the VS course commit only - independent of the MODE row.
int race_mods_duel_spawn_prepare(void) {
    s32 rivalChar;
    s32 i;
    if (gDemoMode != 0 || gModeSelection != GRAND_PRIX || CVarGetInteger("gVersusDuel", 0) != 1) {
        return -1;
    }
    rivalChar = CVarGetInteger("gDuelRival", 0) - 1;
    if (rivalChar >= 0) {
        // The CPU character set only recomputes on a cup's first track, and the VS flow can start
        // anywhere in a cup - so when the pick isn't in the (possibly stale) set, force the seat.
        for (i = 0; i < 7; i++) {
            if (chooseCPUPlayers[i] == rivalChar) { // player i+1 would spawn with this character
                chooseCPUPlayers[i] = chooseCPUPlayers[0];
                break;
            }
        }
        chooseCPUPlayers[0] = (s16) rivalChar;
    } else if (chooseCPUPlayers[0] == gCharacterSelections[0]) {
        // No pick recorded and a stale set put your own character in the rival seat: shift to
        // any other character so the duel isn't a mirror match by accident.
        chooseCPUPlayers[0] = (s16) ((gCharacterSelections[0] + 1) % 8);
    }
    return 1;
}

// --- race balloon match (gRaceMode 2) -------------------------------------------------------------
// Every racer carries the battle balloons into the race: each hit pops one, and running out costs
// what the BALLOON OUT row says (gBalloonOut: 0 shrunk tiny+slow / 1 eliminated / 2 slowed only).
// The balloons are the real battle-mode balloons - code_80057C60.c's tick and render gates let
// them through when this reports active.

static s32 sBalloonsLive;            // the render/tick gates in code_80057C60.c read this
static u32 sBalloonHitPrev[NUM_PLAYERS]; // per-kart dramatic-state mask, for pop edges
static s32 sBalloonOutDone[NUM_PLAYERS]; // consequence fired - later wrecks don't re-punish
static s32 sBalloonEndTimer;         // counting down to the forced match end after a decisive elimination
static s32 sBalloonEndFired;         // the end fired - survivors force-finish until the engine freezes them
static s32 sBalloonWinner = -1;      // the elimination match's winner, for the OBVIOUS banner

int race_mods_balloons_active(void) {
    return sBalloonsLive;
}

// The one-winner match banner (race_mods_draw_status_hud renders it): NULL until a match is
// decided, then "YOU WIN" or "<CHARACTER> WINS" for the rest of the race. Covers the balloon
// elimination winner and the treasure hunt's finder - the place HUD hides (balloons) or lies
// about who mattered (treasure), so without the banner the ending read as nothing at all. (Tag is
// a one-LOSER mode - it has its own race_mods_tag_loser_text banner instead.)
// Character names for the HUD banners, indexed by characterId (the winner/loser banners and the
// tag IS IT line all read it).
static const char* const kCharNames[8] = { "MARIO", "LUIGI",  "YOSHI", "TOAD",
                                           "DK",    "WARIO",  "PEACH", "BOWSER" };

const char* race_mods_match_winner_text(void) {
    static char sBuf[24];
    s32 mode = CVarGetInteger("gRaceMode", 0);
    s32 w = -1;
    if (mode == 2 && CVarGetInteger("gBalloonOut", 0) == 1) {
        w = sBalloonWinner;
    } else if (mode == 4) {
        w = sTreasureWinner;
    }
    if (w < 0) {
        return NULL;
    }
    if (gPlayers[w].type & PLAYER_HUMAN) {
        return "YOU WIN";
    }
    sprintf(sBuf, "%s WINS", kCharNames[gPlayers[w].characterId & 7]);
    return sBuf;
}

// Tag is a one-LOSER mode: the single kart that spent the most time IT loses, everyone else
// survives. This is the finish banner (race_mods_draw_status_hud renders it flashing red): NULL
// until the loser is frozen at the human's finish, then "YOU LOSE" or "<CHARACTER> LOSES".
const char* race_mods_tag_loser_text(void) {
    static char sBuf[24];
    if (CVarGetInteger("gRaceMode", 0) != 3 || sTagLoser < 0) {
        return NULL;
    }
    if (gPlayers[sTagLoser].type & PLAYER_HUMAN) {
        return "YOU LOSE";
    }
    sprintf(sBuf, "%s LOSES", kCharNames[gPlayers[sTagLoser].characterId & 7]);
    return sBuf;
}

// True while a balloon-ELIMINATION race is live. There is only one winner in a last-one-standing
// match, so the HUD's place displays (the big position number, the rank portrait ladder) hide.
int race_mods_balloon_elim_active(void) {
    return sBalloonsLive && CVarGetInteger("gBalloonOut", 0) == 1;
}

// Balloon HUD feed (race_mods_draw_status_hud renders the column): fills seat i's row -
// "YOU 3" / "DK 2" / "TOAD OUT" - and returns 0 hidden / 1 live / 2 out (the menu colors out
// rows red). gPlayerBalloonCount holds the LAST visible balloon's index, so the displayed
// count is index + 1; -1 means the kart popped out. Despawned corpses (an eliminated CPU's
// type is zeroed) fail the EXISTS test and skip their row; an eliminated human stays present
// and reads OUT.
int race_mods_balloon_hud_row(int i, char* out, int outSize) {
    s32 n;
    const char* who;
    if (!sBalloonsLive || i < 0 || i >= NUM_PLAYERS || !player_exists(i) || out == NULL || outSize < 2) {
        return 0;
    }
    who = (gPlayers[i].type & PLAYER_HUMAN) ? "YOU" : kCharNames[gPlayers[i].characterId & 7];
    n = gPlayerBalloonCount[i] + 1;
    if (sBalloonOutDone[i] || n <= 0) {
        snprintf(out, (size_t) outSize, "%s OUT", who);
        return 2;
    }
    snprintf(out, (size_t) outSize, "%s %d", who, n);
    return 1;
}

// The engine only finishes a kart from its lap-CROSSING counter: func_8028EF28 (race_logic.c)
// walks gPlayers[].lapCount toward gLapCountByPlayerId[] one step per tick and runs the real
// finish - cinematic mode, CPU handover, race-state advance - when the follower reaches the
// total ON AN INCREMENT. Writing gPlayers[].lapCount directly just decays back toward the
// counter one step per tick and nothing ever fires (the eliminated kart kept racing). Stage the
// counter at the total and the follower one below: the very next tick runs the authentic finish.
static void race_mods_force_finish(s32 i) {
    s32 total = race_mods_total_laps();
    if (gPlayers[i].type & PLAYER_CINEMATIC_MODE) {
        return;
    }
    gLapCountByPlayerId[i] = total;
    if (gPlayers[i].lapCount < total - 1) {
        gPlayers[i].lapCount = total - 1;
    }
}

// A balloon-eliminated human is OUT: they rank below every kart still in the race and below
// every kart that finishes after them - only the despawned corpses sit underneath. The forced
// finish itself poisons the engine's projection for a frame (a lap count at the total reads as
// "about to win") and the cinematic freeze then KEEPS that slot - witnessed: Ray got eliminated
// and the results crowned him 1st. So this hold runs every tick INCLUDING after the freeze (the
// engine never re-sorts cinematic players, so a post-freeze write sticks), pushing the
// eliminated player down past every existing kart in BOTH rank-table pairs plus currentRank
// (the duplicate pair re-sorts each frame with no cinematic gate; this runs after the engine's
// pass, so the displayed state is ours).
static void balloon_eliminated_rank_hold(s32 humanIdx) {
    s32 r = gGPCurrentRaceRankByPlayerId[humanIdx];
    while (r < NUM_PLAYERS - 1) {
        s32 below = gGPCurrentRacePlayerIdByRank[r + 1];
        if (below < 0 || below >= NUM_PLAYERS || !player_exists(below)) {
            break; // only corpses and empty slots sit beneath an eliminated kart
        }
        gGPCurrentRacePlayerIdByRank[r + 1] = (s16) humanIdx;
        gGPCurrentRacePlayerIdByRank[r] = (s16) below;
        gGPCurrentRaceRankByPlayerId[humanIdx] = r + 1;
        gGPCurrentRaceRankByPlayerId[below] = r;
        gPlayers[humanIdx].currentRank = (s16) (r + 1);
        gPlayers[below].currentRank = (s16) r;
        r++;
    }
    r = gGPCurrentRaceRankByPlayerIdDup[humanIdx];
    while (r < NUM_PLAYERS - 1) {
        s32 below = D_80164378[r + 1];
        if (below < 0 || below >= NUM_PLAYERS || !player_exists(below)) {
            break;
        }
        D_80164378[r + 1] = (s16) humanIdx;
        D_80164378[r] = (s16) below;
        gGPCurrentRaceRankByPlayerIdDup[humanIdx] = r + 1;
        gGPCurrentRaceRankByPlayerIdDup[below] = r;
        r++;
    }
}

// The mirror lift: bubble a SURVIVOR up past the despawned corpses before the forced match end.
// The eliminated karts keep their frozen course progress in the rank sort, so a survivor
// trailing that point would freeze BELOW a corpse - the results would crown a kart that isn't
// even in the race. Only corpse slots are passed: fellow racers and frozen finishers stand.
static void balloon_rank_lift(s32 idx) {
    s32 r = gGPCurrentRaceRankByPlayerId[idx];
    if (gPlayers[idx].type & PLAYER_CINEMATIC_MODE) {
        return; // frozen - the slot is final
    }
    while (r > 0) {
        s32 above = gGPCurrentRacePlayerIdByRank[r - 1];
        if (above < 0 || above >= NUM_PLAYERS || player_exists(above)) {
            break; // a fellow racer or a frozen finisher - their slot stands
        }
        gGPCurrentRacePlayerIdByRank[r - 1] = (s16) idx;
        gGPCurrentRacePlayerIdByRank[r] = (s16) above;
        gGPCurrentRaceRankByPlayerId[idx] = r - 1;
        gGPCurrentRaceRankByPlayerId[above] = r;
        r--;
    }
}

// The same wrecked-state test the flip cam uses, for any kart: spins, tumbles, lightning, squish,
// spinout, or a Lakitu rescue after going out of bounds.
static u32 balloon_hit_state(Player* p) {
    u32 hit = (p->effects & (0x80 | 0x40 | 0x400 | 0x1000000 | 0x2000000 | 0x40000000)) != 0;
    hit = hit || ((p->effects & 0x4000000) != 0 && (p->unk_046 & 0x80) != 0);
    hit = hit || (p->kartProps & 0x4000) != 0;
    hit = hit || (p->lakituProps & 0x3) != 0;
    return hit;
}

// The duel grid: you take pole, the rival lines up beside you on the front row, and the karts
// about to be removed fill the rows behind (each slot stays unique so nothing spawns overlapped).
void race_mods_duel_grid_remap(int rivalIdx, short* slots) {
    s32 next = 2;
    s32 i;
    if (rivalIdx <= 0) {
        return;
    }
    slots[0] = 0;
    slots[rivalIdx] = 1;
    for (i = 1; i < NUM_PLAYERS; i++) {
        if (i != rivalIdx) {
            slots[i] = (s16) next++;
        }
    }
}

// Remove everyone but the human and the rival right after the eight GP karts spawn. The culled
// karts lose their whole type word - every system from the HUD to the ranking skips them - and
// sink far below the track so nothing ever renders them.
void race_mods_duel_spawn_filter(int rivalIdx) {
    s32 i;
    if (rivalIdx <= 0) {
        return;
    }
    for (i = 1; i < NUM_PLAYERS; i++) {
        if (i == rivalIdx) {
            continue;
        }
        gPlayers[i].type = 0;
        gPlayers[i].pos[1] = -30000.0f;
    }
    sDuelSpawnFiltered = 1;
}

// --- infected (gRaceMode 5) -----------------------------------------------------------------------
// One carrier seeds the outbreak (any seat - the human included); touching a survivor converts
// them. Carriers ride a permanent star - rainbow flicker, contact knockdown, item immunity - plus
// a small top-speed bonus, and carrier CPUs abandon the racing line to HUNT the nearest survivor
// (code_80005FD0.c reads race_mods_infected_hunt_target). There are no places and carriers can
// never finish: the match ends only when every clean kart has either crossed the line (survivors
// win) or been converted (the infection wins). A survivor riding a Boo is untouchable - the
// outbreak can't see a ghost. Community-pitched mode.

#define INFECTED_TOUCH_RADIUS 25.0f

static s32 sInfectSeeded;
static s32 sInfectLive; // set per tick while an infected race runs (the sBalloonsLive pattern)
static s32 sInfectEndTimer;
static s32 sInfectEndFired;
static s32 sInfectVerdict; // 0 undecided / 1 survivors won / 2 the infection won
static s32 sInfectGrace;   // opening no-conversion window so the bunched grid can't end it on lap 1

int race_mods_infected_active(void) {
    return sInfectLive;
}

// The one-winner / one-loser modes have no meaningful places: the HUD's position number, rank ladder
// and the finish placement graphic all hide while this reports true. The treasure hunt is one
// (whoever touches the box wins), and tag is another - it's decided by time-as-IT, so "8th" and the
// rank portraits are noise.
int race_mods_hide_place_hud(void) {
    return sInfectLive || sTreasureHuntLive || sTagLive || race_mods_balloon_elim_active();
}

// True while the infected mode is live and kart i carries the infection. The star-gated systems
// that must KEEP WORKING for carriers check this: the CPU item strategy (its timer resets every
// frame for starred karts, and the star-item end state waits for a flag that never clears) and
// the human mushroom trigger (stripped while starred).
int race_mods_infected_carrier(int i) {
    return i >= 0 && i < NUM_PLAYERS && sInfected[i] && CVarGetInteger("gRaceMode", 0) == 5;
}

// HUD feed (race_mods_menu.c draws it): 0 = nothing to show, 1 = the human is a carrier,
// 2 = survivors won, 3 = the infection won. The verdict outranks the carrier status.
int race_mods_infected_status(void) {
    if (CVarGetInteger("gRaceMode", 0) != 5 || !sInfectSeeded) {
        return 0;
    }
    if (sInfectVerdict != 0) {
        return sInfectVerdict + 1;
    }
    return ((gPlayers[0].type & PLAYER_HUMAN) && sInfected[0]) ? 1 : 0;
}

// The infected ladder's per-seat feed (race_mods_draw_infected_ladder, render_objects.c):
// 0 = nothing to draw / not infected mode, 1 = survivor still racing, 2 = carrier,
// 3 = a survivor that already finished (escaped the outbreak).
int race_mods_infected_marker(int i) {
    if (CVarGetInteger("gRaceMode", 0) != 5 || !sInfectSeeded) {
        return 0;
    }
    if (i < 0 || i >= NUM_PLAYERS || !player_exists(i)) {
        return 0;
    }
    if (sInfected[i]) {
        return 2;
    }
    return (gPlayers[i].type & PLAYER_CINEMATIC_MODE) ? 3 : 1;
}

// True when playerId crossed the line on the LOSING side of a one-winner match: someone else
// claimed the treasure, or the survivors held the match while playerId carried the infection.
// The finish fanfare and the character's finish voice line read this (audio/external.c) - your
// kart must not cheer "yippee" while a CPU drives off with the prize.
int race_mods_finish_demoted(int playerId) {
    s32 mode = CVarGetInteger("gRaceMode", 0);
    if (playerId < 0 || playerId >= NUM_PLAYERS) {
        return 0;
    }
    // A culled 1P duel has exactly one place that counts: pole. The runner-up of a two-kart
    // race must not ride the stock top-four celebration - 2nd of 2 IS the loss, in every mode.
    if (sDuelSpawnFiltered && gGPCurrentRaceRankByPlayerId[playerId] != 0) {
        return 1;
    }
    if (mode == 2) {
        // Popping out is a loss in every balloon variant (shrunk, slowed, eliminated alike) -
        // a balloon-out kart that limps across the line doesn't get to cheer about it...
        if (sBalloonOutDone[playerId]) {
            return 1;
        }
        // ...and an elimination match has ONE winner: once the match is decided, every other
        // kart that crosses the line crossed it on the losing side.
        if (CVarGetInteger("gBalloonOut", 0) == 1 && sBalloonWinner >= 0 && playerId != sBalloonWinner) {
            return 1;
        }
    }
    if (mode == 4) {
        return sTreasureWinner >= 0 && playerId != sTreasureWinner;
    }
    if (mode == 3) {
        // Tag is decided by time-as-IT, not finishing place. There is exactly ONE loser - the kart
        // that spent the longest IT - and only they get the losing finish, whatever place they took.
        return sTagLoser >= 0 && playerId == sTagLoser;
    }
    if (mode == 5) {
        return sInfectVerdict == 1 && sInfected[playerId]; // the outbreak lost - carriers don't cheer
    }
    return 0;
}

// The mirror of race_mods_finish_demoted: true when playerId should read as the winner's celebration
// regardless of the rank slot the table froze them in. Tag has one loser and everyone else survives,
// so every NON-loser cheers even from a low finishing place. (Balloon/treasure winners are already
// bubbled to the front, so only tag needs this.)
int race_mods_finish_promoted(int playerId) {
    s32 mode = CVarGetInteger("gRaceMode", 0);
    if (playerId < 0 || playerId >= NUM_PLAYERS) {
        return 0;
    }
    if (mode == 3) {
        return sTagLoser >= 0 && playerId != sTagLoser;
    }
    return 0;
}

// A carrier used a STAR item (effects.c trigger_star reports every star here; non-carriers
// no-op). The permanent plague star already grants the physics perks, so the item's value is a
// real hunting surge: a temporary top-speed spike for the star's own window, on top of the
// carrier bonus. The rainbow replays automatically - trigger_star stamps the start time and the
// dress pass doesn't re-pin until the window runs out.
void race_mods_infected_star_used(int playerId) {
    if (!race_mods_infected_carrier(playerId) || !sInfectSeeded) {
        return;
    }
    if (sInfectStarSurge[playerId] <= 0) {
        gPlayers[playerId].topSpeed *= INFECTED_STAR_SURGE;
    }
    sInfectStarSurge[playerId] = 270; // ~9s at the 30Hz tick, the star's own duration
}

// The hunt order for a carrier CPU (code_80005FD0.c steering + throttle read this): fills the
// nearest clean kart's position and returns 1 while playerId should be hunting. NULL outs are
// allowed for pure "am I hunting" checks. Boo'd survivors are skipped - you can't chase a ghost
// you can't see - and so are finished karts. No target leaves the CPU on the racing line.
int race_mods_infected_hunt_target(int playerId, float* outX, float* outY, float* outZ) {
    f32 bestD = 0.0f;
    s32 best = -1;
    s32 j;

    if (!sInfectLive || !sInfectSeeded || sInfectEndFired || sPreRace) {
        return 0;
    }
    if (playerId < 0 || playerId >= NUM_PLAYERS || !sInfected[playerId] || !player_exists(playerId) ||
        (gPlayers[playerId].type & (PLAYER_HUMAN | PLAYER_CINEMATIC_MODE))) {
        return 0;
    }
    for (j = 0; j < NUM_PLAYERS; j++) {
        f32 dx, dy, dz, d;
        if (sInfected[j] || !player_exists(j) || (gPlayers[j].type & PLAYER_CINEMATIC_MODE) ||
            (gPlayers[j].effects & BOO_EFFECT)) {
            continue;
        }
        dx = gPlayers[j].pos[0] - gPlayers[playerId].pos[0];
        dy = gPlayers[j].pos[1] - gPlayers[playerId].pos[1];
        dz = gPlayers[j].pos[2] - gPlayers[playerId].pos[2];
        d = dx * dx + dy * dy + dz * dz;
        if (best < 0 || d < bestD) {
            best = j;
            bestD = d;
        }
    }
    if (best < 0) {
        return 0;
    }
    if (outX != NULL) {
        *outX = gPlayers[best].pos[0];
    }
    if (outY != NULL) {
        *outY = gPlayers[best].pos[1];
    }
    if (outZ != NULL) {
        *outZ = gPlayers[best].pos[2];
    }
    return 1;
}

// The hunt order for the IT CPU in tag (read via race_mods_cpu_hunt_target): the kart that's IT
// abandons the racing line and runs down the nearest taggable kart - including the human. Returns 1
// while it should be hunting (NULL outs allowed for "am I hunting" checks). Held off during the
// post-tag immunity (sTagCooldown) so a fresh IT regroups instead of instantly tagging back the kart
// that just passed it; the human IT always drives themselves; finished karts are skipped.
static int race_mods_tag_hunt_target(int playerId, float* outX, float* outY, float* outZ) {
    f32 bestD = 0.0f;
    s32 best = -1;
    s32 j;

    if (!sTagLive || sPreRace || sTagIt < 0 || sTagCooldown > 0) {
        return 0;
    }
    if (playerId != sTagIt || !player_exists(playerId) ||
        (gPlayers[playerId].type & (PLAYER_HUMAN | PLAYER_CINEMATIC_MODE))) {
        return 0;
    }
    for (j = 0; j < NUM_PLAYERS; j++) {
        f32 dx, dy, dz, d;
        if (j == sTagIt || !player_exists(j) || (gPlayers[j].type & PLAYER_CINEMATIC_MODE)) {
            continue;
        }
        dx = gPlayers[j].pos[0] - gPlayers[playerId].pos[0];
        dy = gPlayers[j].pos[1] - gPlayers[playerId].pos[1];
        dz = gPlayers[j].pos[2] - gPlayers[playerId].pos[2];
        d = dx * dx + dy * dy + dz * dz;
        if (best < 0 || d < bestD) {
            best = j;
            bestD = d;
        }
    }
    if (best < 0) {
        return 0;
    }
    if (outX != NULL) {
        *outX = gPlayers[best].pos[0];
    }
    if (outY != NULL) {
        *outY = gPlayers[best].pos[1];
    }
    if (outZ != NULL) {
        *outZ = gPlayers[best].pos[2];
    }
    return 1;
}

#define TREASURE_DIVE_RADIUS 160.0f // CPUs only break off the line if the prize is genuinely close -
                                    // a wide ring let them beeline straight to the exact spot and
                                    // end the hunt in seconds (they "knew" where it was)
#define TREASURE_DIVE_CLOSE 80.0f   // close-in: ease the throttle so the turn radius can land

// One steering authority for every CPU hunt (code_80005FD0.c's three hook sites read this):
// 1 = infected carrier chasing a survivor OR the IT kart chasing its next victim in tag (flat out),
// 2 = treasure dive (flat out), 3 = treasure dive close-in (curve throttle - full speed orbits a
// 30-unit prize forever). 0 = race the line.
// In a treasure hunt the box IS every CPU's primary objective: the lap loop carries them into
// range (laps are pinned NO LIMIT), and once the prize is in reach on roughly the same level
// they abandon the line and dive - hidden off-line boxes included. NULL outs allowed for pure
// "am I hunting" checks.
int race_mods_cpu_hunt_target(int playerId, float* outX, float* outY, float* outZ) {
    if (race_mods_infected_hunt_target(playerId, outX, outY, outZ)) {
        return 1;
    }
    if (race_mods_tag_hunt_target(playerId, outX, outY, outZ)) {
        return 1; // tag: the IT CPU chases the nearest kart to pass it on
    }
    if (CVarGetInteger("gRaceMode", 0) == 4 && sTreasureActive && !sPreRace && playerId >= 0 &&
        playerId < NUM_PLAYERS && player_exists(playerId) &&
        !(gPlayers[playerId].type & (PLAYER_HUMAN | PLAYER_CINEMATIC_MODE))) {
        f32 dx = sTreasurePos[0] - gPlayers[playerId].pos[0];
        f32 dy = sTreasurePos[1] - gPlayers[playerId].pos[1];
        f32 dz = sTreasurePos[2] - gPlayers[playerId].pos[2];
        f32 d2 = dx * dx + dz * dz;
        // The same-level gate keeps an overpass from triggering a dive at the wrong altitude.
        if (d2 < TREASURE_DIVE_RADIUS * TREASURE_DIVE_RADIUS && dy * dy < 250.0f * 250.0f) {
            if (outX != NULL) {
                *outX = sTreasurePos[0];
            }
            if (outY != NULL) {
                *outY = sTreasurePos[1];
            }
            if (outZ != NULL) {
                *outZ = sTreasurePos[2];
            }
            return (d2 < TREASURE_DIVE_CLOSE * TREASURE_DIVE_CLOSE) ? 3 : 2;
        }
    }
    return 0;
}

// Seed one random racer as the carrier, and keep every carrier dressed: the star flag re-applied
// (apply_star_effect clears it once the stamp window passes) and the rainbow stamp pinned just
// past its window - so a fresh conversion flashes the rainbow once, then the kart settles into
// the green plague tint (render_player.c func_800235AC). Runs from the intro fly-by on, so the
// carrier is identifiable BEFORE the race starts.
static void infected_dress_carriers(void) {
    s32 i;

    // EVERY seat is in the seed pool - the human included. Starting as the hunter is its own
    // arc (the flashing INFECTED warning plus the ladder tell you it's you). The seed carrier
    // skips the conversion flash (pinned immediately): they were always infected.
    if (!sInfectSeeded) {
        s32 cands[NUM_PLAYERS];
        s32 n = 0;
        for (i = 0; i < NUM_PLAYERS; i++) {
            if (player_exists(i)) {
                cands[n++] = i;
            }
        }
        if (n == 0) {
            return; // nobody spawned yet (degenerate setup)
        }
        i = cands[random_int((u16) (n - 1))];
        sInfected[i] = 1;
        gPlayers[i].topSpeed *= INFECTED_SPEED_BONUS;
        gPlayerStarEffectStartTime[i] = (s32) gCourseTimer - 9;
        sInfectSeeded = 1;
    }
    for (i = 0; i < NUM_PLAYERS; i++) {
        if (sInfected[i] && player_exists(i)) {
            gPlayers[i].effects |= STAR_EFFECT;
            if (((s32) gCourseTimer - gPlayerStarEffectStartTime[i]) > 9) {
                // Pin just past the rainbow window: the flash never replays, and the flag-clear
                // threshold in apply_star_effect (>= 10) is never reached.
                gPlayerStarEffectStartTime[i] = (s32) gCourseTimer - 9;
            }
        }
    }
}

static void infected_tick(void) {
    s32 racing = 0;
    s32 racingInfected = 0;
    s32 cleanFinished = 0;
    s32 totalLaps = race_mods_total_laps();
    s32 i, j;

    infected_dress_carriers();
    if (!sInfectSeeded) {
        return;
    }

    // A used star item's surge runs out with the star's own window.
    for (i = 0; i < NUM_PLAYERS; i++) {
        if (sInfectStarSurge[i] > 0 && --sInfectStarSurge[i] == 0) {
            gPlayers[i].topSpeed /= INFECTED_STAR_SURGE;
        }
    }

    // Opening grace: hold off ALL conversions until the field has spread off the start grid.
    // Without it, the carrier chain-converts the whole bunched pack in the first second and the
    // match is decided on lap 1 (the "I win instantly" bug). The timer only ticks while racing.
    if (sInfectGrace > 0) {
        sInfectGrace--;
    } else {
        // Spread by touch. Conversion is one-way, so no immunity window is needed - a pileup chain
        // converting several karts at once is exactly the horror it should be. A survivor riding a
        // Boo is a ghost: the outbreak can't touch what it can't see.
        for (i = 0; i < NUM_PLAYERS; i++) {
            if (!sInfected[i] || !player_exists(i) || (gPlayers[i].type & PLAYER_CINEMATIC_MODE)) {
                continue;
            }
            for (j = 0; j < NUM_PLAYERS; j++) {
                f32 dx, dy, dz;
                if (sInfected[j] || !player_exists(j) || (gPlayers[j].type & PLAYER_CINEMATIC_MODE) ||
                    (gPlayers[j].effects & BOO_EFFECT)) {
                    continue;
                }
                dx = gPlayers[j].pos[0] - gPlayers[i].pos[0];
                dy = gPlayers[j].pos[1] - gPlayers[i].pos[1];
                dz = gPlayers[j].pos[2] - gPlayers[i].pos[2];
                if (dx * dx + dy * dy + dz * dz < INFECTED_TOUCH_RADIUS * INFECTED_TOUCH_RADIUS) {
                    sInfected[j] = 1;
                    gPlayers[j].topSpeed *= INFECTED_SPEED_BONUS;
                    gPlayerStarEffectStartTime[j] = (s32) gCourseTimer; // the conversion rainbow flash
                    play_sound2(SOUND_MENU_OK_CLICKED);
                }
            }
        }
    }

    // Carriers never finish - their race IS the hunt. The crossing counter is held one lap shy
    // of the total every tick (it only ever climbs by one per crossing, so the pin can't slip),
    // until the match end fires and race_mods_force_finish needs the counter writable again.
    if (!sInfectEndFired) {
        for (i = 0; i < NUM_PLAYERS; i++) {
            if (!sInfected[i] || !player_exists(i) || (gPlayers[i].type & PLAYER_CINEMATIC_MODE)) {
                continue;
            }
            if (gLapCountByPlayerId[i] >= totalLaps) {
                gLapCountByPlayerId[i] = totalLaps - 1;
            }
            if (gPlayers[i].lapCount >= totalLaps) {
                gPlayers[i].lapCount = totalLaps - 1;
            }
        }
    }

    // The outbreak headcount. Carriers can't finish, so a cinematic kart is always an ESCAPED
    // survivor. The carriers' star flag and tint are kept by infected_dress_carriers above.
    for (i = 0; i < NUM_PLAYERS; i++) {
        if (!player_exists(i)) {
            continue;
        }
        if (gPlayers[i].type & PLAYER_CINEMATIC_MODE) {
            if (!sInfected[i]) {
                cleanFinished++;
            }
            continue;
        }
        if (sInfected[i]) {
            racingInfected++;
        }
        racing++;
    }

    // The match is decided only when no clean kart is left ON TRACK: every survivor either
    // crossed the line or got converted. Anyone escaped = the survivors' side held; nobody
    // escaped = the infection took the whole field. No first, no last - just sides.
    if (sInfectSeeded && sInfectGrace == 0 && !sInfectEndFired && sInfectEndTimer == 0 && racing > 0 &&
        racingInfected == racing) {
        if (sInfectVerdict == 0) {
            sInfectVerdict = (cleanFinished > 0) ? 1 : 2;
        }
        sInfectEndTimer = 45; // let the last conversion read before the flag drops
    }
    if (sInfectEndTimer > 0 && --sInfectEndTimer == 0) {
        sInfectEndFired = 1;
    }
    if (sInfectEndFired) {
        for (i = 0; i < NUM_PLAYERS; i++) {
            if (player_exists(i) && !(gPlayers[i].type & PLAYER_CINEMATIC_MODE)) {
                race_mods_force_finish(i);
            }
        }
    }
}

// The tag result: the single LOSER is the kart that spent the MOST cumulative time as IT. Ties
// break to the lower seat. Existing karts only.
static s32 tag_decide_loser(void) {
    s32 worst = -1;
    s32 i;
    for (i = 0; i < NUM_PLAYERS; i++) {
        if (!player_exists(i)) {
            continue;
        }
        if (worst < 0 || sTagItTicks[i] > sTagItTicks[worst]) {
            worst = i;
        }
    }
    return worst;
}

// Tag: whoever is IT rides giant; bump anyone to pass it on. The race still runs to its normal
// finish, but the mode is really about the tag - so we clock time-as-IT and, the moment the human
// crosses the line, freeze the winner (least time IT). race_mods_match_winner_text shouts it.
static void tag_tick(void) {
    s32 i;

    if (sTagIt < 0 || !player_exists(sTagIt)) {
        s32 cands[NUM_PLAYERS];
        s32 n = 0;
        for (i = 0; i < NUM_PLAYERS; i++) {
            if (player_exists(i)) {
                cands[n++] = i;
            }
        }
        if (n == 0) {
            return;
        }
        sTagIt = cands[random_int((u16) (n - 1))];
        sTagCooldown = 60;
    }
    if (sTagCooldown > 0) {
        sTagCooldown--;
    } else {
        for (i = 0; i < NUM_PLAYERS; i++) {
            f32 dx, dy, dz;
            if (i == sTagIt || !player_exists(i)) {
                continue;
            }
            dx = gPlayers[i].pos[0] - gPlayers[sTagIt].pos[0];
            dy = gPlayers[i].pos[1] - gPlayers[sTagIt].pos[1];
            dz = gPlayers[i].pos[2] - gPlayers[sTagIt].pos[2];
            if (dx * dx + dy * dy + dz * dz < 25.0f * 25.0f) {
                s32 prevIt = sTagIt;
                sTagIt = i;
                sTagCooldown = 90; // 3s of immunity so it can't ping-pong in one pileup
                // Only chirp when YOU gain or lose IT. CPU-to-CPU handoffs were beeping the menu
                // select sound across the room every few seconds - that was the "frequent select
                // sound". A pure CPU pack now passes the tag silently.
                if ((gPlayers[i].type & PLAYER_HUMAN) || (prevIt >= 0 && (gPlayers[prevIt].type & PLAYER_HUMAN))) {
                    play_sound2(SOUND_MENU_OK_CLICKED);
                }
                break;
            }
        }
    }
    // Tag scoreboard: clock whoever is IT this tick, then freeze the LOSER the first time a human
    // crosses the finish - most total time IT loses. tag_tick only runs once racing (the pre-race
    // path returns before it), so this never counts the countdown.
    if (sTagIt >= 0 && player_exists(sTagIt)) {
        sTagItTicks[sTagIt]++;
    }
    if (sTagLoser < 0) {
        s32 totalLaps = race_mods_total_laps();
        for (i = 0; i < NUM_PLAYERS; i++) {
            if (player_exists(i) && (gPlayers[i].type & PLAYER_HUMAN) &&
                ((gPlayers[i].type & PLAYER_CINEMATIC_MODE) || gPlayers[i].lapCount >= totalLaps)) {
                sTagLoser = tag_decide_loser();
                if (sTagLoser >= 0) {
                    // Announce the reveal: a sharp sting drags the eye to the YOU LOSE / <CHAR> LOSES
                    // banner (the loser's own kart also plays its losing finish voice via the demote).
                    play_sound2(SOUND_MENU_EXPLOSION);
                }
                break;
            }
        }
    }
    // IT rides giant, and every other kart is forced back to normal size - nothing else ever
    // resets a former IT, so without this every kart that had EVER been IT stayed giant for
    // the rest of the race. Tag overrides the kart-size mod by design: race_mods_tick runs
    // enforce_kart_sizes before tag_tick, so these writes win the frame.
    for (i = 0; i < NUM_PLAYERS; i++) {
        f32 s;
        if (!player_exists(i) || sKO[i]) {
            continue; // the knockout squash keeps its own size
        }
        s = (i == sTagIt) ? 1.35f : 1.0f;
        gPlayers[i].size = s;
        gPlayers[i].boundingBoxSize = gKartBoundingBoxSizeTable[gPlayers[i].characterId] * s;
    }
}

// High-precision steering for the big speed classes. The stock stick->steering map is linear,
// tuned for 150cc - at 300cc+ a thumb's twitch is a lane change and walls collect karts. This
// bends the curve toward squared response as the effective class climbs: half deflection steers
// progressively softer (fine corrections become easy), FULL deflection always stays full lock
// (hairpins unaffected). Identity below 200cc, so stock racing never changes feel. The stick
// arrives deadzoned and clamped to +/-53 (func_80038534).
int race_mods_steering_soften(int stick) {
    f32 cc = 0.0f;
    f32 t;
    f32 curved;
    s32 mag = stick < 0 ? -stick : stick;
    if (CVarGetInteger("gSpeedTurboLaps", 0)) {
        cc = TURBO_BASE_CC + TURBO_STEP_CC * (f32) sMaxLapSeen;
    } else if (CVarGetInteger("gEnableCustomCC", 0)) {
        cc = CVarGetFloat("gCustomCC", 150.0f);
    }
    if (cc <= 200.0f || mag == 0) {
        return stick;
    }
    t = (cc - 200.0f) / 500.0f; // 200cc -> no change, 700cc and beyond -> the full curve
    if (t > 1.0f) {
        t = 1.0f;
    }
    t *= 0.65f; // even at the ceiling, keep 35% of the linear response blended in
    curved = ((f32) stick * (f32) mag) / 53.0f;
    return (s32) ((f32) stick * (1.0f - t) + curved * t);
}

// The IT seat while a tag race runs (-1 otherwise). The HUD banner and the render-side rainbow
// read this. The star FLAG is deliberately NOT reused as the marker: STAR_EFFECT also strips
// hit triggers and skips the terrain slowdown (player_controller.c), and a marker must never
// change the gameplay - so the flash is wired straight into the render color pass instead.
int race_mods_tag_it(void) {
    if (CVarGetInteger("gRaceMode", 0) != 3 || (gModeSelection != GRAND_PRIX && gModeSelection != VERSUS) ||
        gDemoMode != 0) {
        return -1;
    }
    return sTagIt;
}

// render_player.c's color pass: kart i flashes the star rainbow while it is IT.
int race_mods_tag_rainbow(int i) {
    return i >= 0 && i == race_mods_tag_it();
}

// The tag banner (race_mods_draw_status_hud renders it): "YOU ARE IT" while the human is IT -
// the menu flashes it red, the infected-warning idiom - else "<CHARACTER> IS IT" small at the
// top of the screen. NULL outside tag races (and before the first IT is seeded).
const char* race_mods_tag_banner_text(int* outHumanIsIt) {
    static char sBuf[24];
    s32 it = race_mods_tag_it();
    if (outHumanIsIt != NULL) {
        *outHumanIsIt = 0;
    }
    if (it < 0 || !player_exists(it)) {
        return NULL;
    }
    if (gPlayers[it].type & PLAYER_HUMAN) {
        if (outHumanIsIt != NULL) {
            *outHumanIsIt = 1;
        }
        return "YOU ARE IT";
    }
    sprintf(sBuf, "%s IS IT", kCharNames[gPlayers[it].characterId & 7]);
    return sBuf;
}

// Treasure hunt: the prize box can land ANYWHERE REACHABLE - on the line, out in the grass, an
// infield, behind the scenery. Each candidate is a loop waypoint pushed a random heading and
// distance, and a ground probe must land within a vertical band of the waypoint's own level:
// chasms, void, and the water far below boardwalks all miss the band (the probe returns -3000
// on nothing-found) and re-roll. The SHOWN hunt roams up to 300 units - inside the CPUs' dive
// ring, so they keep competing for it; the HIDDEN hunt roams to 600, where far finds are the
// player's edge. Falls back to the racing line itself if every probe lands in the void.
static void treasure_spawn(void) {
    TrackPathPoint* path = gTrackPaths[0];
    s32 hidden;
    s32 tries;
    s32 placed = 0;
    Vec3f hp;

    if (path == NULL || gSelectedPathCount < 16) {
        return;
    }
    hidden = CVarGetInteger("gTreasureHidden", 0);
    for (tries = 0; tries < 12 && !placed; tries++) {
        // The middle band of the loop only (15%-85% of the way around) - a box that rolls a
        // spot next to the start line is no hunt at all.
        s32 idx = gSelectedPathCount * 3 / 20 + (s32) random_int((u16) (gSelectedPathCount * 7 / 10));
        TrackPathPoint* wp = &path[idx % gSelectedPathCount];
        u16 heading = (u16) random_int((u16) 0xFFFF);
        // Always set the prize OFF the racing line (shown: 120-340 out, hidden: 60-600) so karts
        // racing the line don't just drive over it in the first seconds - finding it has to be a hunt.
        f32 dist = hidden ? (60.0f + (f32) random_int(540)) : (120.0f + (f32) random_int(220));
        f32 px = (f32) wp->x + sins(heading) * dist;
        f32 pz = (f32) wp->z + coss(heading) * dist;
        // Probe start sits LOW (+45) so a tunnel's roof stays above it - the ray then finds the
        // tunnel FLOOR (reachable, inside) instead of accepting a spot on top of the roof. The
        // ceiling of the accept band stays under the start for the same reason.
        f32 h = spawn_actor_on_surface(px, (f32) wp->y + 45.0f, pz);
        if (h > (f32) wp->y - 120.0f && h < (f32) wp->y + 40.0f) {
            sTreasurePos[0] = px;
            sTreasurePos[1] = h; // the probed ground, so the pickup ring hugs the actual box
            sTreasurePos[2] = pz;
            placed = 1;
        }
    }
    if (!placed) { // every probe hit void - the racing line itself is always reachable
        s32 idx = (s32) (gSelectedPathCount / 3) + (s32) random_int((u16) (gSelectedPathCount / 2));
        if (idx >= gSelectedPathCount) {
            idx -= gSelectedPathCount;
        }
        sTreasurePos[0] = (f32) path[idx].x;
        sTreasurePos[1] = (f32) path[idx].y;
        sTreasurePos[2] = (f32) path[idx].z;
    }
    hp[0] = sTreasurePos[0] * gTrackDirection; // pre-cancel the spawner's mirror flip
    hp[1] = sTreasurePos[1] + 4.0f;
    hp[2] = sTreasurePos[2];
    sTreasureActorIdx = spawn_treasure_item_box(hp); // -1 just loses the giant look, never the win
    sTreasureActive = 1;
}

// The minimap marker and the giant-box render both key off these.
int race_mods_treasure_world_pos(float* outX, float* outZ) {
    if (!sTreasureActive) {
        return 0;
    }
    *outX = sTreasurePos[0];
    *outZ = sTreasurePos[2];
    return 1;
}

int race_mods_treasure_actor_index(void) {
    return sTreasureActive ? sTreasureActorIdx : -1;
}

// True once the prize has been claimed (treasure hunt is over). The HUD lap counter reads this and
// hides itself: the force-finish flings every kart's lap count up to the NO-LIMIT total to trigger
// the real finish, and we don't want that jump shown as the lap counter "incrementing" at the end.
int race_mods_treasure_decided(void) {
    return sTreasureWinner >= 0;
}

// The finder IS the winner: bubble them to rank 1 in BOTH rank-table pairs plus currentRank,
// past everyone. Re-applied every tick from the claim on - including after the cinematic freeze
// (the engine never re-sorts cinematic players, so post-freeze writes stick) - so the results
// screen crowns the kart that actually found the box, not whoever led the lap chart.
static void treasure_winner_rank_hold(s32 idx) {
    s32 r = gGPCurrentRaceRankByPlayerId[idx];
    while (r > 0) {
        s32 above = gGPCurrentRacePlayerIdByRank[r - 1];
        if (above < 0 || above >= NUM_PLAYERS) {
            break;
        }
        gGPCurrentRacePlayerIdByRank[r - 1] = (s16) idx;
        gGPCurrentRacePlayerIdByRank[r] = (s16) above;
        gGPCurrentRaceRankByPlayerId[idx] = r - 1;
        gGPCurrentRaceRankByPlayerId[above] = r;
        gPlayers[idx].currentRank = (s16) (r - 1);
        gPlayers[above].currentRank = (s16) r;
        r--;
    }
    r = gGPCurrentRaceRankByPlayerIdDup[idx];
    while (r > 0) {
        s32 above = D_80164378[r - 1];
        if (above < 0 || above >= NUM_PLAYERS) {
            break;
        }
        D_80164378[r - 1] = (s16) idx;
        D_80164378[r] = (s16) above;
        gGPCurrentRaceRankByPlayerIdDup[idx] = r - 1;
        gGPCurrentRaceRankByPlayerIdDup[above] = r;
        r--;
    }
}

static void treasure_tick(void) {
    s32 i;

    if (sTreasureActive) {
        for (i = 0; i < NUM_PLAYERS; i++) {
            f32 dx, dy, dz;
            if (!player_exists(i)) {
                continue;
            }
            dx = gPlayers[i].pos[0] - sTreasurePos[0];
            dy = gPlayers[i].pos[1] - sTreasurePos[1];
            dz = gPlayers[i].pos[2] - sTreasurePos[2];
            // 2D ring with a separate vertical window: the spawner surface-snaps the box, and a
            // hidden off-line spot can sit a couple of dozen units under the recorded path height -
            // a strict 3D sphere there made the prize untouchable.
            if (dx * dx + dz * dz < 30.0f * 30.0f && dy * dy < 80.0f * 80.0f) {
                // Found it: the finder is DECLARED the winner - banner, rank 1, and the whole
                // match wraps up after a short grace so a CPU find ends your race too instead
                // of leaving everyone touring an objective that no longer exists. The finder
                // goes through the REAL finish flow. (Writing lapCount alone decays back to
                // the crossing counter and never finished anyone - see race_mods_force_finish.)
                race_mods_force_finish(i);
                sTreasureActive = 0;
                sTreasureWinner = i;
                sTreasureEndTimer = 45; // 1.5s - let the find read before the flag drops
                play_sound2(SOUND_MENU_OK_CLICKED);
                break;
            }
        }
    }
    if (sTreasureWinner >= 0) {
        treasure_winner_rank_hold(sTreasureWinner);
    }
    if (sTreasureEndTimer > 0 && --sTreasureEndTimer == 0) {
        sTreasureEndFired = 1;
    }
    if (sTreasureEndFired) {
        for (i = 0; i < NUM_PLAYERS; i++) {
            if (player_exists(i) && !(gPlayers[i].type & PLAYER_CINEMATIC_MODE)) {
                race_mods_force_finish(i);
            }
        }
    }
}

// Per-frame size/squash enforcement, after the player update so rendering and next-frame physics
// see it. Runs from the intro fly-by on, so a size mode is visible before the countdown too.
static void enforce_kart_sizes(s32 sizeMode) {
    s32 i;
    for (i = 0; i < NUM_PLAYERS; i++) {
        if (!player_exists(i)) {
            continue;
        }
        if (sKO[i]) {
            gPlayers[i].size = KO_SIZE;
            gPlayers[i].boundingBoxSize = gKartBoundingBoxSizeTable[gPlayers[i].characterId] * KO_SIZE;
        } else if (sizeMode != 0) {
            // Stable per-slot scales so RANDOM keeps each kart's size for the whole race.
            static const f32 kRandomScale[NUM_PLAYERS] = { 0.55f, 1.45f, 0.7f, 1.2f, 1.45f, 0.55f, 0.9f, 1.3f };
            f32 s = (sizeMode == 1) ? 0.55f : (sizeMode == 2) ? 1.45f : kRandomScale[i];
            gPlayers[i].size = s;
            gPlayers[i].boundingBoxSize = gKartBoundingBoxSizeTable[gPlayers[i].characterId] * s;
        }
    }
}

// The believable-hazard menu per stock course (LEVEL-THEMED mode). The picks follow each
// course's own world: moles stay on the farm, snowmen in the snow, boos where it's haunted,
// the balloon where there's sky for it. Courses outside the stock TRACKS enum (custom tracks)
// have no theme to honor and get everything - same as WILD.
static u32 hazard_course_mask(void) {
    if (CVarGetInteger("gTrackHazards", 0) != 1) {
        return HZ_ALL; // WILD - any hazard anywhere
    }
    switch (gCurrentCourseId) {
        case TRACK_MARIO_RACEWAY: // pipes and mown grass - piranha country
            return HZ_BASE | HZ_PIRANHA | HZ_BOMBKART | HZ_LAKITU | HZ_STORM | HZ_GUST | HZ_KIWANO;
        case TRACK_CHOCO_MOUNTAIN: // the falling-rock home course
            return HZ_BASE | HZ_BOULDER | HZ_LEADROCK | HZ_FOG | HZ_LAKITU | HZ_STORM | HZ_GUST | HZ_KIWANO;
        case TRACK_BOWSER_CASTLE: // thwomps' home; a chained beast and a ghost belong in a keep
            return HZ_BASE | HZ_THWOMP | HZ_CHOMP | HZ_BOO | HZ_BOMBKART;
        case TRACK_BANSHEE_BOARDWALK: // haunted water at night
            return HZ_BASE | HZ_BOO | HZ_CHEEP | HZ_FOG | HZ_BOMBKART | HZ_STORM;
        case TRACK_YOSHI_VALLEY: // hedgehogs' home, giant eggs, canyon wind
            return HZ_BASE | HZ_HOG | HZ_EGG | HZ_LEADROCK | HZ_LAKITU | HZ_STORM | HZ_GUST | HZ_KIWANO;
        case TRACK_FRAPPE_SNOWLAND: // snowmen's home; blizzard weather
            return HZ_BASE | HZ_SNOWMAN | HZ_PENGUIN | HZ_FOG | HZ_LAKITU | HZ_STORM | HZ_GUST;
        case TRACK_KOOPA_BEACH: // crabs' home, leaping fish, sea breeze
            return HZ_BASE | HZ_CRAB | HZ_CHEEP | HZ_LEADROCK | HZ_LAKITU | HZ_STORM | HZ_GUST | HZ_KIWANO;
        case TRACK_ROYAL_RACEWAY: // palace gardens and fairground sky
            return HZ_BASE | HZ_PIRANHA | HZ_BALLOON | HZ_BOMBKART | HZ_LAKITU | HZ_STORM | HZ_GUST | HZ_KIWANO;
        case TRACK_LUIGI_RACEWAY: // the hot-air balloon's home
            return HZ_BASE | HZ_BALLOON | HZ_BOMBKART | HZ_LAKITU | HZ_STORM | HZ_GUST | HZ_KIWANO;
        case TRACK_MOO_MOO_FARM: // moles' home pasture
            return HZ_BASE | HZ_MOLE | HZ_LAKITU | HZ_STORM | HZ_GUST | HZ_KIWANO;
        case TRACK_TOADS_TURNPIKE: // night traffic - rogue karts, headlight fog, truck drafts
            return HZ_BASE | HZ_BOMBKART | HZ_FOG | HZ_GUST | HZ_STORM;
        case TRACK_KALIMARI_DESERT: // desert wind, dust haze, sun-ripened kiwano
            return HZ_BASE | HZ_BOULDER | HZ_LEADROCK | HZ_FOG | HZ_LAKITU | HZ_STORM | HZ_GUST | HZ_KIWANO;
        case TRACK_SHERBET_LAND: // penguins' home; things leap out of icy water
            return HZ_BASE | HZ_PENGUIN | HZ_SNOWMAN | HZ_CHEEP | HZ_FOG | HZ_LAKITU | HZ_STORM | HZ_GUST;
        case TRACK_RAINBOW_ROAD: // the chomp's home sky - little else survives up here
            return HZ_BASE | HZ_CHOMP | HZ_LAKITU;
        case TRACK_WARIO_STADIUM: // indoor arena - machine hazards only
            return HZ_BASE | HZ_BOMBKART | HZ_LAKITU;
        case TRACK_DK_JUNGLE: // the kiwano's home jungle - river fish, mist, rockfalls
            return HZ_BASE | HZ_EGG | HZ_CHEEP | HZ_FOG | HZ_BOULDER | HZ_LEADROCK | HZ_LAKITU | HZ_STORM |
                   HZ_GUST | HZ_KIWANO;
        default: // custom / battle courses - no stock theme to honor
            return HZ_ALL;
    }
}

// Which family each wave roll lands in (see the roll table in race_mods_tick).
static u32 hazard_roll_bit(s32 roll) {
    switch (roll) {
        case 0:
        case 1:
        case 2: // the bunch arc is still bananas
            return HZ_BANANA;
        case 3:
        case 4:
            return HZ_SHELL;
        case 5:
        case 6: // the pinch gate is still piranhas
            return HZ_PIRANHA;
        case 7:
            return HZ_EGG;
        case 8:
        case 9:
            return HZ_BOULDER;
        case 10:
            return HZ_MOLE;
        case 11:
            return HZ_SNOWMAN;
        case 12:
            return HZ_CHEEP;
        case 13:
            return HZ_CRAB;
        case 14:
            return HZ_HOG;
        case 15:
            return HZ_PENGUIN;
        case 16:
            return HZ_BOMBKART;
        case 17:
            return HZ_CHOMP;
        case 18:
        case 19: // the slam gate is still thwomps
            return HZ_THWOMP;
        case 20:
        case 21: // dive-bomber and egg-lobber share the family
            return HZ_LAKITU;
        case 22:
            return HZ_BOO;
        case 23:
            return HZ_BALLOON;
        case 24:
            return HZ_FOG;
        case 25:
            return HZ_LEADROCK;
        default:
            return HZ_BOXES;
    }
}

// PROP SWAP: nonzero (the mode) only when a real GP/VERSUS race is being set up - demos and the
// credits flyby keep their stock props, and Time Trials never sees the setup screen's CVars.
// Courses gate their signature-mover spawns on this at BeginPlay.
int race_mods_prop_swap_live(void) {
    s32 m;
    if (gModeSelection != GRAND_PRIX && gModeSelection != VERSUS) {
        return 0;
    }
    if (gDemoMode != 0 || gGamestate == CREDITS_SEQUENCE) {
        return 0;
    }
    m = CVarGetInteger("gPropSwap", 0);
    return (m >= 1 && m <= 6) ? m : 0; // 1 chomps / 2 bomb karts / 3 snowmen / 4 penguins / 5 mixed / 6 themed
}

// THEMED prop swap: the replacement kind (1 chomp / 2 bomb kart / 3 snowman / 4 penguin) chosen
// to fit the course. Alternates two fitting kinds along the line on most courses.
static s32 prop_swap_themed_kind(s32 i) {
    switch (gCurrentCourseId) {
        case TRACK_FRAPPE_SNOWLAND:
        case TRACK_SHERBET_LAND: // snow and ice - snowmen and penguins
            return (i & 1) ? 4 : 3;
        case TRACK_BOWSER_CASTLE:
        case TRACK_BANSHEE_BOARDWALK: // a chained beast and roaming menace fit the keep / haunt
            return (i & 1) ? 2 : 1;
        case TRACK_RAINBOW_ROAD: // its own residents
            return 1;
        case TRACK_TOADS_TURNPIKE: // bomb karts read like the traffic they replace
            return 2;
        default: // a roaming chomp-and-bomb-kart menace anywhere else
            return (i & 1) ? 1 : 2;
    }
}

// A course that skipped its movers reports how many stood down; prop_swap_spawn sizes the
// replacement pack from it. Consumed at the staging one-shot, re-registered every course load.
void race_mods_prop_swap_register(int skippedCount) {
    sPropSwapSkipped = skippedCount;
    race_mods_diag_line("prop swap: course registered %d movers (mode=%d)", skippedCount,
                        CVarGetInteger("gPropSwap", 0));
}

// The takeover: replacements for the registered movers, spread evenly along the racing line and
// phase-shifted off the start line. Runs from the staging one-shot so the field is already set
// during the intro fly-by. MIXED rotates the menagerie per spawn point.
static void prop_swap_spawn(void) {
    s32 mode = race_mods_prop_swap_live();
    TrackPathPoint* path = gTrackPaths[0];
    s32 n = sPropSwapSkipped;
    s32 cap;
    s32 chomps = 0; // chomp slots must stay consecutive from the base - MIXED shares the budget
    s32 i;

    if (mode == 0 || n <= 0 || path == NULL || gSelectedPathCount <= 8) {
        return;
    }
    sPropSwapSkipped = 0; // consumed - the next course load registers afresh
    switch (mode) {
        case 2:  cap = PROP_SWAP_MAX_BOMBKARTS; break;
        case 3:  cap = PROP_SWAP_MAX_SNOWMEN; break;
        case 4:  cap = PROP_SWAP_MAX_PENGUINS; break;
        default: cap = PROP_SWAP_MAX_CHOMPS; break; // CHOMPS, MIXED and THEMED ride the chomp budget
    }
    if (n < 6) {
        n = 6; // even a two-train course deserves a real takeover
    }
    if (n > cap) {
        n = cap;
    }
    race_mods_diag_line("prop swap: spawning %d (mode=%d) over %d waypoints", n, mode, gSelectedPathCount);
    for (i = 0; i < n; i++) {
        s32 wpIdx = (gSelectedPathCount / (2 * n) + (i * gSelectedPathCount) / n) % gSelectedPathCount;
        TrackPathPoint* wp = &path[wpIdx];
        s32 kind = (mode == 5) ? (i & 3) + 1 : (mode == 6) ? prop_swap_themed_kind(i) : mode;
        switch (kind) {
            case 1:
                if (chomps < PROP_SWAP_MAX_CHOMPS) {
                    race_mods_spawn_chomp_at(PROP_SWAP_CHOMP_SLOT_BASE + chomps, wpIdx);
                    chomps++;
                }
                break;
            case 2:
                // Mostly waypoint patrols (traffic-shaped), with every fourth let loose to CHASE.
                race_mods_spawn_bomb_kart(wpIdx, (i % 4 == 3) ? 4 : 1);
                break;
            case 3:
                race_mods_spawn_snowman((f32) wp->x, (f32) wp->y, (f32) wp->z);
                break;
            case 4:
                race_mods_spawn_penguin((f32) wp->x, (f32) wp->y, (f32) wp->z, 80.0f + 20.0f * (f32) (i & 3));
                break;
        }
    }
}

// ----------------------------------------------------------------------------------------------
// 1P BATTLE: the main menu's BATTLE row for one player. Real battle mode end to end - the stock
// arenas, the stock balloon kit, the stock bomb-kart conversion and result screen - with CPU
// rivals injected at spawn (spawn_players_1p_battle). The arenas have no waypoint paths and zero
// out the AI path system (AIMaximumSeparation -1), so the rivals drive by line of sight: a hunt
// target from here, consumed by update_player's battle branch (code_80005FD0.c). Their offense
// is also ours - aimed hazard shells and dropped bananas on a per-rival cadence - because the
// stock CPU item strategy keys off path-traversal counters that freeze on pathless courses.
// Setup screen rows (race_mods_menu.c): gBattleBalloons / gBattleRivals / gBattleCpuHeat /
// gBattleLosers / gBattleSudden.
extern u16 gPlayerBalloonStatus[8][3]; // code_80057C60.c - 1 = present, 2 = departing
extern s32 gPlayerCountSelection1;
extern s32 gActiveScreenMode; // main.c - SCREEN_MODE_1P etc. (diag only)

static s32 sBattlePreRace;            // staging one-shot edge, same idiom as sPreRace
static s32 sBattleTimer;              // in-progress ticks, drives sudden death
static s32 sBattleSuddenFired;        // sudden death pops once
static s32 sBattleLastWarned;         // the LAST BALLOON warning fires once per match
static s32 sBattleBannerFlash;        // ticks left on the warning banner
static const char* sBattleBannerText; // what the banner shouts while the flash runs
static s32 sBattleFireTimer[NUM_PLAYERS]; // per-rival attack cadence
static s32 sBattleWinHold;                // >0: holding the result screen to show the WIN banner
static s32 sBattleWinner = -1;            // the decided winner's player id (-1 undecided)
static s32 sBattleTargetId[NUM_PLAYERS];  // the opponent this rival is hunting (-1 = on a box run)
static s32 sBattleObjTimer[NUM_PLAYERS];  // ticks until it re-chooses an objective (target / box)

// Live while a 1P battle runs: BATTLE mode picked with one player, demos out.
static s32 battle_1p_active(void) {
    return gModeSelection == BATTLE && gPlayerCountSelection1 == 1 && gDemoMode == 0;
}

int race_mods_battle_cpu_target(int playerId, float* outX, float* outY, float* outZ) {
    Player* self = &gPlayers[playerId];
    f32 bestD = 1e30f;
    s32 best = -1;
    s32 j;

    if (!battle_1p_active() || !(self->type & PLAYER_CPU) || !(self->type & PLAYER_EXISTS)) {
        return 0;
    }
    if (self->type & PLAYER_CINEMATIC_MODE) {
        return 2; // popped out - the bomb kart AI owns the kart now
    }
    for (j = 0; j < NUM_PLAYERS; j++) {
        f32 dx, dz, d;
        if (j == playerId || !(gPlayers[j].type & PLAYER_EXISTS) ||
            (gPlayers[j].type & PLAYER_CINEMATIC_MODE)) {
            continue;
        }
        dx = gPlayers[j].pos[0] - self->pos[0];
        dz = gPlayers[j].pos[2] - self->pos[2];
        d = dx * dx + dz * dz;
        if (d < bestD) {
            bestD = d;
            best = j;
        }
    }
    if (best < 0) {
        return 2; // nobody left to hunt - coast out the match
    }
    if (outX != NULL && outY != NULL && outZ != NULL) {
        // Aim straight at the prey - the steering layer (race_mods_battle_steer) handles veering
        // around ledges, and the attack aim wants the true position, so no weave here.
        *outX = gPlayers[best].pos[0];
        *outY = gPlayers[best].pos[1];
        *outZ = gPlayers[best].pos[2];
    }
    return 1;
}

float race_mods_battle_cpu_heat(void) {
    switch (CVarGetInteger("gBattleCpuHeat", 1)) {
        case 0:
            return 0.8f; // CALM
        case 2:
            return 1.2f; // RABID
        default:
            return 1.0f; // MEAN
    }
}

// True when solid drivable ground sits at (x, z). The arena's ledges, inner holes and gaps read as
// no-ground: spawn_actor_on_surface returns a far-off value there (-3000 = nothing below / off the
// arena, +3000 = a cell with no triangles), and a real floor returns a height near the kart's. So
// a surface within a sane vertical band of the kart = safe to drive onto; void / a killer drop are
// edges. (A modest drop to a lower deck is allowed - that is drivable.)
static s32 battle_ground_safe(Player* self, f32 x, f32 z) {
    f32 gh = spawn_actor_on_surface(x, self->pos[1] + 90.0f, z);
    return gh > -2999.0f && gh < 2999.0f && gh > self->pos[1] - 300.0f && gh < self->pos[1] + 300.0f;
}

void race_mods_battle_steer(int playerId, float oppX, float oppZ, float* outX, float* outZ, int* outSlow) {
    Player* self = &gPlayers[playerId];
    f32 dx = oppX - self->pos[0];
    f32 dz = oppZ - self->pos[2];
    f32 d = sqrtf(dx * dx + dz * dz);
    f32 dirX, dirZ;
    const f32 probe = 175.0f; // look this far ahead for a ledge - far enough to turn away in time
    // Fan of headings, widening from straight-at-the-target out to a full about-face. The first
    // one with safe ground wins, so a rival keeps aiming where it wants and only veers as much as
    // the ledge forces it to.
    static const s16 kFan[] = { 0,      0x0E39,  -0x0E39, 0x1C71, -0x1C71, 0x2AAA,
                                -0x2AAA, 0x4000, -0x4000, 0x5C71, -0x5C71, 0x7FFF };
    s32 n = (s32) (sizeof(kFan) / sizeof(kFan[0]));
    s32 a;

    if (outSlow != NULL) {
        *outSlow = 0;
    }
    if (d > 0.01f) {
        dirX = dx / d;
        dirZ = dz / d;
    } else {
        dirX = sins(self->rotation[1]);
        dirZ = coss(self->rotation[1]);
    }
    for (a = 0; a < n; a++) {
        f32 c = coss((u16) kFan[a]);
        f32 s = sins((u16) kFan[a]);
        f32 px = dirX * c - dirZ * s; // rotate the aim by the fan angle
        f32 pz = dirX * s + dirZ * c;
        if (battle_ground_safe(self, self->pos[0] + px * probe, self->pos[2] + pz * probe)) {
            *outX = self->pos[0] + px * probe;
            *outZ = self->pos[2] + pz * probe;
            return;
        }
    }
    // Boxed in by ledges on every heading - back away from the drop, toward the ground we came
    // from (which IS safe). NEVER toward the pack centroid: on a ring (Big Donut) or a split arena
    // (Block Fort) the centre of the pack is the hole in the middle - aiming there is the dive.
    *outX = self->pos[0] - dirX * probe;
    *outZ = self->pos[2] - dirZ * probe;
    if (outSlow != NULL) {
        *outSlow = 1;
    }
}

// A battle rival's OBJECTIVE: who or what it is driving at right now. It commits to one for a few
// seconds, then re-chooses, so the field stays a free-for-all instead of dogpiling one kart. Most
// of the time it HUNTS an opponent (picked with spread, not always the nearest, so the human is not
// ganged); now and then it peels off to grab a box. Closing the distance IS the aggression - the
// attack pass (battle_cpu_attack) shells whoever it gets near, which is what "target each other and
// me" means.
static void battle_choose_objective(s32 playerId) {
    Player* self = &gPlayers[playerId];
    f32 bx, bz;
    if (random_int(3) == 0 && find_nearest_item_box(self->pos[0], self->pos[2], &bx, &bz)) {
        sBattleTargetId[playerId] = -1; // box run - the live box position is resolved in roam_target
    } else {
        s32 cand[NUM_PLAYERS];
        s32 nc = 0;
        s32 j;
        for (j = 0; j < NUM_PLAYERS; j++) {
            if (j == playerId || !(gPlayers[j].type & PLAYER_EXISTS) ||
                (gPlayers[j].type & PLAYER_CINEMATIC_MODE)) {
                continue;
            }
            cand[nc++] = j;
        }
        // Random among everyone still in (humans included) - spreads the aggression so no single
        // kart gets chain-rammed, which was the old "driving into me" failure.
        sBattleTargetId[playerId] = (nc > 0) ? cand[random_int((u16) (nc - 1))] : -1;
    }
    sBattleObjTimer[playerId] = 120 + (s32) random_int(120); // commit 4-8s before re-choosing
}

// Where a battle rival DRIVES. When hunting, it aims for a point at HARASS RANGE off its prey (not
// the prey's centre) so it closes to attack distance and circles there rather than plowing through -
// the attack pass does the shooting. On a box run (or once its prey pops out) it heads for the
// nearest box, then the nearest kart as a fallback. The steer layer edge-avoids the actual path.
void race_mods_battle_roam_target(int playerId, float* outX, float* outZ) {
    Player* self = &gPlayers[playerId];
    s32 tgt;
    if (sBattleObjTimer[playerId] <= 0) {
        battle_choose_objective(playerId);
    }
    sBattleObjTimer[playerId]--;
    tgt = sBattleTargetId[playerId];
    if (tgt >= 0 && (gPlayers[tgt].type & PLAYER_EXISTS) && !(gPlayers[tgt].type & PLAYER_CINEMATIC_MODE)) {
        // Aim STRAIGHT at the prey - a stable target that drifts smoothly as the prey drives, so the
        // steering tracks it cleanly. (The old "aim 120 off the prey toward us" point whipped around
        // the prey as they closed in, which is what made them spin and twitch frantically.)
        *outX = gPlayers[tgt].pos[0];
        *outZ = gPlayers[tgt].pos[2];
        return;
    }
    {
        f32 oy;
        if (find_nearest_item_box(self->pos[0], self->pos[2], outX, outZ)) {
            return; // box run, or prey gone - grab a weapon
        }
        if (race_mods_battle_cpu_target(playerId, outX, &oy, outZ) == 1) {
            return; // no box up - close on the nearest kart
        }
        *outX = self->pos[0]; // nothing to chase - hold and let the steer/anti-stuck layer sort it
        *outZ = self->pos[2];
    }
}

// battle_cpu_drive calls this when a rival has wedged - drop its objective so the next tick chooses a
// fresh one (often a different target) instead of grinding into whatever held it.
void race_mods_battle_roam_repick(int playerId) {
    sBattleObjTimer[playerId] = 0;
}

const char* race_mods_battle_banner(void) {
    if (!battle_1p_active() || sBattleBannerFlash <= 0) {
        return NULL;
    }
    return sBattleBannerText;
}

// update_player_battle_status (race_logic.c) calls this the moment a 1P battle is decided. It holds
// the result screen for a few seconds (returns 1 = keep going) so the WIN banner is unmissable,
// then releases it (returns 0 = let the stock result fire). Multiplayer battle is unaffected.
int race_mods_battle_hold_winner(int winnerId) {
    if (!battle_1p_active()) {
        return 0; // stock multiplayer battle ends normally
    }
    if (sBattleWinner < 0) {
        sBattleWinner = (winnerId >= 0 && winnerId < NUM_PLAYERS) ? winnerId : 0; // capture once
        sBattleWinHold = 130; // ~4.3s of victory banner before the result screen
    }
    if (sBattleWinHold > 0) {
        sBattleWinHold--;
        return 1; // keep holding the match open so the banner shows
    }
    return 0; // grace over - let the result screen take it
}

// The big WIN banner: "YOU WIN" when the human (seat 0) took it, "<CHARACTER> WINS" otherwise.
// NULL until the match is decided. race_mods_draw_status_hud renders it large and centred.
const char* race_mods_battle_winner_text(void) {
    static char sWinBuf[24];
    if (!battle_1p_active() || sBattleWinner < 0) {
        return NULL;
    }
    if (sBattleWinner == 0) {
        return "YOU WIN";
    }
    sprintf(sWinBuf, "%s WINS", kCharNames[gPlayers[sBattleWinner].characterId & 7]);
    return sWinBuf;
}

// One rival attack: a green shell aimed down the target's throat (with a touch of velocity
// lead), or a banana minefield dropped in the wake. Shells ride the stock hazard actor, so
// they bounce off the arena walls and pop balloons through the normal wreck chain.
static void battle_cpu_attack(s32 i) {
    f32 huntPos[3];
    Player* self = &gPlayers[i];
    f32 dx, dz, len;

    if (race_mods_battle_cpu_target(i, &huntPos[0], &huntPos[1], &huntPos[2]) != 1) {
        return;
    }
    dx = huntPos[0] - self->pos[0];
    dz = huntPos[2] - self->pos[2];
    len = sqrtf(dx * dx + dz * dz);
    if (len < 30.0f || len > 1300.0f) {
        return; // point blank pops the thrower too; across the arena is a wasted slot
    }
    if (random_int(9) < 7) {
        dx /= len;
        dz /= len;
        // Spawn clear of the thrower's own nose so the shell doesn't pop its own balloon.
        spawn_hazard_shell(self->pos[0] + dx * 34.0f, self->pos[1], self->pos[2] + dz * 34.0f, dx, dz);
    } else {
        spawn_hazard_banana(self->pos[0], self->pos[1], self->pos[2]);
    }
}

// The 1P battle tick: balloon-count trim during staging, then sudden death, the rival attack
// cadence, the GHOST loser option and the kart-size mod while the match runs. The stock battle
// systems own everything else (pops, bomb-kart conversion, the win check in race_logic.c).
static void race_mods_battle_tick(void) {
    s32 i;

    if (!battle_1p_active()) {
        sBattlePreRace = 0;
        return;
    }

    if (gRaceState < 3) {
        s32 target = CVarGetInteger("gBattleBalloons", 3);
        if (target < 1) {
            target = 1;
        }
        if (target > 3) {
            target = 3;
        }
        // Trim the stock three-balloon loadout down to the BALLOONS row pick, every staging
        // frame (spawn re-inits them). Count is highest LIVE index: 3 balloons = 2.
        for (i = 0; i < NUM_PLAYERS; i++) {
            s32 k;
            if (!(gPlayers[i].type & PLAYER_EXISTS)) {
                continue;
            }
            if (gPlayerBalloonCount[i] > target - 1) {
                gPlayerBalloonCount[i] = target - 1;
            }
            for (k = target; k < 3; k++) {
                gPlayerBalloonStatus[i][k] = 0;
            }
        }
        if (!sBattlePreRace) {
            race_mods_diag_line("battle 1p: balloons=%d rivals=%d heat=%d losers=%d sudden=%d", target,
                                CVarGetInteger("gBattleRivals", 3), CVarGetInteger("gBattleCpuHeat", 1),
                                CVarGetInteger("gBattleLosers", 0), CVarGetInteger("gBattleSudden", 0));
        }
        sBattlePreRace = 1;
        sBattleTimer = 0;
        sBattleSuddenFired = 0;
        sBattleBannerFlash = 0;
        sBattleLastWarned = 0;
        sBattleBannerText = NULL;
        sBattleWinHold = 0;
        sBattleWinner = -1;
        for (i = 0; i < NUM_PLAYERS; i++) {
            // Staggered first volley 6-9s after the green light, one rival at a time.
            sBattleFireTimer[i] = 180 + i * 45;
            sBattleObjTimer[i] = 0;  // choose a fresh objective on the first drive tick
            sBattleTargetId[i] = -1; // start untargeted
        }
        return;
    }
    if (gRaceState != 3) {
        return; // the result flow owns everything after RACE_IN_PROGRESS
    }
    sBattlePreRace = 0;
    sBattleTimer++;
    if (sBattleBannerFlash > 0) {
        sBattleBannerFlash--;
    }

    // SUDDEN DEATH: at the 2 or 3 minute mark every kart drops to its last balloon.
    {
        s32 sudden = CVarGetInteger("gBattleSudden", 0);
        s32 threshold = (sudden == 1) ? 3600 : (sudden == 2) ? 5400 : 0;
        if (threshold > 0 && !sBattleSuddenFired && sBattleTimer >= threshold) {
            sBattleSuddenFired = 1;
            sBattleBannerFlash = 150;
            sBattleBannerText = "SUDDEN DEATH";
            for (i = 0; i < NUM_PLAYERS; i++) {
                if (!(gPlayers[i].type & PLAYER_EXISTS) || (gPlayers[i].type & PLAYER_CINEMATIC_MODE)) {
                    continue;
                }
                while (gPlayerBalloonCount[i] > 0) {
                    pop_player_balloon(&gPlayers[i], (s8) i);
                }
            }
        }
    }

    // LAST BALLOON: a one-shot warning flash when the human is down to the final balloon.
    if (!sBattleLastWarned && !sBattleSuddenFired && (gPlayers[0].type & PLAYER_EXISTS) &&
        !(gPlayers[0].type & PLAYER_CINEMATIC_MODE) && gPlayerBalloonCount[0] == 0) {
        sBattleLastWarned = 1;
        sBattleBannerFlash = 120;
        sBattleBannerText = "LAST BALLOON";
    }

    // Rival offense on a per-kart cadence, scaled by CPU HEAT.
    for (i = 0; i < NUM_PLAYERS; i++) {
        if (!(gPlayers[i].type & PLAYER_CPU) || !(gPlayers[i].type & PLAYER_EXISTS) ||
            (gPlayers[i].type & PLAYER_CINEMATIC_MODE)) {
            continue;
        }
        if (--sBattleFireTimer[i] <= 0) {
            s32 heat = CVarGetInteger("gBattleCpuHeat", 1);
            s32 base = (heat == 0) ? 330 : (heat == 2) ? 150 : 240;
            battle_cpu_attack(i);
            sBattleFireTimer[i] = base + random_int(120);
        }
    }
    // GHOST losers: a rival that pops out vanishes instead of staying as a bomb kart (the
    // balloon-elimination despawn idiom). The human always keeps the bomb-kart revenge lap.
    if (CVarGetInteger("gBattleLosers", 0) == 1) {
        for (i = 1; i < NUM_PLAYERS; i++) {
            if ((gPlayers[i].type & PLAYER_CPU) && (gPlayers[i].type & PLAYER_CINEMATIC_MODE)) {
                gPlayers[i].type = 0;
                gPlayers[i].pos[1] = -30000.0f;
            }
        }
    }

    // The KART SIZE row works in the arena too (tiny battle is a delight).
    enforce_kart_sizes(CVarGetInteger("gKartSize", 0));
}

// Hazards spawn FAR AHEAD of the human down the track (40-110 waypoints), not at a random spot
// that can land right beside you - so they appear in the distance and you drive INTO them rather
// than having them pop in on top of you. No human (attract demo) -> ahead of the leader.
static s32 hazard_far_waypoint(void) {
    s32 ref = -1;
    s32 i;
    for (i = 0; i < NUM_PLAYERS; i++) {
        if (player_exists(i) && (gPlayers[i].type & PLAYER_HUMAN)) {
            ref = i;
            break;
        }
    }
    if (ref < 0) {
        ref = gGPCurrentRacePlayerIdByRank[0]; // attract demo - place ahead of the leader
    }
    if (ref < 0 || ref >= NUM_PLAYERS) {
        return (s32) random_int((u16) (gSelectedPathCount - 1)); // last-ditch fallback
    }
    return ((s32) gNearestPathPointByPlayerId[ref] + 40 + (s32) random_int(70)) % gSelectedPathCount;
}

void race_mods_tick(void) {
    s32 raceMode = CVarGetInteger("gRaceMode", 0); // 0 normal / 1 knockout / 2 balloons / 3 tag / 4 treasure / 5 infected
    s32 turbo = CVarGetInteger("gSpeedTurboLaps", 0);
    s32 sizeMode = CVarGetInteger("gKartSize", 0);
    s32 maxLap = 0;
    s32 i;

    sBalloonsLive = 0;
    sInfectLive = 0;
    sTreasureHuntLive = 0;
    race_mods_battle_tick(); // 1P battle has its own lean tick - the race machinery below is GP/VERSUS only

    // --- BATTLE AI DIAGNOSTIC (temporary) -------------------------------------------------
    // One throttled snapshot every ~30 frames whenever a battle is on screen, BEFORE the GP/VERSUS
    // early-return below, so it fires even when the 1P gate is the thing that's broken. Reports the
    // gate inputs (mode/pcount/demo/1pActive) and, per kart, its type flags + position + the floor
    // probe at its own spot - so we can SEE which boundary fails instead of guessing.
    if (gModeSelection == BATTLE) {
        static s32 sBDiagFrame = 0;
        static s32 sBDiagBudget = 90;
        static s32 sBDiagInit = 0;
        if (!sBDiagInit) {
            FILE* tf = fopen("racemods_diag.txt", "w"); // fresh capture each launch
            if (tf != NULL) {
                fclose(tf);
            }
            sBDiagInit = 1;
        }
        if (((sBDiagFrame++ % 30) == 0) && (sBDiagBudget-- > 0)) {
            s32 d;
            race_mods_diag_line("[bdiag] state=%d mode=%d pcount=%d demo=%d 1pActive=%d", gRaceState,
                                gModeSelection, gPlayerCountSelection1, gDemoMode, battle_1p_active());
            for (d = 0; d < NUM_PLAYERS; d++) {
                Player* p = &gPlayers[d];
                f32 gh = spawn_actor_on_surface(p->pos[0], p->pos[1] + 90.0f, p->pos[2]);
                f32 vel = sqrtf(p->velocity[0] * p->velocity[0] + p->velocity[2] * p->velocity[2]);
                race_mods_diag_line(
                    "  id=%d type=%04X E=%d C=%d H=%d X=%d pos=(%.0f,%.0f,%.0f) gh=%.0f vel=%.1f", d,
                    (u32) p->type, (p->type & PLAYER_EXISTS) != 0, (p->type & PLAYER_CPU) != 0,
                    (p->type & PLAYER_HUMAN) != 0, (p->type & PLAYER_CINEMATIC_MODE) != 0, p->pos[0],
                    p->pos[1], p->pos[2], gh, vel);
            }
        }
    }
    // --------------------------------------------------------------------------------------

    if (gModeSelection != GRAND_PRIX && gModeSelection != VERSUS) {
        return;
    }
    if (gDemoMode != 0) {
        return; // attract demos race VERSUS/GP with whatever CVars linger - never mutate them
    }
    sBalloonsLive = (raceMode == 2);   // the balloon tick/render gates in code_80057C60.c read this
    sInfectLive = (raceMode == 5);     // the place-HUD hide + the GP always-advance read this
    sTagLive = (raceMode == 3);        // the place-HUD hide reads this - tag is decided by IT time
    sTreasureHuntLive = (raceMode == 4); // the place-HUD hide reads this - finding the box is the
                                         // only result, so 8th place is noise

    if (gRaceState < 3) { // intro / staging / countdown: reset mutator state, but keep the looks live
        for (i = 0; i < NUM_PLAYERS; i++) {
            sKO[i] = 0;
            sBalloonHitPrev[i] = 0;
            sBalloonOutDone[i] = 0;
            sSlow[i] = 0;
        }
        sBalloonEndTimer = 0;
        sBalloonEndFired = 0;
        sBalloonWinner = -1;
        if (!sPreRace) {
            // One-shot on entering pre-race (restart / next GP course). The infected state resets
            // HERE, not per pre-race frame - the carrier seeds DURING the intro so the player can
            // study the field before the green light, and a per-frame reset would wipe it.
            for (i = 0; i < NUM_PLAYERS; i++) {
                sInfected[i] = 0;
                sInfectStarSurge[i] = 0;
            }
            sInfectSeeded = 0;
            sInfectEndTimer = 0;
            sInfectEndFired = 0;
            sInfectVerdict = 0;
            sInfectGrace = 150; // ~5s after the green light before the infection can spread/decide
            // Tag IT resets HERE (one-shot on entering pre-race), NOT per pre-race frame, so the
            // staging seed below survives the countdown and the "<X> IS IT" banner shows from the
            // start (a per-frame reset would wipe the seed every frame, like infected above).
            sTagIt = -1;
            sTagCooldown = 0;
            sTagLoser = -1;
            for (i = 0; i < NUM_PLAYERS; i++) {
                sTagItTicks[i] = 0;
            }
        }
        if (!sPreRace) { // diag, fires once per race: the mod values the race actually starts with
            FILE* df = fopen("racemods_diag.txt", "a");
            if (df != NULL) {
                fprintf(df,
                        "race start: modeSel=%d itemMode=%d rain=%d hazards=%d mode=%d laps=%d size=%d "
                        "turbo=%d versusDuel=%d duelRival=%d propSwap=%d swapMovers=%d\n",
                        gModeSelection, CVarGetInteger("gItemMode", 0), CVarGetInteger("gItemRain", 0),
                        CVarGetInteger("gTrackHazards", 0), raceMode, race_mods_total_laps(),
                        CVarGetInteger("gKartSize", 0), CVarGetInteger("gSpeedTurboLaps", 0),
                        CVarGetInteger("gVersusDuel", 0), CVarGetInteger("gDuelRival", 0),
                        CVarGetInteger("gPropSwap", 0), sPropSwapSkipped);
                fclose(df);
            }
            // PROP SWAP: the takeover pack spawns once per race, as staging begins, so the
            // replacements are already prowling during the intro fly-by.
            prop_swap_spawn();
        }
        sMaxLapSeen = 0;
        sPreRace = 1;
        sRainTimer = 0;
        sRainDrops = 0;
        sHazardTimer = HAZARD_PERIOD - 120; // first wave lands 4s after the green light
        sHazardWaves = 0;
        sHazardChomps = 0;
        sHazardThwomps = 0;
        sHazardLakitus = 0;
        sHazardBoos = 0;
        sHazardBombers = 0;
        sHazardFog = 0;
        sHazardBombKarts = 0;
        sHazardMoles = 0;
        sHazardSnowmen = 0;
        sHazardCheeps = 0;
        sHazardCrabs = 0;
        sHazardHogs = 0;
        sHazardPenguinWaves = 0;
        sLeaderStrikeTicks = 0;
        sGustTimer = 0;
        sGustNext = 1200 + (s32) random_int(600); // the first gust arrives 40-60s in
        sGustTicks = 0;
        for (i = 0; i < NUM_PLAYERS; i++) {
            sOffroadTicks[i] = 0;
            sKiwanoCooldown[i] = 0;
            sCpuStuckTicks[i] = 0;
            sCpuRescueCool[i] = 0;
            sCpuLastX[i] = gPlayers[i].pos[0];
            sCpuLastZ[i] = gPlayers[i].pos[2];
        }
        sTreasureActive = 0;
        sTreasureActorIdx = -1;
        sTreasureWinner = -1;
        sTreasureEndTimer = 0;
        sTreasureEndFired = 0;
        if (sDuelSpawnFiltered) {
            race_mods_duel_spawn_filter(1); // keep the duel cull pinned through every pre-race init
        }
        if (raceMode == 2) {
            // Balloons up from the intro fly-by on: re-initialized every pre-race frame so they
            // track the staging drive-up, then the battle tick maintains them once racing.
            for (i = 0; i < NUM_PLAYERS; i++) {
                if (player_exists(i)) {
                    init_all_player_balloons(&gPlayers[i], (s8) i);
                }
            }
        }
        if (raceMode == 5) {
            infected_dress_carriers(); // seed + green tint from the intro fly-by on
        }
        if (raceMode == 3 && sTagIt < 0) {
            // Seed IT during staging so "<X> IS IT" shows from the countdown (the field is already
            // set). The reset moved to the one-shot above, so this seed persists; tag_tick takes
            // over the bump/handoff once racing begins.
            s32 cands[NUM_PLAYERS];
            s32 n = 0;
            s32 c;
            for (c = 0; c < NUM_PLAYERS; c++) {
                if (player_exists(c)) {
                    cands[n++] = c;
                }
            }
            if (n > 0) {
                sTagIt = cands[random_int((u16) (n - 1))];
                sTagCooldown = 60;
            }
        }
        enforce_kart_sizes(sizeMode);
        return;
    }
    if (sPreRace) { // first racing frame: turbo lap-1 speeds, treasure placement
        sPreRace = 0;
        if (turbo) {
            apply_cc_to_players(TURBO_BASE_CC);
        }
        if (raceMode == 4) {
            treasure_spawn();
        }
    }

    // Balloon match: any kart entering a wrecked state loses a balloon on that edge; out of
    // balloons means the knockout squash for the rest of the race. The pops ride the real battle
    // balloon kit, so they deflate and float away exactly like battle mode.
    if (raceMode == 2) {
        for (i = 0; i < NUM_PLAYERS; i++) {
            u32 hitNow;
            if (!player_exists(i)) {
                continue;
            }
            hitNow = balloon_hit_state(&gPlayers[i]);
            if (hitNow && !sBalloonHitPrev[i] && !sKO[i] && !sBalloonOutDone[i]) {
                if (gPlayerBalloonCount[i] >= 0) {
                    pop_player_balloon(&gPlayers[i], (s8) i);
                }
                if (gPlayerBalloonCount[i] < 0) {
                    // Out of balloons: the BALLOON OUT row picks what that costs you.
                    s32 out = CVarGetInteger("gBalloonOut", 0);
                    sBalloonOutDone[i] = 1;
                    if (out == 1) { // ELIMINATED - CPUs despawn like the duel cull; your race ends
                        if (gPlayers[i].type & PLAYER_HUMAN) {
                            race_mods_force_finish(i); // the REAL finish flow, next tick
                        } else {
                            gPlayers[i].type = 0;
                            gPlayers[i].pos[1] = -30000.0f;
                        }
                    } else if (out == 2) { // SLOWED - full size, knockout's speed cut to the end
                        sSlow[i] = 1;
                        gPlayers[i].topSpeed *= KO_SPEED_PENALTY;
                    } else { // SHRUNK - tiny and slow, delivered as a REAL lightning strike on
                             // JUST this kart (bolt sfx + spin + shrink animation, same as the
                             // knockout mode's strike), not a silent pop-to-tiny. The per-frame
                             // squash keeps them small after the effect would normally regrow.
                        sKO[i] = 1;
                        trigger_lightning_strike(&gPlayers[i], (s8) i);
                        gPlayers[i].topSpeed *= KO_SPEED_PENALTY;
                    }
                }
            }
            sBalloonHitPrev[i] = hitNow;
        }
        // Keep a balloon-eliminated human pinned to the worst still-racing rank until the
        // engine's cinematic freeze takes over (see balloon_eliminated_rank_hold).
        if (CVarGetInteger("gBalloonOut", 0) == 1) {
            for (i = 0; i < NUM_PLAYERS; i++) {
                if (sBalloonOutDone[i] && player_exists(i) && (gPlayers[i].type & PLAYER_HUMAN)) {
                    balloon_eliminated_rank_hold(i);
                }
            }

            // Finish line or last-one-standing, whichever first: a kart that legitimately
            // crosses the line before the field dies out IS the winner. Detected here (before
            // the end decision below) so the forced finishes after the match end can never
            // steal the banner.
            if (sBalloonWinner < 0 && !sBalloonEndFired) {
                for (i = 0; i < NUM_PLAYERS; i++) {
                    if (player_exists(i) && !sBalloonOutDone[i] &&
                        (gPlayers[i].type & PLAYER_CINEMATIC_MODE)) {
                        sBalloonWinner = i;
                        break;
                    }
                }
            }

            // Elimination decides the match - nobody idles through leftover laps (with NO LIMIT
            // laps the race would otherwise never resolve at all). The match ends when the HUMAN
            // is eliminated (your race is over, so it's over) or when eliminations leave at most
            // one racer standing (last one standing wins). A short grace lets the final wreck
            // play before the flag drops.
            if (sBalloonEndTimer == 0 && !sBalloonEndFired) {
                s32 racing = 0;
                s32 outAny = 0;
                s32 outHuman = 0;
                for (i = 0; i < NUM_PLAYERS; i++) {
                    if (sBalloonOutDone[i]) {
                        outAny = 1;
                        if (gPlayers[i].type & PLAYER_HUMAN) {
                            outHuman = 1;
                        }
                    }
                    if (player_exists(i) && !(gPlayers[i].type & PLAYER_CINEMATIC_MODE) &&
                        !sBalloonOutDone[i]) {
                        racing++;
                    }
                }
                if (outHuman || (outAny && racing <= 1)) {
                    // The decisive moment crowns the winner: the last kart standing, or - when
                    // the human's elimination ends the match early - the best-ranked survivor.
                    if (sBalloonWinner < 0) {
                        s32 bestRank = NUM_PLAYERS + 1;
                        for (i = 0; i < NUM_PLAYERS; i++) {
                            if (player_exists(i) && !sBalloonOutDone[i] &&
                                !(gPlayers[i].type & PLAYER_CINEMATIC_MODE) &&
                                gGPCurrentRaceRankByPlayerId[i] < bestRank) {
                                bestRank = gGPCurrentRaceRankByPlayerId[i];
                                sBalloonWinner = i;
                            }
                        }
                    }
                    sBalloonEndTimer = 45; // 1.5s
                }
            }
            if (sBalloonEndTimer > 0 && --sBalloonEndTimer == 0) {
                sBalloonEndFired = 1;
            }
            if (sBalloonEndFired) {
                s32 k;
                // Best rank first so survivors keep their order; each one is lifted above the
                // despawned corpses, then handed to the engine's finish flow. Re-applied every
                // frame until the cinematic freeze locks everyone in (same discipline as the
                // eliminated-human hold). Eliminated players stay with their hold, never lifted.
                for (k = 0; k < NUM_PLAYERS; k++) {
                    s32 pid = gGPCurrentRacePlayerIdByRank[k];
                    if (pid >= 0 && pid < NUM_PLAYERS && player_exists(pid) && !sBalloonOutDone[pid] &&
                        !(gPlayers[pid].type & PLAYER_CINEMATIC_MODE)) {
                        balloon_rank_lift(pid);
                        race_mods_force_finish(pid);
                    }
                }
            }
        }
    }

    for (i = 0; i < NUM_PLAYERS; i++) {
        if (player_exists(i) && gPlayers[i].lapCount > maxLap) {
            maxLap = gPlayers[i].lapCount;
        }
    }

    if (maxLap > sMaxLapSeen) { // the leader started a new lap
        sMaxLapSeen = maxLap;
        if (turbo) {
            // No ceiling for humans: every lap raises the class again, forever. CPUs ride along
            // capped inside apply_cc_to_players so they keep making their corners.
            apply_cc_to_players(TURBO_BASE_CC + TURBO_STEP_CC * (f32) maxLap);
        }
    }

    if (raceMode == 1) {
        knockout_check();
    }

    // Item rain: item boxes drop out of the sky ahead of each human racer on a timer. They settle
    // at the normal float height and behave like placed boxes, so the rolls go through whatever
    // item mode is active. Spawns are capped per race so the actor pool keeps slots for shells.
    if (CVarGetInteger("gItemRain", 0) && sRainDrops < RAIN_MAX_DROPS && ++sRainTimer >= RAIN_PERIOD) {
        sRainTimer = 0;
        for (i = 0; i < gPlayerCountSelection1 && i < NUM_PLAYERS; i++) {
            Vec3f off;
            s32 side;
            if (!player_exists(i) || gPlayers[i].lapCount >= race_mods_total_laps()) {
                continue;
            }
            for (side = -1; side <= 1; side += 2) { // a pair, left and right of the racing line
                off[0] = (f32) side * 60.0f;
                off[1] = 0.0f;
                off[2] = 520.0f; // ahead of the kart, same forward convention as the item throws
                func_802B64C4(off, (s16) (gPlayers[i].rotation[1] + gPlayers[i].unk_0C0));
                spawn_falling_item_box(gPlayers[i].pos[0] + off[0], gPlayers[i].pos[1] + 320.0f,
                                       gPlayers[i].pos[2] + off[2]);
            }
        }
        sRainDrops++;
    }

    // Track hazards: waves of trouble scatter onto random spots along the racing line - ground
    // bananas, free shells (a green rolling the track, or a red hunting the leader), piranha
    // plants, rolling eggs, real bouncing boulders (singles and slides), CHAIN CHOMPS patrolling
    // the line, THWOMPS guarding spots, a rogue Lakitu that dive-bombs you, and trick boxes with
    // the occasional real one as bait. (The kiwano is no longer a wave roll - it ambushes
    // off-road shortcutters from the event block below.) Spawned at path waypoints so everything
    // lands where karts drive; the persistent heavies are capped.
    if (CVarGetInteger("gTrackHazards", 0) && sHazardWaves < HAZARD_MAX_WAVES &&
        ++sHazardTimer >= HAZARD_PERIOD) {
        TrackPathPoint* path = gTrackPaths[0];
        sHazardTimer = 0;
        if (path != NULL && gSelectedPathCount > 8) {
            u32 allowed = hazard_course_mask();
            for (i = 0; i < 3; i++) {
                s32 wpIdx = hazard_far_waypoint(); // far ahead of the human, not popping in beside them
                TrackPathPoint* wp = &path[wpIdx];
                TrackPathPoint* nx = &path[(wpIdx + 3) % gSelectedPathCount];
                f32 dirX = (f32) (nx->x - wp->x);
                f32 dirZ = (f32) (nx->z - wp->z);
                f32 len = sqrtf(dirX * dirX + dirZ * dirZ);
                f32 fwdX = 0.0f; // the line's forward basis at the waypoint...
                f32 fwdZ = 1.0f;
                f32 perpX = 1.0f; // ...and the sideways one - gates and crossings flank with it
                f32 perpZ = 0.0f;
                s32 roll = random_int(27);
                s32 tries;
                if (len > 0.01f) {
                    fwdX = dirX / len;
                    fwdZ = dirZ / len;
                    perpX = fwdZ;
                    perpZ = -fwdX;
                }
                // LEVEL-THEMED mode: a hazard that doesn't belong on this course re-rolls a few
                // times, then settles for a banana - the wave never fizzles, it just stays in
                // character. WILD mode allows everything, so the loop never runs there.
                for (tries = 0; tries < 4 && !(hazard_roll_bit(roll) & allowed); tries++) {
                    roll = random_int(27);
                }
                if (!(hazard_roll_bit(roll) & allowed)) {
                    roll = 0;
                }
                if (roll <= 1) { // ground banana (runtime coords, no flip)
                    spawn_hazard_banana((f32) wp->x, (f32) wp->y, (f32) wp->z);
                } else if (roll == 2) { // banana bunch arc - the 5-banana item-train layout dropped
                                        // as a ground pattern, bowing up the track
                    s32 h;
                    for (h = 0; h < 5; h++) {
                        f32 t = (f32) (h - 2);
                        f32 bow = ((t < 0.0f ? t : -t) + 2.0f) * 14.0f;
                        spawn_hazard_banana((f32) wp->x + perpX * t * 26.0f + fwdX * bow, (f32) wp->y,
                                            (f32) wp->z + perpZ * t * 26.0f + fwdZ * bow);
                    }
                } else if (roll == 3) { // rolling green shell, sent down the racing line
                    spawn_hazard_shell((f32) wp->x, (f32) wp->y, (f32) wp->z, dirX, dirZ);
                } else if (roll == 4) { // was a loose red shell hunting the leader - REMOVED (too unfair);
                                        // drops a banana instead.
                    spawn_hazard_banana((f32) wp->x, (f32) wp->y, (f32) wp->z);
                } else if (roll == 5) { // piranha plant, planted just off the line
                    spawn_hazard_prop(ACTOR_PIRANHA_PLANT, (f32) wp->x + 14.0f, (f32) wp->y, (f32) wp->z);
                } else if (roll == 6) { // piranha pinch-gate - a matched pair flanking the line
                    spawn_hazard_prop(ACTOR_PIRANHA_PLANT, (f32) wp->x + perpX * 26.0f, (f32) wp->y,
                                      (f32) wp->z + perpZ * 26.0f);
                    spawn_hazard_prop(ACTOR_PIRANHA_PLANT, (f32) wp->x - perpX * 26.0f, (f32) wp->y,
                                      (f32) wp->z - perpZ * 26.0f);
                } else if (roll == 7) { // giant yoshi egg rolling about - VERY RARE now: only ~1 in 10
                                        // egg rolls stays an egg; the rest drop a banana.
                    if (random_int(9) == 0) {
                        spawn_hazard_prop(ACTOR_YOSHI_EGG, (f32) wp->x, (f32) wp->y, (f32) wp->z);
                    } else {
                        spawn_hazard_banana((f32) wp->x, (f32) wp->y, (f32) wp->z);
                    }
                } else if (roll == 8) { // boulder out of the sky, bouncing down whatever it hits
                    race_mods_spawn_boulder((f32) wp->x, (f32) wp->y, (f32) wp->z);
                } else if (roll == 9) { // rockslide - a pair of them straddling the line
                    race_mods_spawn_boulder((f32) wp->x, (f32) wp->y, (f32) wp->z);
                    race_mods_spawn_boulder((f32) nx->x, (f32) nx->y, (f32) nx->z);
                } else if (roll == 10 && sHazardMoles < HAZARD_MAX_MOLES) {
                    // MOLE AMBUSH: a cluster pops out of the ground around the waypoint and
                    // grabs whoever drives through (the Moo Moo Farm kit).
                    race_mods_spawn_mole_ambush((f32) wp->x, (f32) wp->y, (f32) wp->z);
                    sHazardMoles++;
                } else if (roll == 11 && sHazardSnowmen < HAZARD_MAX_SNOWMEN) {
                    // EXPLODING SNOWMAN, planted on the line (detonates on contact, then
                    // rebuilds - the Frappe Snowland kit).
                    race_mods_spawn_snowman((f32) wp->x, (f32) wp->y, (f32) wp->z);
                    sHazardSnowmen++;
                } else if (roll == 12 && sHazardCheeps < HAZARD_MAX_CHEEPS) {
                    // CHEEP CHEEP arcs flopping across this stretch of track (slots 11+ keep
                    // clear of the chomps at 8-10 and any course's stock objects below).
                    race_mods_spawn_cheep_school(11 + sHazardCheeps, (f32) wp->x, (f32) wp->y, (f32) wp->z, wpIdx);
                    sHazardCheeps++;
                } else if (roll == 13 && sHazardCrabs < HAZARD_MAX_CRABS) {
                    // CRAB scuttling back and forth across the line.
                    race_mods_spawn_crab((f32) wp->x + perpX * 45.0f, (f32) wp->z + perpZ * 45.0f,
                                         (f32) wp->x - perpX * 45.0f, (f32) wp->z - perpZ * 45.0f);
                    sHazardCrabs++;
                } else if (roll == 14 && sHazardHogs < HAZARD_MAX_HOGS) {
                    // HEDGEHOG roadblock - same shape as the crab, pricklier.
                    race_mods_spawn_hedgehog((f32) wp->x + perpX * 35.0f, (f32) wp->y, (f32) wp->z + perpZ * 35.0f,
                                             (f32) wp->x - perpX * 35.0f, (f32) wp->z - perpZ * 35.0f);
                    sHazardHogs++;
                } else if (roll == 15 && sHazardPenguinWaves < HAZARD_MAX_PENGUIN_WAVES) {
                    // PENGUIN CROSSING - a waddling column circling across a pinch point.
                    s32 h;
                    for (h = 0; h < 3; h++) {
                        TrackPathPoint* pw = &path[(wpIdx + h * 2) % gSelectedPathCount];
                        race_mods_spawn_penguin((f32) pw->x, (f32) pw->y, (f32) pw->z, 90.0f + 30.0f * (f32) h);
                    }
                    sHazardPenguinWaves++;
                } else if (roll == 16 && sHazardBombKarts < HAZARD_MAX_BOMBKARTS) {
                    // BOMB KART let loose on the line - alternates a waypoint patrol and an
                    // open CHASE (OBombKart behaviours 1 / 4).
                    race_mods_spawn_bomb_kart(wpIdx, (sHazardBombKarts & 1) ? 4 : 1);
                    sHazardBombKarts++;
                } else if (roll == 17 && sHazardChomps < HAZARD_MAX_CHOMPS) {
                    // CHAIN CHOMP: patrols the racing line, tumbling whatever it catches. Slot
                    // base 8 keeps hazard chomps clear of the stock objects (cheep cheeps,
                    // trophies) that own the low indexObjectList2 entries on some courses.
                    race_mods_spawn_chomp(8 + sHazardChomps);
                    sHazardChomps++;
                } else if (roll == 18 && sHazardThwomps < HAZARD_MAX_THWOMPS) {
                    // THWOMP: grounded at the waypoint's own floor (the stock inits assume
                    // Bowser's Castle y=0, so the bridge probes the real ground). Alternates
                    // the sliding patrol and the move-and-rotate prowl (behaviours 5 / 2).
                    // The slide rides primAlpha as its SPEED table (0/1) - 255 fell through
                    // the switch and the patrol never moved.
                    race_mods_spawn_thwomp((f32) wp->x, (f32) wp->y, (f32) wp->z, (sHazardThwomps & 1) ? 5 : 2,
                                           (sHazardThwomps & 1) ? 1 : 255);
                    sHazardThwomps++;
                } else if (roll == 19 && sHazardThwomps + 1 < HAZARD_MAX_THWOMPS) {
                    // THWOMP SLAM-GATE: a pair flanking a chokepoint on alternating timers
                    // (STATIONARY_FAST rides primAlpha as its slam-timer table, 60 vs 120).
                    race_mods_spawn_thwomp((f32) wp->x + perpX * 34.0f, (f32) wp->y, (f32) wp->z + perpZ * 34.0f, 4,
                                           1);
                    race_mods_spawn_thwomp((f32) wp->x - perpX * 34.0f, (f32) wp->y, (f32) wp->z - perpZ * 34.0f, 4,
                                           2);
                    sHazardThwomps += 2;
                } else if (roll == 20 && sHazardLakitus < HAZARD_MAX_LAKITUS) {
                    // EVIL LAKITU: shadows a random human from above and dive-bombs on a timer.
                    // EXTREMELY RARE now - even when the roll lands here, only ~1 in 12 actually
                    // summons him, and the per-race cap is 1, so you never face two. Every other
                    // time he no-shows and the wave just drops a banana.
                    if (random_int(12) == 0) {
                        s32 t = pick_random_human_target();
                        if (t >= 0) {
                            race_mods_spawn_evil_lakitu(t);
                            sHazardLakitus++;
                        } else {
                            spawn_hazard_banana((f32) wp->x, (f32) wp->y, (f32) wp->z);
                        }
                    } else {
                        spawn_hazard_banana((f32) wp->x, (f32) wp->y, (f32) wp->z);
                    }
                } else if (roll == 21) {
                    // SPINY-EGG LAKITU (the SMB Lakitu lobbing spiny eggs at the leader) REMOVED - it
                    // reads as a blue shell and piled onto the egg spam; drops a banana instead.
                    spawn_hazard_banana((f32) wp->x, (f32) wp->y, (f32) wp->z);
                } else if (roll == 22 && sHazardBoos < HAZARD_MAX_BOOS) {
                    // BOO HAUNT: a boo circles a racer for a while, cackling in their ear.
                    // (The item STEAL is deferred - the true held-item storage hasn't been
                    // located; the haunt ships as pure menace.)
                    s32 t = pick_random_human_target();
                    if (t >= 0) {
                        race_mods_spawn_boo(t);
                        sHazardBoos++;
                    } else {
                        spawn_hazard_banana((f32) wp->x, (f32) wp->y, (f32) wp->z);
                    }
                } else if (roll == 23 && sHazardBombers < HAZARD_MAX_BOMBERS) {
                    // BOB-OMB BALLOON: the Luigi Raceway balloon drifts across the track
                    // dropping trick boxes (with the odd real one as bait).
                    race_mods_spawn_balloon_bomber((f32) wp->x, (f32) wp->y, (f32) wp->z, wpIdx);
                    sHazardBombers++;
                } else if (roll == 24) {
                    // FOG BANK (a drive-through patch of dust/smoke billboards) REMOVED - it had zero
                    // collision and no slow, so as a "hazard" it was just view clutter; drops a banana
                    // instead.
                    spawn_hazard_banana((f32) wp->x, (f32) wp->y, (f32) wp->z);
                } else if (roll == 25) {
                    // LEADER BOULDER STRIKE (thunderbolt warning + a boulder on the leader) REMOVED -
                    // it's the "lightning" hazard; drops a banana instead.
                    spawn_hazard_banana((f32) wp->x, (f32) wp->y, (f32) wp->z);
                } else { // boxes expect course-data coords - pre-cancel the mirror flip
                    Vec3f hp;
                    hp[0] = (f32) wp->x * gTrackDirection;
                    hp[1] = (f32) wp->y + 4.0f;
                    hp[2] = (f32) wp->z;
                    if (roll == 27) { // the 1-in-65536 jackpot roll stays a REAL box
                        spawn_item_box(hp);
                    } else { // roll 26 + capped entries fall through to trick-box bait
                        spawn_fake_item_box(hp);
                    }
                }
            }
            sHazardWaves++;
        }
    }

    // The kiwano ambush, a shortcut tax: cut across the grass (sand, snow, dirt, cliff faces)
    // for ~1.5s straight and the DK Jungle pest comes for YOU - it left the random wave table,
    // so staying on the road means never seeing one. Airborne resets the run (hopping the gap
    // between two road ribbons isn't a shortcut), and a per-player cooldown keeps it a sting,
    // not a spam. Runs every tick so the trigger reads the surface the tires actually touch.
    // LEVEL-THEMED mode only arms it where a jungle pest is believable (hazard_course_mask).
    if (CVarGetInteger("gTrackHazards", 0) && (hazard_course_mask() & HZ_KIWANO)) {
        for (i = 0; i < NUM_PLAYERS; i++) {
            if (sKiwanoCooldown[i] > 0) {
                sKiwanoCooldown[i]--;
            }
            if (!player_exists(i) || !(gPlayers[i].type & PLAYER_HUMAN) ||
                (gPlayers[i].type & PLAYER_CINEMATIC_MODE) || gPlayers[i].lapCount >= race_mods_total_laps()) {
                sOffroadTicks[i] = 0;
                continue;
            }
            if (player_is_offroad(i)) {
                sOffroadTicks[i]++;
            } else {
                sOffroadTicks[i] = 0;
            }
            if (sOffroadTicks[i] >= 90 && sKiwanoCooldown[i] == 0) {
                // Only SOMETIMES, not every time: after ~3s of continuous off-road the ambush
                // ARMS, but it only fires on about a 1-in-5 roll. A miss re-arms a few seconds
                // later (short cooldown) so a long shortcut can still eventually get stung - just
                // rarely. A real ambush sets a long 30s grace before another can arm.
                sOffroadTicks[i] = 0;
                if (random_int(5) == 0) {
                    spawn_hazard_kiwano(i);
                    sKiwanoCooldown[i] = 900; // 30s before the next ambush can arm
                } else {
                    sKiwanoCooldown[i] = 150; // missed roll - re-arm in ~5s of more off-road
                }
            }
        }
    }

    // The leader boulder strike's delayed drop: a wave rolled the thunder warning; when the
    // beat runs out a real boulder falls just ahead of whoever leads the race RIGHT NOW - the
    // strike tracks the kart, not the spot the wave rolled.
    if (sLeaderStrikeTicks > 0 && --sLeaderStrikeTicks == 0) {
        s32 lead = gGPCurrentRacePlayerIdByRank[0];
        if (lead >= 0 && lead < NUM_PLAYERS && player_exists(lead) &&
            !(gPlayers[lead].type & PLAYER_CINEMATIC_MODE)) {
            race_mods_spawn_boulder(gPlayers[lead].pos[0] + gPlayers[lead].velocity[0] * 25.0f,
                                    gPlayers[lead].pos[1],
                                    gPlayers[lead].pos[2] + gPlayers[lead].velocity[2] * 25.0f);
        }
    }

    // (The lightning storm hazard was removed - a bolt out of nowhere reads as unfair, not as
    // weather. Lightning stays a deliberate game-MODE consequence only: the knockout last-place
    // strike and the balloon shrink-out, never a random hazard roll.)

    // Crosswind gust: a stretch of the racing line turns windy for ~5s and every kart in it
    // gets a gentle, continuous sideways shove. Velocity-only and deliberately mild - VR
    // comfort sets the ceiling, the wind should read as drift, never as a hit. LEVEL-THEMED
    // mode keeps the wind to open-air courses.
    if (CVarGetInteger("gTrackHazards", 0) && (hazard_course_mask() & HZ_GUST) && gTrackPaths[0] != NULL &&
        gSelectedPathCount > 16) {
        if (sGustTicks == 0 && ++sGustTimer >= sGustNext) {
            TrackPathPoint* path = gTrackPaths[0];
            s32 c;
            f32 gdx, gdz, glen;
            sGustTimer = 0;
            sGustNext = 1350 + (s32) random_int(900); // the next gust 45-75s out
            sGustStart = random_int((u16) (gSelectedPathCount - 1));
            c = (sGustStart + 5) % gSelectedPathCount;
            gdx = (f32) (path[(c + 2) % gSelectedPathCount].x - path[c].x);
            gdz = (f32) (path[(c + 2) % gSelectedPathCount].z - path[c].z);
            glen = sqrtf(gdx * gdx + gdz * gdz);
            if (glen > 0.01f) {
                f32 side = (random_int(2) != 0) ? 1.0f : -1.0f;
                sGustVecX = gdz / glen * 0.45f * side;
                sGustVecZ = -gdx / glen * 0.45f * side;
                sGustTicks = 150;
            }
        }
        if (sGustTicks > 0) {
            sGustTicks--;
            for (i = 0; i < NUM_PLAYERS; i++) {
                s32 d;
                if (!player_exists(i) || (gPlayers[i].type & PLAYER_CINEMATIC_MODE)) {
                    continue;
                }
                d = ((s32) gNearestPathPointByPlayerId[i] - sGustStart + gSelectedPathCount) % gSelectedPathCount;
                if (d < 10) { // the windy stretch is 10 waypoints long
                    gPlayers[i].velocity[0] += sGustVecX;
                    gPlayers[i].velocity[2] += sGustVecZ;
                }
            }
        }
    }

    // CPU stuck watchdog: a CPU that has crawled less than ~2 units a tick for 2.5s straight -
    // and is NOT mid-wreck (those recover on their own) - is pinned against geometry its
    // waypoint steering can't solve (witnessed at high speed classes: the turbo escalations and
    // big custom CCs overshoot corners into walls). The rescue sets it back onto the racing
    // line a couple of waypoints past where it stands, pointed down the path, velocity clean.
    // Humans are never touched - getting yourself unstuck is part of driving.
    if (gTrackPaths[0] != NULL && gSelectedPathCount > 8) {
        for (i = 0; i < NUM_PLAYERS; i++) {
            f32 dx, dz;
            if (sCpuRescueCool[i] > 0) {
                sCpuRescueCool[i]--;
            }
            if (!player_exists(i) || (gPlayers[i].type & (PLAYER_HUMAN | PLAYER_CINEMATIC_MODE))) {
                sCpuStuckTicks[i] = 0;
                continue;
            }
            dx = gPlayers[i].pos[0] - sCpuLastX[i];
            dz = gPlayers[i].pos[2] - sCpuLastZ[i];
            sCpuLastX[i] = gPlayers[i].pos[0];
            sCpuLastZ[i] = gPlayers[i].pos[2];
            if (dx * dx + dz * dz < 4.0f && !balloon_hit_state(&gPlayers[i])) {
                sCpuStuckTicks[i]++;
            } else {
                sCpuStuckTicks[i] = 0;
            }
            if (sCpuStuckTicks[i] >= 75 && sCpuRescueCool[i] == 0) {
                TrackPathPoint* path = gTrackPaths[0];
                s32 wpi = ((s32) gNearestPathPointByPlayerId[i] + 2) % gSelectedPathCount;
                TrackPathPoint* wp = &path[wpi];
                TrackPathPoint* fw = &path[(wpi + 2) % gSelectedPathCount];
                Vec3f from;
                Vec3f to;
                from[0] = (f32) wp->x;
                from[1] = (f32) wp->y;
                from[2] = (f32) wp->z;
                to[0] = (f32) fw->x;
                to[1] = (f32) fw->y;
                to[2] = (f32) fw->z;
                gPlayers[i].pos[0] = (f32) wp->x;
                gPlayers[i].pos[1] = (f32) wp->y + 4.0f;
                gPlayers[i].pos[2] = (f32) wp->z;
                gPlayers[i].velocity[0] = 0.0f;
                gPlayers[i].velocity[1] = 0.0f;
                gPlayers[i].velocity[2] = 0.0f;
                gPlayers[i].speed = 0.0f;
                gPlayers[i].rotation[1] = (s16) get_angle_between_two_vectors(from, to);
                gPlayers[i].unk_0AE = gPlayers[i].rotation[1]; // the spin-restore yaw follows
                gPlayers[i].unk_0C0 = 0;                       // no leftover slide angle
                sCpuStuckTicks[i] = 0;
                sCpuRescueCool[i] = 90; // 3s before the watchdog can fire on this seat again
                race_mods_diag_line("cpu rescue: seat=%d wp=%d", i, wpi);
            }
        }
    }

    enforce_kart_sizes(sizeMode);
    if (raceMode == 3) {
        tag_tick(); // after the size pass so IT's giant build wins
    }
    if (raceMode == 4) {
        treasure_tick();
    }
    if (raceMode == 5) {
        infected_tick();
    }
}

// D-PAD UP cycles the view mode mid-race, no menu required: flatscreen walks Third Person ->
// First Person -> Diorama; VR walks its four modes. The camera override and the VR layer both
// re-read the CVar every frame, so the switch is instant. Skipped while paused (the pause
// overlay owns the d-pad there) and in demos.
static void view_mode_hotkey_tick(void) {
    if (gIsGamePaused != 0 || gDemoMode != 0) {
        return;
    }
    if ((gControllers[0].buttonPressed & U_JPAD) == 0) {
        return;
    }
    if (vr_is_active()) {
        CVarSetInteger("gVRViewMode", (CVarGetInteger("gVRViewMode", 0) + 1) & 3);
    } else {
        CVarSetInteger("gFlatViewMode", (CVarGetInteger("gFlatViewMode", 0) + 1) % 3);
    }
    play_sound2(SOUND_MENU_CURSOR_MOVE);
}

// Flip cam: report once per logic frame whether the player kart is in a dramatic state, so First
// Person VR can pull the eye back and show the kart react. Uses the persistent effect bits (they
// stay set for the whole animation), not the one-frame trigger flags. Runs in every race mode.
void race_mods_action_cam_tick(void) {
    Player* p = &gPlayers[0];
    s32 dramatic = 0;
    s32 finished = 0;
    s16 dramaYaw = 0;
    s16 dramaPitch = 0;

    view_mode_hotkey_tick(); // d-pad up = next view mode, every race mode, flat and VR

    if ((p->type & PLAYER_EXISTS) && gRaceState >= 3) {
        // banana spins (0x80/0x40), green-shell tumble (0x400), red/blue-shell tumble (0x2000000),
        // vertical bomb/launch tumble (0x1000000), lightning (0x40000000) - each persists while its
        // animation plays
        dramatic = (p->effects & (0x80 | 0x40 | 0x400 | 0x1000000 | 0x2000000 | 0x40000000)) != 0;
        // thwomp squish: the hit effect plus the squish marker bit
        dramatic = dramatic || ((p->effects & 0x4000000) != 0 && (p->unk_046 & 0x80) != 0);
        // driving spinout state
        dramatic = dramatic || (p->kartProps & 0x4000) != 0;
        // grabbed or carried by Lakitu after going out of bounds
        dramatic = dramatic || (p->lakituProps & 0x3) != 0;
        // launched high into the air (drift hops stay under this)
        dramatic = dramatic || p->kartHopVelocity > 7.0f;
        // crossed the line: the post-race camera needs the kart visible
        finished = (p->type & PLAYER_CINEMATIC_MODE) != 0;

        // The live drama angles for the First Person in-place simulation:
        // spins write the kart's yaw directly and restore unk_0AE at the end, so the delta is the
        // spin angle so far (and wraps back to ~0 on the final full turn). EVERY tumble flavor
        // (green / red / blue shells, bombs, vertical launches, star hits) accumulates its flip
        // pitch in unk_042 and zeroes it outside tumbles, so it's read unconditionally.
        // NOTE: a "follow cam" that rotated the First Person view with the kart's slide/pivot
        // angle lived here through three iterations and never drove right - chase-cam lag and
        // drift angles fed the view constantly and broke steering feel. REMOVED on Ray's call:
        // First Person drives exactly like every other mode (and in VR you look into a turn by
        // turning your head). Don't reintroduce a view rotation tied to kart state outside the
        // wreck animations above.
        if ((p->effects & (0x80 | 0x40 | 0x20000)) != 0) {
            dramaYaw = (s16) (p->rotation[1] - p->unk_0AE);
        }
        dramaPitch = (s16) p->unk_042;
    }
    vr_set_action_cam(dramatic, finished);
    vr_set_drama_rot(dramaYaw, dramaPitch);
}
