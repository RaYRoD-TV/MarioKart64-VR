// Native MK64-style race setup screen (see race_mods_menu.h). Lives as an overlay on the main menu's
// OK state - main_menu_act feeds it input from the top of the MAIN_MENU_OK_SELECT case, and it draws
// from the tail of the menu-item render pass so it covers the stock widgets. Item modes are consumed
// in src/engine/RaceManager.cpp; knockout / turbo laps / kart size / item rain tick in src/race_mods.c;
// mirror + track roulette hook the menu flow in src/menus.c. Speed / CPU rows ride the port's existing
// CVars (gCustomCC / gHarderCPU / gDisableRubberbanding / gDisableItemboxes), so the ImGui menu stays
// in sync.
#include <libultraship.h>
#include <libultra/types.h>
#include <macros.h>
#include <mk64.h>
#include <defines.h>
#include <sounds.h>
#include <stdio.h>
#include "menu_items.h"
#include "menus.h"
#include "audio/external.h"
#include "save.h"
#include "race_mods.h"
#include "race_mods_menu.h"

extern Gfx* gDisplayListHead;
extern s32 gModeSelection; // main.c - VERSUS through the 1P VS menu flow until the course commit
extern s32 gGlobalTimer;   // main.c - the flash cadence for the in-race status line
extern s32 gRaceState;     // main.c - the status HUD clears once the score screen owns the view
extern s32 gPlayerCountSelection1; // main.c - the live player count (restored on a setup reopen)

extern float CVarGetFloat(const char* name, float defaultValue);
extern void  CVarSetFloat(const char* name, float value);
extern int   CVarGetInteger(const char* name, int defaultValue);
extern void  CVarSetInteger(const char* name, int value);

static s32 sOpen = 0;
static s32 sSel  = 0;
static s32 sBattle = 0; // the screen wears BATTLE trim (opened off the 1P BATTLE row)

// RETURN TO SETUP (the pause-menu R1 overlay's RACE SETUP row): the menu navigation state the
// setup screen was last opened at, so a mid-race quit can land straight back on it. The settings
// themselves are CVars and persist on their own; this just re-drives the menu to the OK screen.
// (is_screen_being_faded is declared in menus.h.)
static s32 sSnapValid = 0;     // a setup screen has been opened at least once this session
static s32 sSnapBattle = 0;    // 1 = the last setup was the battle screen
static s8  sSnapCount = 1;     // gPlayerCount at open
static s32 sSnapMode = 0;      // gModeSelection at open
static s8  sSnapModeCol = 0;   // gGameModeMenuColumn[count-1]
static s8  sSnapSubCol = 0;    // gGameModeSubMenuColumn[count-1][modeCol]
static s32 sPendingReopen = 0; // set by the pause menu; the poll drives the menu back to setup

static void snapshot_setup_nav(s32 battle) {
    s32 count = (gPlayerCount >= 1 && gPlayerCount <= 4) ? gPlayerCount : 1;
    s32 col = gGameModeMenuColumn[count - 1];
    sSnapValid = 1;
    sSnapBattle = battle;
    sSnapCount = (s8) count;
    sSnapMode = gModeSelection;
    sSnapModeCol = (s8) col;
    sSnapSubCol = (s8) gGameModeSubMenuColumn[count - 1][col];
}

enum {
    ROW_ITEMS,
    ROW_SAME_ITEM,
    ROW_CPU_ITEMS,
    ROW_ITEM_RAIN,
    ROW_HAZARDS,
    ROW_PROPS,
    ROW_MODE,
    ROW_BALLOON_OUT,
    ROW_LAPS,
    ROW_SPEED,
    ROW_MIRROR,
    ROW_SIZE,
    ROW_CPU_SKILL,
    ROW_CATCHUP,
    ROW_START,
    ROW_COUNT
};

static const char* const kRowLabels[ROW_COUNT] = {
    "ITEMS",  "SAME ITEM", "CPU ITEMS TOO", "ITEM RAIN", "HAZARDS",   "PROP SWAP", "MODE",      "BALLOON OUT",
    "LAPS",   "SPEED CLASS", "MIRROR", "KART SIZE",   "CPU SKILL", "CPU CATCH UP", "START RACE"
};

// gPropSwap: courses with signature movers (turnpike traffic, kalimari trains, frappe snowmen,
// choco rocks, rainbow chomps) swap them for the chosen menagerie (race_mods.c prop_swap_spawn).
// THEMED picks a course-fitting mix (snowmen + penguins on the ice courses, chomps + bomb karts
// in the castle/haunt, chomps on Rainbow Road, traffic-like bomb karts on the turnpike...).
enum {
    PROPS_STOCK,
    PROPS_CHOMPS,
    PROPS_BOMBKARTS,
    PROPS_SNOWMEN,
    PROPS_PENGUINS,
    PROPS_MIXED,
    PROPS_THEMED,
    PROPS_CHOICES
};
static const char* const kPropsNames[PROPS_CHOICES] = { "STOCK",   "CHOMPS",   "BOMB KARTS", "SNOWMEN",
                                                        "PENGUINS", "MIXED",   "THEMED" };

// gBalloonOut: what losing the last balloon costs (BALLOONS mode only).
static const char* const kBalloonOutNames[3] = { "SHRUNK", "ELIMINATED", "SLOWED" };

// BATTLE trim (1P battle, race_mods.c's battle tick + spawn_players_1p_battle consume these).
enum {
    BROW_BALLOONS,
    BROW_RIVALS,
    BROW_HEAT,
    BROW_LOSERS,
    BROW_SUDDEN,
    BROW_B_ITEMS,
    BROW_B_SAME_ITEM,
    BROW_B_SIZE,
    BROW_B_START,
    BROW_COUNT
};
static const char* const kBattleRowLabels[BROW_COUNT] = {
    "BALLOONS", "RIVALS", "CPU HEAT", "LOSERS", "SUDDEN DEATH", "ITEMS", "SAME ITEM", "KART SIZE", "START BATTLE"
};
static const char* const kHeatNames[3] = { "CALM", "MEAN", "RABID" };
static const char* const kLoserNames[2] = { "BOMB KART", "GHOST" };
static const char* const kSuddenNames[3] = { "OFF", "2 MIN", "3 MIN" };

