/* replay_wipe.c — the replay viewer's private screen cover. See the header
 * for why this is a private njDrawPolygon2D cover and not a call into the
 * engine's WipeOut/WipeIn/Switch_Screen*. */

#include "replay/replay_wipe.h"

#include "sf33rd/Source/Game/rendering/dc_ghost.h"
#include "structs.h"

#include <SDL3/SDL.h>

/* Canvas the engine's own wipe is written against (sc_sub.c -> WipeOut). */
#define RW_CANVAS_H 224.0f

/* Diagonal band geometry, replicated verbatim from sc_sub.c -> WipeOut()'s
 * `type != 0` arm:
 *
 *     for (i = -224; i < 384; i += 8) {
 *         wipe_p[0].x = i;                        // top-left
 *         wipe_p[1].x = (i + dmylim + 1);         // top-right
 *         wipe_p[2].x = 224.0f + wipe_p[0].x;     // bottom-left
 *         wipe_p[3].x = 224.0f + wipe_p[1].x;     // bottom-right
 *         njDrawPolygon2D(&wipe_pc, 4, PrioBase[0], 32);
 *     }
 *
 * i.e. 76 parallelograms at 45 degrees (x at y=224 is x+224), spaced 8 px,
 * each growing from 1 px to 8 px wide over the 8 steps dmylim = 0..7, all
 * solid 0xFF000000. This is the "76 band diagonal" the user asked for. */
#define RW_BAND_X_FIRST (-224)
#define RW_BAND_X_LIMIT 384
#define RW_BAND_SPACING 8
#define RW_BAND_COUNT ((RW_BAND_X_LIMIT - RW_BAND_X_FIRST) / RW_BAND_SPACING) /* 76 */
#define RW_SLANT 224.0f
#define RW_STEPS 8 /* WipeOut runs dmylim 0..7; at 7 the 8-px bands meet */
#define RW_LAST_STEP (RW_STEPS - 1)
#define RW_COLOR 0xFF000000u

/* Depth. mtrans.c -> Init_PrioBase sets PrioBase[i] = ((i * 512) + 1) /
 * 65535.0f, so PrioBase[0] == 1/65535 is the smallest slot the engine can
 * name — and flPS2ConvScreenFZ maps engine z with a NEGATIVE scale, so a
 * SMALLER engine z ends up drawn last, i.e. on top. Equal-z ties in
 * software_renderer.c -> sort_quads_fast break on `~index`, which puts the
 * EARLIER-submitted quad on top; our cover is submitted after the engine's
 * frame, so a tie at PrioBase[0] would put us UNDER the engine's own wipe
 * quads. z = 0.0f sits just in front of PrioBase[0] and settles it outright:
 * the cover is on top of everything, engine wipes and overlay text alike. */
#define RW_Z 0.0f

/* Safety valve. Nothing in the viewer can leave the cover up forever by
 * design, but a set of entirely corrupt .3sr files leaves the shuffle viewer
 * idle in RS_EMPTY with no replay to reach character select — and a
 * permanently black screen is a worse failure than an uncovered attract
 * loop. 60 s at 60 fps. */
#define RW_COVER_MAX_FRAMES 3600

typedef enum RwState {
    RW_OFF = 0,
    RW_WIPING,  /* diagonal wipe-out in progress */
    RW_COVERED, /* solid black, collapsed to a single full-screen quad */
} RwState;

static RwState s_state = RW_OFF;
static int s_budget = RW_STEPS; /* live frames the wipe-out has to finish in */
static int s_frame_idx = 0;     /* 0-based frame of the wipe-out */
static int s_cover_frames = 0;
static bool s_armed_this_frame = false;

/* Step to draw on wipe-out frame `s_frame_idx`. Frame 0 is always step 0 and
 * the last frame of the budget is always the fully-covering step 7, however
 * short the budget is — the Perfect round-end path only affords 6 frames
 * (manage.c -> Game_Manage_7_6's C_Timer = 6), so its wipe runs the same 8
 * steps at a faster cadence rather than stopping half-drawn. */
static int rw_step(void) {
    if (s_budget <= 1) {
        return RW_LAST_STEP;
    }

    int step = (s_frame_idx * RW_LAST_STEP) / (s_budget - 1);

    if (step > RW_LAST_STEP) {
        step = RW_LAST_STEP;
    }

    return step;
}

void ReplayWipe_Cover(const char* why) {
    if (s_state == RW_COVERED) {
        return;
    }

    s_state = RW_COVERED;
    s_cover_frames = 0;
    s_armed_this_frame = false;
    SDL_Log("replay-wipe: cover UP — solid black, 1 full-screen quad per live frame (%s)", why);
}

