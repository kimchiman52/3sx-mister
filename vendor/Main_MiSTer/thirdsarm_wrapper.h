#ifndef THIRDSARM_WRAPPER_H
#define THIRDSARM_WRAPPER_H

#include <stddef.h>

int thirdsarm_wrapper_run(int argc, char *argv[]);

/*
 * Direct-P2P handoff between the OSD menu (menu.cpp) and the game runtime.
 *
 * Step 10 adds the OSD entry points: the user picks "Host Game" or enters
 * a peer code via the on-screen keyboard and picks "Join Game". The menu
 * calls one of these writer functions, which serializes the intent to a
 * short file at `kDirectP2PHandoffPath`, sets `g_direct_p2p_handoff_armed`,
 * then triggers a runtime restart (SIGTERM to the child + set
 * `g_wrapper_restart_requested`). Step 11 adds the exec-arg injection
 * that forwards `--direct-p2p-handoff <path>` to the relaunched game
 * when the arming flag is set.
 */
#ifdef __cplusplus
extern "C" {
#endif

/* Read-only Balance status row for the CONF_STR menu.
 *
 * Formats " Balance: <status>" from line 1 of
 * /media/fat/games/3s-arm/balance.status (the file the game rewrites on
 * every boot with the outcome of arcade-vs-PS2 auto-select), or
 * " Balance: (unknown)" when that file is missing or empty. menu.cpp calls
 * this when it renders the CONF_STR "P1-,Balance:;" text row on the Game
 * page -- see tools/mister-wrapper/main-mister-full-menu.patch.
 *
 * This row reports the OUTCOME. The REQUEST is the separate
 * "P1O[48],Balance,Arcade,PS2;" toggle directly above it, which writes the
 * `balance` key in `config` (docs/config.md "balance"); the game resolves
 * that request once at boot and can still land on PS2 -- which is exactly
 * what this row is for.
 *
 * Result is cached and re-read only when the file's mtime/size change, so
 * calling it once per OSD render is cheap. */
void thirdsarm_balance_status_line(char *out, size_t out_size);

/* Serialize "mode=host\n" to the handoff file; arm + request restart. */
void direct_p2p_handoff_host(void);

/* Serialize "mode=join\ncode=<code>\n"; arm + request restart.
 * `code` must be a NUL-terminated string. Writer does not validate the
 * checksum — the game side (src/netplay/room_code.c) rejects bad codes. */
void direct_p2p_handoff_join(const char *code);

/* Recent-joins history helpers. See thirdsarm_wrapper.cpp for the storage
 * format; the menu uses these to render the "Recently Joined" submenu.
 *
 * load_recent_joins: fills codes_out (NUL-terminated, up to 24 chars incl. NUL —
 * the v4 room code is 12 chars, no dashes; see src/netplay/room_code.h. 24
 * is generous slack, kept rather than shrunk, so a saved legacy-length code
 * still round-trips through this buffer)
 * and optional epochs_out with up to max_entries entries (most-recent-first).
 * Returns the number of entries written.
 * save_recent_join: called from direct_p2p_handoff_join() after the handoff
 * file is committed. Dedupes, prepends, caps at 10. */
#define RECENT_JOIN_CODE_BUF 24
int  load_recent_joins(char codes_out[][RECENT_JOIN_CODE_BUF], long epochs_out[], int max_entries);
void save_recent_join(const char *code);

extern int g_direct_p2p_handoff_armed;

/*
 * Replay launch-by-path handoff. The caller passes the absolute path of a
 * `.3sr`; it stores the path, arms a restart and SIGTERMs the child, and the
 * wrapper relaunch injects `--play-replay <path>` (an existing game flag,
 * src/args.c) into child argv so the game boots into that replay. Carries a
 * payload (the path) unlike a bare flag, but like one there
 * is NO on-disk handoff file — argv is the payload channel. No caller at
 * present: the OSD replay browser is gone and the shuffle viewer re-points at
 * this in a later step.
 */
void replay_play_handoff(const char *path_3sr);
extern int g_replay_play_armed;
extern char g_replay_play_path[512];

/*
 * Weekly-best shuffle viewer handoff ("Watch Replays", OSD status bit
 * T[31]). Mirrors replay_play_handoff() exactly, minus the payload: it arms
 * a restart and SIGTERMs the child, and the wrapper relaunch injects the
 * bare `--watch-replays` flag (src/args.c) into child argv. The game then
 * plays the whole cached `.3sr` set back to back in a random order. A bare
 * flag IS the whole payload — there is no on-disk handoff file and no path.
 */
void replay_shuffle_handoff(void);
extern int g_replay_shuffle_armed;

/*
 * Quick Training ("Quick Training", OSD status bit T[15]). Unlike the two
 * handoffs above there is NO restart and NO argv payload: the game keeps
 * running and this just raises SIGRTMIN+5 (kRuntimeQuickTrainingSignal) at
 * the child, whose handler (src/main.c -> on_shutdown_signal ->
 * handle_signal_requests) hands the request to the in-process jump driver
 * (src/quick_training.c). The menu side must dismiss the OSD itself
 * (menustate = MENU_NONE1) — nothing restarts to do it for us. No-op when
 * no child is running.
 */
void quick_training_signal(void);

#ifdef __cplusplus
}
#endif

#endif
