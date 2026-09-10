/**
 * @file gd3rd.c
 * AFS file reading
 */

#include "sf33rd/Source/Game/io/gd3rd.h"
#include "common.h"
#include "main.h"
#include "port/utils.h"
#include "sf33rd/AcrSDK/MiddleWare/PS2/CapSndEng/cse.h"
#include "sf33rd/AcrSDK/ps2/flps2debug.h"
#include "sf33rd/AcrSDK/ps2/foundaps2.h"
#include "sf33rd/Source/Game/debug/Debug.h"
#include "sf33rd/Source/Game/engine/workuser.h"
#include "sf33rd/Source/Game/rendering/color3rd.h"
#include "sf33rd/Source/Game/rendering/texgroup.h"
#include "sf33rd/Source/Game/system/ramcnt.h"
#include "sf33rd/Source/Game/system/work_sys.h"
#include "structs.h"

#include "port/io/afs.h"
#include "netplay/netplay.h"

#include <SDL3/SDL.h>

typedef struct {
    u8 type;
    u8 ix;
    u8 frre;
    u8 kokey;
} LDREQ_TBL;

typedef void (*LDREQ_Process_Func)(REQ*);

const u8 lpr_wrdata[3] = { 0x03, 0xC0, 0x3C };
const u8 lpc_seldat[2] = { 10, 11 };
const u8 lpt_seldat[4] = { 3, 4, 5, 0 };

/* NETPLAY ROLLBACK DISPOSITION of this file's globals (task #60, 2026-08-24).
 * None of them are in the rollback save set (zero hits for any of these names
 * in netplay/game_state.c) and none of them CAN be. Do not "fix" a
 * select-phase desync by adding them; each was checked individually:
 *
 *   afs_handle    Index into port/io/afs.c's requests[] table (afs.c:408).
 *                 The slot owns an SDL_AsyncIO* (afs.c:38, opened at
 *                 afs.c:417) and a monotonically advancing read cursor
 *                 (afs.c:442); restoring the index rewinds neither. AFS_Close zeroes the slot
 *                 (afs.c:297/519/530) and AFS_Open hands out the first free
 *                 one (afs.c:377-393) from a pool the ADX music streamer also
 *                 draws from (port/sound/adx.c:455), so a restored stale
 *                 handle can alias another file's in-flight read.
 *
 *   q_ldreq       Restoring it rewinds `rno`, replaying steps that already
 *                 ran: Pull_ramcnt_key (texgroup.c:379, color3rd.c:108)
 *                 against an allocator that is NOT rewound, plus fsOpen /
 *                 AFS_Read. That is the double-allocation the dedupe below
 *                 and the reclaim in q_ldreq_texture_group case 2 exist to
 *                 contain. It also rewinds the queue back into a non-empty
 *                 state at the select->battle boundary, where Game2_0 /
 *                 Game2_2 (game.c:479, :622) call fatal_error("Load queue
 *                 failed to drain in time") — a hard process kill.
 *
 *   ldreq_result  Plain u8[294], so physically saveable, but rewinding a
 *                 completion bit whose queue entry has already drained
 *                 (be == 0, shifted out by Check_LDREQ_Queue) leaves
 *                 Check_LDREQ_Queue_Player waiting on a byte nobody will
 *                 ever set. Exit_6th (sel_pl.c:1701) has no timeout, only a
 *                 `return`. Saving it without q_ldreq is a hang.
 *
 *   plt_req       A latch that must agree with the (unsaved) queue:
 *                 q_ldreq_texture_group case 4 publishes into
 *                 char_init_data[plid_data[plt_req[id]]] (texgroup.c:430).
 *                 Rewinding it while the queue still holds the newer
 *                 character's request publishes into the wrong slot.
 *
 *                 CLOSED 2026-08-25 (task #69). plt_req is the one member of
 *                 this cluster the rollback-determinism harness still reports
 *                 DIVERGENT at production select depth 8, and it is now
 *                 measured rather than argued. It is a PHASE LEAD OF AN
 *                 IDENTICAL VALUE, not a value divergence, and its single
 *                 in-select reader is structurally dispatched after the
 *                 window has closed. Both halves:
 *
 *                 VALUE. plt_req[id] is written only here (Push_LDREQ_Queue_
 *                 Player, below), which sets it to the character whose
 *                 requests it enqueues in the same call. Every call site
 *                 reachable in MODE_NETWORK passes My_char[id] (sel_pl.c:790,
 *                 :996, win.c:178, menu.c:1571-1572, ranking.c:330-331), and
 *                 My_char is GS_SAVE'd (game_state.c:247/986). The one site
 *                 that passes something else -- Push_LDREQ_Queue_Player(
 *                 COM_id, 17) at next_cpu.c:958 -- sits under Game05, entered
 *                 only from the `default:` arm of Game03's Mode_Type switch
 *                 (game.c:822); `case MODE_VERSUS: case MODE_NETWORK:` divert
 *                 at game.c:794-795.
 *
 *                 ORDER. The only in-select reader is Exit_6th
 *                 (sel_pl.c:1701-1706) via Check_PL_Load (sys_sub.c:899) ->
 *                 Check_LDREQ_Queue_Player (below). Exit_6th is dispatched
 *                 only at Exit_No == 5 (sel_pl.c:1551-1553), and Exit_No
 *                 leaves 0 only through Exit_1st (sel_pl.c:1556-1562), which
 *                 returns early unless BOTH operator-active players have
 *                 Sel_Arts_Complete < 0. That is set at sel_pl.c:865, strictly
 *                 downstream of the PL_Sel_1st push (sel_pl.c:788-790), which
 *                 is itself gated on the Sel_PL_Complete == -0x8000 sentinel
 *                 (effd8.c:87) that follows the Sel_PL_3rd confirm push
 *                 (sel_pl.c:984-996). Sel_PL_Complete, Sel_Arts_Complete,
 *                 Exit_No and SP_No are ALL in the save set (game_state.c:
 *                 253/993, 550/1290, 456/1196, 457/1197), so both peers agree
 *                 on that ordering exactly. The confirmed timeline's plt_req
 *                 write therefore always lands before Exit_No can leave 0,
 *                 and four further Exit_Nth transitions separate that from
 *                 Exit_6th.
 *
 *                 MEASURED (host, ASLR off, pinned RNG, 1500 frames, both
 *                 fast scenarios, barrier off AND forced on, select depth 8,
 *                 select period 8 and the adversarial period 1) with the
 *                 tools/ldreq-timing per-frame trace driven by --rbd-*
 *                 rollback injection instead of injected AFS latency:
 *
 *                   plt_req1  no-rollback  0 -> 11 at frame 210
 *                   plt_req1  rollback(8)  0 -> 11 at frame 208
 *                   divergent frames: 2 (period 8) / 7 (period 1), per slot
 *                   Exit_No first reaches 5 at frame 317 in EVERY run
 *                   G_No[0..3], G_Timer, Exit_No, Exit_Timer: byte-identical
 *                   on all 1500 frames in all TWELVE rollback-vs-baseline
 *                   comparisons (2 scenarios x barrier off/on x select
 *                   {d8p8, d2p8, d8p1}); eight of those are at depth 8
 *
 *                 Do NOT read this as "plt_req is harmless". It is harmless
 *                 because of the Exit_1st ordering above. A new reader of
 *                 plt_req or of Check_PL_Load inside character select, before
 *                 Exit_No leaves 0, voids this.
 *
 * The cluster is all-in or all-out and all-in is impossible, so it is out.
 *
 * The residual is real and is NOT a rollback bug: AFS_Read is a genuine
 * async disk read (afs.c:434), so ldreq_result's completion frame is
 * wall-clock, not frame-count. Measured proof — the rollback-determinism
 * harness classifies q_ldreq, rckey_work, rckey_mmobj, texgrplds,
 * char_init_data, requests, afs and asyncio_queue as A1-vs-A2 BASELINE
 * NOISE: they differ between two identical no-rollback runs of one binary
 * on one machine. Two peers cannot agree on them either. Since Exit_6th
 * (sel_pl.c:1701-1722 -> Exit_No, Exit_Timer) and Game09's stage wait
 * (game.c:1383-1387 -> G_No, G_Timer) gate SAVED state on ldreq_result, the
 * correct treatment is a barrier that keeps the simulation from observing an
 * in-flight load while a GekkoNet session is running — not a wider save set
 * and not a replay scheme. See the CORRECTION block in
 * tools/rollback-determinism/allowlist.txt and known limit 9 in
 * docs/rollback-determinism-harness.md.
 *
 * That barrier is now implemented — see the block comment above
 * Check_LDREQ_Queue() below. It does NOT make any of these four symbols
 * saveable and does not change this disposition; it makes them
 * unobservable in an intermediate state, which is a different fix. The
 * paragraphs above remain the reason not to try the save-set route again.
 *
 * One correction to the paragraph above, measured in task #66: that stage
 * wait (game.c:1383-1387, inside Game09 case 1 -- Bonus_Sub itself, at
 * game.c:1466, is only ever called from Game09's later cases) is NOT
 * reachable in MODE_NETWORK. Game09 needs G_No[1] == 9, written only at
 * game.c:1004 (inside Game05) and game.c:1588 (inside Game11); Game05 is entered only from the `default:` arm of
 * Game03's switch (game.c:822), and `case MODE_VERSUS: case MODE_NETWORK:`
 * (game.c:794-795) divert before it. The netplay-reachable feedback edge is
 * Exit_6th's alone. */