// gItemMode order: 0 stock / 1 truly random / 2 all same / 3 frantic / 4 triples / 5 inverted;
// "NO BOXES" is gDisableItemboxes with the mode back at stock.
enum { ITEMS_STOCK, ITEMS_RANDOM, ITEMS_SAME, ITEMS_FRANTIC, ITEMS_TRIPLES, ITEMS_INVERTED, ITEMS_NONE,
       ITEMS_CHOICES };
static const char* const kItemsNames[ITEMS_CHOICES] = { "STOCK",   "TRULY RANDOM", "ALL SAME", "FRANTIC",
                                                        "TRIPLES", "INVERTED",     "NO BOXES" };

// Indexed by item id (defines.h ITEM_ enum, 1..15).
static const char* const kItemNames[ITEM_MAX] = {
    "---",          "BANANA",      "BANANA BUNCH", "GREEN SHELL", "3X GREEN SHELL", "RED SHELL",
    "3X RED SHELL", "BLUE SHELL",  "LIGHTNING",    "FAKE BOX",    "STAR",           "BOO",
    "MUSHROOM",     "2X MUSHROOM", "3X MUSHROOM",  "GOLD MUSHROOM"
};

enum { SPEED_STOCK, SPEED_200, SPEED_300, SPEED_500, SPEED_TURBO, SPEED_CHOICES };
static const char* const kSpeedNames[SPEED_CHOICES] = { "STOCK", "200CC", "300CC", "500CC", "TURBO LAPS" };
static const float kSpeedCC[SPEED_CHOICES] = { 0.0f, 200.0f, 300.0f, 500.0f, 150.0f };

static const s32 kLapChoices[] = { 1, 2, 0, 4, 5, 100 }; // ascending; 0 = stock three, 100 = no limit
static const char* const kLapNames[] = { "1", "2", "3", "4", "5", "NO LIMIT" };
#define LAP_CHOICES 6

static const char* const kSizeNames[4] = { "STOCK", "TINY", "GIANT", "MIXED" };

// The items row folds two CVars into one choice list.
static s32 items_choice(void) {
    s32 mode;
    if (CVarGetInteger("gDisableItemboxes", 0)) {
        return ITEMS_NONE;
    }
    mode = CVarGetInteger("gItemMode", 0);
    if (mode < 0 || mode > 5) {
        mode = 0;
    }
    return mode; // 0..5 line up with ITEMS_STOCK..ITEMS_INVERTED
}

static void set_items_choice(s32 choice) {
    CVarSetInteger("gDisableItemboxes", choice == ITEMS_NONE ? 1 : 0);
    CVarSetInteger("gItemMode", (choice >= ITEMS_RANDOM && choice <= ITEMS_INVERTED) ? choice : 0);
}

static s32 speed_choice(void) {
    if (CVarGetInteger("gSpeedTurboLaps", 0)) {
        return SPEED_TURBO;
    }
    if (CVarGetInteger("gEnableCustomCC", 0)) {
        f32 cc = CVarGetFloat("gCustomCC", 150.0f);
        s32 i;
        for (i = SPEED_200; i <= SPEED_500; i++) {
            if (cc > kSpeedCC[i] - 25.0f && cc < kSpeedCC[i] + 25.0f) {
                return i;
            }
        }
    }
    return SPEED_STOCK;
}

static void set_speed_choice(s32 choice) {
    CVarSetInteger("gSpeedTurboLaps", choice == SPEED_TURBO ? 1 : 0);
    if (choice == SPEED_STOCK) {
        CVarSetInteger("gEnableCustomCC", 0);
    } else {
        CVarSetInteger("gEnableCustomCC", 1);
        CVarSetFloat("gCustomCC", kSpeedCC[choice]); // turbo seeds at 150; race_mods.c escalates per lap
    }
}

static s32 laps_choice(void) {
    s32 v = CVarGetInteger("gLapsOverride", 0);
    s32 i;
    for (i = 0; i < LAP_CHOICES; i++) {
        if (kLapChoices[i] == v) {
            return i;
        }
    }
    return 0;
}

// A treasure hunt runs until the box is found - race_mods_total_laps pins NO LIMIT for it, so
// the LAPS row locks to match instead of showing a choice that does nothing. (Balloon
// elimination keeps the lap choice: finish line or last-one-standing, whichever comes first.)
static s32 laps_locked(void) {
    return CVarGetInteger("gRaceMode", 0) == 4;
}

