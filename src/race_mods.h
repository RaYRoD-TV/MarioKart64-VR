// Runtime race mutators configured on the race setup screen (race_mods_menu.c): knockout
// elimination, turbo laps, kart size, item rain, lap-count override. Ticked once per logic
// frame from process_game_tick; all state self-resets while the race is staging, so restarts
// and the next GP course start clean.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Once per logic frame during the race gamestate.
void race_mods_tick(void);
// Append one line to racemods_diag.txt (printf-style). The heavy-hazard bridges and the heavies'
// first tick/draw log through this so a single race localizes a broken chain.
void race_mods_diag_line(const char* fmt, ...);
// Once per logic frame: reports the player kart's dramatic state to the VR flip cam.
void race_mods_action_cam_tick(void);
// Total laps for the current race: 3 unless the LAPS row overrides it (GP and VS only).
// Consumed by the finish-line / HUD / music comparisons that used to hardcode 3.
int race_mods_total_laps(void);
// LOW GRAVITY mode: factor player_controller.c multiplies kart gravity by (1.0 = off). Floaty karts.
float race_mods_gravity_scale(void);

// True while the BALLOONS race mode is live: code_80057C60.c's battle-balloon tick and render
// gates let the balloons through outside battle mode when this reports active.
int race_mods_balloons_active(void);
// True while a balloon-ELIMINATION race is live - one winner only, so the HUD's place displays
// (position number, rank portrait ladder) hide while this reports active.
int race_mods_balloon_elim_active(void);
// Balloon HUD feed: fills seat i's count row ("YOU 3" / "DK 2" / "TOAD OUT") and returns
// 0 hidden / 1 live / 2 out. race_mods_draw_status_hud renders the column.
int race_mods_balloon_hud_row(int i, char* out, int outSize);

// Infected-mode HUD feed: 0 nothing to show / 1 the human is a carrier / 2 survivors won /
// 3 the infection won. race_mods_draw_status_hud (race_mods_menu.c) renders it.
int race_mods_infected_status(void);
// True while kart i carries the infection in a live infected race. Star-gated systems that must
// keep working for carriers (CPU item strategy, the human mushroom trigger, the item-use
// blacklist) check this.
int race_mods_infected_carrier(int i);
// True while an infected race runs: the GP flow always advances to the next course (no
// game-over on a bad placement - the team verdict is the result).
int race_mods_infected_active(void);
// Per-seat ladder feed: 0 nothing / 1 survivor racing / 2 carrier / 3 escaped (finished clean).
int race_mods_infected_marker(int i);
// The square infected status ladder (render_objects.c draws it in place of the rank portraits).
void race_mods_draw_infected_ladder(void);
// effects.c trigger_star reports every star use here; carriers get a hunting speed surge for
// the star's window, everyone else no-ops.
void race_mods_infected_star_used(int playerId);
// Carrier-CPU hunt order (code_80005FD0.c): fills the nearest clean kart's position and returns
// 1 while playerId should be hunting instead of racing the line. NULL outs allowed.
int race_mods_infected_hunt_target(int playerId, float* outX, float* outY, float* outZ);
// The umbrella CPU hunt authority the steering hooks actually call: 0 race the line / 1 infected
// chase (flat out) / 2 treasure dive (flat out) / 3 treasure dive close-in (curve throttle).
int race_mods_cpu_hunt_target(int playerId, float* outX, float* outY, float* outZ);

// One-winner match banner (balloon elimination's last-standing AND the treasure hunt's finder):
// NULL until the match is decided, then "YOU WIN" or "<CHARACTER> WINS" for the rest of the
// race (race_mods_draw_status_hud renders it).
const char* race_mods_match_winner_text(void);
// Tag's one-LOSER finish banner: NULL until the loser (most time IT) is frozen at the human's
// finish, then "YOU LOSE" or "<CHARACTER> LOSES" (race_mods_draw_status_hud renders it red).
const char* race_mods_tag_loser_text(void);
// True when playerId finished on the LOSING side (another kart took the treasure; the carriers lost
// the outbreak; the tag loser spent the longest IT) - the finish fanfare and voice (audio/external.c)
// play the losing reaction regardless of the frozen rank slot.
int race_mods_finish_demoted(int playerId);
// The mirror of the above: true when playerId should read as a winner (every tag survivor that
// isn't the one loser) but finished in a lower slot - the finish reaction promotes them to cheer.
int race_mods_finish_promoted(int playerId);
// One-winner modes (infected, balloon elimination): the HUD's place displays hide.
int race_mods_hide_place_hud(void);

// High-precision steering for the big speed classes (player_controller.c's stick extractor
// routes through it): bends the linear stick map toward squared response above 200cc so fine
// corrections stay fine at speed. Identity at stock classes.
int race_mods_steering_soften(int stick);