s16 plt_req[2];
u8 ldreq_break;
REQ q_ldreq[16];
u8 ldreq_result[294];

static AFSHandle afs_handle = AFS_NONE;

// forward decls
s32 Push_LDREQ_Queue(REQ* ldreq);
void Push_LDREQ_Queue_Metamor();
void q_ldreq_error(REQ* curr);
void disp_ldreq_status();
void Push_LDREQ_Queue_Union(s16 ix);
s32 Check_LDREQ_Queue_Union(s16 ix);

const LDREQ_Process_Func ldreq_process[6];
s8* ldreq_process_name[];
const LDREQ_TBL ldreq_tbl[294];
const s16 ldreq_ix[43][2];

/* Push_LDREQ_Queue_Direct takes one `ix` and subscripts BOTH tables with it:
 * ldreq_tbl[ix] for the request fields, then `ldreq.result = &ldreq_result[ix]`.
 * Push_LDREQ_Queue's dedup comment says so outright -- "`result` is included
 * on purpose: it is 1:1 with the ldreq_tbl[] index". Nothing enforced the 1:1,
 * and the two lengths are written as separate literals a dozen lines apart, so
 * adding table entries without growing ldreq_result yields an out-of-bounds
 * write of a pointer into the request queue -- and the dedup then compares
 * `result` pointers that no longer identify a real slot. */
_Static_assert(SDL_arraysize(ldreq_result) == SDL_arraysize(ldreq_tbl),
               "ldreq_result must have one slot per ldreq_tbl entry — "
               "Push_LDREQ_Queue_Direct indexes both with the same ix, and "
               "Push_LDREQ_Queue's dedup relies on result being 1:1 with it");

/* q_ldreq's length is open-coded as a bare literal at every one of its scan
 * loops -- gd3rd.c's four `i < 16` sweeps plus the `i < 15` compaction shift,
 * which additionally depends on being exactly length-1 so the freed tail slot
 * is the one it clears. src/test/ldreq_timing_trace.c re-declares the array
 * with its own `[16]` as well. A tripwire is the honest guard here: nothing
 * can derive those bounds automatically, so changing the queue length must
 * stop the build and name the loops that have to change with it. */
_Static_assert(SDL_arraysize(q_ldreq) == 16,
               "q_ldreq length changed — update the scan loops in "
               "Push_LDREQ_Queue/Check_LDREQ_Clear/disp_ldreq_status, the "
               "length-1 compaction shift in Check_LDREQ_Queue, and the "
               "duplicate extern in src/test/ldreq_timing_trace.c");

s32 fsOpen(REQ* req) {
    if (req->fnum >= AFS_GetFileCount()) {
        return 0;
    }

    if (afs_handle != AFS_NONE) {
        AFS_Close(afs_handle);
    }

    afs_handle = AFS_Open(req->fnum);

    req->info.number = 1;
    return 1;
}

void fsClose(REQ* /* unused */) {
    AFS_Close(afs_handle);
    afs_handle = AFS_NONE;
}

u32 fsGetFileSize(u16 fnum) {
    if (fnum >= AFS_GetFileCount()) {
        return 0;
    }

    return AFS_GetSize(fnum);
}

u32 fsCalSectorSize(u32 size) {
    return (size + 2048 - 1) / 2048;
}

s32 fsCansel(REQ* /* unused */) {
    if ((afs_handle != AFS_NONE) && (AFS_GetState(afs_handle) == AFS_READ_STATE_READING)) {
        AFS_Stop(afs_handle);
    }

    return 1;
}

s32 fsCheckCommandExecuting() {
    if (afs_handle == AFS_NONE) {
        return 0;
    }

    const AFSReadState state = AFS_GetState(afs_handle);

    switch (state) {
    case AFS_READ_STATE_READING:
    case AFS_READ_STATE_ERROR:
        return 1;

    case AFS_READ_STATE_IDLE:
    case AFS_READ_STATE_FINISHED:
        return 0;

    default:
        fatal_error("Unhandled AFS state: %d", state);
    }
}

s32 fsRequestFileRead(REQ* /* unused */, u32 sec, void* buff) {
    AFS_Read(afs_handle, sec, buff);
    return 1;
}

s32 fsCheckFileReaded(REQ* /* unused */) {
    const AFSReadState state = AFS_GetState(afs_handle);

    switch (state) {
    case AFS_READ_STATE_ERROR:
        return 2;

    case AFS_READ_STATE_READING:
        return 0;

    case AFS_READ_STATE_IDLE:
    case AFS_READ_STATE_FINISHED:
        return 1;

    default:
        fatal_error("Unhandled AFS state: %d", state);
    }
}

s32 fsFileReadSync(REQ* req, u32 sec, void* buff) {
    AFS_ReadSync(afs_handle, sec, buff);
    const s32 rnum = fsCheckFileReaded(req);
    return (rnum == 1) ? 1 : 0;
}

void waitVsyncDummy() {
    AFS_RunServer(); // FIXME: Ideally we should only call this from the main loop
    cseExecServer();
}

s32 load_it_use_any_key2(u16 fnum, void** adrs, s16* key, u8 kokey, u8 group) {
    u32 size;
    u32 err;

    if (fnum >= AFS_GetFileCount()) {
        flLogOut("ファイルナンバーに異常があります。ファイル番号：%d\n", fnum);
        // originally while(1){} from arcade source; skip + log instead of hanging
#if ENABLE_PERF_TELEMETRY
        flLogOut("[gd3rd-skip] %s fnum-out-of-range fnum=%d max=%d\n", __func__, fnum, AFS_GetFileCount());
#endif
        return 0;
    }

    size = fsGetFileSize(fnum);
    *key = Pull_ramcnt_key(fsCalSectorSize(size) << 11, kokey, group, 0);
    if (*key < 0) {
#if ENABLE_PERF_TELEMETRY
        flLogOut("[gd3rd-skip] %s pull-ramcnt-key-failed fnum=%d size=%u kokey=%d group=%d\n",
                 __func__, fnum, size, kokey, group);
#endif
        return 0;
    }
    *adrs = (void*)Get_ramcnt_address(*key);

    err = load_it_use_this_key(fnum, *key);

    if (err != 0) {
        return size;
    }

    Push_ramcnt_key(*key);
    return 0;
}

s16 load_it_use_any_key(u16 fnum, u8 kokey, u8 group) {
    u32 err;
    void* adrs;
    s16 key;

    err = load_it_use_any_key2(fnum, &adrs, &key, kokey, group);

    if (err != 0) {
        return key;
    }

    return 0;
}

s32 load_it_use_this_key(u16 fnum, s16 key) {
    REQ req;
    u32 err;

    req.fnum = fnum;

    while (1) {
        err = fsOpen(&req);

        if (err == 0) {
            continue;
        }

        req.size = fsGetFileSize(req.fnum);
        req.sect = fsCalSectorSize(req.size);
        err = fsFileReadSync(&req, req.sect, (void*)Get_ramcnt_address(key));
        fsClose(&req);
        Set_size_data_ramcnt_key(key, req.size);

        if (err != 0) {
            return 1;
        }

        flLogOut("ファイルの読み込みに失敗しました。ファイル番号：%d\n", fnum);
    }
}

void Init_Load_Request_Queue_1st() {
    s16 i;

    for (i = 0; i < (s16)(sizeof(q_ldreq) / sizeof(REQ)); i++) {
        q_ldreq[i].be = 0;
        q_ldreq[i].type = 0;
    }

    ldreq_break = 0;
}

void Request_LDREQ_Break() {
    ldreq_break = 1;
}

u8 Check_LDREQ_Break() {
    if (ldreq_break) {
        return 1;
    }

    return fsCheckCommandExecuting();
}