static void row_change(s32 dir) {
    s32 step = (dir == 0) ? 1 : dir; // A cycles forward like the VR pause menu rows
    switch (sSel) {
        case ROW_ITEMS:
            set_items_choice((items_choice() + step + ITEMS_CHOICES) % ITEMS_CHOICES);
            break;
        case ROW_SAME_ITEM: {
            if (items_choice() == ITEMS_SAME) {
                s32 item = CVarGetInteger("gItemModeItem", ITEM_BANANA);
                item += step;
                if (item < 1) item = ITEM_MAX - 1;
                if (item > ITEM_MAX - 1) item = 1;
                CVarSetInteger("gItemModeItem", item);
            }
            break;
        }
        case ROW_CPU_ITEMS:
            if (items_choice() != ITEMS_STOCK && items_choice() != ITEMS_NONE) {
                CVarSetInteger("gItemModeCPU", !CVarGetInteger("gItemModeCPU", 1));
            }
            break;
        case ROW_ITEM_RAIN:
            CVarSetInteger("gItemRain", !CVarGetInteger("gItemRain", 0));
            break;
        case ROW_HAZARDS:
            // 0 off / 1 level themed (only hazards that belong on this course) / 2 wild
            // (anything anywhere). Old saves with the boolean ON land on THEMED.
            CVarSetInteger("gTrackHazards", (CVarGetInteger("gTrackHazards", 0) + step + 3) % 3);
            break;
        case ROW_PROPS:
            CVarSetInteger("gPropSwap",
                           (CVarGetInteger("gPropSwap", 0) + step + PROPS_CHOICES) % PROPS_CHOICES);
            break;
        case ROW_MODE:
            CVarSetInteger("gRaceMode", (CVarGetInteger("gRaceMode", 0) + (dir < 0 ? 5 : 1)) % 6);
            break;
        case ROW_BALLOON_OUT:
            // The mode-option row pulls double duty: the balloon consequence in BALLOONS, the
            // hidden-box toggle in TREASURE HUNT (label and value swap with the mode).
            if (CVarGetInteger("gRaceMode", 0) == 2) {
                CVarSetInteger("gBalloonOut", (CVarGetInteger("gBalloonOut", 0) + step + 3) % 3);
            } else if (CVarGetInteger("gRaceMode", 0) == 4) {
                CVarSetInteger("gTreasureHidden", !CVarGetInteger("gTreasureHidden", 0));
            }
            break;
        case ROW_LAPS: {
            if (laps_locked()) {
                break; // the treasure hunt owns the lap count
            }
            s32 c = (laps_choice() + step + LAP_CHOICES) % LAP_CHOICES;
            CVarSetInteger("gLapsOverride", kLapChoices[c]);
            break;
        }
        case ROW_SPEED:
            set_speed_choice((speed_choice() + step + SPEED_CHOICES) % SPEED_CHOICES);
            break;
        case ROW_MIRROR:
            CVarSetInteger("gMirrorForce", !CVarGetInteger("gMirrorForce", 0));
            break;
        case ROW_SIZE:
            CVarSetInteger("gKartSize", (CVarGetInteger("gKartSize", 0) + step + 4) % 4);
            break;
        case ROW_CPU_SKILL:
            CVarSetInteger("gHarderCPU", !CVarGetInteger("gHarderCPU", 0));
            break;
        case ROW_CATCHUP:
            CVarSetInteger("gDisableRubberbanding", !CVarGetInteger("gDisableRubberbanding", 0));
            break;
    }
}

// BATTLE trim equivalents: a leaner table, same idiom. Clamp helpers keep stale saves sane.
static s32 cvar_cycle(const char* name, s32 def, s32 count, s32 step) {
    s32 v = CVarGetInteger(name, def);
    if (v < 0 || v >= count) {
        v = def;
    }
    return (v + step + count) % count;
}

static void battle_row_change(s32 dir) {
    s32 step = (dir == 0) ? 1 : dir;
    switch (sSel) {
        case BROW_BALLOONS: // stored 1..3, shown as is
            CVarSetInteger("gBattleBalloons", 1 + cvar_cycle("gBattleBalloons", 3, 3, step - 1));
            break;
        case BROW_RIVALS: // stored 1..3
            CVarSetInteger("gBattleRivals", 1 + cvar_cycle("gBattleRivals", 3, 3, step - 1));
            break;
        case BROW_HEAT:
            CVarSetInteger("gBattleCpuHeat", cvar_cycle("gBattleCpuHeat", 1, 3, step));
            break;
        case BROW_LOSERS:
            CVarSetInteger("gBattleLosers", !CVarGetInteger("gBattleLosers", 0));
            break;
        case BROW_SUDDEN:
            CVarSetInteger("gBattleSudden", cvar_cycle("gBattleSudden", 0, 3, step));
            break;
        case BROW_B_ITEMS:
            set_items_choice((items_choice() + step + ITEMS_CHOICES) % ITEMS_CHOICES);
            break;
        case BROW_B_SAME_ITEM: {
            if (items_choice() == ITEMS_SAME) {
                s32 item = CVarGetInteger("gItemModeItem", ITEM_BANANA);
                item += step;
                if (item < 1) item = ITEM_MAX - 1;
                if (item > ITEM_MAX - 1) item = 1;
                CVarSetInteger("gItemModeItem", item);
            }
            break;
        }
        case BROW_B_SIZE:
            CVarSetInteger("gKartSize", (CVarGetInteger("gKartSize", 0) + step + 4) % 4);
            break;
    }
}

static const char* battle_row_help(void) {
    switch (sSel) {
        case BROW_BALLOONS:
            return "BALLOONS EACH KART STARTS WITH. EVERY HIT POPS ONE";
        case BROW_RIVALS:
            return "HOW MANY CPU KARTS WANT YOUR BALLOONS";
        case BROW_HEAT:
            switch (CVarGetInteger("gBattleCpuHeat", 1)) {
                case 0:  return "RIVALS CRUISE AND RARELY THROW";
                case 2:  return "RIVALS FLOOR IT AND THROW CONSTANTLY";
                default: return "RIVALS HUNT AND THROW AT A FAIR PACE";
            }
        case BROW_LOSERS:
            return CVarGetInteger("gBattleLosers", 0) ? "POPPED RIVALS VANISH FROM THE ARENA"
                                                      : "POPPED KARTS BECOME ROVING BOMB KARTS";
        case BROW_SUDDEN:
            return CVarGetInteger("gBattleSudden", 0) ? "AT THE MARK EVERY KART DROPS TO ONE BALLOON"
                                                      : "NO TIMER. THE LAST BALLOON STANDING WINS";
        case BROW_B_ITEMS:
            switch (items_choice()) {
                case ITEMS_RANDOM:   return "EVERY ROLL CAN BE ANY ITEM IN ANY PLACE";
                case ITEMS_SAME:     return "EVERYONE DRAWS THE ITEM PICKED BELOW";
                case ITEMS_FRANTIC:  return "POWER ITEMS ONLY";
                case ITEMS_TRIPLES:  return "EVERY ITEM COMES AS A TRIPLE";
                case ITEMS_INVERTED: return "THE LEADER GETS THE STRONG ITEMS";
                case ITEMS_NONE:     return "NO ITEM BOXES IN THE ARENA";
            }
            return "NORMAL BATTLE ITEM ODDS";
        case BROW_B_SAME_ITEM:
            return "THE ITEM EVERYONE DRAWS";
        case BROW_B_SIZE:
            return "TINY OR GIANT OR EVERY KART ITS OWN SIZE";
        case BROW_B_START:
            return "DEFEND YOUR BALLOONS";
    }
    return "";
}

