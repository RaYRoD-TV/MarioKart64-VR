#ifndef VR_PAUSE_MENU_H
#define VR_PAUSE_MENU_H

// Native in-game VR options menu, opened by pressing R1 while the race is paused. Drawn with the game's own
// UI (print_text / draw_box) so it lands on the head-locked HUD plane in VR automatically. Coexists with the
// stock pause menu and edits the same VR CVars as the desktop (ImGui) menu.

#ifdef __cplusplus
extern "C" {
#endif

// Called from the stock pause input handler. Toggles the overlay open on a fresh R1 press while paused, and
// while open it handles navigation. Returns nonzero if it consumed input this frame (caller then SKIPS the
// stock pause-menu navigation so the two don't fight).
int vr_pause_menu_input(void);

// Draw the overlay if open. Call from the pause render path, AFTER the 3D world has rendered (so the VR HUD
// remap puts it on the head-locked plane). No-op when closed.
void vr_pause_menu_draw(void);

// Is the overlay currently open? (lets the stock pause menu hide its own input/visuals while ours is up)
int vr_pause_menu_is_open(void);

#ifdef __cplusplus
}
#endif

#endif // VR_PAUSE_MENU_H