void Push_LDREQ_Queue_Player(s16 id, s16 ix) {
    REQ ldreq;
    s16 i;
    s16 kara;
    s16 made;

    kara = ldreq_ix[ix][0];
    made = kara + ldreq_ix[ix][1];
    plt_req[id] = ix;

    for (i = kara; i < made; i++) {
        ldreq.type = ldreq_tbl[i].type;
        ldreq.id = id;
        ldreq.ix = ldreq_tbl[i].ix;
        ldreq.frre = ldreq_tbl[i].frre;
        ldreq.key = 0;
        ldreq.group = 0;
        ldreq.result = &ldreq_result[i];

        if (ldreq.type == 2) {
            ldreq.kokey = lpc_seldat[id];
        } else {
            ldreq.kokey = lpt_seldat[id];
        }

        Push_LDREQ_Queue(&ldreq);
    }
}

void Push_LDREQ_Queue_BG(s16 ix) {
    Push_LDREQ_Queue_Union(ix + 20);
    Push_LDREQ_Queue_Metamor();
}

void Push_LDREQ_Queue_Union(s16 ix) {
    REQ ldreq;
    s16 i;
    s16 kara;
    s16 made;

    kara = ldreq_ix[ix][0];
    made = kara + ldreq_ix[ix][1];

    for (i = kara; i < made; i++) {
        ldreq.type = ldreq_tbl[i].type;
        ldreq.id = 2;
        ldreq.ix = ldreq_tbl[i].ix;
        ldreq.frre = ldreq_tbl[i].frre;
        ldreq.kokey = ldreq_tbl[i].kokey;
        ldreq.key = 0;
        ldreq.group = 0;
        ldreq.result = &ldreq_result[i];
        Push_LDREQ_Queue(&ldreq);
    }
}

void Push_LDREQ_Queue_Metamor() {
    switch ((My_char[0] == 0x12) + (My_char[1] == 0x12) * 2) {
    case 1:
        Push_LDREQ_Queue_Direct(My_char[1] + 0xD4, 0);
        break;

    case 2:
        Push_LDREQ_Queue_Direct(My_char[0] + 0xD4, 1);
        break;

    case 3:
        Push_LDREQ_Queue_Direct(0xE6, 2);
        break;
    }
}

void Push_LDREQ_Queue_Direct(s16 ix, s16 id) {
    REQ ldreq;
    ldreq.type = ldreq_tbl[ix].type;
    ldreq.id = id;
    ldreq.ix = ldreq_tbl[ix].ix;
    ldreq.frre = ldreq_tbl[ix].frre;
    ldreq.kokey = ldreq_tbl[ix].kokey;
    ldreq.key = 0;
    ldreq.group = 0;
    ldreq.result = &ldreq_result[ix];
    Push_LDREQ_Queue(&ldreq);
}

s32 Push_LDREQ_Queue(REQ* ldreq) {
    s16 i;
    u8 masknum;

    /* Rollback re-simulation rewinds the one-shot gate that issues a load
     * request but not the request queue itself, so the same request can be
     * issued twice across a rollback boundary. Sel_PL_3rd (sel_pl.c:996)
     * calls Push_LDREQ_Queue_Player() and then bumps SP_No[ID][0], which is
     * the dispatch selector in Sel_PL() (sel_pl.c:905) and IS in the
     * rollback save set (game_state.c:437 GS_SAVE / :1177 GS_LOAD). q_ldreq
     * is NOT (zero hits in game_state.c). A rollback across the confirm
     * frame therefore restores the gate and re-issues the request.
     *
     * The duplicate does not simply reload: it re-enters
     * q_ldreq_texture_group() (texgroup.c) at rno==1 after the
     * dup-transfer guard, skipping the lds->ok check, and allocates a
     * second ramcnt block over texgrplds[grp].key -- a single-slot key
     * holder. The first block then has no remaining reference and can
     * never be freed (purge_texture_group only frees texgrplds[grp].key),
     * permanently losing 3.34 MB for a character group. Drop the duplicate
     * here instead.
     *
     * Identity is (type, ix, id, result). `result` is included on purpose:
     * it is 1:1 with the ldreq_tbl[] index, and two different table entries
     * can carry the same (type, ix) -- e.g. entries 1 and 26 are both
     * {type 1, ix 0x1B}. Matching on `result` too guarantees we never
     * swallow a request whose completion bits live in a *different*
     * ldreq_result[] slot, which would leave Check_LDREQ_Queue_Player()
     * waiting forever on a byte nobody sets.
     *
     * Only entries with be != 0 are live; an already-drained request leaves
     * be == 0 and is not matched, so a genuine reload still goes through.
     *
     * SCOPE -- this is the shallow-rollback half of the fix, NOT the whole
     * fix. It only catches a duplicate that arrives while the original is
     * still queued. Whether that holds depends on rollback depth:
     *
     *   select depth 2:     dedupe=4  reclaim=0  dup-transfer=0
     *   select depth 3/5/8: dedupe=7  reclaim=1  dup-transfer=1
     *
     * At depth >= 3 the head request has already drained (be == 0) by the
     * time the duplicate is issued, so this scan misses it entirely and
     * the reclaim in q_ldreq_texture_group's case 2 (texgroup.c) is what
     * actually prevents the leak. Production predicts 8 frames ahead by
     * default (input_prediction_window, netplay.c:914-916 -- read from
     * CFG_KEY_NETPLAY_INPUT_PREDICTION_WINDOW, clamped to 1..32, 8 when
     * unset or out of range), so depth >= 3 is the case that matters in
     * the field. The texgroup.c reclaim is therefore
     * load-bearing and must not be removed on the grounds that this dedupe
     * exists. */
    for (i = 0; i < 16; i++) {
        if (q_ldreq[i].be != 0 && q_ldreq[i].type == ldreq->type && q_ldreq[i].ix == ldreq->ix &&
            q_ldreq[i].id == ldreq->id && q_ldreq[i].result == ldreq->result) {
#if ENABLE_PERF_TELEMETRY
            flLogOut("[ldreq-dedupe] %s dropped duplicate slot=%d type=%d ix=%d id=%d be=%d rno=%d\n",
                     __func__, (int)i, (int)ldreq->type, (int)ldreq->ix, (int)ldreq->id, (int)q_ldreq[i].be,
                     (int)q_ldreq[i].rno);
#endif
            return 1;
        }
    }

    for (i = 0; i < 16; i++) {
        if (q_ldreq[i].be == 0) {
            break;
        }
    }

    if (i != 0x10) {
        q_ldreq[i] = ldreq[0];
        q_ldreq[i].be = 2;
        q_ldreq[i].rno = 0;
        q_ldreq[i].retry = 0x40;

        switch (ldreq->id) {
        case 0:
            masknum = 3;
            break;

        case 1:
            masknum = 0xC0;
            break;

        default:
            masknum = 0x3C;
            break;
        }

        *q_ldreq[i].result &= ~masknum;
        return 1;
    }

    flLogOut("ファイル読み込み要求バッファがオーバーしました。\n");
    return 0;
}

static bool ldreq_barrier_forced = false;

void Ldreq_SetBarrierForced(bool forced) {
    ldreq_barrier_forced = forced;
}