static void battle_value_string(s32 row, char* out) {
    switch (row) {
        case BROW_BALLOONS: {
            s32 v = CVarGetInteger("gBattleBalloons", 3);
            sprintf(out, "%d", (v < 1 || v > 3) ? 3 : v);
            break;
        }
        case BROW_RIVALS: {
            s32 v = CVarGetInteger("gBattleRivals", 3);
            sprintf(out, "%d", (v < 1 || v > 3) ? 3 : v);
            break;
        }
        case BROW_HEAT: {
            s32 v = CVarGetInteger("gBattleCpuHeat", 1);
            sprintf(out, "%s", kHeatNames[(v >= 0 && v < 3) ? v : 1]);
            break;
        }
        case BROW_LOSERS:
            sprintf(out, "%s", kLoserNames[CVarGetInteger("gBattleLosers", 0) ? 1 : 0]);
            break;
        case BROW_SUDDEN: {
            s32 v = CVarGetInteger("gBattleSudden", 0);
            sprintf(out, "%s", kSuddenNames[(v >= 0 && v < 3) ? v : 0]);
            break;
        }
        case BROW_B_ITEMS:
            sprintf(out, "%s", kItemsNames[items_choice()]);
            break;
        case BROW_B_SAME_ITEM:
            if (items_choice() == ITEMS_SAME) {
                s32 item = CVarGetInteger("gItemModeItem", ITEM_BANANA);
                if (item < 1 || item > ITEM_MAX - 1) {
                    item = ITEM_BANANA;
                }
                sprintf(out, "%s", kItemNames[item]);
            } else {
                out[0] = (char) 0; // inapplicable - draw nothing
            }
            break;
        case BROW_B_SIZE:
            sprintf(out, "%s", kSizeNames[CVarGetInteger("gKartSize", 0) & 3]);
            break;
        default:
            out[0] = '\0'; // START BATTLE has no value
            break;
    }
}

// One line under the rows explaining what the hovered setting does, choice-aware where it matters.
static const char* row_help(void) {
    switch (sSel) {
        case ROW_ITEMS:
            switch (items_choice()) {
                case ITEMS_RANDOM:   return "EVERY ROLL CAN BE ANY ITEM IN ANY PLACE";
                case ITEMS_SAME:     return "EVERYONE DRAWS THE ITEM PICKED BELOW";
                case ITEMS_FRANTIC:  return "POWER ITEMS ONLY";
                case ITEMS_TRIPLES:  return "EVERY ITEM COMES AS A TRIPLE";
                case ITEMS_INVERTED: return "THE LEADER GETS THE STRONG ITEMS";
                case ITEMS_NONE:     return "NO ITEM BOXES ON THE TRACK";
            }
            return "NORMAL ITEM ODDS";
        case ROW_SAME_ITEM: return "THE ITEM EVERYONE DRAWS";
        case ROW_CPU_ITEMS: return "WHETHER CPUS FOLLOW THE ITEM MODE";
        case ROW_ITEM_RAIN: return "ITEM BOXES FALL FROM THE SKY AHEAD OF RACERS";
        case ROW_HAZARDS:
            switch (CVarGetInteger("gTrackHazards", 0)) {
                case 1: return "ONLY HAZARDS THAT BELONG ON THIS COURSE";
                case 2: return "ANY HAZARD ANYWHERE. BANANAS TO BOULDERS TO BOOS";
            }
            return "BANANAS SHELLS PIRANHAS EGGS ROCKS AND WORSE";
        case ROW_PROPS:
            switch (CVarGetInteger("gPropSwap", 0)) {
                case PROPS_CHOMPS:    return "TRAFFIC TRAINS AND PROPS BECOME CHAIN CHOMPS";
                case PROPS_BOMBKARTS: return "THE COURSE PROPS BECOME ROAMING BOMB KARTS";
                case PROPS_SNOWMEN:   return "THE COURSE PROPS BECOME EXPLODING SNOWMEN";
                case PROPS_PENGUINS:  return "THE COURSE PROPS BECOME WADDLING PENGUINS";
                case PROPS_MIXED:     return "A BIT OF EVERYTHING TAKES OVER THE COURSE";
                case PROPS_THEMED:    return "A MIX THAT FITS THE COURSE TAKES OVER";
            }
            return "COURSE PROPS AS NINTENDO SHIPPED THEM";
        case ROW_MODE:
            switch (CVarGetInteger("gRaceMode", 0)) {
                case 1: return "THE LAST KART TO REACH EACH NEW LAP IS STRUCK TINY";
                case 2: return "3 BALLOONS EACH. EVERY HIT POPS ONE";
                case 3: return "BUMP THE GIANT IT KART TO PASS IT ON";
                case 4: return "FIRST TO FIND THE HIDDEN BOX WINS INSTANTLY";
                case 5: return "ONE CARRIER SPREADS BY TOUCH. FINISH TO SURVIVE";
                default: return "A NORMAL RACE";
            }
        case ROW_BALLOON_OUT:
            if (CVarGetInteger("gRaceMode", 0) == 4) {
                return CVarGetInteger("gTreasureHidden", 0)
                           ? "BURIED ANYWHERE REACHABLE. NO MAP MARKER"
                           : "GIANT GOLD BOX MARKED ON THE MAP";
            }
            if (CVarGetInteger("gRaceMode", 0) != 2) {
                return "PICK THE BALLOONS OR TREASURE MODE TO USE THIS";
            }
            switch (CVarGetInteger("gBalloonOut", 0)) {
                case 1:  return "NO BALLOONS LEFT = OUT. LAST ONE STANDING WINS";
                case 2:  return "NO BALLOONS LEFT = SLOW BUT FULL SIZE";
                default: return "NO BALLOONS LEFT = TINY AND SLOW TO THE END";
            }
        case ROW_LAPS:
            if (laps_locked()) {
                return "TREASURE HUNT RUNS UNTIL THE BOX IS FOUND";
            }
            return laps_choice() == 5 ? "THE RACE NEVER ENDS" : "LAPS IN THE RACE";
        case ROW_SPEED:
            return speed_choice() == SPEED_TURBO ? "SPEED RISES 100CC EVERY LAP" : "ENGINE CLASS FOR THIS RACE";
        case ROW_MIRROR:   return "FLIP THE TRACK LEFT TO RIGHT";
        case ROW_SIZE:     return "TINY OR GIANT OR EVERY KART ITS OWN SIZE";
        case ROW_CPU_SKILL: return "HARD CPUS DRIVE FASTER LINES";
        case ROW_CATCHUP:   return "OFF STOPS CPUS CHEATING TO KEEP UP";
        case ROW_START:     return "OFF YOU GO";
    }
    return "";
}

