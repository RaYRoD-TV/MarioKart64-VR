// Native in-game options menu (pause + R1), VR AND flatscreen. See vr_pause_menu.h. Uses the game's own
// text/box rendering so it shows on the VR head-locked HUD plane automatically; edits the same CVars as the
// desktop ImGui menu. In VR it carries the full VR tuning set; on flatscreen it slims down to the flat view
// modes (Third Person / First Person / Diorama, gFlatViewMode) plus the shared race actions - the VR-only
// rows (world scale, stereo, flip cam) hide and the list compacts.
#include <libultraship.h>
#include <libultra/types.h>
#include <macros.h>
#include <mk64.h>
#include <defines.h>
#include <stdio.h>
#include "code_800029B0.h"
#include "menu_items.h"
#include "vr_pause_menu.h"
#include "race_mods_menu.h" // the shared fit-to-width text helpers

// --- externs we lean on (declared here to keep this file's includes light) -------------------------------
extern Gfx*           gDisplayListHead;
extern struct Controller gControllers[];

extern float CVarGetFloat(const char* name, float defaultValue);
extern void  CVarSetFloat(const char* name, float value);
extern int   CVarGetInteger(const char* name, int defaultValue);
extern void  CVarSetInteger(const char* name, int value);
extern bool  vr_is_active(void);
extern bool  vr_fp_switch_locked(void); // pre-race: block switching INTO First Person (skip it in the cycle)

// Stock pause-menu game actions + the unpause sequence (see the pause recon).
extern void func_8028DF38(void); // restore saved controller state (must run on unpause)
extern void func_800C9F90(s32);  // pause(1) / resume(0) audio
// Snaps the selection-highlight brightness wave (gCycleFlashMenu) to full bright. The wave only ticks in
// the menu gamestates, so in-race it is FROZEN at whatever phase course load left it - without this reset
// the highlight bar can sit near-black and the selection looks invisible.
extern void reset_cycle_flash_menu(void);

// --- state -----------------------------------------------------------------------------------------------
static s32 sOpen   = 0;
static s32 sSel    = 0;
static s32 sAction = 0; // 0 none / 2 restart / 3 quit - returned to func_800ADF48 which runs the real transition

enum {
    ROW_VIEWMODE,
    ROW_SCALE,      // mode-aware (Third/First/Diorama world scale)
    ROW_DISTANCE,   // mode-aware (Third dist / First forward / Diorama dist)
    ROW_EYEHEIGHT,  // mode-aware (Third/First/Diorama eye height)
    ROW_STEREO,
    ROW_OPACITY,
    ROW_HUDDIST,    // VR HUD plane distance (gVRHudDist)
    ROW_HUDHIDE,    // hide ALL race HUD info but the item box (gVRHideHud)
    ROW_HUDLOCK,    // HUD follows head (HEAD) or pins in the world so you can look around it (WORLD)
    ROW_FLIPCAM,    // First Person simulates spins / tumbles from the driver's seat
    ROW_EASEBACK,   // opt-in: pull back to the chase view during the drama instead
    ROW_DEFAULTS,
    ROW_RESTART,
    ROW_RACESETUP,  // back out to the game-mode customizations you raced from (custom / VS / battle)
    ROW_COURSESEL,  // back out to course select (the stock pause menu's Course Change)
    ROW_DRIVERSEL,  // back out to character select (the stock pause menu's Driver Change)
    ROW_QUIT,
    ROW_RESUME,
    ROW_COUNT
};

// No parentheses in labels - the menu font has no ( ) glyphs and renders them as stray letters.
static const char* const kRowLabels[ROW_COUNT] = {
    "VIEW MODE",     "WORLD SCALE",   "CAM DISTANCE",     "EYE HEIGHT",       "STEREO DEPTH",
    "MENU OPACITY",  "HUD DISTANCE",  "HIDE HUD",         "HUD LOCK",         "FLIP CAM FP",
    "EASE BACK FP",  "DEFAULT SETTINGS", "RESTART RACE",  "RACE SETUP",       "COURSE SELECT",
    "DRIVER SELECT", "QUIT TO MENU",  "RESUME GAME"
};
static const char* const kViewModeNames[4] = { "THIRD PERSON", "FIRST PERSON", "THEATER", "DIORAMA" };
static const char* const kFlatViewModeNames[3] = { "THIRD PERSON", "FIRST PERSON", "DIORAMA" };