void ReplayWipe_BeginExit(int budget, const char* why) {
    if (s_state != RW_OFF) {
        return;
    }

    if (budget < 1) {
        budget = 1;
    }
    if (budget > RW_STEPS) {
        budget = RW_STEPS;
    }

    s_state = RW_WIPING;
    s_budget = budget;
    s_frame_idx = 0;
    s_cover_frames = 0;
    s_armed_this_frame = true;
    SDL_Log("replay-wipe: exit wipe START — %d diagonal bands, %d steps over %d live frame(s) (%s)", RW_BAND_COUNT,
            RW_STEPS, budget, why);
}

bool ReplayWipe_IsActive(void) {
    return s_state != RW_OFF;
}

void ReplayWipe_Reveal(const char* why) {
    if (s_state == RW_OFF) {
        return;
    }

    SDL_Log("replay-wipe: cover DROPPED after %d covered live frame(s) — %s "
            "(the engine's own Switch_Screen_Revival -> WipeIn(0) does the reveal)",
            s_cover_frames, why);
    ReplayWipe_Reset();
}

void ReplayWipe_Reset(void) {
    s_state = RW_OFF;
    s_budget = RW_STEPS;
    s_frame_idx = 0;
    s_cover_frames = 0;
    s_armed_this_frame = false;
}

void ReplayWipe_Tick(bool live_frame) {
    switch (s_state) {
    case RW_OFF:
        return;

    case RW_COVERED:
        if (!live_frame) {
            /* Held frames are black already and draw no cover, so they must
               not age the safety valve. */
            return;
        }

        s_cover_frames += 1;

        if (s_cover_frames == RW_COVER_MAX_FRAMES) {
            SDL_Log("replay-wipe: cover has been up for %d live frames with no character-select reveal — "
                    "dropping it rather than sit on a black screen",
                    s_cover_frames);
            ReplayWipe_Reset();
        }

        return;

    case RW_WIPING:
        if (s_armed_this_frame) {
            /* Armed earlier in THIS frame (replay_player.c's Tick runs before
               ours): draw step 0 before advancing. */
            s_armed_this_frame = false;
            return;
        }

        if (!live_frame) {
            /* The engine tick is being skipped, so the frame renders black
               with no geometry underneath: there is nothing left to wipe
               over. Snap to the solid cover. */
            ReplayWipe_Cover("held frame reached before the wipe-out finished");
            return;
        }

        s_frame_idx += 1;
        return;
    }
}

void ReplayWipe_Draw(void) {
    if (s_state == RW_OFF) {
        return;
    }

    PAL_CURSOR pc;
    PAL_CURSOR_P p[4];
    PAL_CURSOR_COL col[4];

    pc.p = p;
    pc.col = col;
    pc.tex = 0;
    pc.num = 4;
    col[0].color = col[1].color = col[2].color = col[3].color = RW_COLOR;

    if (s_state == RW_COVERED) {
        /* ONE quad, not 76. NJDP2D_PRIM_MAX and QUADS_MAX are both 512 and
           both drop silently on overflow (dc_ghost.c -> njdp2d_sort logs once
           and returns; software_renderer.c -> Renderer_DrawSolidQuad just
           returns), and the viewer submits LAST — so an overflowing frame
           eats the cover, the one quad that must never go missing. The
           oversized 640x448 rectangle is the engine's own full-screen extent
           (sc_data.c -> Fade_Pos_tbl, used by overwrite_panel). */
        p[0].x = 0.0f;
        p[0].y = 0.0f;
        p[1].x = 640.0f;
        p[1].y = 0.0f;
        p[2].x = 0.0f;
        p[2].y = 448.0f;
        p[3].x = 640.0f;
        p[3].y = 448.0f;
        njDrawPolygon2D(&pc, 4, RW_Z, 0x20);
        return;
    }

    /* RW_WIPING: sc_sub.c -> WipeOut()'s `type != 0` geometry, with our own
       private step counter in place of the engine's global WipeLimit. */
    const int step = rw_step();

    p[0].y = p[1].y = 0.0f;
    p[2].y = p[3].y = RW_CANVAS_H;

    int bands = 0;

    for (int i = RW_BAND_X_FIRST; i < RW_BAND_X_LIMIT; i += RW_BAND_SPACING) {
        p[0].x = (f32)i;
        p[1].x = (f32)(i + step + 1);
        p[2].x = RW_SLANT + p[0].x;
        p[3].x = RW_SLANT + p[1].x;
        njDrawPolygon2D(&pc, 4, RW_Z, 0x20);
        bands += 1;
    }

    SDL_Log("replay-wipe: exit wipe frame %d/%d — step %d/%d (%d px bands), %d band quads submitted",
            s_frame_idx + 1, s_budget, step + 1, RW_STEPS, step + 1, bands);

    if (step >= RW_LAST_STEP) {
        /* The 8-px bands meet at 8-px spacing: the screen is solid from this
           frame on, so collapse 76 quads to 1 for every frame after it. */
        SDL_Log("replay-wipe: exit wipe COMPLETE after %d live frame(s) — collapsing %d band quads to 1 "
                "full-screen quad",
                s_frame_idx + 1, RW_BAND_COUNT);
        ReplayWipe_Cover("wipe-out reached full coverage");
    }
}