void race_mods_open(void) {
    sOpen = 1;
    sSel  = 0;
    sBattle = 0;
    snapshot_setup_nav(0); // remember where we are, for the pause-menu RACE SETUP return
    reset_cycle_flash_menu();
}

void race_mods_open_battle(void) {
    sOpen = 1;
    sSel  = 0;
    sBattle = 1;
    snapshot_setup_nav(1);
    reset_cycle_flash_menu();
}

// The pause-menu R1 overlay's RACE SETUP row shows only when a setup screen has been visited.
int race_mods_has_setup(void) {
    return sSnapValid;
}

// Pause menu picked RACE SETUP: arm the return. The caller also runs the quit-to-main-menu
// transition; reopen_poll finishes the job once the menu is back up.
void race_mods_request_reopen(void) {
    if (sSnapValid) {
        sPendingReopen = 1;
    }
}

// Polled every frame from update_menus. Once the quit transition has settled on the main menu,
// drive the menu straight to the OK screen the setup overlay lives on and re-open it.
void race_mods_menu_reopen_poll(void) {
    s32 count;
    if (!sPendingReopen) {
        return;
    }
    if (is_screen_being_faded()) {
        return; // let the quit-to-menu wipe finish first
    }
    if (gMenuSelection != MAIN_MENU && gMenuSelection != MAIN_MENU_FROM_QUIT) {
        return; // not back at the main menu yet
    }
    count = (sSnapCount >= 1 && sSnapCount <= 4) ? sSnapCount : 1;
    gPlayerCount = (s8) count;
    gPlayerCountSelection1 = (s8) count;
    gModeSelection = sSnapMode;
    gGameModeMenuColumn[count - 1] = sSnapModeCol;
    gGameModeSubMenuColumn[count - 1][sSnapModeCol] = sSnapSubCol;
    gMainMenuSelection = MAIN_MENU_OK_SELECT;
    sPendingReopen = 0;
    if (sSnapBattle) {
        race_mods_open_battle();
    } else {
        race_mods_open();
    }
}

int custom_cc_row_index(void) {
    return has_unlocked_extra_mode() ? 4 : 3;
}

void race_mods_reset_to_stock(void) {
    CVarSetInteger("gItemMode", 0);
    CVarSetInteger("gItemRain", 0);
    CVarSetInteger("gTrackHazards", 0);
    CVarSetInteger("gPropSwap", 0);
    CVarSetInteger("gRaceMode", 0);
    CVarSetInteger("gLapsOverride", 0);
    CVarSetInteger("gMirrorForce", 0);
    CVarSetInteger("gKartSize", 0);
    CVarSetInteger("gSpeedTurboLaps", 0);
    CVarSetInteger("gDisableItemboxes", 0);
    CVarSetInteger("gEnableCustomCC", 0);
    CVarSetInteger("gDuelRival", 0);
    CVarSetInteger("gVersusDuel", 0);
    // gItemModeItem / gItemModeCPU / gBalloonOut keep their values - they're preferences, inert
    // while their parent mode is off. The port's own cheats (gHarderCPU / gDisableRubberbanding)
    // stay user-managed.
}

// The stock CC rows are baked texture art, so the CUSTOM row is drawn by hand in the same slot
// style: dark box, cycling flash while hovered, solid once chosen. Position and visibility ride
// the 50CC menu item so every player-count layout and screen transition stays in sync.
void custom_cc_draw_row(void) {
    MenuItem* base = find_menu_items(MAIN_MENU_50CC);
    s32 row, x, y, subMode, mode;
    if (base == NULL) {
        return;
    }
    // From the game-mode screen (GP highlighted, CC column previewing) through OK - the menu items
    // carry stale positions on screens before that, which drew the row in the wrong spot.
    if (gMainMenuSelection < MAIN_MENU_MODE_SELECT || gMainMenuSelection > MAIN_MENU_OK_SELECT_GO_BACK) {
        return;
    }
    // And only the CC class lists get the row - the Time Trials and Battle submenus share the same
    // menu-item pool, so without this gate the row rendered under their options too.
    mode = gGameModePlayerSelection[gPlayerCount - 1][gGameModeMenuColumn[gPlayerCount - 1]];
    if (mode != GRAND_PRIX && mode != VERSUS) {
        return;
    }
    row = custom_cc_row_index();
    x = base->column;
    y = base->row + row * 0x12; // the stock 18 px row pitch
    subMode = gGameModeSubMenuColumn[gPlayerCount - 1][gGameModeMenuColumn[gPlayerCount - 1]];

    if (gMainMenuSelection >= MAIN_MENU_MODE_SUB_SELECT && subMode == row) {
        if (gMainMenuSelection >= MAIN_MENU_OK_SELECT) {
            gDisplayListHead = draw_box_fill(gDisplayListHead, x, y, x + 0x3F, y + 0x11, 0xFF, 0xF9, 0xDC, 0xFF);
        } else {
            gDisplayListHead = draw_flash_select_case_slow(gDisplayListHead, x, y, x + 0x3F, y + 0x11);
        }
    } else {
        gDisplayListHead = draw_box_fill(gDisplayListHead, x, y, x + 0x3F, y + 0x11, 1, 1, 1, 0xFF);
    }
    set_text_color(TEXT_YELLOW);
    race_mods_text_fit_center(x + 0x20, y + 0xE, "CUSTOM", 0.55f, 56.0f); // inside the 63-wide row box
}

