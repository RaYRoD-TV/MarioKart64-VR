// Native MK64-style race setup screen, shown between the main menu's OK and the character select.
// Items mode (truly random / everyone gets the same item / frantic / no boxes), speed class, CPU
// skill and rubberbanding - all the race mods in one place, drawn with the game's own box/glyph idiom.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// All input while the screen is up. Returns 0 when closed (the stock OK handling proceeds), 1 while
// the screen owns the input, 2 when START RACE was chosen this frame - the caller then runs the stock
// begin transition (wipe + click + setup_game_mode_selected).
int race_mods_input(unsigned short btnAndStick);
// Opens the screen (called when the CUSTOM CC row is picked).
void race_mods_open(void);
// Opens the screen in BATTLE trim (called when the 1P BATTLE row is picked): balloons, rivals,
// CPU heat, loser fate, sudden death, plus the shared items/size rows. START BATTLE proceeds.
void race_mods_open_battle(void);
int race_mods_is_open(void);
// Draws the screen. Called after the whole menu-item render pass so it sits on top of the stock widgets.
void race_mods_draw(void);
// The CC list's CUSTOM row index: right after EXTRA when it's unlocked, right after 150CC otherwise.
int custom_cc_row_index(void);
// Draws the hand-made CUSTOM row under the stock CC rows (they're baked texture art, this one is not).
void custom_cc_draw_row(void);
// Stock CC picks call this so a plain 50/100/150/EXTRA race is always a vanilla race.
void race_mods_reset_to_stock(void);
// "Z RANDOM" hint on the course select screen (the Z handling lives in course_select_menu_act).
void track_roulette_draw_hint(void);
// True while the 1P VERSUS duel flow owns the menus (VS row picked, course not yet committed).
int versus_1p_flow_active(void);
// "PICK YOUR DRIVER" / "NOW PICK YOUR RIVAL" guidance on the character screen in the 1P VS flow.
void versus_1p_rival_hint(void);
// Fit-to-width text: drawn at the given scale, shrunk so the TRUE rendered extent (advance table
// plus glyph quad overhang - HD font packs lean far past the advance) never passes maxWidth.
// Every menu string goes through these, never a raw fixed scale.
void race_mods_text_fit(s32 x, s32 y, const char* text, f32 scale, f32 maxWidth);
void race_mods_text_fit_center(s32 cx, s32 y, const char* text, f32 scale, f32 maxWidth);
// In-race status line (infected mode: the flashing INFECTED warning + the winner banner).
// Called from the HUD pass in code_80057C60.c.
void race_mods_draw_status_hud(void);

// Return-to-setup (the pause R1 overlay's RACE SETUP row). has_setup gates the row (only shows
// once a setup screen has been visited); request_reopen arms the return after the pause menu
// quits to the main menu; reopen_poll (called every frame from update_menus) finishes it by
// driving the menu back to the OK screen and re-opening the setup overlay with the last settings.
int  race_mods_has_setup(void);
void race_mods_request_reopen(void);
void race_mods_menu_reopen_poll(void);

#ifdef __cplusplus
}
#endif