/* Task #72 — the gate covers every session state that can advance the
 * simulation, not just RUNNING.
 *
 * The task-#66 barrier keyed on NETPLAY_SESSION_RUNNING alone. That is
 * narrower than the set of states in which Check_LDREQ_Queue() runs, so
 * the frames outside it pumped the loader on the stock single-step path
 * — the exact wall-clock-coupled behaviour #66 exists to remove. The
 * full enumeration of paths that reach njUserMain()/Game_Task() (and
 * therefore Check_LDREQ_Queue(), game.c:195) while a session exists:
 *
 *   IDLE           main.c:718 calls njUserMain() directly. Offline. The
 *                  barrier MUST stay off here — that is the whole
 *                  "offline is untouched" clause below.
 *   TRANSITIONING  netplay.c:1688 step_game(true), once per frame until
 *                  game_ready_to_run_character_select(). Unbounded frame
 *                  count: measured 9 pumps per peer locally, and the
 *                  Netplay_Run timeline has shown 66 vs 12 ticks on two
 *                  peers of the same session.
 *   CONNECTING     netplay.c:1829 run_netplay(). GekkoNet emits no
 *                  advance until AllActorsValid() first returns true
 *                  (GekkoLib game_session.cpp:125, `GEKKONET_REF`=7be848c
 *                  in build-deps.sh:498), and that same UpdateSession call
 *                  both queues SessionStarted (game_session.cpp:425) and
 *                  advances (:145). Netplay_Run reads session
 *                  events BEFORE update_session (step_logic, netplay.c
 *                  :1356-1357), so the RUNNING flip lands one tick late
 *                  and session frame 0 always executes here. Measured:
 *                  exactly one pump per session, on both peers.
 *   RUNNING        netplay.c:1840 run_netplay(). What #66 covered.
 *   EXITING        no run_netplay(), no step_game(). Never pumps under a
 *                  live engine; handle_disconnection() has already run
 *                  Soft_Reset_Sub() -> Init_Load_Request_Queue_1st()
 *                  (sys_sub.c:1059), which wipes the queue.
 *
 * So TRANSITIONING and CONNECTING are added and IDLE/EXITING are not.
 *
 * WHY THIS IS THE FIX FOR THE SESSION-START SKEW (task #69.3) TOO.
 * #69.3 is open because nothing on the start path clears the queue, so
 * two peers have no structural guarantee of holding the same loader
 * state when the engine takes over. With the barrier live during
 * TRANSITIONING, every frame of the pre-session run ends with an empty
 * queue and a settled ldreq_result[] — including the last one before
 * configure_gekko() — so both peers enter session frame 0 identical by
 * construction rather than by measurement. CONNECTING alone would not
 * have achieved that: frame 0's game logic runs BEFORE frame 0's pump
 * (game.c:195 is the tail of the Game_Task ix loop), so a disagreement
 * inherited from TRANSITIONING would already have been read into frame
 * 0's saved state.
 *
 * COST IS ZERO WHENEVER THE QUEUE IS EMPTY. Check_LDREQ_Queue() returns
 * at the `q_ldreq->be == 0` guard below before it ever consults this
 * function, so on every path measured so far (all 10 pre-RUNNING pumps
 * per peer carried head_be == 0) this widening changes nothing at all.
 * When the queue is NOT empty the added work is the same bounded drain
 * RUNNING has been shipping since #66 — LDREQ_BARRIER_BUDGET_MS and
 * LDREQ_BARRIER_MAX_STEPS, blown budget logs and returns. The two new
 * states are also strictly safer to stall than RUNNING: no peer is
 * counting our packets yet, so GekkoNet's 5000 ms DISCONNECT_TIMEOUT is
 * not in play, and CONNECTING's own 15 s deadline (CONNECT_TIMEOUT_
 * CONNECTING_MS, connect_fail.h:270) sees at most one such drain. */
bool Ldreq_BarrierActive(void) {
    if (ldreq_barrier_forced) {
        return true;
    }

    switch (Netplay_GetSessionState()) {
    case NETPLAY_SESSION_TRANSITIONING:
    case NETPLAY_SESSION_CONNECTING:
    case NETPLAY_SESSION_RUNNING:
        return true;

    case NETPLAY_SESSION_IDLE:
    case NETPLAY_SESSION_EXITING:
        return false;
    }

    /* No `default:` arm ON PURPOSE. Every NetplaySessionState is listed
     * above, so -Wswitch (in -Wall, and this tree builds -Werror) turns a
     * newly added state into a compile error naming the unhandled enumerator.
     *
     * A `default: return false;` is what this used to have, and it is exactly
     * the wrong shape here: it makes a new state silently barrier-free, and
     * "the gate was narrower than the set of states that can advance the
     * simulation" is precisely the task-#66 defect that task #72 had to come
     * back and fix (see the block comment above this function). With a
     * default, that recurrence compiles clean and the loader quietly pumps on
     * the wall-clock-coupled path again for whatever frames the new state
     * covers.
     *
     * This return is for a value outside the enum only (a corrupt or
     * uninitialised read); it is not the new-state path.
     *
     * A _Static_assert on the last enumerator was tried first and REJECTED as
     * vacuous: appending a state after NETPLAY_SESSION_EXITING -- the most
     * likely way one gets added -- leaves its value unchanged, so the
     * assertion never fires for the case that matters. -Wswitch catches both
     * append and insert. */
    return false;
}

#if ENABLE_PERF_TELEMETRY
/* Task #69.3 — see the block comment on the declaration in gd3rd.h.
 * Every read here is of state this file already owns; nothing is mutated,
 * so enabling the probe cannot move the thing it is measuring. */
void Ldreq_LogSessionProbe(const char* tag, int frame) {
    char be_map[17];
    int nonempty = 0;
    unsigned bits = 0;
    unsigned hash = 5381u;
    int afs_open = 0;
    const int afs_reading = AFS_GetInFlightCount(&afs_open);

    for (int i = 0; i < 16; i++) {
        const int be = (int)q_ldreq[i].be;
        be_map[i] = (char)((be >= 0 && be <= 9) ? ('0' + be) : '?');

        if (be != 0) {
            nonempty += 1;
        }
    }

    be_map[16] = '\0';

    for (unsigned i = 0; i < (unsigned)sizeof(ldreq_result); i++) {
        u8 v = ldreq_result[i];

        hash = ((hash << 5) + hash) ^ (unsigned)v;

        while (v != 0) {
            bits += (unsigned)(v & 1u);
            v = (u8)(v >> 1);
        }
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[ldreq-session-probe] tag=%s frame=%d barrier=%d q_be=%s q_nonempty=%d head_type=%d head_rno=%d "
                "afs_handle=%d afs_reading=%d afs_open=%d fs_busy=%d ldreq_break=%d ldreq_result_h=%08x "
                "ldreq_result_bits=%u plt_req=%d/%d",
                tag, frame, (int)Ldreq_BarrierActive(), be_map, nonempty, (int)q_ldreq[0].type, (int)q_ldreq[0].rno,
                (int)afs_handle, afs_reading, afs_open, (int)fsCheckCommandExecuting(), (int)ldreq_break, hash, bits,
                (int)plt_req[0], (int)plt_req[1]);
}
#endif

/* One step of the head request's state machine, plus the queue shift the
 * original Check_LDREQ_Queue() performed inline when the head drained.
 * Factored out verbatim so the barrier loop below and the stock
 * single-step path share exactly one implementation. */
static void ldreq_pump_head(void) {
    s16 i;

    ldreq_process[q_ldreq->type](q_ldreq);

    if (q_ldreq->be == 0) {
        for (i = 0; i < 15; i++) {
            q_ldreq[i] = q_ldreq[i + 1];
        }

        q_ldreq[i].be = 0;
        q_ldreq[i].type = 0;
    }
}