int race_mods_is_open(void) {
    return sOpen;
}

// The 1P VERSUS duel flow: live from the moment the VS row is picked on the main menu until the
// course OK commits the race onto the GP path (course_select_menu_act flips gModeSelection there).
int versus_1p_flow_active(void) {
    return gPlayerCount == 1 && gModeSelection == VERSUS;
}

// Character-screen guidance while the 1P VERSUS flow picks two karts with one controller.
void versus_1p_rival_hint(void) {
    if (!versus_1p_flow_active() || gMenuSelection != CHARACTER_SELECT_MENU) {
        return;
    }
    if (gPlayerSelectMenuSelection != PLAYER_SELECT_MENU_MAIN) {
        return; // both picked - the stock OK takes over
    }
    set_text_color(TEXT_YELLOW);
    if (gCharacterGridIsSelected[0]) {
        race_mods_text_fit_center(160, 214, "NOW PICK YOUR RIVAL", 0.5f, 280.0f);
    } else {
        race_mods_text_fit_center(160, 214, "PICK YOUR DRIVER", 0.5f, 280.0f);
    }
}

// In-race status line for the modes that need one. Infected: a flashing INFECTED warning while
// the human is a carrier, then the team verdict once the match is decided - the font has no
// colon glyph, so the banner reads INFECTED WINS / SURVIVORS WIN. Drawn from the HUD pass
// (code_80057C60.c), so it lands on the head-locked HUD plane in VR automatically.
void race_mods_draw_status_hud(void) {
    s32 status = race_mods_infected_status();
    const char* matchWin = race_mods_match_winner_text();
    const char* tagLose = race_mods_tag_loser_text();
    s32 tagHuman = 0;
    const char* tagText = race_mods_tag_banner_text(&tagHuman);

    // Tag's one-LOSER result is decided AT the finish line, so unlike every other banner it has no
    // in-race window - it must paint on the finish/score screen, BEFORE the score-screen bailout
    // below (which otherwise hid it instantly). Flashing red, centred, name and all.
    if (tagLose != NULL) {
        if (!(gGlobalTimer & 8)) {
            set_text_color(TEXT_RED);
            race_mods_text_fit_center(160, 100, tagLose, 0.7f, 220.0f);
        }
        return;
    }

    if (gRaceState >= RACE_FINISHED) {
        return; // the score screen / post-race menu owns the view - no banners or rows over it
    }

    // 1P battle WIN banner: when the match is decided, race_mods holds the result open a few
    // seconds (race_mods_battle_hold_winner) and we shout the winner big and centred so there is
    // no doubt who took it - YOU WIN, or the winning character's name.
    {
        const char* bw = race_mods_battle_winner_text();
        if (bw != NULL) {
            if (!(gGlobalTimer & 4)) { // a brisk flash so it pops
                set_text_color(TEXT_YELLOW);
                race_mods_text_fit_center(160, 110, bw, 0.9f, 250.0f);
            }
            return; // the win banner owns the screen
        }
    }

    // 1P battle warnings: SUDDEN DEATH at the timer mark, LAST BALLOON when the human is one
    // hit from a bomb kart. Flashing red mid-screen, the INFECTED alert idiom.
    {
        const char* bb = race_mods_battle_banner();
        if (bb != NULL) {
            if (!(gGlobalTimer & 8)) {
                set_text_color(TEXT_RED);
                race_mods_text_fit_center(160, 124, bb, 0.55f, 200.0f);
            }
            return; // nothing else draws in battle - the stock kit owns the rest of the HUD
        }
    }

    // Balloon match: the live balloon counts, one row per present kart - "YOU 3", "DK 2",
    // "TOAD OUT" (out rows red). Deliberately small and tucked against the left edge under
    // the item window: it's reference information, not a billboard - the first cut at 0.4
    // scale ate a third of the screen. Drawn under everything else (no early return).
    {
        char row[24];
        s32 y = 64;
        s32 i;
        for (i = 0; i < 8; i++) {
            s32 r = race_mods_balloon_hud_row(i, row, sizeof(row));
            if (r != 0) {
                set_text_color((r == 2) ? TEXT_RED : TEXT_YELLOW);
                race_mods_text_fit(10, y, row, 0.26f, 50.0f);
                y += 8;
            }
        }
    }

    // One-winner matches (balloon elimination, treasure hunt): the decided match gets an
    // unmissable mid-screen banner - YOU WIN, or the winning character's name.
    if (matchWin != NULL) {
        set_text_color(TEXT_YELLOW);
        race_mods_text_fit_center(160, 124, matchWin, 0.55f, 180.0f);
        return;
    }
    // (Tag's one-LOSER finish banner is drawn at the top of this function, before the score-screen
    // bailout, so it survives onto the finish screen.)
    // Tag: there must never be any doubt about who is IT. The human IT gets the flashing red
    // alert (the INFECTED idiom); anyone else's name sits below the item box (clear of it).
    if (tagText != NULL) {
        if (tagHuman) {
            if (gGlobalTimer & 8) {
                return; // flash so it reads as an alert
            }
            set_text_color(TEXT_RED);
            race_mods_text_fit_center(160, 198, tagText, 0.5f, 160.0f);
        } else {
            set_text_color(TEXT_YELLOW);
            // Tucked under the lap counter (~83,40) at the top-left, clear of the item box and the
            // rank portraits on the far left.
            race_mods_text_fit_center(100, 64, tagText, 0.45f, 110.0f);
        }
        return;
    }
    if (status == 0) {
        return;
    }
    // Sizes are deliberately conservative: HD font art renders wider and taller than the stock
    // glyph metrics report, so a "fitting" big banner still sprawled across the whole screen.
    if (status == 1) {
        if (gGlobalTimer & 8) {
            return; // flash so it reads as an alert
        }
        set_text_color(TEXT_RED);
        race_mods_text_fit_center(160, 198, "INFECTED", 0.5f, 160.0f);
        return;
    }
    // Mid-screen, clear of the item window above and the lap counter below.
    set_text_color(TEXT_YELLOW);
    race_mods_text_fit_center(160, 124, (status == 2) ? "SURVIVORS WIN" : "INFECTED WINS", 0.55f, 180.0f);
}