// Tag: the IT seat while a tag race runs (-1 otherwise)...
int race_mods_tag_it(void);
// ...the render-side rainbow flash for the IT kart (render_player.c's color pass - the star
// FLAG isn't used, it would change the physics)...
int race_mods_tag_rainbow(int i);
// ...and the HUD banner: "YOU ARE IT" (sets *outHumanIsIt, drawn flashing red) or
// "<CHARACTER> IS IT". NULL outside tag races.
const char* race_mods_tag_banner_text(int* outHumanIsIt);

// EXISTS test for HUD draws (rank ladder, minimap): false for the duel's culled seats, so a
// two-kart race shows two portraits and two dots, not the full GP eight.
int race_mods_player_exists(int i);

// Treasure hunt: where the prize sits (runtime world x/z; returns 0 when no hunt is live) and
// which actor slot the prize box occupies (-1 when none) - the minimap marker and the giant-box
// render read these.
int race_mods_treasure_world_pos(float* outX, float* outZ);
int race_mods_treasure_actor_index(void);
// True once the prize is claimed - the HUD lap counter hides itself so the force-finish's jump to the
// NO-LIMIT lap total isn't shown ticking up at the end of a treasure hunt.
int race_mods_treasure_decided(void);

// PROP SWAP (the setup screen's PROP SWAP row): courses with signature movers ask this at
// BeginPlay - nonzero (the mode: 1 chomps / 2 bomb karts / 3 snowmen / 4 penguins / 5 mixed)
// means skip the stock props - then register how many stood down. race_mods spawns the
// replacement pack along the racing line as staging begins.
int race_mods_prop_swap_live(void);
void race_mods_prop_swap_register(int skippedCount);

// 1P BATTLE (the main menu's BATTLE row for one player - real battle mode, arenas and balloons,
// against CPU rivals). The arenas have no waypoint paths, so the rivals drive by line of sight:
// update_player's battle branch asks for a target every tick instead of running the path follower.
// Returns 0 = not a 1P-battle CPU (stock logic owns the kart), 1 = drive at the filled target,
// 2 = out of the match (coast - the bomb kart AI owns the kart now). NULL outs allowed.
int race_mods_battle_cpu_target(int playerId, float* outX, float* outY, float* outZ);
// Throttle scale for the rivals: CALM 0.8 / MEAN 1.0 / RABID 1.2 (the setup screen's CPU HEAT row).
float race_mods_battle_cpu_heat(void);
// Edge-aware steering for a 1P-battle rival: given the opponent it wants to attack (oppX/oppZ),
// returns a steering point (outX/outZ) that aims at the opponent UNLESS a drop is that way, in
// which case it fans out to the safe heading closest to the opponent - so rivals hunt without
// diving off the arena's ledges. *outSlow is set to 1 when no safe heading exists (the caller
// eases off the gas). Probes the floor ahead with the collision mesh.
void race_mods_battle_steer(int playerId, float oppX, float oppZ, float* outX, float* outZ, int* outSlow);
// Where a 1P-battle rival drives: a point along a committed roam heading (mostly toward the nearest
// item box, sometimes a random sweep) that refreshes every few seconds. They travel ALONG it rather
// than re-aiming each frame, so they prowl and gather without orbiting an unreachable box. The
// attack pass shells nearby racers separately - a free-for-all.
void race_mods_battle_roam_target(int playerId, float* outX, float* outZ);
// Called when a rival wedges: drops its committed roam heading so it strikes off a new way next tick.
void race_mods_battle_roam_repick(int playerId);
// In-race banner feed for the battle status HUD: "SUDDEN DEATH" / "LAST BALLOON" flashes, NULL
// when there is nothing to shout about.
const char* race_mods_battle_banner(void);
// Battle end: update_player_battle_status (race_logic.c) calls this when a 1P battle is decided.
// Returns 1 to HOLD the match open a few seconds so the WIN banner shows, 0 once that grace is up
// (then the stock result screen fires). Multiplayer battle always gets 0 (unchanged).
int race_mods_battle_hold_winner(int winnerId);
// The big decided-match banner: "YOU WIN" / "<CHARACTER> WINS", NULL until decided.
const char* race_mods_battle_winner_text(void);

// Duel spawn filter (spawn_players.c drives these so a duel is two karts from the first frame):
// reset at the top of every spawn pass...
void race_mods_duel_spawn_reset(void);
// ...then the rival lands in the gPlayerTwo seat (returns 1, or -1 when it isn't a 1P GP duel)...
int race_mods_duel_spawn_prepare(void);
// ...the grid remap that puts the pair on the front row (slots = D_80165270)...
void race_mods_duel_grid_remap(int rivalIdx, short* slots);
// ...and the cull that removes everyone else right after the eight karts spawn.
void race_mods_duel_spawn_filter(int rivalIdx);

#ifdef __cplusplus
}
#endif