/* === NETPLAY LDREQ FRAME BARRIER (task #66) ===
 *
 * THE BUG. Every value this file exposes to the simulation —
 * ldreq_result[] through Check_LDREQ_Queue_Player/_Union/_Direct,
 * q_ldreq[].be through Check_LDREQ_Clear(), afs_handle through
 * Check_LDREQ_Break()/fsCheckCommandExecuting() — advances on the frame
 * an OS async read happens to land. AFS_Read is a real SDL_ReadAsyncIO
 * (port/io/afs.c:434) drained by AFS_RunServer via SDL_GetAsyncIOResult
 * (afs.c:313-319), so the completion frame is WALL CLOCK, not frame
 * count. Two peers with different disks therefore disagree about it,
 * with no rollback involved at all. That divergence is not cosmetic:
 * Exit_6th (screen/sel_pl.c:1701-1722) gates the SAVED Exit_No /
 * Exit_Timer on Check_PL_Load() + Check_LDREQ_Queue_BG(), so the two
 * peers leave character select on different frames and every frame of
 * the match after that is misaligned.
 *
 * Measured, not argued: the rollback-determinism harness classifies
 * q_ldreq, rckey_work, rckey_mmobj, texgrplds, char_init_data, requests,
 * afs and asyncio_queue as A1-vs-A2 BASELINE NOISE — they differ between
 * two identical NO-ROLLBACK runs of one binary on one machine with ASLR
 * off (docs/rollback-determinism-harness.md, known limit 9).
 *
 * WHY NOT THE OBVIOUS FIXES. Both were tried and both are refuted in the
 * disposition comment above plt_req: widening the rollback save set
 * cannot work (afs_handle is an index into a slot that owns a live
 * SDL_AsyncIO* and a monotonic cursor; rewinding q_ldreq replays
 * Pull_ramcnt_key against a non-rewound allocator and can strand the
 * queue non-empty at the Game2_0/Game2_2 drain assertion), and a
 * per-frame record/replay scheme cannot work either, because the
 * divergence exists between two runs of a SINGLE peer. There is nothing
 * to replay that both peers would agree on.
 *
 * THE BARRIER. Make the queue unobservable in an intermediate state:
 * while a session is running, the frame that pumps the queue finishes
 * every request in it before returning. The invariant that buys is
 *
 *     at every simulated-frame boundary the queue is empty, afs_handle
 *     is AFS_NONE, and ldreq_result[] is a pure function of the sequence
 *     of Push_LDREQ_Queue_* calls
 *
 * and that sequence is driven by SP_No / G_No / bg_w.stage, all of which
 * are in the rollback save set.
 *
 * SCOPE OF THAT INVARIANT -- corrected 2026-08-25 (task #69), because the
 * sentence that used to close this paragraph ("so every observation the
 * simulation can make of this subsystem becomes a function of saved state
 * alone") is too strong and would mislead the next reader. The barrier
 * buys WALL-CLOCK invariance, not ROLLBACK invariance. The push SEQUENCE
 * includes pushes issued by speculative legs that a rollback then discards
 * from the save set but NOT from ldreq_result[] (its bits are only ever
 * OR'd in -- see `*curr->result |= lpr_wrdata[curr->id]`, texgroup.c and
 * color3rd.c -- never cleared). So a peer that rolled back across a
 * character-select confirm has the load already COMPLETE while a peer that
 * did not is still waiting.
 *
 * MEASURED, with the barrier forced on: Check_PL_Load() -- the exact
 * expression Exit_6th gates the saved Exit_No/Exit_Timer on -- differs
 * between a no-rollback run and a rollback run of this one binary for
 * 2 frames at select period 8 / 7 frames at select period 1, both
 * scenarios, select depth 8. That is a real, reproducible cross-peer
 * disagreement in this subsystem's observable surface.
 *
 * It is nonetheless not a desync, and the reason is ORDER, not the
 * barrier: Exit_6th is dispatched only at Exit_No == 5, which cannot be
 * reached until both players' confirmed-timeline pushes have landed. See
 * the plt_req entry in the disposition block at the top of this file for
 * the full chain and the frame numbers (window ends at 209 / reader first
 * dispatched at 317, in all sixteen traces). If a reader of ldreq_result,
 * Check_PL_Load or Check_LDREQ_Clear is ever added inside character select
 * BEFORE Exit_No leaves 0, that reasoning is void and this barrier alone
 * will not save it.
 *
 * It is rollback-order independent, which the narrower alternatives are
 * not. A speculative leg that re-issues a request (Sel_PL_3rd's one-shot
 * gate is restored by a rollback but the queue is not) drains it inside
 * that same leg, so a peer that rolled back and a peer that did not
 * finish the frame with identical ldreq_result[]. Suppressing the pump
 * on resimulated frames instead does NOT achieve this: the confirm
 * input reaches the two peers at different points (local input is known
 * at forward-advance time, remote input arrives late and lands in a
 * resim), so the push falls on opposite sides of that frame's pump and
 * the peers end up one pump-step apart.
 *
 * It also strictly reduces the risk of the Game2_0/Game2_2
 * fatal_error("Load queue failed to drain in time") (game.c:479, :622):
 * on the barrier path the queue is already empty when those run.
 *
 * COST. The frame that issues a load absorbs the whole read instead of
 * spreading it across frames. That is a stall, so it is bounded twice —
 * LDREQ_BARRIER_BUDGET_MS wall clock and LDREQ_BARRIER_MAX_STEPS pump
 * steps — and the budget is set well under GekkoNet's 5000 ms
 * NetStats::DISCONNECT_TIMEOUT (third_party/GekkoNet/build/include/
 * net.h:125), which is measured from the last packet of any type
 * received from us. A remote peer that runs out of prediction window
 * while we stall simply stops advancing (GekkoLib src/game_session.cpp:
 * 145-152) and resumes when we do; it does not error. Blowing the budget
 * logs and returns control to the frame loop — degraded to stock
 * behaviour, never a hang, which matters because Exit_6th has no timeout
 * of its own.
 *
 * OFFLINE IS UNTOUCHED. Ldreq_BarrierActive() is false with no session
 * and no harness override, and the early return below is placed after
 * the stock single-step pump, so a non-netplay frame executes exactly
 * the instructions it executed before this change.
 */
void Check_LDREQ_Queue() {
    disp_ldreq_status();

    if (ldreq_break) {
        if (q_ldreq->be == 1) {
            fsCansel(q_ldreq);
        }

        Init_Load_Request_Queue_1st();
        return;
    }

    if (q_ldreq->be == 0) {
        return;
    }

    ldreq_pump_head();

    if (!Ldreq_BarrierActive()) {
        return;
    }

    const Uint64 start_ns = SDL_GetTicksNS();
    const Uint64 budget_ns = (Uint64)LDREQ_BARRIER_BUDGET_MS * SDL_NS_PER_MS;
#if ENABLE_PERF_TELEMETRY
    /* Sampled HERE, before the drain loop, because the telemetry block at
     * the bottom reports the DELTA across the stall; taking it at the point
     * of use would measure nothing. Guarded rather than hoisted for the same
     * reason: with ENABLE_PERF_TELEMETRY=OFF -- the Miyoo profile, see
     * sdl_app.c's note on the same option -- nothing consumes it and an
     * unconditional declaration is an unused variable, which -Werror turns
     * into a hard build failure. That failure was real and pre-existing on
     * this branch; task #67's -Wundef work surfaced it by building the OFF
     * configuration, and it is fixed here rather than deferred. start_ns
     * above stays unguarded: the budget check uses it in both configs. */
    const unsigned long long start_bytes = AFS_GetTotalBytesRequested();
#endif
    int steps = 0;
#if ENABLE_PERF_TELEMETRY
    int io_wait_steps = 0;
    Uint64 io_wait_ns = 0;
#endif

    while (q_ldreq->be != 0) {
        if (steps >= LDREQ_BARRIER_MAX_STEPS || (SDL_GetTicksNS() - start_ns) > budget_ns) {
            /* Budget blown. Never spin: hand the frame back so the outer
             * loop keeps polling the network and rendering. The session
             * is now exposed to the original divergence, which is why
             * this is logged unconditionally rather than behind the
             * telemetry gate. */
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "[ldreq-barrier] budget exceeded after %d steps / %llu ms — head be=%d type=%d rno=%d",
                         steps,
                         (unsigned long long)((SDL_GetTicksNS() - start_ns) / SDL_NS_PER_MS),
                         (int)q_ldreq->be, (int)q_ldreq->type, (int)q_ldreq->rno);
            break;
        }

        /* A 1 ms wait is needed only while AFS owns an outstanding read or
         * close.  `be` is not enough to tell: after a completed request is
         * shifted, the next head can still be at a purely local transition
         * while the old handle closes, and an ordinary head starts at be=2.
         *
         * Waiting unconditionally charged every state-machine transition a
         * full millisecond when no I/O was pending.  The field trace exposed
         * that shape directly: 132 steps took 135 ms and 164 took 169 ms.
         * Polling in that case preserves the full-drain invariant while
         * letting open/allocate/queue/complete transitions run immediately.
         * When a read or close is in flight, retain the bounded wait so an
         * actual slow disk cannot turn this loop into a busy spin. */
        if (fsCheckCommandExecuting() != 0) {
#if ENABLE_PERF_TELEMETRY
            const Uint64 wait_start_ns = SDL_GetTicksNS();
#endif
            AFS_PumpBlocking(1);
#if ENABLE_PERF_TELEMETRY
            io_wait_ns += SDL_GetTicksNS() - wait_start_ns;
            io_wait_steps += 1;
#endif
        } else {
            AFS_RunServer();
        }
        ldreq_pump_head();
        steps += 1;
    }

#if ENABLE_PERF_TELEMETRY
    /* Field measurement of exactly what this stall costs: how long the
     * frame loop was blocked and how many bytes of AFS the block covered.
     * The byte figure is the one that transfers across machines — the
     * same bytes are read on MiSTer, just from SD instead of a warm page
     * cache — so a device log answers the stall-length question with a
     * measurement instead of an extrapolation. */
    if (steps > 0) {
        const Uint64 elapsed_ms = (SDL_GetTicksNS() - start_ns) / SDL_NS_PER_MS;
        flLogOut("[ldreq-barrier] drained in %d steps / %u ms / %u bytes / io-wait=%u ms (%d steps)\n",
                 steps, (unsigned)elapsed_ms,
                 (unsigned)(AFS_GetTotalBytesRequested() - start_bytes),
                 (unsigned)(io_wait_ns / SDL_NS_PER_MS), io_wait_steps);
    }
#endif
}

void disp_ldreq_status() {
    s16 i;

    flPrintColor(0xFFFFFF8F);

    if (Debug_w[0xE]) {
        for (i = 0; i < 16; i++) {
            flPrintL(2, i + 18, "%1d", q_ldreq[i].be);
            flPrintL(3, i + 18, ldreq_process_name[q_ldreq[i].type]);
        }

        flPrintL(2, i + 18, "%4d", system_timer);
    }
}

s32 Check_LDREQ_Clear() {
    return q_ldreq->be == 0 && q_ldreq[1].be == 0;
}

