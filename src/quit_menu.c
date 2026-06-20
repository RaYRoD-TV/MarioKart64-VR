// Native MK64-style QUIT button on the main menu's bottom row. Visuals clone the L OPTION / R DATA boxes
// (same row, same 64-px pitch, same idle box / flashing-select / fold-away animation); the label uses the
// game glyph font in the button-hint convention ("Z QUIT" like "L OPTION"). Quitting plays the native menu
// click, fades the music, runs the stock colored menu wipe, then exits through GameEngine_RequestQuit -
// byte-identical teardown to clicking the window X.
#include <libultraship.h>
#include <libultra/types.h>
#include <macros.h>
#include <mk64.h>
#include <defines.h>
#include <sounds.h>
#include "menu_items.h"
#include "menus.h"
#include "audio/external.h"
#include "quit_menu.h"

extern Gfx* gDisplayListHead;

// Clean app quit bridge (src/port/Engine.cpp): backend mIsRunning = false -> SDL_main's
// while (WindowIsRunning()) exits -> CustomEngineDestroy() + GameEngine::Destroy().
extern void GameEngine_RequestQuit(void);

// Bottom row, continuing the stock 64-px pitch: OPTION x=0x15, DATA x=0x55, QUIT x=0x95 (OK sits at 0x10A).
#define QUIT_X 0x95
#define QUIT_Y 0xC8
#define QUIT_W 58 // the OPTION/DATA box texture metrics (58x19) - see seg2_textureMenuLOption
#define QUIT_H 19

// Frames until the actual quit. The native wipe started in quit_menu_trigger runs 20 frames, so the app
// exits mid-wipe (flash + click + fade-out are all visible) and the wipe's completion dispatch never runs.
static s32 sQuitCountdown = 0;
static s32 sConfirm = 0; // A YES / B NO dialog up

int quit_menu_input(unsigned short btnAndStick) {
    if (sQuitCountdown > 0) {
        return 1; // quit pending - the wipe gates input too, but don't let the stock chain race it
    }
    if (sConfirm) {
        if (btnAndStick & A_BUTTON) {
            sConfirm = 0;
            quit_menu_trigger();
        } else if (btnAndStick & (B_BUTTON | CONT_G)) {
            sConfirm = 0;
            play_sound2(SOUND_MENU_GO_BACK);
        }
        return 1; // dialog owns all input while it's up (dpad must not move the selection behind it)
    }
    if (btnAndStick & CONT_G) {
        sConfirm = 1;
        play_sound2(SOUND_MENU_SELECT);
        return 1;
    }
    return 0;
}

void quit_menu_trigger(void) {
    if (sQuitCountdown != 0) {
        return;
    }
    sQuitCountdown = 15;
    play_sound2(SOUND_MENU_OK_CLICKED);
    func_800CA330(0x19);                   // fade the menu music out (same call as the B-back path)
    gMenuFadeType = MENU_FADE_TYPE_OPTION; // belt + braces: if the wipe ever completed, it lands on Options
    func_8009DFE0(20);                     // native colored menu wipe; also gates all menu input via
                                           // is_screen_being_faded() so nothing else can fire meanwhile
}

void quit_menu_update(void) {
    if (sQuitCountdown > 0 && --sQuitCountdown == 0) {
        GameEngine_RequestQuit();
    }
}

void quit_menu_draw_box(void) {
    // Mirror the sibling OPTION box's param1 so QUIT folds away in lock-step when the menu advances to
    // mode select (param1 animates 0 -> 0x20 = collapsed) and expands back with it.
    MenuItem* opt = find_menu_items(MAIN_MENU_OPTION_GFX);
    s32 p = (opt != NULL) ? opt->param1 : 0;
    if (p >= 0x20) {
        return;
    }
    s32 t = (p * 0x3A) / 64; // the stock horizontal collapse amount
    if (sQuitCountdown > 0 || sConfirm) {
        gDisplayListHead = draw_flash_select_case_fast(gDisplayListHead, QUIT_X + 1 + t, QUIT_Y,
                                                       (QUIT_X - t) + QUIT_W - 1, QUIT_Y + QUIT_H - 2);
    } else {
        gDisplayListHead = draw_box_fill(gDisplayListHead, QUIT_X + 1 + t, QUIT_Y, (QUIT_X - t) + QUIT_W - 1,
                                         QUIT_Y + QUIT_H - 2, 1, 1, 1, 0x000000FF);
    }
    if (p == 0) { // label only when fully expanded (the texture boxes collapse their art; text can't)
        set_text_color(TEXT_YELLOW);
        print_text1_center_mode_1(QUIT_X + QUIT_W / 2, QUIT_Y + QUIT_H - 4, "Z QUIT", 0, 0.6f, 0.6f);
    }
}

void quit_menu_draw_confirm(void) {
    if (!sConfirm) {
        return;
    }
    // Drawn from the tail of the menu-item render pass, so the dialog covers the player-select widgets.
    gDisplayListHead = draw_box_wide(gDisplayListHead, 84, 94, 236, 150, 0, 0, 40, 235);
    set_text_color(TEXT_YELLOW);
    print_text1_center_mode_1(160, 118, "QUIT GAME?", 0, 0.8f, 0.8f);
    set_text_color(TEXT_BLUE);
    print_text1_center_mode_1(160, 142, "A YES   B NO", 0, 0.7f, 0.7f);
}