// Flatscreen runs the same overlay with the flat CVar set; VR-only rows hide (see row_hidden).
static s32 menu_is_flat(void) {
    return !vr_is_active();
}

// For a mode-aware row, return the CVar name + adjust range for the CURRENT view mode. Returns NULL when the
// row doesn't apply to the active mode (e.g. Theater has no world scale). On flatscreen the rows map to the
// gFlat* camera knobs camera.c reads instead.
static const char* mode_cvar(s32 row, f32* lo, f32* hi, f32* step) {
    s32 vm;
    if (menu_is_flat()) {
        vm = CVarGetInteger("gFlatViewMode", 0);
        switch (row) {
            case ROW_DISTANCE: // diorama trail distance - the only flat mode with a distance knob
                if (vm == 2) { *lo = 0.0f;  *hi = 700.0f; *step = 10.0f; return "gFlatDioramaDist"; }
                return NULL;
            case ROW_EYEHEIGHT:
                if (vm == 1) { *lo = 1.0f;  *hi = 40.0f;  *step = 0.5f;  return "gFlatFPHeight"; }
                if (vm == 2) { *lo = 30.0f; *hi = 900.0f; *step = 10.0f; return "gFlatDioramaHeight"; }
                return NULL;
            default:
                return NULL;
        }
    }
    vm = CVarGetInteger("gVRViewMode", 0);
    switch (row) {
        case ROW_SCALE:
            if (vm == 0) { *lo = 5.0f;  *hi = 2000.0f; *step = 5.0f;  return "gVRWorldScale"; }
            if (vm == 1) { *lo = 5.0f;  *hi = 500.0f;  *step = 5.0f;  return "gVRFirstPersonScale"; }
            if (vm == 3) { *lo = 20.0f; *hi = 2000.0f; *step = 5.0f;  return "gVRDioramaWorldScale"; }
            return NULL;
        case ROW_DISTANCE:
            if (vm == 0) { *lo = -15.0f; *hi = 50.0f; *step = 1.0f;   return "gVRThirdPersonDist"; }
            if (vm == 1) { *lo = -10.0f; *hi = 15.0f; *step = 0.25f;  return "gVRFirstPersonFwd"; }
            if (vm == 3) { *lo = 0.1f;   *hi = 3.0f;  *step = 0.05f;  return "gVRDioramaDist"; }
            return NULL;
        case ROW_EYEHEIGHT:
            if (vm == 0) { *lo = -1.0f; *hi = 1.0f; *step = 0.02f; return "gVREyeHeight"; }
            if (vm == 1) { *lo = -1.0f; *hi = 1.0f; *step = 0.02f; return "gVRFirstPersonEyeHeight"; }
            if (vm == 3) { *lo = -1.5f; *hi = 1.0f; *step = 0.05f; return "gVRDioramaHeight"; }
            return NULL;
        default:
            return NULL;
    }
}

static void clampf_cvar(const char* name, f32 def, f32 lo, f32 hi, f32 step, s32 dir) {
    f32 v = CVarGetFloat(name, def) + (f32) dir * step; // start from the row's real default, not its min
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    CVarSetFloat(name, v);
}

// Flatscreen hides the VR-only rows and any tuning row the active flat mode has no knob for;
// the draw loop compacts the survivors so there are no gaps. VR shows the full list unchanged.
static s32 row_hidden(s32 row) {
    f32 lo, hi, step;
    // RACE SETUP shows (VR and flat alike) only when there's a customizations screen to go back to.
    if (row == ROW_RACESETUP) {
        return !race_mods_has_setup();
    }
    if (!menu_is_flat()) {
        return 0;
    }
    switch (row) {
        case ROW_SCALE:
        case ROW_STEREO:
        case ROW_FLIPCAM:
        case ROW_EASEBACK:
        case ROW_HUDDIST:  // VR HUD plane - no flatscreen equivalent
        case ROW_HUDHIDE:
        case ROW_HUDLOCK:
            return 1;
        case ROW_DISTANCE:
        case ROW_EYEHEIGHT:
            return mode_cvar(row, &lo, &hi, &step) == NULL;
        default:
            return 0;
    }
}