s32 Check_LDREQ_Queue_Player(s16 id) {
    s16 i;
    s16 kara;
    s16 made;

    kara = ldreq_ix[plt_req[id]][0];
    made = kara + ldreq_ix[plt_req[id]][1];

    for (i = kara; i < made; i++) {
        if (!(ldreq_result[i] & lpr_wrdata[id])) {
            break;
        }
    }

    if (i != made) {
        return 0;
    }

    return 1;
}

s32 Check_LDREQ_Queue_BG(s16 ix) {
    return Check_LDREQ_Queue_Union(ix + 20);
}

s32 Check_LDREQ_Queue_Union(s16 ix) {
    s16 i;
    s16 kara;
    s16 made;

    kara = ldreq_ix[ix][0];
    made = kara + ldreq_ix[ix][1];

    for (i = kara; i < made; i++) {
        if (!(ldreq_result[i] & lpr_wrdata[2])) {
            break;
        }
    }

    if (i != made) {
        return 0;
    }

    return 1;
}

s32 Check_LDREQ_Queue_Direct(s16 ix) {
    if (!(ldreq_result[ix] & lpr_wrdata[2])) {
        return 0;
    }

    return 1;
}

void q_ldreq_error(REQ* curr) {
    curr->be = 0;
    flLogOut("Q_LDREQ_ERROR : ロード処理の指定に誤りがあります。\n");
}

const LDREQ_Process_Func ldreq_process[6] = { q_ldreq_error,      q_ldreq_texture_group, q_ldreq_color_data,
                                              q_ldreq_color_data, q_ldreq_color_data,    q_ldreq_color_data };

s8* ldreq_process_name[] = { "EMP", "TEX", "COL", "SCR", "SND", "KNJ" };

/* These two are parallel tables indexed by the same `q_ldreq[].type`:
 * ldreq_process[type](...) dispatches the handler, ldreq_process_name[type]
 * labels it on the debug overlay. Only the first is bounded -- the name table
 * is declared `s8* ldreq_process_name[]`, size inferred from its initialiser
 * -- so adding a seventh handler without a seventh name is not a compile
 * error, it is an out-of-bounds read of a char* that the overlay then
 * dereferences and prints. Asserted here, after both definitions, because the
 * forward declaration of the name table is an incomplete type. */
_Static_assert(SDL_arraysize(ldreq_process) == SDL_arraysize(ldreq_process_name),
               "ldreq_process and ldreq_process_name must stay parallel — both "
               "are indexed by q_ldreq[].type, but only ldreq_process has a "
               "declared bound");