// Bottom-left hint on the course select screen: Z rerolls the course (or shuffles the GP cup order).
void track_roulette_draw_hint(void) {
    set_text_color(TEXT_YELLOW);
    race_mods_text_fit(18, 226, "Z RANDOM", 0.5f, 80.0f); // off the bottom edge for tall HD fonts
}

int race_mods_input(unsigned short btnAndStick) {
    s32 rowCount = sBattle ? BROW_COUNT : ROW_COUNT;
    s32 startRow = sBattle ? BROW_B_START : ROW_START;

    if (!sOpen) {
        return 0;
    }
    if (btnAndStick & B_BUTTON) { // back to the OK button
        sOpen = 0;
        play_sound2(SOUND_MENU_GO_BACK);
        return 1;
    }
    // reset_cycle_flash_menu on every press is the stock idiom: the selection highlight is an OPAQUE box
    // whose brightness rides the free-running gCycleFlashMenu wave (bright -> black -> bright, ~2s), and
    // the reset snaps it to full bright. Without it the bar strobes dark on its own schedule - reads as
    // the menu flickering while you change values.
    if (btnAndStick & U_JPAD) {
        sSel = (sSel + rowCount - 1) % rowCount;
        reset_cycle_flash_menu();
        play_sound2(SOUND_MENU_CURSOR_MOVE);
    }
    if (btnAndStick & D_JPAD) {
        sSel = (sSel + 1) % rowCount;
        reset_cycle_flash_menu();
        play_sound2(SOUND_MENU_CURSOR_MOVE);
    }
    if (btnAndStick & L_JPAD) {
        sBattle ? battle_row_change(-1) : row_change(-1);
        reset_cycle_flash_menu();
        play_sound2(SOUND_MENU_CURSOR_MOVE);
    }
    if (btnAndStick & R_JPAD) {
        sBattle ? battle_row_change(+1) : row_change(+1);
        reset_cycle_flash_menu();
        play_sound2(SOUND_MENU_CURSOR_MOVE);
    }
    if (btnAndStick & A_BUTTON) {
        if (sSel == startRow) {
            sOpen = 0;
            return 2; // caller runs the stock begin transition
        }
        sBattle ? battle_row_change(0) : row_change(0);
        reset_cycle_flash_menu();
        play_sound2(SOUND_MENU_SELECT);
    }
    return 1;
}

static void value_string(s32 row, char* out) {
    s32 items = items_choice();
    switch (row) {
        case ROW_ITEMS:
            sprintf(out, "%s", kItemsNames[items]);
            break;
        case ROW_SAME_ITEM:
            if (items == ITEMS_SAME) {
                s32 item = CVarGetInteger("gItemModeItem", ITEM_BANANA);
                if (item < 1 || item > ITEM_MAX - 1) {
                    item = ITEM_BANANA;
                }
                sprintf(out, "%s", kItemNames[item]);
            } else {
                out[0] = (char) 0; // inapplicable - draw nothing (HD dash art reads as cut-off dots)
            }
            break;
        case ROW_CPU_ITEMS:
            if (items != ITEMS_STOCK && items != ITEMS_NONE) {
                sprintf(out, "%s", CVarGetInteger("gItemModeCPU", 1) ? "YES" : "NO");
            } else {
                out[0] = (char) 0; // inapplicable - draw nothing (HD dash art reads as cut-off dots)
            }
            break;
        case ROW_ITEM_RAIN:
            sprintf(out, "%s", CVarGetInteger("gItemRain", 0) ? "ON" : "OFF");
            break;
        case ROW_HAZARDS:
            {
                static const char* const kHazardNames[3] = { "OFF", "THEMED", "WILD" };
                s32 hz = CVarGetInteger("gTrackHazards", 0);
                sprintf(out, "%s", kHazardNames[(hz >= 0 && hz < 3) ? hz : 0]);
            }
            break;
        case ROW_PROPS:
            {
                s32 ps = CVarGetInteger("gPropSwap", 0);
                sprintf(out, "%s", kPropsNames[(ps >= 0 && ps < PROPS_CHOICES) ? ps : 0]);
            }
            break;
        case ROW_MODE:
            {
                static const char* const kModeNames[6] = { "STOCK", "KNOCKOUT",      "BALLOONS",
                                                           "TAG",   "TREASURE HUNT", "INFECTED" };
                s32 m = CVarGetInteger("gRaceMode", 0);
                sprintf(out, "%s", kModeNames[(m >= 0 && m < 6) ? m : 0]);
            }
            break;
        case ROW_BALLOON_OUT:
            if (CVarGetInteger("gRaceMode", 0) == 2) {
                s32 b = CVarGetInteger("gBalloonOut", 0);
                sprintf(out, "%s", kBalloonOutNames[(b >= 0 && b < 3) ? b : 0]);
            } else if (CVarGetInteger("gRaceMode", 0) == 4) {
                sprintf(out, "%s", CVarGetInteger("gTreasureHidden", 0) ? "HIDDEN" : "ON THE MAP");
            } else {
                out[0] = (char) 0; // inapplicable - draw nothing (HD dash art reads as cut-off dots)
            }
            break;
        case ROW_LAPS:
            sprintf(out, "%s", laps_locked() ? "NO LIMIT" : kLapNames[laps_choice()]);
            break;
        case ROW_SPEED:
            sprintf(out, "%s", kSpeedNames[speed_choice()]);
            break;
        case ROW_MIRROR:
            sprintf(out, "%s", CVarGetInteger("gMirrorForce", 0) ? "ON" : "OFF");
            break;
        case ROW_SIZE:
            sprintf(out, "%s", kSizeNames[CVarGetInteger("gKartSize", 0) & 3]);
            break;
        case ROW_CPU_SKILL:
            sprintf(out, "%s", CVarGetInteger("gHarderCPU", 0) ? "HARD" : "STOCK");
            break;
        case ROW_CATCHUP:
            sprintf(out, "%s", CVarGetInteger("gDisableRubberbanding", 0) ? "OFF" : "ON");
            break;
        default:
            out[0] = '\0'; // START RACE has no value
            break;
    }
}