// Reset the CURRENT view mode's tuning (plus the shared knobs) back to the defaults. The view mode
// itself and the other modes' tuning are left alone - resetting while dialing in Diorama shouldn't
// dump you back into Third Person or wipe your First Person setup.
static void reset_defaults(void) {
    if (menu_is_flat()) {
        switch (CVarGetInteger("gFlatViewMode", 0)) {
            case 1: // First Person
                CVarSetFloat("gFlatFPHeight", 8.0f);
                break;
            case 2: // Diorama - Ray's tuned framing
                CVarSetFloat("gFlatDioramaHeight", 50.0f);
                CVarSetFloat("gFlatDioramaDist", 190.0f);
                break;
            default: // Third Person is the stock chase cam - nothing to reset
                break;
        }
        return;
    }
    switch (CVarGetInteger("gVRViewMode", 0)) {
        case 0: // Third Person
            CVarSetFloat("gVRWorldScale", 20.0f);
            CVarSetFloat("gVRThirdPersonDist", 0.0f);
            CVarSetFloat("gVREyeHeight", 0.16f);
            break;
        case 1: // First Person
            CVarSetFloat("gVRFirstPersonScale", 20.0f);
            CVarSetFloat("gVRFirstPersonFwd", 3.0f);
            CVarSetFloat("gVRFirstPersonEyeHeight", 0.0f);
            break;
        case 3: // Diorama
            CVarSetFloat("gVRDioramaWorldScale", 115.0f);
            CVarSetFloat("gVRDioramaDist", 0.30f);
            CVarSetFloat("gVRDioramaHeight", -0.15f);
            break;
        default: // Theater has no per-mode tuning here
            break;
    }
    CVarSetFloat("gVRStereo", 0.5f);
    CVarSetFloat("gVRMenuOpacity", 0.5f);
    CVarSetFloat("gVRHudDist", 2.90f);
    CVarSetInteger("gVRHideHud", 0);
    CVarSetInteger("gVRHudWorldLock", 1); // HUD world-locked by default
    CVarSetInteger("gVRFlipCam", 0);
    CVarSetInteger("gVRFPDrama", 1);
}

// Adjust the selected row by dir (-1 left / +1 right), or activate it when dir==0 (A button).
static void row_change(s32 dir) {
    switch (sSel) {
        case ROW_VIEWMODE: {
            if (menu_is_flat()) {
                s32 m = CVarGetInteger("gFlatViewMode", 0);
                m = (m + (dir == 0 ? 1 : dir) + 3) % 3; // Third / First / Diorama
                CVarSetInteger("gFlatViewMode", m);
            } else {
                s32 step = (dir == 0 ? 1 : dir);
                s32 m = (CVarGetInteger("gVRViewMode", 0) + step + 4) & 3; // A or right -> next, left -> prev
                if (m == 1 /* First Person */ && vr_fp_switch_locked()) {
                    m = (m + step + 4) & 3; // pre-race: can't switch INTO First Person; skip over it
                }
                CVarSetInteger("gVRViewMode", m);
            }
            break;
        }
        case ROW_SCALE:
        case ROW_DISTANCE:
        case ROW_EYEHEIGHT: {
            if (dir != 0) {
                f32 lo, hi, step;
                const char* cv = mode_cvar(sSel, &lo, &hi, &step);
                if (cv != NULL) { clampf_cvar(cv, lo, lo, hi, step, dir); } // def = lo (matches value_string)
            }
            break;
        }
        case ROW_STEREO:   if (dir != 0) clampf_cvar("gVRStereo",      0.5f, 0.0f, 1.5f, 0.05f, dir); break;
        case ROW_OPACITY:  if (dir != 0) clampf_cvar("gVRMenuOpacity", 0.5f, 0.3f, 1.0f, 0.05f, dir); break;
        case ROW_HUDDIST:  if (dir != 0) clampf_cvar("gVRHudDist",     2.90f, 0.5f, 15.0f, 0.5f, dir); break;
        case ROW_HUDHIDE:  CVarSetInteger("gVRHideHud", !CVarGetInteger("gVRHideHud", 0)); break;
        case ROW_HUDLOCK:  CVarSetInteger("gVRHudWorldLock", !CVarGetInteger("gVRHudWorldLock", 1)); break;
        case ROW_FLIPCAM:  CVarSetInteger("gVRFPDrama", !CVarGetInteger("gVRFPDrama", 1)); break;
        case ROW_EASEBACK: CVarSetInteger("gVRFlipCam", !CVarGetInteger("gVRFlipCam", 0)); break;
        case ROW_DEFAULTS: if (dir == 0) reset_defaults(); break;
        case ROW_RESTART:  if (dir == 0) { sOpen = 0; sAction = 2; } break; // restart - real transition in func_800ADF48
        case ROW_QUIT:     if (dir == 0) { sOpen = 0; sAction = 3; } break; // quit    - real transition in func_800ADF48
        case ROW_COURSESEL: if (dir == 0) { sOpen = 0; sAction = 4; } break; // course select (Course Change)
        case ROW_DRIVERSEL: if (dir == 0) { sOpen = 0; sAction = 5; } break; // character select (Driver Change)
        case ROW_RACESETUP: if (dir == 0) { sOpen = 0; sAction = 6; } break; // back to the game-mode customizations
        case ROW_RESUME:   if (dir == 0) {                                  // unpause straight to the race
            sOpen = 0;
            gIsGamePaused = 0;
            func_8028DF38();
            func_800C9F90(0);
        } break;
    }
}