const LDREQ_TBL ldreq_tbl[294] = {
    {
        0x1,
        0x1,
        0x2,
        0x3,
    },
    {
        0x1,
        0x1B,
        0x2,
        0x3,
    },
    {
        0x1,
        0x23,
        0x2,
        0x3,
    },
    {
        0x2,
        0x0,
        0x1,
        0xA,
    },
    {
        0x4,
        0x82,
        0x1,
        0x16,
    },
    {
        0x1,
        0x2,
        0x2,
        0x3,
    },
    {
        0x2,
        0x1,
        0x1,
        0xA,
    },
    {
        0x4,
        0x83,
        0x1,
        0x16,
    },
    {
        0x4,
        0x99,
        0x1,
        0x15,
    },
    {
        0x1,
        0x26,
        0x1,
        0xC,
    },
    {
        0x1,
        0x3,
        0x2,
        0x3,
    },
    {
        0x1,
        0x59,
        0x2,
        0x3,
    },
    {
        0x2,
        0x2,
        0x1,
        0xA,
    },
    {
        0x4,
        0x84,
        0x1,
        0x16,
    },
    {
        0x0,
        0x0,
        0x0,
        0x0,
    },
    {
        0x1,
        0x4,
        0x2,
        0x3,
    },
    {
        0x2,
        0x3,
        0x1,
        0xA,
    },
    {
        0x2,
        0x55,
        0x1,
        0xA,
    },
    {
        0x4,
        0x85,
        0x1,
        0x16,
    },
    {
        0x1,
        0x5,
        0x2,
        0x3,
    },
    {
        0x2,
        0x4,
        0x1,
        0xA,
    },
    {
        0x4,
        0x86,
        0x1,
        0x16,
    },
    {
        0x2,
        0x54,
        0x1,
        0x2,
    },
    {
        0x1,
        0x3D,
        0x1,
        0x2,
    },
    {
        0x3,
        0x9A,
        0x1,
        0x1D,
    },
    {
        0x1,
        0x6,
        0x2,
        0x3,
    },
    {
        0x1,
        0x1B,
        0x2,
        0x3,
    },
    {
        0x2,
        0x5,
        0x1,
        0xA,
    },
    {
        0x2,
        0x56,
        0x1,
        0xA,
    },
    {
        0x4,
        0x87,
        0x1,
        0x16,
    },
    {
        0x1,
        0x7,
        0x2,
        0x3,
    },
    {
        0x2,
        0x6,
        0x1,
        0xA,
    },
    {
        0x2,
        0x9B,
        0x1,
        0xA,
    },
    {
        0x4,
        0x88,
        0x1,
        0x16,
    },
    {
        0x0,
        0x0,
        0x0,
        0x0,
    },
    {
        0x1,
        0x8,
        0x2,
        0x3,
    },
    {
        0x1,
        0x5A,
        0x2,
        0x3,
    },
    {
        0x2,
        0x7,
        0x1,
        0xA,
    },
    {
        0x2,
        0x57,
        0x1,
        0xA,
    },
    {
        0x4,
        0x89,
        0x1,
        0x16,
    },
    {
        0x1,
        0x9,
        0x2,
        0x3,
    },
    {
        0x2,
        0x8,
        0x1,
        0xA,
    },
    {
        0x4,
        0x8A,
        0x1,
        0x16,
    },
    {
        0x0,
        0x0,
        0x0,
        0x0,
    },
    {
        0x0,
        0x0,
        0x0,
        0x0,
    },
    {
        0x1,
        0xA,
        0x2,
        0x3,
    },
    {
        0x1,
        0x5B,
        0x2,
        0x3,
    },
    {
        0x2,
        0x9,
        0x1,
        0xA,
    },
    {
        0x4,
        0x8B,
        0x1,
        0x16,
    },
    {
        0x0,
        0x0,
        0x0,
        0x0,
    },
    {
        0x1,
        0xB,
        0x2,
        0x3,
    },
    {
        0x2,
        0xA,
        0x1,
        0xA,
    },
    {
        0x4,
        0x8C,
        0x1,
        0x16,
    },
    {
        0x0,
        0x0,
        0x0,
        0x0,
    },
    {
        0x0,
        0x0,
        0x0,
        0x0,
    },
    {
        0x1,
        0xC,
        0x2,
        0x3,
    },
    {
        0x1,
        0x5C,
        0x2,
        0x3,
    },
    {
        0x2,
        0xB,
        0x1,
        0xA,
    },
    {
        0x4,
        0x8D,
        0x1,
        0x16,
    },
    {
        0x0,
        0x0,
        0x0,
        0x0,
    },
    {
        0x1,
        0xD,
        0x2,
        0x3,
    },
    {
        0x1,
        0x5D,
        0x2,
        0x3,
    },
    {
        0x2,
        0xC,
        0x1,
        0xA,
    },
    {
        0x4,
        0x8E,
        0x1,
        0x16,
    },
    {
        0x0,
        0x0,
        0x0,
        0x0,
    },
    {
        0x1,
        0xE,
        0x2,
        0x3,
    },
    {
        0x1,
        0x5E,
        0x2,
        0x3,
    },
    {
        0x2,
        0xD,
        0x1,
        0xA,
    },
    {
        0x4,
        0x8F,
        0x1,
        0x16,
    },
    {
        0x0,
        0x0,
        0x0,
        0x0,
    },
    {
        0x1,
        0xF,
        0x2,
        0x3,
    },
    {
        0x1,
        0x5F,
        0x2,
        0x3,
    },
    {
        0x2,
        0xE,
        0x1,
        0xA,
    },
    {
        0x4,
        0x90,
        0x1,
        0x16,
    },
    {
        0x0,
        0x0,
        0x0,
        0x0,
    },
    {
        0x1,
        0x10,
        0x2,
        0x3,
    },
    {
        0x1,
        0x60,
        0x2,
        0x3,
    },
    {
        0x2,
        0xF,
        0x1,
        0xA,
    },
    {
        0x4,
        0x91,
        0x1,
        0x16,
    },
    {
        0x0,
        0x0,
        0x0,
        0x0,
    },
    {
        0x1,
        0x11,
        0x2,
        0x3,
    },
    {
        0x2,
        0x10,
        0x1,
        0xA,
    },
    {
        0x4,
        0x92,
        0x1,
        0x16,
    },
    {
        0x0,
        0x0,
        0x0,
        0x0,
    },
    {
        0x0,
        0x0,
        0x0,
        0x0,
    },
    {
        0x1,
        0x12,
        0x2,
        0x3,
    },
    {
        0x2,
        0x11,
        0x1,
        0xA,
    },
    {
        0x4,
        0x93,
        0x1,
        0x16,
    },
    {
        0x0,
        0x0,
        0x0,
        0x0,
    },
    {
        0x0,
        0x0,
        0x0,
        0x0,
    },
    {
        0x1,
        0x13,
        0x2,
        0x3,
    },
    {
        0x2,
        0x12,
        0x1,
        0xA,
    },
    {
        0x4,
        0x94,
        0x1,
        0x16,
    },
    {
        0x0,
        0x0,
        0x0,
        0x0,
    },
    {
        0x0,
        0x0,
        0x0,
        0x0,
    },
    {
        0x1,
        0x14,
        0x2,
        0x3,
    },
    {
        0x1,
        0x61,
        0x2,
        0x3,
    },
    {
        0x2,
        0x13,
        0x1,
        0xA,
    },
    {
        0x4,
        0x95,
        0x1,
        0x16,
    },
    {
        0x0,
        0x0,
        0x0,
        0x0,
    },
    {
        0x2,
        0x15,
        0x1,
        0x2,
    },
    {
        0x1,
        0x53,
        0x1,
        0x13,
    },
    {
        0x1,
        0x54,
        0x1,
        0x13,
    },
    {
        0x1,
        0x34,
        0x1,
        0x13,
    },
    {
        0x3,
        0x29,
        0x1,
        0x12,
    },
    {
        0x2,
        0x16,
        0x1,
        0x2,
    },
    {
        0x1,
        0x2C,
        0x1,
        0x13,
    },
    {
        0x3,
        0x2A,
        0x1,
        0x12,
    },
    {
        0x0,
        0x0,
        0x0,
        0x0,
    },
    {
        0x0,
        0x0,
        0x0,
        0x0,
    },
    {
        0x2,
        0x17,
        0x1,
        0x2,
    },
    {
        0x1,
        0x3A,
        0x1,
        0x13,
    },
    {
        0x3,
        0x2B,
        0x1,
        0x12,
    },
    {
        0x0,
        0x0,
        0x0,
        0x0,
    },
    {
        0x0,
        0x0,
        0x0,
        0x0,
    },
    {
        0x2,
        0x18,
        0x1,
        0x2,
    },
    {
        0x1,
        0x2D,
        0x1,
        0x13,
    },
    {
        0x3,
        0x2C,
        0x1,
        0x12,
    },
    {
        0x0,
        0x0,
        0x0,
        0x0,
    },
    {
        0x0,
        0x0,
        0x0,
        0x0,
    },
    {
        0x2,
        0x19,
        0x1,
        0x2,
    },
    {
        0x1,
        0x32,
        0x1,
        0x13,
    },
    {
        0x3,
        0x2D,
        0x1,
        0x12,
    },
    {
        0x0,
        0x0,
        0x0,
        0x0,
    },
    {
        0x0,
        0x0,
        0x0,
        0x0,
    },
    {
        0x2,
        0x1A,
        0x1,
        0x2,
    },
    {
        0x1,
        0x2A,
        0x1,
        0x13,
    },
    {
        0x3,
        0x2E,
        0x1,
        0x12,
    },
    {
        0x0,
        0x0,
        0x0,
        0x0,
    },
    {
        0x0,
        0x0,
        0x0,
        0x0,
    },
    {
        0x2,
        0x1B,
        0x1,
        0x2,
    },
    {
        0x1,
        0x2F,
        0x1,
        0x13,
    },
    {
        0x3,
        0x2F,
        0x1,
        0x12,
    },
    {
        0x0,
        0x0,
        0x0,
        0x0,
    },
    {
        0x0,
        0x0,
        0x0,
        0x0,
    },
    {
        0x2,
        0x1C,
        0x1,
        0x2,
    },
    {
        0x1,
        0x35,
        0x1,
        0x13,
    },
    {
        0x3,
        0x30,
        0x1,
        0x12,
    },
    {
        0x0,
        0x0,
        0x0,
        0x0,
    },
    {
        0x0,
        0x0,
        0x0,
        0x0,
    },
    {
        0x2,
        0x1D,
        0x1,
        0x2,
    },
    {
        0x1,
        0x2B,
        0x1,
        0x13,
    },
    {
        0x3,
        0x31,
        0x1,
        0x12,
    },
    {
        0x0,
        0x0,
        0x0,
        0x0,
    },
    {
        0x0,
        0x0,
        0x0,
        0x0,
    },
    {
        0x2,
        0x1E,
        0x1,
        0x2,
    },
    {
        0x1,
        0x30,
        0x1,
        0x13,
    },
    {
        0x3,
        0x32,
        0x1,
        0x12,
    },
    {
        0x0,
        0x0,
        0x0,
        0x0,
    },
    {
        0x0,
        0x0,
        0x0,
        0x0,
    },
    {
        0x2,
        0x1F,
        0x1,
        0x2,
    },
    {
        0x1,
        0x55,
        0x1,
        0x13,
    },
    {
        0x3,
        0x33,
        0x1,
        0x12,
    },
    {
        0x0,
        0x0,
        0x0,
        0x0,
    },
    {
        0x0,
        0x0,
        0x0,
        0x0,
    },
    {
        0x2,
        0x20,
        0x1,
        0x2,
    },
    {
        0x1,
        0x56,
        0x1,
        0x13,
    },
    {
        0x3,
        0x34,
        0x1,
        0x12,
    },
    {
        0x0,
        0x0,
        0x0,
        0x0,
    },
    {
        0x0,
        0x0,
        0x0,
        0x0,
    },
    {
        0x2,
        0x58,
        0x1,
        0x2,
    },
    {
        0x1,
        0x57,
        0x1,
        0x13,
    },
    {
        0x3,
        0x35,
        0x1,
        0x12,
    },
    {
        0x0,
        0x0,
        0x0,
        0x0,
    },
    {
        0x0,
        0x0,
        0x0,
        0x0,
    },
    {
        0x2,
        0x21,
        0x1,
        0x2,
    },
    {
        0x1,
        0x31,
        0x1,
        0x13,
    },
    {
        0x3,
        0x36,
        0x1,
        0x12,
    },
    {
        0x0,
        0x0,
        0x0,
        0x0,
    },
    {
        0x0,
        0x0,
        0x0,
        0x0,
    },
    {
        0x2,
        0x22,
        0x1,
        0x2,
    },
    {
        0x1,
        0x2E,
        0x1,
        0x13,
    },
    {
        0x3,
        0x37,
        0x1,
        0x12,
    },
    {
        0x0,
        0x0,
        0x0,
        0x0,
    },
    {
        0x0,
        0x0,
        0x0,
        0x0,
    },
    {
        0x2,
        0x23,
        0x1,
        0x2,
    },
    {
        0x1,
        0x38,
        0x1,
        0x13,
    },
    {
        0x3,
        0x38,
        0x1,
        0x12,
    },
    {
        0x0,
        0x0,
        0x0,
        0x0,
    },
    {
        0x0,
        0x0,
        0x0,
        0x0,
    },
    {
        0x2,
        0x24,
        0x1,
        0x2,
    },
    {
        0x1,
        0x33,
        0x1,
        0x13,
    },
    {
        0x3,
        0x39,
        0x1,
        0x12,
    },
    {
        0x0,
        0x0,
        0x0,
        0x0,
    },
    {
        0x0,
        0x0,
        0x0,
        0x0,
    },
    {
        0x2,
        0x20,
        0x1,
        0x2,
    },
    {
        0x1,
        0x56,
        0x1,
        0x13,
    },
    {
        0x3,
        0x34,
        0x1,
        0x12,
    },
    {
        0x0,
        0x0,
        0x0,
        0x0,
    },
    {
        0x0,
        0x0,
        0x0,
        0x0,
    },
    {
        0x2,
        0x25,
        0x1,
        0x2,
    },
    {
        0x1,
        0x58,
        0x1,
        0x13,
    },
    {
        0x3,
        0x3B,
        0x1,
        0x12,
    },
    {
        0x0,
        0x0,
        0x0,
        0x0,
    },
    {
        0x0,
        0x0,
        0x0,
        0x0,
    },
    {
        0x2,
        0x26,
        0x1,
        0x2,
    },
    {
        0x1,
        0x37,
        0x1,
        0x13,
    },
    {
        0x3,
        0x3C,
        0x1,
        0x12,
    },
    {
        0x0,
        0x0,
        0x0,
        0x0,
    },
    {
        0x0,
        0x0,
        0x0,
        0x0,
    },
    {
        0x2,
        0x27,
        0x1,
        0x2,
    },
    {
        0x1,
        0x36,
        0x1,
        0x13,
    },
    {
        0x1,
        0x22,
        0x2,
        0x13,
    },
    {
        0x3,
        0x3D,
        0x1,
        0x12,
    },
    {
        0x1,
        0x21,
        0x2,
        0x13,
    },
    {
        0x0,
        0x0,
        0x0,
        0x0,
    },
    {
        0x2,
        0x28,
        0x1,
        0x2,
    },
    {
        0x1,
        0x3B,
        0x1,
        0x13,
    },
    {
        0x1,
        0x22,
        0x2,
        0x13,
    },
    {
        0x3,
        0x3E,
        0x1,
        0x12,
    },
    {
        0x0,
        0x0,
        0x0,
        0x0,
    },
    {
        0x0,
        0x0,
        0x0,
        0x0,
    },
    {
        0x2,
        0x40,
        0x1,
        0x2,
    },
    {
        0x2,
        0x41,
        0x1,
        0x2,
    },
    {
        0x2,
        0x42,
        0x1,
        0x2,
    },
    {
        0x2,
        0x43,
        0x1,
        0x2,
    },
    {
        0x2,
        0x44,
        0x1,
        0x2,
    },
    {
        0x2,
        0x45,
        0x1,
        0x2,
    },
    {
        0x2,
        0x46,
        0x1,
        0x2,
    },
    {
        0x2,
        0x47,
        0x1,
        0x2,
    },
    {
        0x2,
        0x48,
        0x1,
        0x2,
    },
    {
        0x2,
        0x49,
        0x1,
        0x2,
    },
    {
        0x2,
        0x4A,
        0x1,
        0x2,
    },
    {
        0x2,
        0x4B,
        0x1,
        0x2,
    },
    {
        0x2,
        0x4C,
        0x1,
        0x2,
    },
    {
        0x2,
        0x4D,
        0x1,
        0x2,
    },
    {
        0x2,
        0x4E,
        0x1,
        0x2,
    },
    {
        0x2,
        0x4F,
        0x1,
        0x2,
    },
    {
        0x2,
        0x50,
        0x1,
        0x2,
    },
    {
        0x2,
        0x51,
        0x1,
        0x2,
    },
    {
        0x2,
        0x52,
        0x1,
        0x2,
    },
    {
        0x2,
        0x53,
        0x1,
        0x2,
    },
    {
        0x4,
        0x6E,
        0x1,
        0x16,
    },
    {
        0x4,
        0x6F,
        0x1,
        0x16,
    },
    {
        0x4,
        0x70,
        0x1,
        0x16,
    },
    {
        0x4,
        0x71,
        0x1,
        0x16,
    },
    {
        0x4,
        0x72,
        0x1,
        0x16,
    },
    {
        0x4,
        0x73,
        0x1,
        0x16,
    },
    {
        0x4,
        0x74,
        0x1,
        0x16,
    },
    {
        0x4,
        0x75,
        0x1,
        0x16,
    },
    {
        0x4,
        0x76,
        0x1,
        0x16,
    },
    {
        0x4,
        0x77,
        0x1,
        0x16,
    },
    {
        0x4,
        0x78,
        0x1,
        0x16,
    },
    {
        0x4,
        0x79,
        0x1,
        0x16,
    },
    {
        0x4,
        0x7A,
        0x1,
        0x16,
    },
    {
        0x4,
        0x7B,
        0x1,
        0x16,
    },
    {
        0x4,
        0x7C,
        0x1,
        0x16,
    },
    {
        0x4,
        0x7D,
        0x1,
        0x16,
    },
    {
        0x4,
        0x7E,
        0x1,
        0x16,
    },
    {
        0x4,
        0x7F,
        0x1,
        0x16,
    },
    {
        0x4,
        0x80,
        0x1,
        0x16,
    },
    {
        0x4,
        0x81,
        0x1,
        0x16,
    },
    {
        0x4,
        0x82,
        0x1,
        0x16,
    },
    {
        0x4,
        0x83,
        0x1,
        0x16,
    },
    {
        0x4,
        0x84,
        0x1,
        0x16,
    },
    {
        0x4,
        0x85,
        0x1,
        0x16,
    },
    {
        0x4,
        0x86,
        0x1,
        0x16,
    },
    {
        0x4,
        0x87,
        0x1,
        0x16,
    },
    {
        0x4,
        0x88,
        0x1,
        0x16,
    },
    {
        0x4,
        0x89,
        0x1,
        0x16,
    },
    {
        0x4,
        0x8A,
        0x1,
        0x16,
    },
    {
        0x4,
        0x8B,
        0x1,
        0x16,
    },
    {
        0x4,
        0x82,
        0x1,
        0x16,
    },
    {
        0x4,
        0x83,
        0x1,
        0x16,
    },
    {
        0x4,
        0x84,
        0x1,
        0x16,
    },
    {
        0x4,
        0x85,
        0x1,
        0x16,
    },
    {
        0x4,
        0x86,
        0x1,
        0x16,
    },
    {
        0x4,
        0x87,
        0x1,
        0x16,
    },
    {
        0x4,
        0x88,
        0x1,
        0x16,
    },
    {
        0x4,
        0x89,
        0x1,
        0x16,
    },
    {
        0x4,
        0x8A,
        0x1,
        0x16,
    },
    {
        0x4,
        0x8B,
        0x1,
        0x16,
    },
    {
        0x2,
        0x0,
        0x1,
        0xA,
    },
    {
        0x2,
        0x1,
        0x1,
        0xA,
    },
    {
        0x2,
        0x2,
        0x1,
        0xA,
    },
    {
        0x2,
        0x3,
        0x1,
        0xA,
    },
    {
        0x2,
        0x4,
        0x1,
        0xA,
    },
    {
        0x2,
        0x5,
        0x1,
        0xA,
    },
    {
        0x2,
        0x6,
        0x1,
        0xA,
    },
    {
        0x2,
        0x7,
        0x1,
        0xA,
    },
    {
        0x2,
        0x8,
        0x1,
        0xA,
    },
    {
        0x2,
        0x9,
        0x1,
        0xA,
    },
    {
        0x2,
        0xA,
        0x1,
        0xA,
    },
    {
        0x2,
        0xB,
        0x1,
        0xA,
    },
    {
        0x2,
        0xC,
        0x1,
        0xA,
    },
    {
        0x2,
        0xD,
        0x1,
        0xA,
    },
    {
        0x2,
        0xE,
        0x1,
        0xA,
    },
    {
        0x2,
        0xF,
        0x1,
        0xA,
    },
    {
        0x2,
        0x10,
        0x1,
        0xA,
    },
    {
        0x2,
        0x11,
        0x1,
        0xA,
    },
    {
        0x2,
        0x12,
        0x1,
        0xA,
    },
    {
        0x2,
        0x13,
        0x1,
        0xA,
    },
    {
        0x5,
        0x97,
        0x2,
        0x19,
    },
    {
        0x5,
        0x98,
        0x2,
        0x1A,
    },
};

