// Native MK64-style QUIT button on the main menu's bottom row (next to L OPTION / R DATA, press Z).
// Drawn with the game's own box/flash/glyph idiom so it matches the stock menu; quits the app through
// the same clean shutdown path as the window close button.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// All QUIT input on the game-select (player count) screen. Z opens an A YES / B NO confirm dialog;
// A starts the click sfx + flash + music fade + native menu wipe + clean app quit a few frames later;
// B (or Z again) cancels. Returns nonzero when the press was consumed (dialog open or quit pending) -
// the caller must then skip the stock input handling for the frame.
int quit_menu_input(unsigned short btnAndStick);
// Starts the actual quit sequence. Idempotent while a quit is already pending.
void quit_menu_trigger(void);
// Ticks the pending-quit countdown. Called once per frame from the main menu's item update.
void quit_menu_update(void);
// Draws the bottom-row QUIT box (mirrors the OPTION/DATA collapse animation + flash highlight).
void quit_menu_draw_box(void);
// Draws the QUIT confirm dialog. Called after the whole menu-item render pass so it sits on top.
void quit_menu_draw_confirm(void);

#ifdef __cplusplus
}
#endif