int vr_pause_menu_input(void) {
    if (gIsGamePaused == 0) { sOpen = 0; return 0; } // VR and flatscreen both get the overlay
    // Stick edges count as d-pad presses so the menu navigates with the thumbstick too - that's
    // the only d-pad a VR motion controller has. (stickPressed carries the same U/D/L/R_JPAD bits.)
    u16 pressed = gControllers[0].buttonPressed | gControllers[0].stickPressed;

    if (pressed & R_TRIG) {            // R1 toggles the overlay
        sOpen = !sOpen;
        if (sOpen) {
            sSel = 0;
            reset_cycle_flash_menu();
        }
        return 1;                      // consume the R1 either way
    }
    if (!sOpen) return 0;

    sAction = 0;
    if (pressed & U_JPAD)   { do { sSel = (sSel + ROW_COUNT - 1) % ROW_COUNT; } while (row_hidden(sSel)); reset_cycle_flash_menu(); }
    if (pressed & D_JPAD)   { do { sSel = (sSel + 1) % ROW_COUNT; } while (row_hidden(sSel)); reset_cycle_flash_menu(); }
    if (pressed & L_JPAD)   { row_change(-1); reset_cycle_flash_menu(); }
    if (pressed & R_JPAD)   { row_change(+1); reset_cycle_flash_menu(); }
    if (pressed & A_BUTTON) { row_change(0); reset_cycle_flash_menu(); }
    if (pressed & B_BUTTON) { sOpen = 0; } // back to the stock pause menu
    if (row_hidden(sSel)) {
        // A view-mode change can hide the selected tuning row (flat Third Person has no knobs) -
        // hop to the next visible row so the highlight never vanishes.
        do { sSel = (sSel + 1) % ROW_COUNT; } while (row_hidden(sSel));
    }
    if (sAction != 0) { return sAction; }  // 2 restart / 3 quit -> caller runs the transition
    return 1;                              // while open, we eat all input
}