const s16 ldreq_ix[43][2] = { { 0x0000, 0x0005 }, { 0x0005, 0x0003 }, { 0x000A, 0x0004 }, { 0x000F, 0x0004 },
                              { 0x0013, 0x0003 }, { 0x0019, 0x0005 }, { 0x001E, 0x0004 }, { 0x0023, 0x0005 },
                              { 0x0028, 0x0003 }, { 0x002D, 0x0004 }, { 0x0032, 0x0003 }, { 0x0037, 0x0004 },
                              { 0x003C, 0x0004 }, { 0x0041, 0x0004 }, { 0x0046, 0x0004 }, { 0x004B, 0x0004 },
                              { 0x0050, 0x0003 }, { 0x0055, 0x0003 }, { 0x005A, 0x0003 }, { 0x005F, 0x0004 },
                              { 0x0064, 0x0005 }, { 0x0069, 0x0003 }, { 0x006E, 0x0003 }, { 0x0073, 0x0003 },
                              { 0x0078, 0x0003 }, { 0x007D, 0x0003 }, { 0x0082, 0x0003 }, { 0x0087, 0x0003 },
                              { 0x008C, 0x0003 }, { 0x0091, 0x0003 }, { 0x0096, 0x0003 }, { 0x009B, 0x0003 },
                              { 0x00A0, 0x0003 }, { 0x00A5, 0x0003 }, { 0x00AA, 0x0003 }, { 0x00AF, 0x0003 },
                              { 0x00B4, 0x0003 }, { 0x00B9, 0x0003 }, { 0x00BE, 0x0003 }, { 0x00C3, 0x0003 },
                              { 0x00C8, 0x0005 }, { 0x00CE, 0x0004 }, { 0x0016, 0x0003 } };