// The string's TRUE rendered extent in font units: the advance table the renderer steps by,
// plus each glyph's quad overhang (dX + width from its MenuTexture chain - italic fonts, and
// the wider art HD packs ship, lean well past their advance; get_string_width alone
// underreported and text still clipped). Slightly conservative: the unscaled dX offset is
// treated as scaling, which only ever shrinks a touch more.
static f32 text_extent(const char* text) {
    char* p = (char*) text;
    f32 advance = 0.0f;
    f32 extent = 0.0f;
    while (*p != 0) {
        s32 gi = char_to_glyph_index(p);
        if (gi >= 0) {
            MenuTexture* t = gGlyphTextureLUT[gi];
            while (t != NULL && t->textureData != NULL) { // a glyph can be several stacked quads
                f32 e = advance + (f32) t->dX + (f32) t->width;
                if (e > extent) {
                    extent = e;
                }
                t++;
            }
            advance += (f32) gGlyphDisplayWidth[gi];
        } else if (gi == -1) {
            advance += 7.0f;
        } else {
            break;
        }
        if (advance > extent) {
            extent = advance;
        }
        p += (gi >= 0x30) ? 2 : 1;
    }
    return extent;
}

// Draw text at the given scale, shrunk just enough that it never runs past maxWidth - whatever
// font is installed. Shared with the VR pause menu (race_mods_menu.h).
void race_mods_text_fit(s32 x, s32 y, const char* text, f32 scale, f32 maxWidth) {
    f32 w = text_extent(text) * scale;
    if (w > maxWidth && w > 0.0f) {
        scale *= maxWidth / w;
    }
    print_text_mode_1(x, y, (char*) text, 0, scale, scale);
}

// The font has no infinity glyph - two O's overlapped by half read as one. Used wherever the
// lap count is unlimited.
static void print_infinity(s32 x, s32 y, f32 scale) {
    s32 step = (s32) ((f32) get_string_width("O") * scale * 0.62f);
    if (step < 2) {
        step = 2;
    }
    print_text_mode_1(x, y, "O", 0, scale, scale);
    print_text_mode_1(x + step, y, "O", 0, scale, scale);
}

void race_mods_text_fit_center(s32 cx, s32 y, const char* text, f32 scale, f32 maxWidth) {
    f32 w = text_extent(text) * scale;
    if (w > maxWidth && w > 0.0f) {
        scale *= maxWidth / w;
    }
    print_text1_center_mode_1(cx, y, (char*) text, 0, scale, scale);
}

// Layout sized for tall fonts: HD packs ship italic art with real ascenders and descenders, and
// the stock-font spacing let the title's tails ride the selection bar and the footer's tails sit
// on the screen edge. Title high with a gap before row 0, baselines at 0.55 inside the 11px
// pitch, footer well off the 240 line.
void race_mods_draw(void) {
    s32 rowCount = sBattle ? BROW_COUNT : ROW_COUNT;
    s32 i;
    if (!sOpen) {
        return;
    }
    gDisplayListHead = draw_box_wide(gDisplayListHead, 20, 20, 300, 234, 0, 0, 40, 235);
    set_text_color(TEXT_YELLOW);
    race_mods_text_fit_center(160, 36, sBattle ? "BATTLE SETUP" : "RACE SETUP", 0.7f, 270.0f);

    for (i = 0; i < rowCount; i++) {
        // The race trim packs 15 rows at a 10px pitch; battle's 9 rows breathe at 14.
        s32 y = sBattle ? (58 + i * 14) : (53 + i * 10);
        const char* label = sBattle ? kBattleRowLabels[i] : kRowLabels[i];
        char val[32];
        if (!sBattle && i == ROW_BALLOON_OUT && CVarGetInteger("gRaceMode", 0) == 4) {
            label = "TREASURE BOX"; // the mode-option row wears the treasure hat in mode 4
        }
        if (i == sSel) {
            gDisplayListHead = draw_flash_select_case_slow(gDisplayListHead, 28, y - 8, 292, y + 2);
        }
        set_text_color(i == sSel ? TEXT_GREEN : TEXT_BLUE);
        race_mods_text_fit(32, y, label, 0.55f, 122.0f); // the value column starts at 158

        if (sBattle) {
            battle_value_string(i, val);
        } else {
            value_string(i, val);
        }
        if (!sBattle && i == ROW_LAPS && (laps_locked() || laps_choice() == LAP_CHOICES - 1)) {
            print_infinity(158, y, 0.55f); // unlimited laps draw as an infinity sign
        } else if (val[0] != '\0') {
            race_mods_text_fit(158, y, val, 0.55f, 132.0f); // the panel edge is 292
        }
    }

    set_text_color(TEXT_YELLOW);
    race_mods_text_fit_center(160, 211, sBattle ? battle_row_help() : row_help(), 0.48f, 272.0f);
    set_text_color(TEXT_BLUE);
    race_mods_text_fit_center(160, 224, "DPAD MOVE   LR CHANGE   A SELECT   B BACK", 0.48f, 272.0f);
}