static void value_string(s32 row, char* out) {
    switch (row) {
        case ROW_VIEWMODE: {
            if (menu_is_flat()) {
                s32 m = CVarGetInteger("gFlatViewMode", 0);
                sprintf(out, "%s", kFlatViewModeNames[(m >= 0 && m < 3) ? m : 0]);
            } else {
                s32 m = CVarGetInteger("gVRViewMode", 0);
                sprintf(out, "%s", kViewModeNames[m & 3]);
            }
            break;
        }
        case ROW_SCALE:
        case ROW_DISTANCE:
        case ROW_EYEHEIGHT: {
            f32 lo, hi, step;
            const char* cv = mode_cvar(row, &lo, &hi, &step);
            if (cv != NULL) { sprintf(out, "%.2f", CVarGetFloat(cv, lo)); }
            else            { out[0] = (char) 0; } // inapplicable - draw nothing
            break;
        }
        case ROW_STEREO:   sprintf(out, "%.2f", CVarGetFloat("gVRStereo", 0.5f)); break;
        case ROW_OPACITY:  sprintf(out, "%.2f", CVarGetFloat("gVRMenuOpacity", 0.5f)); break;
        case ROW_HUDDIST:  sprintf(out, "%.2f", CVarGetFloat("gVRHudDist", 2.90f)); break;
        case ROW_HUDHIDE:  sprintf(out, "%s", CVarGetInteger("gVRHideHud", 0) ? "ON" : "OFF"); break;
        case ROW_HUDLOCK:  sprintf(out, "%s", CVarGetInteger("gVRHudWorldLock", 1) ? "WORLD" : "HEAD"); break;
        case ROW_FLIPCAM:  sprintf(out, "%s", CVarGetInteger("gVRFPDrama", 1) ? "ON" : "OFF"); break;
        case ROW_EASEBACK: sprintf(out, "%s", CVarGetInteger("gVRFlipCam", 0) ? "ON" : "OFF"); break;
        default:           out[0] = '\0'; break; // actions have no value
    }
}

void vr_pause_menu_draw(void) {
    if (!sOpen) return;

    // Backing panel. Its opacity follows gVRMenuOpacity - this IS the VR menu (drawn into the eyes), so the
    // "Menu Opacity" row makes THIS panel see-through over the race. draw_box_wide ALPHA-BLENDS (draw_box_fill
    // is an opaque fast-fill). Floored so it stays readable.
    s32 panelA = (s32) (CVarGetFloat("gVRMenuOpacity", 0.5f) * 255.0f);
    if (panelA < 60)  panelA = 60;
    if (panelA > 255) panelA = 255;
    // Top edge at 4 so the panel also covers the rank portraits (1st-4th) the game draws at the upper left.
    gDisplayListHead = draw_box_wide(gDisplayListHead, 16, 2, 304, 238, 0, 0, 40, (u32) panelA);
    set_text_color(TEXT_YELLOW);
    race_mods_text_fit_center(160, 30, menu_is_flat() ? "VIEW OPTIONS" : "VR OPTIONS", 0.72f, 276.0f);

    // Gather the visible (non-hidden) rows, then draw a SCROLLING window of them at a readable 12px
    // pitch. The list grows past what fits, so instead of cramming we slide the window to keep the
    // selected row on screen. Footer always sits just below the last drawn row (never overlaps it).
    s32 vis[ROW_COUNT];
    s32 nvis   = 0;
    s32 selPos = 0;
    s32 i;
    for (i = 0; i < ROW_COUNT; i++) {
        if (row_hidden(i)) continue;
        if (i == sSel) selPos = nvis;
        vis[nvis++] = i;
    }

    enum { MAXSHOW = 14, PITCH = 12, ROW0Y = 48 };
    s32 start = 0;
    if (nvis > MAXSHOW) {
        start = selPos - MAXSHOW / 2;           // center the selection
        if (start < 0) start = 0;
        if (start > nvis - MAXSHOW) start = nvis - MAXSHOW;
    }
    s32 end = start + MAXSHOW;
    if (end > nvis) end = nvis;

    s32 k;
    for (k = start; k < end; k++) {
        s32 row = vis[k];
        s32 y = ROW0Y + (k - start) * PITCH;
        if (row == sSel) {
            // print_text's y is ~the baseline (glyphs draw UPWARD from y), so the highlight sits ABOVE y.
            gDisplayListHead = draw_flash_select_case_slow(gDisplayListHead, 26, y - 10, 294, y + 2);
        }
        set_text_color(row == sSel ? TEXT_GREEN : TEXT_BLUE);
        race_mods_text_fit(38, y, kRowLabels[row], 0.55f, 136.0f); // the value column starts at 178

        char val[32];
        value_string(row, val);
        if (val[0] != '\0') {
            race_mods_text_fit(178, y, val, 0.55f, 114.0f); // the panel edge is 294
        }
    }

    // Footer just below the last drawn row.
    set_text_color(TEXT_BLUE);
    race_mods_text_fit_center(160, ROW0Y + (end - start) * PITCH + 6, "DPAD MOVE   LR CHANGE   A SELECT   R1 CLOSE", 0.46f, 276.0f);
}

int vr_pause_menu_is_open(void) {
    return sOpen;
}
