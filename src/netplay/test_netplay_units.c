/*
 * test_netplay_units.c — task #132 priority 3.
 *
 * THE PROBLEM THIS FILE SOLVES IS WALL CLOCK, NOT COVERAGE. Every
 * assertion here already existed; every one of them lived inside
 * src/netplay/test_bilateral_punch.c, behind scenario tests that open
 * real sockets, spawn threads and sleep. Measured on this machine
 * (per-test SDL_GetTicks instrumentation, whole harness 110.3 s):
 *
 *     test_race_worst_case_timing         26447 ms
 *     test_race_two_peer_convergence      17956 ms
 *     test_s7_lost_mapping                11776 ms
 *     test_joiner_cookie_handshake        10481 ms
 *     test_joiner_self_deliver            10236 ms
 *     test_s7_review_fixes                 7549 ms
 *     ... 27 more
 *
 * Because the harness is one linear dispatch, the CUMULATIVE cost to
 * REACH a block is what an engineer iterating on it actually pays. The
 * NAT-PMP/PCP wire codec below sat ~88 s into the run; the ladder-shape
 * and renewal-cadence tables sat ~90 s in. Not one of those assertions
 * touches a socket, a thread, a clock or the filesystem.
 *
 * So they were MOVED here — moved, not copied: nothing is asserted
 * twice, and test_bilateral_punch.c keeps every scenario test it had,
 * because the integration path is exactly what those tests are for and
 * a mocked unit would pass vacuously in their place.
 *
 * THE RULE FOR THIS FILE: no sockets, no threads, no SDL_Delay, no
 * wall-clock waits, no filesystem, no network. A test that needs any of
 * those belongs in test_bilateral_punch.c. The whole point is that this
 * harness finishes in milliseconds, so if it ever starts taking a
 * measurable amount of time, something has been added that does not
 * belong.
 *
 * Config is a deliberate borderline case and is NOT used here: the
 * config table is in-memory (Config_Save is a no-op), but a test that
 * writes global config is order-dependent on every other test in the
 * process. test_nav_orch_deadline_is_derived stays in the scenario
 * harness for that reason.
 */

#include <stdio.h>
#include <stdlib.h>

#ifdef ENABLE_NETPLAY_TESTS

#ifndef NETPLAY_TEST_HOOKS
#error "test_netplay_units.c needs BOTH -DENABLE_NETPLAY_TESTS and -DNETPLAY_TEST_HOOKS. Half of these blocks reach production decisions through DirectP2P_TestHook_* / Natpmp_TestHook_* trampolines; without the hooks this TU would compile to a harness that silently tests less than it claims."
#endif

#include "netplay/connect_fail.h"
#include "netplay/direct_p2p.h"
#include "netplay/natpmp.h"
#include "netplay/netplay.h" /* #145: Netplay_TestHook_MenuExit* predicates */
#include "netplay/rendezvous.h"
#include "netplay/stun.h"
#include "platform/video/software/software_renderer.h"
#include "rendering/game_renderer.h"
#include "sf33rd/Source/Common/PPGWork.h"
#include "sf33rd/Source/Game/stage/bg.h"
#include "sf33rd/Source/Game/system/ramcnt.h"

#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#endif

/* Wire constants must mirror rendezvous.c — duplicated locally so this
 * TU is independent of rendezvous.c's file-statics. Same duplication,
 * for the same reason, as test_bilateral_punch.c:89-104. */
#define REND_MAGIC_BYTES_0 0x33u  /* '3' */
#define REND_MAGIC_BYTES_1 0x53u  /* 'S' */
#define REND_MAGIC_BYTES_2 0x58u  /* 'X' */
#define REND_MAGIC_BYTES_3 0x52u  /* 'R' */
#define REND_VERSION       2  /* S4c: protocol v2 */
#define REND_TYPE_REGISTER 1
#define REND_TYPE_DELIVER  2
#define REND_TYPE_POLL     3
#define REND_TYPE_CHALLENGE 4  /* S4c: server -> client cookie challenge */

#define REND_REGISTER_LEN  36  /* S4c: 28 + 8-byte cookie tail */
#define REND_DELIVER_LEN   32
#define REND_CHALLENGE_LEN 32
#define REND_KEY_LEN       16

static int fail_count = 0;
static int tests_run = 0;
static int checks_run = 0;

/* Anti-vacuity, the same two devices the other fast harnesses carry.
 * EXPECTED_TESTS is a literal, not a count of anything this file
 * computes, so commenting a call out of the dispatch is a FAILURE and
 * not a smaller green run. The assertion floor catches the other shape:
 * a test that runs but whose body was short-circuited. */
#define EXPECTED_TESTS 18

/* The real figure is 1100 and is printed in the summary. This sits below
 * it and above what a short-circuited run would produce. Not an exact
 * count — that invites bumping the number instead of asking why it
 * moved. Note it counts only EXPECT_TRUE/EXPECT_FALSE; several moved
 * blocks also assert through raw fprintf + fail_count++, so the true
 * assertion density is higher than this number suggests. */
#define EXPECTED_MIN_CHECKS 900

static void fail_at(const char* file, int line, const char* tag, const char* why) {
    fprintf(stderr, "[test_netplay_units] FAIL: %s:%d: %s: %s\n",
            file, line, tag, why);
    fail_count++;
}

#define FAIL(tag, why) fail_at(__FILE__, __LINE__, (tag), (why))

#define EXPECT_TRUE(tag, cond) do { \
    checks_run++; \
    if (!(cond)) { fail_at(__FILE__, __LINE__, (tag), "expected true: " #cond); } \
} while (0)

#define EXPECT_FALSE(tag, cond) do { \
    checks_run++; \
    if ((cond)) { fail_at(__FILE__, __LINE__, (tag), "expected false: " #cond); } \
} while (0)

/* Build an S4c CHALLENGE packet, v2 wire layout:
 *   magic(4) ver(1) type(1)=4 reserved(2) key(16) cookie(8).
 * The key echoed back is the one the REQUEST carried — that is what
 * Rendezvous_ParseChallenge's session-key gate matches against. Mirrors
 * test_bilateral_punch.c's helper of the same name. */
static int build_challenge(uint8_t out[REND_CHALLENGE_LEN],
                           const uint8_t key[REND_KEY_LEN],
                           const uint8_t cookie[REND_COOKIE_LEN]) {
    memset(out, 0, REND_CHALLENGE_LEN);
    out[0] = REND_MAGIC_BYTES_0;
    out[1] = REND_MAGIC_BYTES_1;
    out[2] = REND_MAGIC_BYTES_2;
    out[3] = REND_MAGIC_BYTES_3;
    out[4] = (uint8_t)REND_VERSION;
    out[5] = (uint8_t)REND_TYPE_CHALLENGE;
    /* reserved [6..7] = 0 */
    memcpy(&out[8], key, REND_KEY_LEN);
    memcpy(&out[24], cookie, REND_COOKIE_LEN);
    return REND_CHALLENGE_LEN;
}

/* Build a DELIVER packet for `peer` (or zeros if peer is NULL meaning
 * "not yet registered"). Returns the 32-byte packet length. Mirrors
 * test_bilateral_punch.c's helper of the same name; the host-datagram
 * gate block below needs one DELIVER-shaped datagram and nothing else. */
static int build_deliver(uint8_t out[REND_DELIVER_LEN],
                         const uint8_t key[REND_KEY_LEN],
                         const struct sockaddr_in* peer,
                         uint16_t peer_public_port) {
    memset(out, 0, REND_DELIVER_LEN);
    out[0] = REND_MAGIC_BYTES_0;
    out[1] = REND_MAGIC_BYTES_1;
    out[2] = REND_MAGIC_BYTES_2;
    out[3] = REND_MAGIC_BYTES_3;
    out[4] = (uint8_t)REND_VERSION;
    out[5] = (uint8_t)REND_TYPE_DELIVER;
    /* reserved [6..7] = 0 */
    memcpy(&out[8], key, REND_KEY_LEN);
    if (peer) {
        /* peer_ip raw 4 bytes at out[24..27] (network byte order). */
        memcpy(&out[24], &peer->sin_addr.s_addr, 4);
        /* peer_port BE at out[28..29]. */
        out[28] = (uint8_t)((peer_public_port >> 8) & 0xFFu);
        out[29] = (uint8_t)(peer_public_port & 0xFFu);
    }
    /* reserved2 [30..31] = 0 */
    return REND_DELIVER_LEN;
}

/* ---------------------------------------------------------------- */
static int unit_session_key_stability(void) {
    tests_run++;
    fprintf(stderr, "[test_netplay_units] test 2: session-key stability\n");

    uint8_t k1[REND_KEY_LEN];
    uint8_t k2[REND_KEY_LEN];
    EXPECT_TRUE("test2", Rendezvous_DeriveSessionKey(0x0A0B0C0Du, 12345, 0x222, k1));
    EXPECT_TRUE("test2", Rendezvous_DeriveSessionKey(0x0A0B0C0Du, 12345, 0x222, k2));
    if (memcmp(k1, k2, REND_KEY_LEN) != 0) {
        FAIL("test2", "deterministic derivation produced different keys");
        return 1;
    }

    /* Different ip yields different key. */
    uint8_t k3[REND_KEY_LEN];
    EXPECT_TRUE("test2", Rendezvous_DeriveSessionKey(0x0A0B0C0Eu, 12345, 0x222, k3));
    if (memcmp(k1, k3, REND_KEY_LEN) == 0) {
        FAIL("test2", "different ip produced identical key (collision)");
        return 1;
    }

    /* S4b: different NONCE (same ip:port) yields a different key —
     * this is the whole point of the nonce: (ip, port) alone no longer
     * determines the rendezvous slot. */
    uint8_t k5[REND_KEY_LEN];
    EXPECT_TRUE("test2", Rendezvous_DeriveSessionKey(0x0A0B0C0Du, 12345, 0x223, k5));
    if (memcmp(k1, k5, REND_KEY_LEN) == 0) {
        FAIL("test2", "different nonce produced identical session key");
        return 1;
    }
    /* v3: the nonce is a full 32 bits — 0x1000 is now a PERFECTLY
     * VALID nonce (it was out of range under v2's 12-bit mask), and a
     * high-bit nonce must reach the hash rather than being masked off.
     * Regression net for the widening: if any layer silently truncated
     * the nonce back to 12 or 16 bits these two would collide. */
    uint8_t k6[REND_KEY_LEN];
    EXPECT_TRUE("test2", Rendezvous_DeriveSessionKey(0x0A0B0C0Du, 12345, 0x1000u, k6));
    if (memcmp(k1, k6, REND_KEY_LEN) == 0) {
        FAIL("test2", "nonce 0x1000 produced the same key as 0x222");
        return 1;
    }
    uint8_t k7[REND_KEY_LEN];
    uint8_t k8[REND_KEY_LEN];
    EXPECT_TRUE("test2", Rendezvous_DeriveSessionKey(0x0A0B0C0Du, 12345, 0x00000222u, k7));
    EXPECT_TRUE("test2", Rendezvous_DeriveSessionKey(0x0A0B0C0Du, 12345, 0xDEAD0222u, k8));
    if (memcmp(k7, k8, REND_KEY_LEN) == 0) {
        FAIL("test2", "nonce high 16 bits ignored — 32-bit nonce truncated");
        return 1;
    }

    /* ip_be == 0 must return false. */
    uint8_t k4[REND_KEY_LEN];
    if (Rendezvous_DeriveSessionKey(0u, 12345, 0x222, k4)) {
        FAIL("test2", "ip_be=0 should return false");
        return 1;
    }

    /* --- S4a: punch-token derivation. Deterministic; sensitive to both
     * inputs; DOMAIN-SEPARATED from the session key (a token must never
     * equal a session-key prefix for the same payload); ip_be==0 fails
     * and zeroes the output. */
    uint8_t t1[REND_PUNCH_TOKEN_LEN];
    uint8_t t2[REND_PUNCH_TOKEN_LEN];
    EXPECT_TRUE("test2-token", Rendezvous_DerivePunchToken(0x0A0B0C0Du, 12345, 0x222, t1));
    EXPECT_TRUE("test2-token", Rendezvous_DerivePunchToken(0x0A0B0C0Du, 12345, 0x222, t2));
    if (memcmp(t1, t2, REND_PUNCH_TOKEN_LEN) != 0) {
        FAIL("test2-token", "deterministic token derivation produced different tokens");
        return 1;
    }
    uint8_t t3[REND_PUNCH_TOKEN_LEN];
    EXPECT_TRUE("test2-token", Rendezvous_DerivePunchToken(0x0A0B0C0Du, 12346, 0x222, t3));
    if (memcmp(t1, t3, REND_PUNCH_TOKEN_LEN) == 0) {
        FAIL("test2-token", "different port produced identical token");
        return 1;
    }
    /* S4b: different nonce -> different token. */
    uint8_t t5[REND_PUNCH_TOKEN_LEN];
    EXPECT_TRUE("test2-token", Rendezvous_DerivePunchToken(0x0A0B0C0Du, 12345, 0x223, t5));
    if (memcmp(t1, t5, REND_PUNCH_TOKEN_LEN) == 0) {
        FAIL("test2-token", "different nonce produced identical token");
        return 1;
    }
    /* v3: the token must see all 32 nonce bits too — the punch gate is
     * the thing the widening exists for (room_code.h). */
    uint8_t t6[REND_PUNCH_TOKEN_LEN];
    uint8_t t7[REND_PUNCH_TOKEN_LEN];
    EXPECT_TRUE("test2-token", Rendezvous_DerivePunchToken(0x0A0B0C0Du, 12345, 0x00000222u, t6));
    EXPECT_TRUE("test2-token", Rendezvous_DerivePunchToken(0x0A0B0C0Du, 12345, 0xDEAD0222u, t7));
    if (memcmp(t6, t7, REND_PUNCH_TOKEN_LEN) == 0) {
        FAIL("test2-token", "nonce high 16 bits ignored in punch token");
        return 1;
    }
    /* Domain separation vs the session key over the SAME payload. */
    if (memcmp(t1, k1, REND_PUNCH_TOKEN_LEN) == 0) {
        FAIL("test2-token", "token equals session-key prefix — no domain separation");
        return 1;
    }
    uint8_t t4[REND_PUNCH_TOKEN_LEN];
    memset(t4, 0xEE, sizeof(t4));
    if (Rendezvous_DerivePunchToken(0u, 12345, 0x222, t4)) {
        FAIL("test2-token", "ip_be=0 should return false");
        return 1;
    }
    for (int i = 0; i < REND_PUNCH_TOKEN_LEN; i++) {
        if (t4[i] != 0) {
            FAIL("test2-token", "failed derivation must zero the output");
            return 1;
        }
    }

    fprintf(stderr, "[test_netplay_units] test 2 OK\n");
    return 0;
}

/* ---------------------------------------------------------------- */
static int unit_lan_bypass(void) {
    tests_run++;
    fprintf(stderr, "[test_netplay_units] test 3: LAN bypass table\n");

    /* Truth table per direct_p2p.c:direct_p2p_is_lan_peer ranges. */
    struct {
        const char* ip;
        bool expect;
    } cases[] = {
        { "127.0.0.1",       true  },
        { "10.0.0.1",        true  },
        { "172.16.0.1",      true  },
        { "172.31.255.255",  true  },
        { "192.168.1.1",     true  },
        { "169.254.0.1",     true  },
        { "8.8.8.8",         false },
        { "1.2.3.4",         false },
        { "172.32.0.1",      false },
        { "192.169.0.1",     false },
    };
    const int N = (int)(sizeof(cases) / sizeof(cases[0]));
    int local_fails = 0;
    for (int i = 0; i < N; ++i) {
        const bool got = DirectP2P_TestHook_IsLanPeer(cases[i].ip);
        if (got != cases[i].expect) {
            fprintf(stderr,
                    "[test_netplay_units] FAIL: test3: %s -> %s, expected %s\n",
                    cases[i].ip,
                    got ? "true" : "false",
                    cases[i].expect ? "true" : "false");
            local_fails++;
        }
    }
    if (local_fails > 0) {
        fail_count += local_fails;
        return 1;
    }

    fprintf(stderr, "[test_netplay_units] test 3 OK (%d cases)\n", N);
    return 0;
}

/* ---------------------------------------------------------------- */
static int unit_failure_taxonomy(void) {
    tests_run++;
    /* --- 7a: Rendezvous_ParseDeliverEx tri-state split. Pre-S3 the
     * bool API conflated zero-sentinel with malformed. */
    uint8_t key[REND_KEY_LEN];
    memset(key, 0xAB, sizeof(key));

    uint8_t deliver[REND_DELIVER_LEN];
    memset(deliver, 0, sizeof(deliver));
    deliver[0] = REND_MAGIC_BYTES_0;
    deliver[1] = REND_MAGIC_BYTES_1;
    deliver[2] = REND_MAGIC_BYTES_2;
    deliver[3] = REND_MAGIC_BYTES_3;
    deliver[4] = REND_VERSION;
    deliver[5] = REND_TYPE_DELIVER;
    memcpy(&deliver[8], key, REND_KEY_LEN);
    /* peer = 0.0.0.0:0 (already zero) — the "not yet registered" sentinel. */

    char ip[64] = { 0 };
    uint16_t port = 0;
    EXPECT_TRUE("7a-empty",
                Rendezvous_ParseDeliverEx(deliver, sizeof(deliver), key, ip, &port) ==
                    REND_DELIVER_EMPTY);
    EXPECT_TRUE("7a-empty-outputs", ip[0] == '\0' && port == 0);
    /* Legacy bool wrapper still reports "no peer" for the sentinel. */
    EXPECT_FALSE("7a-empty-legacy",
                 Rendezvous_ParseDeliver(deliver, sizeof(deliver), key, ip, &port));

    /* Real endpoint 9.8.7.6:4321. */
    deliver[24] = 9; deliver[25] = 8; deliver[26] = 7; deliver[27] = 6;
    deliver[28] = (uint8_t)(4321 >> 8);
    deliver[29] = (uint8_t)(4321 & 0xFF);
    EXPECT_TRUE("7a-peer",
                Rendezvous_ParseDeliverEx(deliver, sizeof(deliver), key, ip, &port) ==
                    REND_DELIVER_PEER);
    EXPECT_TRUE("7a-peer-outputs", strcmp(ip, "9.8.7.6") == 0 && port == 4321);
    EXPECT_TRUE("7a-peer-legacy",
                Rendezvous_ParseDeliver(deliver, sizeof(deliver), key, ip, &port));

    /* Wrong session key -> MALFORMED (not EMPTY). */
    uint8_t wrong_key[REND_KEY_LEN];
    memset(wrong_key, 0xCD, sizeof(wrong_key));
    EXPECT_TRUE("7a-wrongkey",
                Rendezvous_ParseDeliverEx(deliver, sizeof(deliver), wrong_key, ip, &port) ==
                    REND_DELIVER_MALFORMED);
    /* Truncated frame -> MALFORMED. */
    EXPECT_TRUE("7a-short",
                Rendezvous_ParseDeliverEx(deliver, 16, key, ip, &port) ==
                    REND_DELIVER_MALFORMED);
    /* Wrong type (REGISTER) -> MALFORMED. */
    deliver[5] = REND_TYPE_REGISTER;
    EXPECT_TRUE("7a-wrongtype",
                Rendezvous_ParseDeliverEx(deliver, sizeof(deliver), key, ip, &port) ==
                    REND_DELIVER_MALFORMED);
    deliver[5] = REND_TYPE_DELIVER;

    /* --- 7b: STUN discovery classification (cause 1 vs cause 2). */
    /* All DNS failed, nothing answered -> no network / DNS dead. */
    EXPECT_TRUE("7b-dns-alldown",
                ConnectFail_ClassifyStunDiscover(0, 0, 0, true) == CONNECT_FAIL_DNS_ALLDOWN);
    /* DNS dead but numeric fallbacks probed + sent, still silent ->
     * DNS blackout stays the primary diagnosis. */
    EXPECT_TRUE("7b-dns-alldown-fallbacks",
                ConnectFail_ClassifyStunDiscover(3, 0, 9, true) == CONNECT_FAIL_DNS_ALLDOWN);
    /* DNS fine, sends went out, zero responses -> UDP filtered. */
    EXPECT_TRUE("7b-stun-alldown",
                ConnectFail_ClassifyStunDiscover(4, 0, 12, false) == CONNECT_FAIL_STUN_ALLDOWN);
    /* Sends all failed at the socket layer -> no-network bucket. */
    EXPECT_TRUE("7b-sends-failed",
                ConnectFail_ClassifyStunDiscover(4, 0, 0, false) == CONNECT_FAIL_DNS_ALLDOWN);
    /* Discovery succeeded -> not a discovery failure. */
    EXPECT_TRUE("7b-ok",
                ConnectFail_ClassifyStunDiscover(4, 2, 8, false) == CONNECT_FAIL_NONE);

    /* --- 7c: joiner fallback classification (causes 3-7). */
    ConnectJoinEvidence ev;
    memset(&ev, 0, sizeof(ev));
    /* Zero DELIVERs of any kind -> server down (it answers EVERY
     * REGISTER with a DELIVER, so silence is the server, not the host). */
    EXPECT_TRUE("7c-rendezvous-down",
                ConnectFail_ClassifyJoin(&ev) == CONNECT_FAIL_RENDEZVOUS_DOWN);
    /* Only zero-sentinel DELIVERs -> host offline / code stale. */
    ev.deliver_any = true;
    EXPECT_TRUE("7c-host-offline",
                ConnectFail_ClassifyJoin(&ev) == CONNECT_FAIL_HOST_OFFLINE);
    /* Real endpoint arrived, punch failed, cone-family NAT -> blocked. */
    ev.deliver_real = true;
    EXPECT_TRUE("7c-nat-blocked",
                ConnectFail_ClassifyJoin(&ev) == CONNECT_FAIL_NAT_BLOCKED);
    /* Same + our S2 symmetric signal -> symmetric-both class. */
    ev.port_disagreement = true;
    EXPECT_TRUE("7c-symmetric-both",
                ConnectFail_ClassifyJoin(&ev) == CONNECT_FAIL_SYMMETRIC_BOTH);
    /* S4a: bad-token evidence outranks the NAT diagnoses — the peer's
     * datagrams arrived, they just failed auth. */
    ev.punch_bad_token = true;
    EXPECT_TRUE("7c-punch-auth",
                ConnectFail_ClassifyJoin(&ev) == CONNECT_FAIL_PUNCH_AUTH);
    ev.punch_bad_token = false;
    /* Hairpin outranks everything. */
    ev.hairpin = true;
    EXPECT_TRUE("7c-hairpin",
                ConnectFail_ClassifyJoin(&ev) == CONNECT_FAIL_HAIRPIN);
    /* Punch succeeded -> no failure (a stray bad-token sighting must
     * not fail a join that actually connected). */
    memset(&ev, 0, sizeof(ev));
    ev.deliver_any = ev.deliver_real = ev.bilateral_punched = true;
    ev.punch_bad_token = true;
    EXPECT_TRUE("7c-ok", ConnectFail_ClassifyJoin(&ev) == CONNECT_FAIL_NONE);

    /* --- 7c2 (S4c): CHALLENGE evidence splits "server down" from
     * "our cookie echo never bound". Pre-S4c both looked identical
     * (zero DELIVERs) and every auth/version problem was misreported as
     * RENDEZVOUS_DOWN, sending users to check their internet. */
    memset(&ev, 0, sizeof(ev));
    EXPECT_TRUE("7c2-silence-is-down",
                ConnectFail_ClassifyJoin(&ev) == CONNECT_FAIL_RENDEZVOUS_DOWN);
    /* Task #105. ONE challenge is the healthy opening of every v2 session
     * (the server challenges until a cookie verifies, and the joiner then
     * echoes it on every later REGISTER). So a lone challenge must NOT be
     * read as an auth failure — that is what told users to "update the
     * game" when a DELIVER had merely been lost or the server was still
     * holding the room's joiner slot. */
    ev.challenge_any = true;
    EXPECT_TRUE("7c2-challenged-once-is-not-auth-failure",
                ConnectFail_ClassifyJoin(&ev) == CONNECT_FAIL_RENDEZVOUS_NOPAIR);
    /* Being challenged AGAIN after echoing a cookie is the real refusal,
     * and only that still classifies as COOKIE_REJECTED. */
    ev.cookie_rechallenged = true;
    EXPECT_TRUE("7c2-rechallenged-is-auth-failure",
                ConnectFail_ClassifyJoin(&ev) == CONNECT_FAIL_COOKIE_REJECTED);
    /* The two verdicts must not collapse into one another. */
    EXPECT_TRUE("7c2-nopair-distinct-from-cookie",
                strcmp(ConnectFail_Code(CONNECT_FAIL_RENDEZVOUS_NOPAIR),
                       ConnectFail_Code(CONNECT_FAIL_COOKIE_REJECTED)) != 0);
    /* #105: the NOPAIR text must not send the user to update the game —
     * that was the wrong diagnosis this split exists to remove. */
    EXPECT_TRUE("7c2-nopair-text-not-update",
                strstr(ConnectFail_UserText(CONNECT_FAIL_RENDEZVOUS_NOPAIR),
                       "Update") == NULL);
    /* A CHALLENGE plus at least one DELIVER means the cookie DID bind —
     * the failure is downstream, so cookie blame must not stick. */
    ev.cookie_rechallenged = false;
    ev.deliver_any = true;
    EXPECT_TRUE("7c2-deliver-outranks-challenge",
                ConnectFail_ClassifyJoin(&ev) == CONNECT_FAIL_HOST_OFFLINE);
    /* Hairpin still outranks the cookie diagnosis. */
    memset(&ev, 0, sizeof(ev));
    ev.challenge_any = true;
    ev.hairpin = true;
    EXPECT_TRUE("7c2-hairpin-outranks",
                ConnectFail_ClassifyJoin(&ev) == CONNECT_FAIL_HAIRPIN);
    /* COOKIE_REJECTED is its own cause, not an alias of an older one. */
    EXPECT_TRUE("7c2-distinct-code",
                strcmp(ConnectFail_Code(CONNECT_FAIL_COOKIE_REJECTED),
                       ConnectFail_Code(CONNECT_FAIL_RENDEZVOUS_DOWN)) != 0);
    EXPECT_TRUE("7c2-code-string",
                strcmp(ConnectFail_Code(CONNECT_FAIL_COOKIE_REJECTED),
                       "P2P_FAIL_COOKIE_REJECTED") == 0);

    /* --- 7d: host-waiting advisory (cause 8). */
    EXPECT_TRUE("7d-too-early",
                ConnectFail_ClassifyHostWaiting(false, false, false, CONNECT_HOST_ADVISORY_MS - 1) ==
                    CONNECT_FAIL_NONE);
    EXPECT_TRUE("7d-unmappable",
                ConnectFail_ClassifyHostWaiting(false, false, false, CONNECT_HOST_ADVISORY_MS) ==
                    CONNECT_FAIL_HOST_UNMAPPABLE);
    EXPECT_TRUE("7d-rendezvous-down-with-upnp",
                ConnectFail_ClassifyHostWaiting(true, false, false, CONNECT_HOST_ADVISORY_MS) ==
                    CONNECT_FAIL_RENDEZVOUS_DOWN);
    EXPECT_TRUE("7d-deliver-seen",
                ConnectFail_ClassifyHostWaiting(false, true, false, CONNECT_HOST_ADVISORY_MS * 2) ==
                    CONNECT_FAIL_NONE);

    /* --- 7d2 (S4c): same CHALLENGE split on the HOST advisory path.
     * A host that is being challenged but never DELIVERed must be told
     * "auth/version trouble", not "no UPnP mapping" or "server down". */
    EXPECT_TRUE("7d2-challenged-no-upnp",
                ConnectFail_ClassifyHostWaiting(false, false, true, CONNECT_HOST_ADVISORY_MS) ==
                    CONNECT_FAIL_COOKIE_REJECTED);
    EXPECT_TRUE("7d2-challenged-with-upnp",
                ConnectFail_ClassifyHostWaiting(true, false, true, CONNECT_HOST_ADVISORY_MS) ==
                    CONNECT_FAIL_COOKIE_REJECTED);
    /* A DELIVER means the cookie bound — no advisory at all, challenges
     * notwithstanding (every v2 session starts with one). */
    EXPECT_TRUE("7d2-deliver-outranks-challenge",
                ConnectFail_ClassifyHostWaiting(false, true, true, CONNECT_HOST_ADVISORY_MS * 2) ==
                    CONNECT_FAIL_NONE);
    /* Still inside the advisory window: say nothing yet. */
    EXPECT_TRUE("7d2-too-early",
                ConnectFail_ClassifyHostWaiting(false, false, true, CONNECT_HOST_ADVISORY_MS - 1) ==
                    CONNECT_FAIL_NONE);

    /* --- 7e: deadline + abort-hold policy helpers (Part A). */
    EXPECT_FALSE("7e-unarmed", ConnectFail_DeadlineExpired(123456, 0, 15000));
    EXPECT_FALSE("7e-young", ConnectFail_DeadlineExpired(10000, 1, 15000));
    EXPECT_TRUE("7e-expired", ConnectFail_DeadlineExpired(15001, 1, 15000));
    /* Wrap-safe: now < since via u64 wraparound still measures elapsed. */
    EXPECT_TRUE("7e-wrap",
                ConnectFail_DeadlineExpired(5000, UINT64_MAX - 20000ULL, 15000));

    int held = 0;
    for (int i = 0; i < CONNECT_ABORT_HOLD_FRAMES - 1; i++) {
        held = ConnectFail_AbortHoldTick(held, true);
    }
    EXPECT_FALSE("7e-hold-not-yet", ConnectFail_AbortHoldFired(held));
    held = ConnectFail_AbortHoldTick(held, true);
    EXPECT_TRUE("7e-hold-fired", ConnectFail_AbortHoldFired(held));
    held = ConnectFail_AbortHoldTick(held, false);
    EXPECT_FALSE("7e-hold-release-resets", ConnectFail_AbortHoldFired(held));
    EXPECT_TRUE("7e-hold-zero", held == 0);

    /* --- 7f: every code has a distinct machine string and a user string. */
    /* S4-review L-4: "older" and "newer" must be DISTINCT machine codes.
     * Collapsed into one, log triage could not tell which side of a
     * failed pairing was stale — the entire question a support thread
     * has to answer. The generic sweep below proves every code has a
     * distinct string; this proves these two specifically exist, differ,
     * and carry user text that points at the right person. */
    EXPECT_TRUE("test7f-l4",
                strcmp(ConnectFail_Code(CONNECT_FAIL_CODE_VERSION_OLDER),
                       ConnectFail_Code(CONNECT_FAIL_CODE_VERSION_NEWER)) != 0);
    EXPECT_TRUE("test7f-l4",
                strcmp(ConnectFail_UserText(CONNECT_FAIL_CODE_VERSION_OLDER),
                       ConnectFail_UserText(CONNECT_FAIL_CODE_VERSION_NEWER)) != 0);
    EXPECT_TRUE("test7f-l4",
                strstr(ConnectFail_UserText(CONNECT_FAIL_CODE_VERSION_OLDER),
                       "older") != NULL);
    EXPECT_TRUE("test7f-l4",
                strstr(ConnectFail_UserText(CONNECT_FAIL_CODE_VERSION_NEWER),
                       "newer") != NULL);

    for (int c = CONNECT_FAIL_NONE; c <= CONNECT_FAIL_LAST_; c++) {
        const char* mc = ConnectFail_Code((ConnectFailCode)c);
        EXPECT_TRUE("7f-code-nonnull", mc != NULL && mc[0] != '\0');
        EXPECT_TRUE("7f-user-nonnull", ConnectFail_UserText((ConnectFailCode)c) != NULL);
        for (int d = CONNECT_FAIL_NONE; d < c; d++) {
            if (strcmp(mc, ConnectFail_Code((ConnectFailCode)d)) == 0) {
                FAIL("7f-code-distinct", "duplicate machine code string");
            }
        }
    }

    if (fail_count == 0) {
        fprintf(stderr, "[test_netplay_units] test 7 OK — DELIVER tri-state + "
                        "taxonomy classifiers + deadline/abort policy\n");
    }
    return fail_count > 0 ? 1 : 0;
}

/* ---------------------------------------------------------------- */
static int unit_host_datagram_gate(void) {
    tests_run++;
    fprintf(stderr, "[test_netplay_units] test 10: S4a host datagram gate\n");
    const int fails_before = fail_count;

    uint8_t token[STUN_PUNCH_TOKEN_LEN] = { 9, 8, 7, 6, 5, 4, 3, 2 };
    uint8_t wrong[STUN_PUNCH_TOKEN_LEN] = { 9, 8, 7, 6, 5, 4, 3, 1 };

    /* (a) Authenticated punch -> PEER_PUNCH. */
    uint8_t punch[STUN_PUNCH_PAYLOAD_LEN];
    Stun_BuildPunchPayload(token, punch);
    EXPECT_TRUE("10-auth-punch",
                DirectP2P_TestHook_ClassifyHostDatagram(punch, sizeof(punch), token, true) ==
                    DP2P_HOST_DGRAM_PEER_PUNCH);

    /* (b) WRONG token -> IGNORE (never the peer). */
    uint8_t bad[STUN_PUNCH_PAYLOAD_LEN];
    Stun_BuildPunchPayload(wrong, bad);
    EXPECT_TRUE("10-wrong-token",
                DirectP2P_TestHook_ClassifyHostDatagram(bad, sizeof(bad), token, true) ==
                    DP2P_HOST_DGRAM_IGNORE);

    /* (c) Legacy pre-S4a 9-byte "3SX_PUNCH" -> IGNORE. */
    EXPECT_TRUE("10-legacy-punch",
                DirectP2P_TestHook_ClassifyHostDatagram((const uint8_t*)"3SX_PUNCH", 9,
                                                        token, true) ==
                    DP2P_HOST_DGRAM_IGNORE);

    /* (d) Arbitrary garbage (the classic slot-consumer: one stray
     * packet from a scanner) -> IGNORE. */
    static const uint8_t garbage[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x41 };
    EXPECT_TRUE("10-garbage",
                DirectP2P_TestHook_ClassifyHostDatagram(garbage, (int)sizeof(garbage),
                                                        token, true) ==
                    DP2P_HOST_DGRAM_IGNORE);

    /* (e) Valid punch but NO valid token on the host (fail closed) ->
     * IGNORE. */
    EXPECT_TRUE("10-no-token-fail-closed",
                DirectP2P_TestHook_ClassifyHostDatagram(punch, sizeof(punch), token, false) ==
                    DP2P_HOST_DGRAM_IGNORE);

    /* (f) Rendezvous frame ('3SXR', 32 bytes) -> RENDEZVOUS. */
    uint8_t key[REND_KEY_LEN];
    memset(key, 0x5A, sizeof(key));
    uint8_t deliver[REND_DELIVER_LEN];
    (void)build_deliver(deliver, key, NULL, 0);
    EXPECT_TRUE("10-rendezvous",
                DirectP2P_TestHook_ClassifyHostDatagram(deliver, sizeof(deliver), token, true) ==
                    DP2P_HOST_DGRAM_RENDEZVOUS);

    /* (g) STUN Binding Response (type 0x0101 + magic cookie) -> STUN. */
    uint8_t stun_resp[20] = { 0 };
    stun_resp[0] = 0x01; stun_resp[1] = 0x01;
    stun_resp[4] = 0x21; stun_resp[5] = 0x12;
    stun_resp[6] = 0xA4; stun_resp[7] = 0x42;
    EXPECT_TRUE("10-stun",
                DirectP2P_TestHook_ClassifyHostDatagram(stun_resp, sizeof(stun_resp),
                                                        token, true) ==
                    DP2P_HOST_DGRAM_STUN);

    /* (h) Empty / NULL -> IGNORE. */
    EXPECT_TRUE("10-empty",
                DirectP2P_TestHook_ClassifyHostDatagram(punch, 0, token, true) ==
                    DP2P_HOST_DGRAM_IGNORE);
    EXPECT_TRUE("10-null",
                DirectP2P_TestHook_ClassifyHostDatagram(NULL, 17, token, true) ==
                    DP2P_HOST_DGRAM_IGNORE);

    if (fail_count == fails_before) {
        fprintf(stderr, "[test_netplay_units] test 10 OK — unauthenticated datagrams "
                        "can no longer consume the host's peer slot\n");
        return 0;
    }
    return 1;
}

/* ---------------------------------------------------------------- */
static int unit_rendezvous_cookie_codec(void) {
    tests_run++;
    fprintf(stderr, "[test_netplay_units] test 11: S4c rendezvous v2 cookie codec\n");
    const int fails_before = fail_count;

    uint8_t key[REND_KEY_LEN];
    uint8_t other_key[REND_KEY_LEN];
    for (int i = 0; i < REND_KEY_LEN; i++) {
        key[i] = (uint8_t)(0xA0 + i);
        other_key[i] = (uint8_t)(0x10 + i);
    }
    uint8_t cookie[REND_COOKIE_LEN] = { 0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03, 0x04 };

    /* (a) Sizes agree with the wire spec. */
    EXPECT_TRUE("11-sizes", REND_REGISTER_PKT_LEN == 36 && REND_COOKIE_LEN == 8);
    EXPECT_TRUE("11-local-len-agrees", REND_REGISTER_LEN == REND_REGISTER_PKT_LEN);

    /* (b) NULL cookie -> zero tail, version byte 2. */
    uint8_t reg_nc[REND_REGISTER_PKT_LEN];
    memset(reg_nc, 0xFF, sizeof(reg_nc)); /* poison: catch a missing memset */
    EXPECT_TRUE("11-build-null", Rendezvous_BuildRegister(4321, key, NULL, reg_nc));
    EXPECT_TRUE("11-version-2", reg_nc[4] == REND_VERSION);
    EXPECT_TRUE("11-type-register", reg_nc[5] == REND_TYPE_REGISTER);
    EXPECT_TRUE("11-key-echoed", memcmp(&reg_nc[8], key, REND_KEY_LEN) == 0);
    EXPECT_TRUE("11-port-be", reg_nc[24] == 0x10 && reg_nc[25] == 0xE1); /* 4321 */
    {
        bool tail_zero = true;
        for (int i = 28; i < REND_REGISTER_PKT_LEN; i++) {
            if (reg_nc[i] != 0) tail_zero = false;
        }
        EXPECT_TRUE("11-null-cookie-tail-zero", tail_zero);
    }

    /* (c) Cookie present -> exact tail, head byte-identical to (b). */
    uint8_t reg_c[REND_REGISTER_PKT_LEN];
    memset(reg_c, 0xFF, sizeof(reg_c));
    EXPECT_TRUE("11-build-cookie", Rendezvous_BuildRegister(4321, key, cookie, reg_c));
    EXPECT_TRUE("11-cookie-tail", memcmp(&reg_c[28], cookie, REND_COOKIE_LEN) == 0);
    EXPECT_TRUE("11-head-unchanged", memcmp(reg_c, reg_nc, 28) == 0);

    /* (d) POLL carries the cookie the same way, with its own type. */
    uint8_t poll_c[REND_REGISTER_PKT_LEN];
    memset(poll_c, 0xFF, sizeof(poll_c));
    EXPECT_TRUE("11-build-poll", Rendezvous_BuildPoll(key, cookie, poll_c));
    EXPECT_TRUE("11-poll-type", poll_c[5] == REND_TYPE_POLL);
    EXPECT_TRUE("11-poll-version", poll_c[4] == REND_VERSION);
    EXPECT_TRUE("11-poll-cookie-tail", memcmp(&poll_c[28], cookie, REND_COOKIE_LEN) == 0);
    uint8_t poll_nc[REND_REGISTER_PKT_LEN];
    memset(poll_nc, 0xFF, sizeof(poll_nc));
    EXPECT_TRUE("11-build-poll-null", Rendezvous_BuildPoll(key, NULL, poll_nc));
    {
        bool tail_zero = true;
        for (int i = 28; i < REND_REGISTER_PKT_LEN; i++) {
            if (poll_nc[i] != 0) tail_zero = false;
        }
        EXPECT_TRUE("11-poll-null-cookie-tail-zero", tail_zero);
    }

    /* (e) Build a well-formed CHALLENGE the way the server does:
     *     magic(4) ver(1) type(1) reserved(2) key(16) cookie(8). */
    uint8_t chal[REND_CHALLENGE_LEN];
    memset(chal, 0, sizeof(chal));
    chal[0] = REND_MAGIC_BYTES_0;
    chal[1] = REND_MAGIC_BYTES_1;
    chal[2] = REND_MAGIC_BYTES_2;
    chal[3] = REND_MAGIC_BYTES_3;
    chal[4] = (uint8_t)REND_VERSION;
    chal[5] = (uint8_t)REND_TYPE_CHALLENGE;
    memcpy(&chal[8], key, REND_KEY_LEN);
    memcpy(&chal[24], cookie, REND_COOKIE_LEN);

    uint8_t out[REND_COOKIE_LEN];
    memset(out, 0x77, sizeof(out));
    EXPECT_TRUE("11-parse-ok",
                Rendezvous_ParseChallenge(chal, sizeof(chal), key, out));
    EXPECT_TRUE("11-parse-cookie", memcmp(out, cookie, REND_COOKIE_LEN) == 0);

    /* (f) Reject table. Every reject must ALSO zero the output so a
     * caller that ignores the return value cannot echo attacker bytes. */
#define EXPECT_CHAL_REJECT(tag, pkt, len, k) do {                            \
        uint8_t _o[REND_COOKIE_LEN];                                         \
        memset(_o, 0x77, sizeof(_o));                                        \
        EXPECT_FALSE(tag, Rendezvous_ParseChallenge((pkt), (len), (k), _o)); \
        bool _z = true;                                                      \
        for (int _i = 0; _i < REND_COOKIE_LEN; _i++) {                       \
            if (_o[_i] != 0) _z = false;                                     \
        }                                                                    \
        EXPECT_TRUE(tag "-zeroed", _z);                                      \
    } while (0)

    {   /* wrong magic */
        uint8_t bad[REND_CHALLENGE_LEN];
        memcpy(bad, chal, sizeof(bad));
        bad[0] ^= 0xFF;
        EXPECT_CHAL_REJECT("11-bad-magic", bad, sizeof(bad), key);
    }
    {   /* v1 version byte — a v1 server's frame must never be consumed */
        uint8_t bad[REND_CHALLENGE_LEN];
        memcpy(bad, chal, sizeof(bad));
        bad[4] = 1;
        EXPECT_CHAL_REJECT("11-v1-version", bad, sizeof(bad), key);
    }
    {   /* future version */
        uint8_t bad[REND_CHALLENGE_LEN];
        memcpy(bad, chal, sizeof(bad));
        bad[4] = 3;
        EXPECT_CHAL_REJECT("11-future-version", bad, sizeof(bad), key);
    }
    {   /* wrong type: a DELIVER must not be mistaken for a CHALLENGE */
        uint8_t bad[REND_CHALLENGE_LEN];
        memcpy(bad, chal, sizeof(bad));
        bad[5] = (uint8_t)REND_TYPE_DELIVER;
        EXPECT_CHAL_REJECT("11-deliver-not-challenge", bad, sizeof(bad), key);
    }
    /* cross-talk / forgery: right shape, someone else's session key */
    EXPECT_CHAL_REJECT("11-wrong-key", chal, sizeof(chal), other_key);
    /* truncated by one byte */
    EXPECT_CHAL_REJECT("11-short", chal, REND_CHALLENGE_LEN - 1, key);
    /* zero length / negative length */
    EXPECT_CHAL_REJECT("11-zero-len", chal, 0, key);
    EXPECT_CHAL_REJECT("11-neg-len", chal, -1, key);
#undef EXPECT_CHAL_REJECT

    /* (g) NULL-argument safety. */
    EXPECT_FALSE("11-null-pkt", Rendezvous_ParseChallenge(NULL, REND_CHALLENGE_LEN, key, out));
    EXPECT_FALSE("11-null-key", Rendezvous_ParseChallenge(chal, REND_CHALLENGE_LEN, NULL, out));
    EXPECT_FALSE("11-null-out", Rendezvous_ParseChallenge(chal, REND_CHALLENGE_LEN, key, NULL));
    EXPECT_FALSE("11-null-buf-register", Rendezvous_BuildRegister(1, key, cookie, NULL));
    EXPECT_FALSE("11-null-key-register", Rendezvous_BuildRegister(1, NULL, cookie, reg_c));

    if (fail_count == fails_before) {
        fprintf(stderr, "[test_netplay_units] test 11 OK — v2 cookie tail + "
                        "CHALLENGE parse gate\n");
        return 0;
    }
    return 1;
}

/* ---------------------------------------------------------------- */
static int unit_rendezvous_frame_router(void) {
    tests_run++;
    fprintf(stderr, "[test_netplay_units] test 32: '3SXR' frame router — "
                    "HasMagic (version-independent) vs FrameType\n");
    const int fails_before = fail_count;

    uint8_t key[REND_KEY_LEN];
    for (int i = 0; i < REND_KEY_LEN; i++) key[i] = (uint8_t)(0x10u + i);

    struct sockaddr_in peer;
    memset(&peer, 0, sizeof(peer));
    peer.sin_family = AF_INET;
    peer.sin_addr.s_addr = htonl(0xC6336407u); /* 198.51.100.7 */

    uint8_t deliver[REND_DELIVER_LEN];
    (void)build_deliver(deliver, key, &peer, 6000);
    uint8_t challenge[REND_CHALLENGE_LEN];
    uint8_t cookie[REND_COOKIE_LEN];
    for (int i = 0; i < REND_COOKIE_LEN; i++) cookie[i] = (uint8_t)(0xC0u + i);
    (void)build_challenge(challenge, key, cookie);

    /* (1) FrameType routes the two shipping server->client types. */
    EXPECT_TRUE("32-ft-deliver",
                Rendezvous_FrameType(deliver, (int)sizeof(deliver)) ==
                    REND_FRAME_DELIVER);
    EXPECT_TRUE("32-ft-challenge",
                Rendezvous_FrameType(challenge, (int)sizeof(challenge)) ==
                    REND_FRAME_CHALLENGE);

    /* (2) FrameType rejects everything that is not a well-formed frame
     *     of THIS version. */
    {
        uint8_t v3[REND_DELIVER_LEN];
        memcpy(v3, deliver, sizeof(v3));
        v3[4] = 3;
        EXPECT_TRUE("32-ft-wrong-version",
                    Rendezvous_FrameType(v3, (int)sizeof(v3)) == 0);
        uint8_t nm[REND_DELIVER_LEN];
        memcpy(nm, deliver, sizeof(nm));
        nm[1] = 0x00;
        EXPECT_TRUE("32-ft-wrong-magic",
                    Rendezvous_FrameType(nm, (int)sizeof(nm)) == 0);
        EXPECT_TRUE("32-ft-short", Rendezvous_FrameType(deliver, 5) == 0);
        EXPECT_TRUE("32-ft-null", Rendezvous_FrameType(NULL, 32) == 0);
    }

    /* (3) HasMagic is the STRAGGLER test and is deliberately WEAKER.
     *     Every case here is one where the two functions must DISAGREE,
     *     which is exactly what a `HasMagic -> FrameType(...) != 0`
     *     substitution destroys. */
    {
        EXPECT_TRUE("32-magic-deliver",
                    Rendezvous_HasMagic(deliver, (int)sizeof(deliver)));
        EXPECT_TRUE("32-magic-challenge",
                    Rendezvous_HasMagic(challenge, (int)sizeof(challenge)));

        /* A v3 '3SXR' straggler. HasMagic must still claim it — this is
         * the packet that otherwise reaches GekkoNet as type 51. */
        uint8_t v3[REND_DELIVER_LEN];
        memcpy(v3, deliver, sizeof(v3));
        v3[4] = 3;
        EXPECT_TRUE("32-magic-v3-still-ours",
                    Rendezvous_HasMagic(v3, (int)sizeof(v3)));
        EXPECT_TRUE("32-magic-v3-invisible-to-frametype",
                    Rendezvous_FrameType(v3, (int)sizeof(v3)) == 0);

        /* An unknown TYPE on the right version is likewise still ours. */
        uint8_t t99[REND_DELIVER_LEN];
        memcpy(t99, deliver, sizeof(t99));
        t99[5] = 99;
        EXPECT_TRUE("32-magic-unknown-type-still-ours",
                    Rendezvous_HasMagic(t99, (int)sizeof(t99)));

        /* A frame too short to carry a type is still ours by magic. */
        EXPECT_TRUE("32-magic-4-bytes", Rendezvous_HasMagic(deliver, 4));
        EXPECT_TRUE("32-magic-4-bytes-invisible-to-frametype",
                    Rendezvous_FrameType(deliver, 4) == 0);

        /* And the negatives: wrong magic, too short for the magic, NULL. */
        uint8_t nm[REND_DELIVER_LEN];
        memcpy(nm, deliver, sizeof(nm));
        nm[1] = 0x00;
        EXPECT_FALSE("32-magic-wrong-magic",
                     Rendezvous_HasMagic(nm, (int)sizeof(nm)));
        EXPECT_FALSE("32-magic-short", Rendezvous_HasMagic(deliver, 3));
        EXPECT_FALSE("32-magic-null", Rendezvous_HasMagic(NULL, 32));
    }

    /* (4) EXACTNESS. No GekkoNet packet may look like a '3SXR' frame to
     *     EITHER function — that is what makes the straggler drop an
     *     exact test rather than a heuristic. PacketType is 1..7 by
     *     construction (GekkoNet net.h:28-36) and can never be 0x33. */
    for (uint8_t t = 1; t <= 7; t++) {
        uint8_t gek[32];
        memset(gek, 0x5A, sizeof(gek));
        gek[0] = t;
        EXPECT_FALSE("32-magic-gekko-never-matches",
                     Rendezvous_HasMagic(gek, (int)sizeof(gek)));
        EXPECT_TRUE("32-ft-gekko-never-matches",
                    Rendezvous_FrameType(gek, (int)sizeof(gek)) == 0);
    }

    if (fail_count == fails_before) {
        fprintf(stderr,
                "[test_netplay_units] test 32 OK — HasMagic claims a '3SXR' frame of "
                "ANY version while FrameType routes only v2 DELIVER/CHALLENGE, and no "
                "GekkoNet packet can be mistaken for either\n");
        return 0;
    }
    return 1;
}

/* ---------------------------------------------------------------- */
static int unit_punch_gate_throttle(void) {
    tests_run++;
    fprintf(stderr, "[test_netplay_units] test 12: S4-review HIGH-1b punch-gate "
                    "throttle (v4/#155: mute is the sole layer — see below)\n");
    const int fails_before = fail_count;

    int src_max_bad = 0, src_table = 0;
    uint32_t mute_ms = 0;
    DirectP2P_TestHook_PunchGateLimits(&src_max_bad, &mute_ms, &src_table);

    /* Sanity on the shipped numbers themselves: a joiner's Stun_HolePunch
     * emits ~10 datagrams in its first 500 ms then 5/s, so the per-source
     * budget must clear that opening burst or a peer that merely raced a
     * drift re-encode would be muted mid-handshake. */
    if (src_max_bad <= 10) {
        FAIL("test12", "HOST_PUNCH_SRC_MAX_BAD <= the joiner's 10-datagram "
                       "opening punch burst — a legitimate peer could be muted");
    }
    if (mute_ms == 0) {
        FAIL("test12", "mute lifetime is 0 — mutes would never take effect");
    }

    /* --- 12a: one source is muted at exactly the threshold, not before. */
    DirectP2P_TestHook_PunchGateReset();
    const uint32_t t0 = 1000u;
    for (int i = 1; i < src_max_bad; i++) {
        DirectP2P_TestHook_PunchGateNoteBad("198.51.100.9", t0);
        if (DirectP2P_TestHook_PunchGateIsMuted("198.51.100.9", t0)) {
            fprintf(stderr, "[test_netplay_units] FAIL: test12a: muted early "
                            "after %d bad punches (threshold %d)\n", i, src_max_bad);
            fail_count++;
            break;
        }
    }
    DirectP2P_TestHook_PunchGateNoteBad("198.51.100.9", t0);
    EXPECT_TRUE("test12a", DirectP2P_TestHook_PunchGateIsMuted("198.51.100.9", t0));

    /* --- 12b: muting is PER SOURCE — an unrelated address is unaffected.
     * This is the assertion that stops the throttle from becoming a
     * global denial of hosting. */
    EXPECT_FALSE("test12b", DirectP2P_TestHook_PunchGateIsMuted("203.0.113.5", t0));

    /* --- 12c: the mute EXPIRES. A permanent mute would turn this
     * defence into a self-inflicted lockout: friend typos the code,
     * burns the budget, then types it correctly and is dropped forever
     * with no diagnosis. v4 (#155): this is now the ONLY recovery path
     * — the session-total "re-roll" escalation that used to sit
     * alongside it is gone, because it drew a fresh nonce and v4 has no
     * nonce left to draw (direct_p2p.c's punch-gate throttle comment). */
    EXPECT_TRUE("test12c", DirectP2P_TestHook_PunchGateIsMuted("198.51.100.9",
                                                               t0 + mute_ms - 1u));
    EXPECT_FALSE("test12c", DirectP2P_TestHook_PunchGateIsMuted("198.51.100.9",
                                                                t0 + mute_ms));

    /* --- 12d: the session-total bad-punch counter still accrues (it is
     * still logged and still surfaced via the counters hook), even
     * though nothing acts on it crossing a threshold any more. */
    DirectP2P_TestHook_PunchGateReset();
    const int charge_count = src_max_bad * 3;
    for (int i = 0; i < charge_count; i++) {
        char ip[64];
        snprintf(ip, sizeof(ip), "192.0.2.%d", (i % 200) + 1);
        DirectP2P_TestHook_PunchGateNoteBad(ip, t0);
    }
    {
        int bad_total = 0;
        DirectP2P_TestHook_PunchGateCounters(&bad_total);
        if (bad_total != charge_count) {
            fprintf(stderr, "[test_netplay_units] FAIL: test12d: session total %d "
                            "!= %d\n", bad_total, charge_count);
            fail_count++;
        }
    }

    /* --- 12f: a muted source cannot evict a quieter one from the table
     * by rotating addresses. Fill the table with muted sources, then push
     * (table + 4) fresh addresses through: the original mutes must all
     * survive, otherwise an attacker clears its own mute for free. */
    DirectP2P_TestHook_PunchGateReset();
    for (int slot = 0; slot < src_table; slot++) {
        char ip[64];
        snprintf(ip, sizeof(ip), "198.51.100.%d", slot + 1);
        for (int i = 0; i < src_max_bad; i++) {
            DirectP2P_TestHook_PunchGateNoteBad(ip, t0);
        }
    }
    for (int i = 0; i < src_table + 4; i++) {
        char ip[64];
        snprintf(ip, sizeof(ip), "203.0.113.%d", i + 1);
        DirectP2P_TestHook_PunchGateNoteBad(ip, t0);
    }
    for (int slot = 0; slot < src_table; slot++) {
        char ip[64];
        snprintf(ip, sizeof(ip), "198.51.100.%d", slot + 1);
        if (!DirectP2P_TestHook_PunchGateIsMuted(ip, t0)) {
            fprintf(stderr, "[test_netplay_units] FAIL: test12f: muted source %s "
                            "was evicted by address rotation — an attacker can "
                            "clear its own mute for free\n", ip);
            fail_count++;
            break;
        }
    }

    DirectP2P_TestHook_PunchGateReset();

    if (fail_count == fails_before) {
        fprintf(stderr,
                "[test_netplay_units] test 12 OK — punch gate is capped "
                "(mute at %d/source, %u ms) and cannot lock out a legitimate "
                "peer; no re-roll escalation as of v4 (#155)\n",
                src_max_bad, (unsigned)mute_ms);
        return 0;
    }
    return 1;
}

/* ---------------------------------------------------------------- */
static int unit_race_budget_wrap_safety(void) {
    tests_run++;
    fprintf(stderr,
            "[test_netplay_units] test 28: the race deadline survives the "
            "SDL_GetTicks 32-bit wrap (M-1), and the confirmation tail is pinned "
            "(M-3)\n");
    const int fails_before = fail_count;

    /* M-3: pinned by LITERAL. H-1's hard cap is budget + this value, and
     * the punch's own confirmation burst is sized by it. */
    if (STUN_PUNCH_CONFIRM_MS != 600) {
        fprintf(stderr,
                "[test_netplay_units] FAIL: test28: STUN_PUNCH_CONFIRM_MS is %d, "
                "expected the shipped literal 600 — the post-confirmation burst is what "
                "gets the peer its last datagram, and H-1's budget exemption is sized by "
                "it. Changing it is a deliberate act, not a refactor\n",
                (int)STUN_PUNCH_CONFIRM_MS);
        fail_count++;
    }

    {
        const int budget = 8000;
        /* t0 sits 256 ms before the uint32 wrap, so t0 + budget overflows
         * to 7744 — a number `now` is already far past. */
        const uint32_t t0 = 0xFFFFFF00u;

        EXPECT_FALSE("28-wrap-first-iteration",
                     DirectP2P_TestHook_RaceBudgetExpired(t0, t0, budget, false));
        EXPECT_FALSE("28-wrap-1ms-in",
                     DirectP2P_TestHook_RaceBudgetExpired(t0 + 1u, t0, budget, false));
        EXPECT_FALSE("28-wrap-just-inside",
                     DirectP2P_TestHook_RaceBudgetExpired(t0 + 7999u, t0, budget, false));
        EXPECT_TRUE("28-wrap-at-budget",
                    DirectP2P_TestHook_RaceBudgetExpired(t0 + 8000u, t0, budget, false));

        /* And the same at a t0 nowhere near the wrap, so the fix is not
         * "always false". */
        const uint32_t mid = 1000000u;
        EXPECT_FALSE("28-mid-just-inside",
                     DirectP2P_TestHook_RaceBudgetExpired(mid + 7999u, mid, budget, false));
        EXPECT_TRUE("28-mid-at-budget",
                    DirectP2P_TestHook_RaceBudgetExpired(mid + 8000u, mid, budget, false));

        /* H-1's exemption: hard-bounded at budget + one tail, across the
         * wrap as well. */
        EXPECT_FALSE("28-tail-holds-at-budget",
                     DirectP2P_TestHook_RaceBudgetExpired(t0 + 8000u, t0, budget, true));
        EXPECT_FALSE("28-tail-holds-just-inside",
                     DirectP2P_TestHook_RaceBudgetExpired(
                         t0 + (uint32_t)(8000 + STUN_PUNCH_CONFIRM_MS - 1), t0, budget, true));
        EXPECT_TRUE("28-tail-is-hard-capped",
                    DirectP2P_TestHook_RaceBudgetExpired(
                        t0 + (uint32_t)(8000 + STUN_PUNCH_CONFIRM_MS), t0, budget, true));
    }

    if (fail_count == fails_before) {
        fprintf(stderr,
                "[test_netplay_units] test 28 OK — the deadline is wrap-safe at "
                "t0=0xFFFFFF00, the tail exemption is hard-capped at budget+%d ms, and "
                "STUN_PUNCH_CONFIRM_MS is pinned to 600\n",
                STUN_PUNCH_CONFIRM_MS);
        return 0;
    }
    return 1;
}

/* ---------------------------------------------------------------- */
static int unit_natpmp_codec(void) {
    tests_run++;
    fprintf(stderr, "[test_netplay_units] natpmp_codec: PCP + NAT-PMP wire codec vs literal RFC 6886/6887 bytes\n");
    const int fails_before = fail_count;
    /* ================= (A) codec vs literal RFC bytes ================= */

    uint8_t nonce[NATPMP_PCP_NONCE_LEN];
    for (int i = 0; i < NATPMP_PCP_NONCE_LEN; i++) nonce[i] = (uint8_t)(0xA0u + i);

    {
        /* PCP MAP request, RFC 6887 §7.1 (24-byte common header) +
         * §11.1 (36-byte MAP block) = 60 bytes. Expectation hand-built
         * from the figures, byte by byte. */
        uint8_t want[NATPMP_PCP_MAP_LEN];
        memset(want, 0, sizeof(want));
        want[0] = 2;    /* Version = 2                                  */
        want[1] = 1;    /* R = 0 (request) | Opcode = MAP (1)           */
        /* want[2..4) Reserved (16 bits) = 0                            */
        want[4] = 0x00; want[5] = 0x00; want[6] = 0x0E; want[7] = 0x10; /* 3600 */
        /* PCP Client's IP Address, 128 bits, IPv4-mapped (§5):
         * ::ffff:192.168.1.77 */
        want[18] = 0xFF; want[19] = 0xFF;
        want[20] = 192; want[21] = 168; want[22] = 1; want[23] = 77;
        memcpy(&want[24], nonce, NATPMP_PCP_NONCE_LEN); /* Mapping Nonce */
        want[36] = 17;  /* Protocol = UDP                               */
        /* want[37..40) Reserved (24 bits) = 0                          */
        /* S7 review H-7.1: these two ports MUST DIFFER.
         *
         * This block used to ask for internal 54321 and suggested
         * external 54321, so the two adjacent 16-bit fields held
         * identical bytes and SWAPPING them in Natpmp_BuildPcpMapRequest
         * produced a byte-identical frame — the neutralisation the
         * reviewer applied (swap the Internal-Port and
         * Suggested-External-Port writes) left the whole suite green.
         * With 54321 / 40001 the swap moves bytes and is caught, both by
         * the whole-frame memcmp below and by the explicit
         * offset assertions after it. */
        want[40] = 0xD4; want[41] = 0x31; /* Internal Port 54321        */
        want[42] = 0x9C; want[43] = 0x41; /* Suggested External 40001   */
        /* Suggested External IP = the all-zeros address for "no
         * preference" (§11.1: "it MUST use the address-family-specific
         * all-zeros address (see Section 5)"). For IPv4 that is NOT 16
         * zero bytes — §5 is explicit: "The all-zeros IPv4 address MUST
         * be expressed by 80 bits of zeros, 16 bits of ones, and 32 bits
         * of zeros (::ffff:0:0)." Sending bare :: would be the IPv6
         * unspecified address in an otherwise IPv4 request. */
        want[54] = 0xFF; want[55] = 0xFF;

        struct in_addr client;
        memset(&client, 0, sizeof(client));
        EXPECT_TRUE("22-pton", inet_pton(AF_INET, "192.168.1.77", &client) == 1);

        uint8_t got[NATPMP_PCP_MAP_LEN];
        EXPECT_TRUE("22-pcp-build",
                    Natpmp_BuildPcpMapRequest(got, nonce, NATPMP_PROTO_UDP, 54321,
                                              40001, 0, (uint32_t)client.s_addr, 3600));
        /* Named, so a swap reports WHICH field moved rather than only an
         * offset. RFC 6887 §11.1: Internal Port at octets 40-41,
         * Suggested External Port at 42-43. */
        if (((got[40] << 8) | got[41]) != 54321) {
            fprintf(stderr,
                    "[test_netplay_units] FAIL: 22-pcp-req-internal-port: octets 40-41 "
                    "carry %u, expected the Internal Port 54321 (RFC 6887 §11.1). A "
                    "gateway reading a swapped frame would map the wrong port.\n",
                    (unsigned)((got[40] << 8) | got[41]));
            fail_count++;
        }
        if (((got[42] << 8) | got[43]) != 40001) {
            fprintf(stderr,
                    "[test_netplay_units] FAIL: 22-pcp-req-suggested-ext-port: octets "
                    "42-43 carry %u, expected the Suggested External Port 40001 (RFC "
                    "6887 §11.1)\n",
                    (unsigned)((got[42] << 8) | got[43]));
            fail_count++;
        }
        if (memcmp(got, want, sizeof(want)) != 0) {
            int off = -1;
            for (int i = 0; i < (int)sizeof(want); i++) {
                if (got[i] != want[i]) { off = i; break; }
            }
            fprintf(stderr,
                    "[test_netplay_units] FAIL: 18-pcp-req-bytes: first difference at "
                    "offset %d (got 0x%02X, RFC 6887 §7.1/§11.1 layout wants 0x%02X)\n",
                    off, off >= 0 ? got[off] : 0, off >= 0 ? want[off] : 0);
            fail_count++;
        }
        /* And a delete is the same frame with Requested Lifetime 0
         * (§11.1: "The value 0 indicates 'delete'."). */
        uint8_t del[NATPMP_PCP_MAP_LEN];
        EXPECT_TRUE("22-pcp-del-build",
                    Natpmp_BuildPcpMapRequest(del, nonce, NATPMP_PROTO_UDP, 54321, 0, 0,
                                              (uint32_t)client.s_addr, 0));
        EXPECT_TRUE("22-pcp-del-lifetime0",
                    del[4] == 0 && del[5] == 0 && del[6] == 0 && del[7] == 0);
        EXPECT_TRUE("22-pcp-del-still-map", del[1] == 1);
    }

    {
        /* NAT-PMP public-address request, RFC 6886 §3.2: two bytes,
         * "Vers = 0 | OP = 0". */
        uint8_t a[NATPMP_PMP_ADDR_REQ_LEN];
        EXPECT_TRUE("22-pmp-addr-build", Natpmp_BuildPmpAddrRequest(a));
        EXPECT_TRUE("22-pmp-addr-bytes", a[0] == 0 && a[1] == 0);
        EXPECT_TRUE("22-pmp-addr-len", (int)sizeof(a) == 2);
    }

    {
        /* NAT-PMP mapping request, RFC 6886 §3.3, 12 bytes:
         * Vers=0 | OP=x | Reserved(2) | Internal Port(2) |
         * Suggested External Port(2) | Lifetime(4), network byte order. */
        uint8_t want[NATPMP_PMP_MAP_REQ_LEN];
        memset(want, 0, sizeof(want));
        want[0] = 0;    /* Vers = 0                       */
        want[1] = 1;    /* OP = 1 (Map UDP)               */
        want[4] = 0xD4; want[5] = 0x31; /* Internal 54321 */
        want[6] = 0x9C; want[7] = 0x41; /* Suggested ext 40001 */
        want[8] = 0x00; want[9] = 0x00; want[10] = 0x0E; want[11] = 0x10; /* 3600 */

        uint8_t got[NATPMP_PMP_MAP_REQ_LEN];
        EXPECT_TRUE("22-pmp-map-build",
                    Natpmp_BuildPmpMapRequest(got, NATPMP_PMP_OP_MAP_UDP, 54321, 40001,
                                              3600));
        if (memcmp(got, want, sizeof(want)) != 0) {
            int off = -1;
            for (int i = 0; i < (int)sizeof(want); i++) {
                if (got[i] != want[i]) { off = i; break; }
            }
            fprintf(stderr,
                    "[test_netplay_units] FAIL: 18-pmp-req-bytes: first difference at "
                    "offset %d (got 0x%02X, RFC 6886 §3.3 layout wants 0x%02X)\n",
                    off, off >= 0 ? got[off] : 0, off >= 0 ? want[off] : 0);
            fail_count++;
        }

        /* RFC 6886 §3.4 deletion: lifetime 0 AND "The Suggested External
         * Port MUST be set to zero by the client on sending". The builder
         * must force it even when the caller passes one. */
        uint8_t del[NATPMP_PMP_MAP_REQ_LEN];
        EXPECT_TRUE("22-pmp-del-build",
                    Natpmp_BuildPmpMapRequest(del, NATPMP_PMP_OP_MAP_UDP, 54321, 40001, 0));
        if (del[6] != 0 || del[7] != 0) {
            fprintf(stderr,
                    "[test_netplay_units] FAIL: 18-pmp-del-extport0: deletion request "
                    "carries Suggested External Port %u; RFC 6886 §3.4 says it MUST be "
                    "zero on sending\n",
                    (unsigned)((del[6] << 8) | del[7]));
            fail_count++;
        }
        EXPECT_TRUE("22-pmp-del-lifetime0",
                    del[8] == 0 && del[9] == 0 && del[10] == 0 && del[11] == 0);
        EXPECT_TRUE("22-pmp-del-internal-kept", del[4] == 0xD4 && del[5] == 0x31);
        /* Opcodes other than 1/2 do not exist on this wire (§3.3). */
        uint8_t junk[NATPMP_PMP_MAP_REQ_LEN];
        EXPECT_FALSE("22-pmp-op7", Natpmp_BuildPmpMapRequest(junk, 7, 1, 1, 60));
    }

    /* ---- PCP response parsing: success, then the reject table ---- */

    uint8_t resp[NATPMP_PCP_MAP_LEN];
    memset(resp, 0, sizeof(resp));
    resp[0] = 2;
    resp[1] = 0x80u | 1u;          /* R = 1, Opcode = MAP        */
    resp[3] = 0;                   /* SUCCESS                    */
    resp[4] = 0; resp[5] = 0; resp[6] = 0x02; resp[7] = 0x58; /* lifetime 600 */
    resp[8] = 0; resp[9] = 0; resp[10] = 0; resp[11] = 0x2A;  /* epoch 42     */
    memcpy(&resp[24], nonce, NATPMP_PCP_NONCE_LEN);
    resp[36] = 17;
    resp[40] = 0xD4; resp[41] = 0x31;  /* internal 54321          */
    resp[42] = 0x9C; resp[43] = 0x41;  /* assigned external 40001 */
    resp[54] = 0xFF; resp[55] = 0xFF;  /* ::ffff:198.51.100.7     */
    resp[56] = 198; resp[57] = 51; resp[58] = 100; resp[59] = 7;

    {
        NatpmpPcpMap m;
        memset(&m, 0xAA, sizeof(m));
        EXPECT_TRUE("22-pcp-parse-ok",
                    Natpmp_ParsePcpMapResponse(resp, (int)sizeof(resp), nonce,
                                               NATPMP_PROTO_UDP, 54321, &m) ==
                        NATPMP_PARSE_OK);
        EXPECT_TRUE("22-pcp-parse-lifetime", m.lifetime_s == 600u);
        EXPECT_TRUE("22-pcp-parse-epoch", m.epoch_s == 42u);
        EXPECT_TRUE("22-pcp-parse-internal", m.internal_port == 54321);
        if (m.external_port != 40001) {
            fprintf(stderr,
                    "[test_netplay_units] FAIL: 18-pcp-parse-extport: got %u, expected "
                    "40001 (Assigned External Port is big-endian at [42..44), RFC 6887 "
                    "§11.2)\n", (unsigned)m.external_port);
            fail_count++;
        }
        struct in_addr got_ip;
        memcpy(&got_ip.s_addr, &m.external_ip_be, 4);
        char ipbuf[64] = { 0 };
        inet_ntop(AF_INET, &got_ip, ipbuf, sizeof(ipbuf));
        if (strcmp(ipbuf, "198.51.100.7") != 0) {
            fprintf(stderr,
                    "[test_netplay_units] FAIL: 18-pcp-parse-extip: got %s, expected "
                    "198.51.100.7 (IPv4-mapped IPv6 at [44..60), RFC 6887 §5)\n", ipbuf);
            fail_count++;
        }
    }

    {
        /* A forged or misdirected frame must never become a mapping.
         * Every row below is NOT_OURS — "keep listening", not "refused"
         * — and must leave the output zeroed. */
        struct { const char* tag; int off; uint8_t val; } bad[] = {
            { "22-pcp-rej-version",  0,  3    },  /* not version 2            */
            { "22-pcp-rej-rbit",     1,  0x01 },  /* R clear: it is a request */
            { "22-pcp-rej-opcode",   1,  0x82 },  /* R set, but PEER not MAP  */
            { "22-pcp-rej-nonce",    24, 0x00 },  /* someone else's mapping   */
            { "22-pcp-rej-nonce2",   35, 0x00 },  /* last nonce byte counts   */
            { "22-pcp-rej-protocol", 36, 6    },  /* TCP answer to a UDP ask  */
            { "22-pcp-rej-intport",  40, 0x00 },  /* not the port we asked    */
            { "22-pcp-rej-v4mapped", 50, 0x01 },  /* §5: all 96 bits checked  */
            { "22-pcp-rej-v4mapped2",54, 0xFE },  /* 0xFFFF marker corrupted  */
        };
        for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
            uint8_t b[NATPMP_PCP_MAP_LEN];
            memcpy(b, resp, sizeof(b));
            b[bad[i].off] = bad[i].val;
            NatpmpPcpMap m;
            memset(&m, 0xAA, sizeof(m));
            const NatpmpParse v = Natpmp_ParsePcpMapResponse(b, (int)sizeof(b), nonce,
                                                             NATPMP_PROTO_UDP, 54321, &m);
            if (v != NATPMP_PARSE_NOT_OURS) {
                fprintf(stderr,
                        "[test_netplay_units] FAIL: %s: verdict %d, expected "
                        "NATPMP_PARSE_NOT_OURS (%d) — a frame that is not provably an "
                        "answer to our request must never be accepted\n",
                        bad[i].tag, (int)v, (int)NATPMP_PARSE_NOT_OURS);
                fail_count++;
            }
            EXPECT_TRUE("22-pcp-rej-zeroes",
                        m.external_port == 0 && m.external_ip_be == 0 &&
                            m.lifetime_s == 0);
        }
        /* Short frames, including one truncated one byte below the
         * documented 60. */
        NatpmpPcpMap m;
        for (int len = 0; len < NATPMP_PCP_MAP_LEN; len++) {
            uint8_t b[NATPMP_PCP_MAP_LEN];
            memcpy(b, resp, sizeof(b));
            memset(&m, 0xAA, sizeof(m));
            if (Natpmp_ParsePcpMapResponse(b, len, nonce, NATPMP_PROTO_UDP, 54321, &m) ==
                NATPMP_PARSE_OK) {
                fprintf(stderr,
                        "[test_netplay_units] FAIL: 18-pcp-short: a %d-byte frame parsed "
                        "OK; the PCP MAP response is 60 bytes (RFC 6887 §7.2 + §11.2)\n",
                        len);
                fail_count++;
                break;
            }
        }

        /* An error result for OUR request is REFUSED, not NOT_OURS, and
         * must not carry a port out. */
        uint8_t e[NATPMP_PCP_MAP_LEN];
        memcpy(e, resp, sizeof(e));
        e[3] = (uint8_t)NATPMP_PCP_NO_RESOURCES; /* 8, RFC 6887 §7.4 */
        memset(&m, 0xAA, sizeof(m));
        EXPECT_TRUE("22-pcp-refused",
                    Natpmp_ParsePcpMapResponse(e, (int)sizeof(e), nonce, NATPMP_PROTO_UDP,
                                               54321, &m) == NATPMP_PARSE_REFUSED);
        EXPECT_TRUE("22-pcp-refused-code", m.result_code == 8);
        EXPECT_TRUE("22-pcp-refused-noport", m.external_port == 0);

        /* THE DOWNGRADE SIGNAL. RFC 6886 §3.5's Unsupported Version frame
         * is Vers=0 | OP=0 | Result Code=1 | Epoch — eight bytes, and its
         * OP byte has the PCP R bit CLEAR. RFC 6887 §9 step 4 says a
         * version-zero UNSUPP_VERSION means "this is a NAT-PMP server". */
        uint8_t uv[8];
        memset(uv, 0, sizeof(uv));
        uv[0] = 0; uv[1] = 0; uv[2] = 0; uv[3] = 1; uv[7] = 0x2A;
        memset(&m, 0xAA, sizeof(m));
        const NatpmpParse dv = Natpmp_ParsePcpMapResponse(uv, (int)sizeof(uv), nonce,
                                                          NATPMP_PROTO_UDP, 54321, &m);
        if (dv != NATPMP_PARSE_PCP_IS_NATPMP) {
            fprintf(stderr,
                    "[test_netplay_units] FAIL: 18-pcp-downgrade: verdict %d, expected "
                    "NATPMP_PARSE_PCP_IS_NATPMP (%d). RFC 6887 §9 step 4: a version-zero "
                    "UNSUPP_VERSION response IS the NAT-PMP server signal; discarding it "
                    "(e.g. by testing the R bit first) makes the gateway look silent.\n",
                    (int)dv, (int)NATPMP_PARSE_PCP_IS_NATPMP);
            fail_count++;
        }
    }

    /* ---- NAT-PMP response parsing ---- */
    {
        uint8_t mr[NATPMP_PMP_MAP_RESP_LEN];
        memset(mr, 0, sizeof(mr));
        mr[0] = 0;
        mr[1] = 128 + 1;
        mr[7] = 0x2A;                      /* epoch 42                */
        mr[8] = 0xD4; mr[9] = 0x31;        /* internal 54321          */
        mr[10] = 0x9C; mr[11] = 0x41;      /* mapped external 40001   */
        mr[14] = 0x0E; mr[15] = 0x10;      /* lifetime 3600           */

        NatpmpPmpMap m;
        memset(&m, 0xAA, sizeof(m));
        EXPECT_TRUE("22-pmp-parse-ok",
                    Natpmp_ParsePmpMapResponse(mr, (int)sizeof(mr), NATPMP_PMP_OP_MAP_UDP,
                                               54321, &m) == NATPMP_PARSE_OK);
        EXPECT_TRUE("22-pmp-parse-extport", m.external_port == 40001);
        EXPECT_TRUE("22-pmp-parse-lifetime", m.lifetime_s == 3600u);
        EXPECT_TRUE("22-pmp-parse-epoch", m.epoch_s == 42u);

        /* Reject table. */
        struct { const char* tag; int off; uint8_t val; } bad[] = {
            { "22-pmp-rej-version", 0, 1        }, /* Vers must be 0            */
            { "22-pmp-rej-op-req",  1, 1        }, /* a request, not a response */
            { "22-pmp-rej-op-tcp",  1, 128 + 2  }, /* §3.3: 'x' MUST match      */
            { "22-pmp-rej-intport", 8, 0x00     }, /* §3.5 correlator mismatch  */
        };
        for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
            uint8_t b[NATPMP_PMP_MAP_RESP_LEN];
            memcpy(b, mr, sizeof(b));
            b[bad[i].off] = bad[i].val;
            memset(&m, 0xAA, sizeof(m));
            const NatpmpParse v = Natpmp_ParsePmpMapResponse(
                b, (int)sizeof(b), NATPMP_PMP_OP_MAP_UDP, 54321, &m);
            if (v != NATPMP_PARSE_NOT_OURS) {
                fprintf(stderr,
                        "[test_netplay_units] FAIL: %s: verdict %d, expected "
                        "NATPMP_PARSE_NOT_OURS (%d)\n",
                        bad[i].tag, (int)v, (int)NATPMP_PARSE_NOT_OURS);
                fail_count++;
            }
            EXPECT_TRUE("22-pmp-rej-zeroes", m.external_port == 0 && m.lifetime_s == 0);
        }
        for (int len = 0; len < NATPMP_PMP_MAP_RESP_LEN; len++) {
            memset(&m, 0xAA, sizeof(m));
            if (Natpmp_ParsePmpMapResponse(mr, len, NATPMP_PMP_OP_MAP_UDP, 54321, &m) ==
                NATPMP_PARSE_OK) {
                fprintf(stderr,
                        "[test_netplay_units] FAIL: 18-pmp-short: a %d-byte frame parsed "
                        "OK; the NAT-PMP mapping response is 16 bytes (RFC 6886 §3.3)\n",
                        len);
                fail_count++;
                break;
            }
        }

        /* A non-zero result code (RFC 6886 §3.5) is a refusal, and the
         * external port / lifetime must NOT come out with it. */
        for (uint8_t rc = 1; rc <= 5; rc++) {
            uint8_t b[NATPMP_PMP_MAP_RESP_LEN];
            memcpy(b, mr, sizeof(b));
            b[3] = rc;
            memset(&m, 0xAA, sizeof(m));
            const NatpmpParse v = Natpmp_ParsePmpMapResponse(
                b, (int)sizeof(b), NATPMP_PMP_OP_MAP_UDP, 54321, &m);
            if (v != NATPMP_PARSE_REFUSED) {
                fprintf(stderr,
                        "[test_netplay_units] FAIL: 18-pmp-refuse-%u: verdict %d, "
                        "expected NATPMP_PARSE_REFUSED (%d)\n",
                        (unsigned)rc, (int)v, (int)NATPMP_PARSE_REFUSED);
                fail_count++;
            }
            EXPECT_TRUE("22-pmp-refuse-code", m.result_code == rc);
            EXPECT_TRUE("22-pmp-refuse-noport",
                        m.external_port == 0 && m.lifetime_s == 0);
        }

        /* Public-address response, RFC 6886 §3.2. */
        uint8_t ar[NATPMP_PMP_ADDR_RESP_LEN];
        memset(ar, 0, sizeof(ar));
        ar[0] = 0; ar[1] = 128 + 0; ar[7] = 0x2A;
        ar[8] = 198; ar[9] = 51; ar[10] = 100; ar[11] = 7;
        NatpmpPmpAddr a;
        memset(&a, 0xAA, sizeof(a));
        EXPECT_TRUE("22-pmp-addr-parse",
                    Natpmp_ParsePmpAddrResponse(ar, (int)sizeof(ar), &a) ==
                        NATPMP_PARSE_OK);
        {
            struct in_addr got_ip;
            memcpy(&got_ip.s_addr, &a.external_ip_be, 4);
            char ipbuf[64] = { 0 };
            inet_ntop(AF_INET, &got_ip, ipbuf, sizeof(ipbuf));
            EXPECT_TRUE("22-pmp-addr-ip", strcmp(ipbuf, "198.51.100.7") == 0);
        }
        ar[1] = 128 + 1; /* a mapping response, not an address response */
        memset(&a, 0xAA, sizeof(a));
        EXPECT_TRUE("22-pmp-addr-rej-op",
                    Natpmp_ParsePmpAddrResponse(ar, (int)sizeof(ar), &a) ==
                        NATPMP_PARSE_NOT_OURS);
        /* §3.2: on a non-zero result "the value of the External IPv4
         * Address field is undefined ... MUST be ignored on reception." */
        ar[1] = 128 + 0;
        ar[3] = 3; /* Network Failure */
        memset(&a, 0xAA, sizeof(a));
        EXPECT_TRUE("22-pmp-addr-refused",
                    Natpmp_ParsePmpAddrResponse(ar, (int)sizeof(ar), &a) ==
                        NATPMP_PARSE_REFUSED);
        EXPECT_TRUE("22-pmp-addr-refused-noip", a.external_ip_be == 0);
    }
    return (fail_count == fails_before) ? 0 : 1;
}

/* ---------------------------------------------------------------- */
static int unit_natpmp_ladder_shape(void) {
    tests_run++;
    fprintf(stderr, "[test_netplay_units] natpmp_ladder_shape: the retransmit ladder is 3 rungs, and the budgets are derived from it\n");
    const int fails_before = fail_count;
    Natpmp_TestHook_SetGateway(NULL, 0);
    Natpmp_TestHook_ResetState();
    /* ================= H-7.2: the ladder's SHAPE ====================== *
     *
     * The reviewer restored RFC 6886 §3.1's full nine-rung ladder and the
     * suite stayed green, because the only ladder assertion measured
     * elapsed wall clock — which the phase budget clamps identically
     * either way. The shape is now pinned directly, and the phase budget
     * is DERIVED from the ladder in natpmp.c so the two cannot drift.
     */
    {
        const int* steps = NULL;
        const int n = Natpmp_TestHook_Ladder(&steps);
        if (n != 3 || steps == NULL) {
            fprintf(stderr,
                    "[test_netplay_units] FAIL: 23b-ladder-steps: the retransmit "
                    "ladder has %d rung(s), expected the documented truncation to 3 "
                    "(plan §9.3). RFC 6886 §3.1's full ladder is nine rungs and ~127 s, "
                    "which is not shippable behind a Host Game click.\n", n);
            fail_count++;
        } else {
            const int want[3] = { 250, 500, 1000 };
            for (int i = 0; i < 3; i++) {
                if (steps[i] != want[i]) {
                    fprintf(stderr,
                            "[test_netplay_units] FAIL: 23b-ladder-rung%d: %d ms, "
                            "expected %d ms — §3.1's doubling shape, truncated at three "
                            "rungs\n", i, steps[i], want[i]);
                    fail_count++;
                }
            }
        }
        const int phase = Natpmp_TestHook_PhaseBudgetMs();
        if (phase != NATPMP_PHASE_BUDGET_MS) {
            fprintf(stderr,
                    "[test_netplay_units] FAIL: 23b-phase-budget: natpmp.c derives %d "
                    "ms per phase from its ladder but the header advertises %d ms. A "
                    "phase shorter than its ladder truncates the ladder silently; a "
                    "longer one inflates the worst case nobody budgeted for.\n",
                    phase, NATPMP_PHASE_BUDGET_MS);
            fail_count++;
        }
        /* L-1: these two were written as
         *   NATPMP_PROBE_BUDGET_MS == 3 * NATPMP_PHASE_BUDGET_MS
         *   NATPMP_RENEW_BUDGET_MS == 2 * NATPMP_PHASE_BUDGET_MS
         * but natpmp.h:267 and natpmp.h:275 DEFINE those two macros as
         * exactly those expressions, so each assertion expanded to
         * (3 * 1750) == 3 * 1750 and could not fail for any edit to any
         * of the three constants. Unfalsifiable, therefore worthless.
         *
         * Pinned by LITERAL instead, the way test 28 pins
         * STUN_PUNCH_CONFIRM_MS: the numbers below are wall-clock
         * ceilings a user waits behind a Host Game click, and 23b's
         * neighbours only check INTERNAL CONSISTENCY (the ladder against
         * the phase, the phase against natpmp.c's derivation) — all of
         * which stay satisfied while the absolute totals drift. Changing
         * these is a deliberate product decision, so make it red. */
        if (NATPMP_PROBE_BUDGET_MS != 5250) {
            fprintf(stderr,
                    "[test_netplay_units] FAIL: 23b-probe-budget-literal: "
                    "NATPMP_PROBE_BUDGET_MS is %d ms, expected the shipped literal "
                    "5250 (three 1750 ms phases). This is the worst case a user waits "
                    "for a first-time port-mapping probe.\n",
                    (int)NATPMP_PROBE_BUDGET_MS);
            fail_count++;
        }
        if (NATPMP_RENEW_BUDGET_MS != 3500) {
            fprintf(stderr,
                    "[test_netplay_units] FAIL: 23b-renew-budget-literal: "
                    "NATPMP_RENEW_BUDGET_MS is %d ms, expected the shipped literal "
                    "3500 (two 1750 ms phases). A renewal budget shorter than a slow "
                    "gateway needs loses the mapping mid-session.\n",
                    (int)NATPMP_RENEW_BUDGET_MS);
            fail_count++;
        }
    }
    return (fail_count == fails_before) ? 0 : 1;
}

/* ---------------------------------------------------------------- */
static int unit_pcp_short_error(void) {
    tests_run++;
    fprintf(stderr, "[test_netplay_units] pcp_short_error: RFC 6887 section 8.3 length floor: a header-only error is REFUSED, not silence\n");
    const int fails_before = fail_count;
    Natpmp_TestHook_ResetState();
        /* Codec level first — no timing, no sockets. */
        uint8_t hdr[NATPMP_PCP_HDR_LEN];
        memset(hdr, 0, sizeof(hdr));
        hdr[0] = 2;
        hdr[1] = 0x80u | 1u;
        hdr[3] = (uint8_t)NATPMP_PCP_NOT_AUTHORIZED;
        hdr[11] = 0x2A;
        uint8_t any_nonce[NATPMP_PCP_NONCE_LEN];
        for (int i = 0; i < NATPMP_PCP_NONCE_LEN; i++) any_nonce[i] = (uint8_t)i;
        NatpmpPcpMap sm;
        memset(&sm, 0xAA, sizeof(sm));
        const NatpmpParse sv = Natpmp_ParsePcpMapResponse(hdr, (int)sizeof(hdr),
                                                          any_nonce, NATPMP_PROTO_UDP,
                                                          54321, &sm);
        if (sv != NATPMP_PARSE_REFUSED) {
            fprintf(stderr,
                    "[test_netplay_units] FAIL: 23b-short-error: a 24-octet PCP error "
                    "response parsed as %d, expected NATPMP_PARSE_REFUSED (%d). RFC 6887 "
                    "§8.3 puts the floor at 24 octets, not at the 60-octet MAP "
                    "response; discarding it makes a refusing gateway look silent.\n",
                    (int)sv, (int)NATPMP_PARSE_REFUSED);
            fail_count++;
        }
        EXPECT_TRUE("23b-short-error-code",
                    sm.result_code == (uint8_t)NATPMP_PCP_NOT_AUTHORIZED);
        EXPECT_TRUE("23b-short-error-noport", sm.external_port == 0);
        /* A short SUCCESS still cannot be believed: §11.4 matches on the
         * protocol, internal port and nonce, none of which are present. */
        hdr[3] = 0;
        memset(&sm, 0xAA, sizeof(sm));
        EXPECT_TRUE("23b-short-success-rejected",
                    Natpmp_ParsePcpMapResponse(hdr, (int)sizeof(hdr), any_nonce,
                                               NATPMP_PROTO_UDP, 54321, &sm) ==
                        NATPMP_PARSE_NOT_OURS);
        /* §8.3's other two length rules. */
        uint8_t odd[26];
        memset(odd, 0, sizeof(odd));
        odd[0] = 2; odd[1] = 0x80u | 1u; odd[3] = 2;
        memset(&sm, 0xAA, sizeof(sm));
        EXPECT_TRUE("23b-not-multiple-of-4",
                    Natpmp_ParsePcpMapResponse(odd, 26, any_nonce, NATPMP_PROTO_UDP,
                                               54321, &sm) == NATPMP_PARSE_NOT_OURS);
    return (fail_count == fails_before) ? 0 : 1;
}

/* ---------------------------------------------------------------- */
static int unit_nonpublic_gate(void) {
    tests_run++;
    fprintf(stderr, "[test_netplay_units] nonpublic_gate: the CGNAT / non-public IP predicate fails CLOSED on empty and NULL\n");
    const int fails_before = fail_count;
    /* The gate predicate itself: EMPTY must fail closed, present-but-
     * unparseable must not (it proves nothing). */
    {
        EXPECT_TRUE("23b-gate-empty-closed", DirectP2P_TestHook_IpIsNonPublic(""));
        EXPECT_TRUE("23b-gate-null-closed", DirectP2P_TestHook_IpIsNonPublic(NULL));
        EXPECT_TRUE("23b-gate-cgn", DirectP2P_TestHook_IpIsNonPublic("100.64.5.9"));
        EXPECT_TRUE("23b-gate-rfc1918", DirectP2P_TestHook_IpIsNonPublic("192.168.1.1"));
        EXPECT_FALSE("23b-gate-public", DirectP2P_TestHook_IpIsNonPublic("198.51.100.30"));
        EXPECT_FALSE("23b-gate-garbage", DirectP2P_TestHook_IpIsNonPublic("not-an-ip"));
    }

    return (fail_count == fails_before) ? 0 : 1;
}

/* ---------------------------------------------------------------- */
static int unit_portmap_renew_cadence(void) {
    tests_run++;
    fprintf(stderr, "[test_netplay_units] portmap_renew_cadence: renewal cadence follows the GRANTED lease, and the retry never outlasts it\n");
    const int fails_before = fail_count;
    /* ====== H-7.3 / M-5.2: renewal cadence follows the GRANTED lease == *
     *
     * The reviewer pinned portmap_renew_interval_ms to a flat 30 minutes
     * and the suite stayed green — nothing asserted it at all. RFC 6886
     * §3.3: "The client SHOULD begin trying to renew the mapping halfway
     * to expiry time, like DHCP". RFC 6887 §11.2.1 floors renewals at
     * four seconds apart.
     */
    {
        struct { uint32_t lease_s; uint64_t want_interval; uint64_t want_retry; } iv[] = {
            /* UPnP: no granted lease reported, keep the old constants. */
            { 0u,     30u * 60u * 1000u, 5u * 60u * 1000u },
            /* A router that shortens our 3600 s request to 120 s. A flat
             * 30-minute timer would fire 28 minutes after it died, and a
             * flat 5-minute RETRY 4.5 minutes after that. */
            { 120u,   60u * 1000u,       30u * 1000u },
            { 3600u,  30u * 60u * 1000u, 5u * 60u * 1000u },
            /* Longer than the UPnP cap: clamped, never longer. */
            { 7200u,  30u * 60u * 1000u, 5u * 60u * 1000u },
            /* §11.2.1's floor: "renewal requests MUST NOT be sent less
             * than four seconds apart". */
            { 4u,     4000u,             4000u },
        };
        for (size_t i = 0; i < sizeof(iv) / sizeof(iv[0]); i++) {
            const uint64_t got_i =
                DirectP2P_TestHook_PortmapRenewIntervalMs(iv[i].lease_s);
            if (got_i != iv[i].want_interval) {
                fprintf(stderr,
                        "[test_netplay_units] FAIL: 23b-renew-interval-%u: got %u ms, "
                        "expected %u ms. RFC 6886 §3.3 renews at half the lease the "
                        "GATEWAY granted, not half the one we asked for.\n",
                        (unsigned)iv[i].lease_s, (unsigned)got_i,
                        (unsigned)iv[i].want_interval);
                fail_count++;
            }
            const uint64_t got_r = DirectP2P_TestHook_PortmapRenewRetryMs(iv[i].lease_s);
            if (got_r != iv[i].want_retry) {
                fprintf(stderr,
                        "[test_netplay_units] FAIL: 23b-renew-retry-%u: got %u ms, "
                        "expected %u ms. A failed renewal must be retried while the "
                        "mapping is still alive; a flat five minutes is long after a "
                        "120 s lease has gone.\n",
                        (unsigned)iv[i].lease_s, (unsigned)got_r,
                        (unsigned)iv[i].want_retry);
                fail_count++;
            }
            /* The retry can never outlast the lease it is retrying. */
            if (iv[i].lease_s != 0 && got_r > (uint64_t)iv[i].lease_s * 1000u) {
                fprintf(stderr,
                        "[test_netplay_units] FAIL: 23b-renew-retry-past-lease-%u: "
                        "retry %u ms exceeds the whole %u s lease\n",
                        (unsigned)iv[i].lease_s, (unsigned)got_r,
                        (unsigned)iv[i].lease_s);
                fail_count++;
            }
        }
    }

    return (fail_count == fails_before) ? 0 : 1;
}

/* ==================================================================
 * Task #132 P2 — the ORCH_* timing cascade, derived INDEPENDENTLY.
 *
 * THE RULE, and it is the whole reason this test exists: the expected
 * values below are computed from primitive constants re-listed HERE.
 * They are never derived from ORCH_HOST_LADDER_MS or any of its
 * relatives. A test that computes ORCH_HOST_LADDER_MS from
 * ORCH_HOST_LADDER_MS passes through any error in it, which is precisely
 * how #131 shipped: the ladder paid WORKER_STARTUP_DELAY_MS ONCE instead
 * of once per rung, and every number in the tree was self-consistent with
 * the wrong formula — [D] asserted 86450 until it became 87050, the
 * shipped-defaults bound was 42450 until it became 43050, the corner was
 * 30450 until it became 31050, and #96's upper guard was 120200 until it
 * became 120800. Four static asserts, all green, all wrong.
 *
 * So this file re-states the primitives. That is deliberate duplication
 * with a job: if someone changes WORKER_STARTUP_DELAY_MS in
 * direct_p2p.c, this test goes red and the change becomes a decision
 * instead of a silent slide. The duplication is the alarm.
 * ================================================================== */

/* --- primitives, re-listed. src/netplay/direct_p2p.c unless noted. --- */
#define U_HOST_STUN_MAX_RETRIES       3     /* direct_p2p.c:479 */
#define U_HOST_STUN_RETRY_BACKOFF_MS  5000  /* direct_p2p.c:480 */
#define U_WORKER_STARTUP_DELAY_MS     200   /* direct_p2p.c:513 */
#define U_JOIN_MAX_ATTEMPTS           2     /* direct_p2p.c:521 */
#define U_RESOLVE_POLL_ATTEMPTS       100   /* direct_p2p.c:532 */
#define U_RESOLVE_POLL_STEP_MS        1     /* direct_p2p.c:533 */
#define U_RESOLVE_POLL_MAX_MS         (U_RESOLVE_POLL_ATTEMPTS * U_RESOLVE_POLL_STEP_MS)
#define U_UPNP_PROBE_BUDGET_MS        6000  /* direct_p2p.c:2600 */
#define U_NATPMP_PHASE_BUDGET_MS      1750  /* natpmp.h:260 */
#define U_NATPMP_PROBE_BUDGET_MS      (3 * U_NATPMP_PHASE_BUDGET_MS) /* natpmp.h:267 */
#define U_PORTMAP_PROBE_BUDGET_MS     (U_UPNP_PROBE_BUDGET_MS + U_NATPMP_PROBE_BUDGET_MS)
#define U_STUN_PUNCH_CONFIRM_MS       600   /* stun.h */
#define U_ORCH_JOIN_PORTMAP_SERIAL_MS 0     /* direct_p2p.c:6120 */
/* clamps, direct_p2p.c:1006-1008 and :1190-1192 */
#define U_RACE_BUDGET_DEFAULT_MS 8000
#define U_RACE_BUDGET_MIN_MS     2000
#define U_RACE_BUDGET_MAX_MS     30000
#define U_STUN_BUDGET_DEFAULT_MS 4000
#define U_STUN_BUDGET_MIN_MS     1000
#define U_STUN_BUDGET_MAX_MS     15000

/* --- the cascade, recomputed here from those primitives only --- */
static int u_race_hard_cap_ms(int race_ms) {
    return race_ms + 2 * U_STUN_PUNCH_CONFIRM_MS;
}

static int u_host_ladder_ms(int stun_ms) {
    /* The startup delay is paid PER RUNG — that is the #131 fix, and
     * writing it out here is what makes a regression to "once" visible. */
    return (1 + U_HOST_STUN_MAX_RETRIES) * U_WORKER_STARTUP_DELAY_MS
         + U_PORTMAP_PROBE_BUDGET_MS
         + (1 + U_HOST_STUN_MAX_RETRIES) * stun_ms
         + U_HOST_STUN_MAX_RETRIES * U_HOST_STUN_RETRY_BACKOFF_MS;
}

static int u_host_postwait_ms(int race_ms) {
    return U_RESOLVE_POLL_MAX_MS + u_race_hard_cap_ms(race_ms);
}

static int u_host_worst_ms(int stun_ms, int race_ms) {
    const int a = u_host_ladder_ms(stun_ms);
    const int b = u_host_postwait_ms(race_ms);
    return (a >= b) ? a : b;
}

static int u_join_attempt_ms(int stun_ms, int race_ms) {
    return stun_ms + U_RESOLVE_POLL_MAX_MS + U_ORCH_JOIN_PORTMAP_SERIAL_MS
         + u_race_hard_cap_ms(race_ms);
}

static int u_join_worst_ms(int stun_ms, int race_ms) {
    return U_WORKER_STARTUP_DELAY_MS
         + U_JOIN_MAX_ATTEMPTS * u_join_attempt_ms(stun_ms, race_ms);
}

static void u_cmp(const char* tag, const char* what, int got, int want) {
    checks_run++;
    if (got != want) {
        fprintf(stderr,
                "[test_netplay_units] FAIL: %s: %s: macro says %d, the "
                "independently derived value is %d\n",
                tag, what, got, want);
        fail_count++;
    }
}

/* ---------------------------------------------------------------- */
static int unit_orch_cascade(void) {
    tests_run++;
    fprintf(stderr, "[test_netplay_units] orch_cascade: the ORCH_* ladder, "
                    "derived from independently listed primitives\n");
    const int fails_before = fail_count;

    /* Every (stun, race) pair the shipped clamps admit at their edges,
     * plus the interior default. The corner (min stun, max race) is the
     * row that makes the MAX in ORCH_HOST_WORST_CASE_MS load-bearing:
     * there the postwait branch WINS, and a version of the macro that
     * simply returned the ladder would be green on every other row. */
    static const struct { int stun; int race; const char* what; } k_rows[] = {
        { U_STUN_BUDGET_DEFAULT_MS, U_RACE_BUDGET_DEFAULT_MS, "shipped defaults" },
        { U_STUN_BUDGET_MAX_MS,     U_RACE_BUDGET_MAX_MS,     "both clamps at ceiling" },
        { U_STUN_BUDGET_MIN_MS,     U_RACE_BUDGET_MAX_MS,     "the postwait corner" },
        { U_STUN_BUDGET_MIN_MS,     U_RACE_BUDGET_MIN_MS,     "both clamps at floor" },
        { U_STUN_BUDGET_MAX_MS,     U_RACE_BUDGET_MIN_MS,     "slow STUN, fast race" },
        { U_STUN_BUDGET_DEFAULT_MS, U_RACE_BUDGET_MAX_MS,     "default STUN, max race" },
        { U_STUN_BUDGET_MIN_MS,     U_RACE_BUDGET_DEFAULT_MS, "min STUN, default race" },
    };
    const int nrows = (int)(sizeof(k_rows) / sizeof(k_rows[0]));

    int rows_done = 0;
    for (int i = 0; i < nrows; i++) {
        const int stun = k_rows[i].stun;
        const int race = k_rows[i].race;
        int hard = 0, ladder = 0, postwait = 0, host = 0, attempt = 0, join = 0;
        DirectP2P_TestHook_OrchCascade(stun, race, &hard, &ladder, &postwait,
                                       &host, &attempt, &join);
        u_cmp(k_rows[i].what, "RACE_HARD_CAP_MS", hard, u_race_hard_cap_ms(race));
        u_cmp(k_rows[i].what, "ORCH_HOST_LADDER_MS", ladder, u_host_ladder_ms(stun));
        u_cmp(k_rows[i].what, "ORCH_HOST_POSTWAIT_MS", postwait, u_host_postwait_ms(race));
        u_cmp(k_rows[i].what, "ORCH_HOST_WORST_CASE_MS", host, u_host_worst_ms(stun, race));
        u_cmp(k_rows[i].what, "ORCH_JOIN_ATTEMPT_MS", attempt, u_join_attempt_ms(stun, race));
        u_cmp(k_rows[i].what, "ORCH_JOIN_WORST_CASE_MS", join, u_join_worst_ms(stun, race));
        rows_done++;
    }
    EXPECT_TRUE("orch-rows", rows_done == nrows);

    /* THE FOUR NUMBERS #131 GOT WRONG, as literals. The table above
     * would stay green if BOTH the macro and this file's re-derivation
     * were changed together — which is exactly what a careless "fix the
     * test" would do. These four are the historical record, and they are
     * only correct for the per-rung ladder. */
    {
        int ladder_ceiling = 0, host_default = 0, host_corner = 0, host_ceiling = 0;
        DirectP2P_TestHook_OrchCascade(U_STUN_BUDGET_MAX_MS, U_RACE_BUDGET_MAX_MS,
                                       NULL, &ladder_ceiling, NULL, &host_ceiling,
                                       NULL, NULL);
        DirectP2P_TestHook_OrchCascade(U_STUN_BUDGET_DEFAULT_MS,
                                       U_RACE_BUDGET_DEFAULT_MS, NULL, NULL, NULL,
                                       &host_default, NULL, NULL);
        DirectP2P_TestHook_OrchCascade(U_STUN_BUDGET_MIN_MS, U_RACE_BUDGET_MAX_MS,
                                       NULL, NULL, NULL, &host_corner, NULL, NULL);
        u_cmp("#131", "the ceiling ladder (was 86450 with a once-paid startup delay)",
              ladder_ceiling, 87050);
        u_cmp("#131", "shipped defaults (was 42450)", host_default, 43050);
        u_cmp("#131", "the corner (was 30450 on the ladder branch)",
              host_corner, u_host_postwait_ms(U_RACE_BUDGET_MAX_MS));
        u_cmp("#131", "the ceiling host bound", host_ceiling, 87050);
        /* #96's upper guard: the port-map probe must not re-enter the
         * per-rung multiplier. 120800 is the number that guard carries. */
        EXPECT_TRUE("#96-guard", host_ceiling < 120800);
        /* ...and the guard must not be vacuous: it has to be the ladder
         * that is bounded, not a bound so loose anything passes. */
        EXPECT_TRUE("#96-guard-tight", host_ceiling > 120800 / 2);
    }

    /* The MAX is load-bearing in BOTH directions, which one row cannot
     * show: at the corner the postwait branch must win, at the defaults
     * the ladder branch must win, and the two must differ. */
    {
        int corner_ladder = 0, corner_postwait = 0, corner_host = 0;
        DirectP2P_TestHook_OrchCascade(U_STUN_BUDGET_MIN_MS, U_RACE_BUDGET_MAX_MS,
                                       NULL, &corner_ladder, &corner_postwait,
                                       &corner_host, NULL, NULL);
        EXPECT_TRUE("orch-max-corner", corner_postwait > corner_ladder);
        EXPECT_TRUE("orch-max-corner", corner_host == corner_postwait);

        int def_ladder = 0, def_postwait = 0, def_host = 0;
        DirectP2P_TestHook_OrchCascade(U_STUN_BUDGET_DEFAULT_MS,
                                       U_RACE_BUDGET_DEFAULT_MS, NULL, &def_ladder,
                                       &def_postwait, &def_host, NULL, NULL);
        EXPECT_TRUE("orch-max-default", def_ladder > def_postwait);
        EXPECT_TRUE("orch-max-default", def_host == def_ladder);
    }

    /* Monotonicity and slope, which catch a coefficient error that
     * happens to land on the right value for one row. */
    {
        int slope_ok = 0;
        for (int stun = U_STUN_BUDGET_MIN_MS; stun <= U_STUN_BUDGET_MAX_MS; stun += 250) {
            int a = 0, b = 0;
            DirectP2P_TestHook_OrchCascade(stun, U_RACE_BUDGET_DEFAULT_MS, NULL,
                                           &a, NULL, NULL, NULL, NULL);
            DirectP2P_TestHook_OrchCascade(stun + 1000, U_RACE_BUDGET_DEFAULT_MS,
                                           NULL, &b, NULL, NULL, NULL, NULL);
            /* one rung per retry, plus the first attempt */
            u_cmp("orch-slope", "1000 ms more STUN budget costs (1+retries) x 1000",
                  b - a, (1 + U_HOST_STUN_MAX_RETRIES) * 1000);
            slope_ok++;
        }
        EXPECT_TRUE("orch-slope", slope_ok >= 50);

        int jslope = 0;
        for (int race = U_RACE_BUDGET_MIN_MS; race <= U_RACE_BUDGET_MAX_MS; race += 500) {
            int a = 0, b = 0;
            DirectP2P_TestHook_OrchCascade(U_STUN_BUDGET_DEFAULT_MS, race, NULL,
                                           NULL, NULL, NULL, NULL, &a);
            DirectP2P_TestHook_OrchCascade(U_STUN_BUDGET_DEFAULT_MS, race + 1000,
                                           NULL, NULL, NULL, NULL, NULL, &b);
            u_cmp("orch-join-slope", "1000 ms more race budget costs attempts x 1000",
                  b - a, U_JOIN_MAX_ATTEMPTS * 1000);
            jslope++;
        }
        EXPECT_TRUE("orch-join-slope", jslope >= 50);
    }

    /* The join bound must contain a WHOLE race including both H-1
     * confirmation tails, at the worst clamp corner — the property
     * static assert [B] carries. */
    {
        int attempt = 0, hard = 0;
        DirectP2P_TestHook_OrchCascade(U_STUN_BUDGET_MIN_MS, U_RACE_BUDGET_MAX_MS,
                                       &hard, NULL, NULL, NULL, &attempt, NULL);
        EXPECT_TRUE("orch-join-contains-race", attempt >= hard);
        u_cmp("orch-hard-cap", "the hard cap carries BOTH 600 ms tails",
              hard, U_RACE_BUDGET_MAX_MS + 2 * U_STUN_PUNCH_CONFIRM_MS);
    }

    return (fail_count == fails_before) ? 0 : 1;
}

/* ==================================================================
 * Task #132 P2 — natpmp deadline arithmetic, with the clock supplied.
 *
 * Every one of these used to be reachable only by letting real time
 * pass, so the existing natpmp tests assert elapsed wall clock and cost
 * seconds each. Elapsed time also cannot SEE most of this: an
 * already-expired deadline, the 2000000000 ms clamp, and a phase ceiling
 * that truncates a rung all look identical from outside.
 * ================================================================== */

#define U_LADDER_0 250
#define U_LADDER_1 500
#define U_LADDER_2 1000
#define U_LADDER_STEPS 3

/* ---------------------------------------------------------------- */
static int unit_natpmp_deadline_math(void) {
    tests_run++;
    fprintf(stderr, "[test_netplay_units] natpmp_deadline_math: the timeout "
                    "cascade as pure integer arithmetic\n");
    const int fails_before = fail_count;

    /* --- np_remaining_ms ------------------------------------------- */
    EXPECT_TRUE("np-rem", Natpmp_TestHook_RemainingMs(1000, 0) == 1000);
    EXPECT_TRUE("np-rem", Natpmp_TestHook_RemainingMs(1000, 999) == 1);
    /* Exactly at the deadline is EXPIRED, not "one more millisecond". */
    EXPECT_TRUE("np-rem-edge", Natpmp_TestHook_RemainingMs(1000, 1000) == 0);
    /* Past it clamps to 0 and never goes negative — a negative would
     * become a select() timeout of "wait forever" one cast later. */
    EXPECT_TRUE("np-rem-past", Natpmp_TestHook_RemainingMs(1000, 1001) == 0);
    EXPECT_TRUE("np-rem-past", Natpmp_TestHook_RemainingMs(0, 0xFFFFFFFFFFFFFFFFull) == 0);
    EXPECT_TRUE("np-rem-zero", Natpmp_TestHook_RemainingMs(0, 0) == 0);

    /* The clamp. A deadline more than 2e9 ms out must not overflow the
     * int this returns. Sweep the boundary. */
    EXPECT_TRUE("np-rem-clamp", Natpmp_TestHook_RemainingMs(2000000000ull, 0) == 2000000000);
    EXPECT_TRUE("np-rem-clamp", Natpmp_TestHook_RemainingMs(2000000001ull, 0) == 2000000000);
    EXPECT_TRUE("np-rem-clamp", Natpmp_TestHook_RemainingMs(0xFFFFFFFFFFFFFFFFull, 0) == 2000000000);
    EXPECT_TRUE("np-rem-clamp", Natpmp_TestHook_RemainingMs(1999999999ull, 0) == 1999999999);

    /* A dense sweep so an off-by-one anywhere in the subtraction shows. */
    {
        int swept = 0;
        for (uint64_t now = 0; now <= 4000; now += 7) {
            const uint64_t deadline = 2000;
            const int want = (now >= deadline) ? 0 : (int)(deadline - now);
            const int got = Natpmp_TestHook_RemainingMs(deadline, now);
            if (got != want) {
                fprintf(stderr,
                        "[test_netplay_units] FAIL: np-rem-sweep: now=%llu -> %d, "
                        "want %d\n", (unsigned long long)now, got, want);
                fail_count++;
            }
            checks_run++;
            swept++;
        }
        EXPECT_TRUE("np-rem-sweep", swept >= 500);
    }

    /* Large `now` values: SDL_GetTicks is 64-bit and does not wrap in any
     * realistic uptime, but the arithmetic must still be correct up
     * there, and this is the only way to get there without waiting
     * 584 million years. */
    {
        const uint64_t big = 0xFFFFFFFF00000000ull;
        EXPECT_TRUE("np-rem-big", Natpmp_TestHook_RemainingMs(big + 250, big) == 250);
        EXPECT_TRUE("np-rem-big", Natpmp_TestHook_RemainingMs(big, big + 1) == 0);
    }

    /* --- np_step_deadline ------------------------------------------ */
    {
        const uint64_t now = 10000;
        const uint64_t phase = now + 5000; /* a phase with room to spare */
        static const int k_want[U_LADDER_STEPS] = { U_LADDER_0, U_LADDER_1, U_LADDER_2 };
        int rungs = 0;
        for (int step = 0; step < U_LADDER_STEPS; step++) {
            const uint64_t d = Natpmp_TestHook_StepDeadline(phase, step, false, now);
            if (d != now + (uint64_t)k_want[step]) {
                fprintf(stderr,
                        "[test_netplay_units] FAIL: np-step: rung %d deadline is "
                        "now+%llu, want now+%d — the RFC 6886 3.1 ladder is not "
                        "{250,500,1000}\n",
                        step, (unsigned long long)(d - now), k_want[step]);
                fail_count++;
            }
            checks_run++;
            rungs++;
        }
        EXPECT_TRUE("np-step-count", rungs == U_LADDER_STEPS);

        /* The phase ceiling truncates a rung rather than overrunning it.
         * This is the H-6 fix: a phase must never eat the next one's
         * budget, and it is invisible to an elapsed-time assertion. */
        EXPECT_TRUE("np-step-clamp",
                    Natpmp_TestHook_StepDeadline(now + 100, 2, false, now) == now + 100);
        EXPECT_TRUE("np-step-clamp",
                    Natpmp_TestHook_StepDeadline(now, 0, false, now) == now);
        /* Exactly-fits is not truncated. */
        EXPECT_TRUE("np-step-exact",
                    Natpmp_TestHook_StepDeadline(now + U_LADDER_1, 1, false, now)
                        == now + U_LADDER_1);

        /* A SUPPRESSED rung inherits the whole remaining phase in one go
         * — that is what stops a proven-alive gateway from being
         * retransmitted at. */
        EXPECT_TRUE("np-step-suppressed",
                    Natpmp_TestHook_StepDeadline(phase, 0, true, now) == phase);
        EXPECT_TRUE("np-step-suppressed",
                    Natpmp_TestHook_StepDeadline(phase, 2, true, now) == phase);

        /* Out-of-range steps fall back to the phase ceiling rather than
         * indexing off the end of the ladder. */
        EXPECT_TRUE("np-step-oob",
                    Natpmp_TestHook_StepDeadline(phase, -1, false, now) == phase);
        EXPECT_TRUE("np-step-oob",
                    Natpmp_TestHook_StepDeadline(phase, U_LADDER_STEPS, false, now) == phase);
        EXPECT_TRUE("np-step-oob",
                    Natpmp_TestHook_StepDeadline(phase, 999, false, now) == phase);
    }

    /* --- np_phase_deadline ----------------------------------------- */
    {
        const uint64_t now = 50000;
        const int phase_budget = Natpmp_TestHook_PhaseBudgetMs();
        EXPECT_TRUE("np-phase-budget", phase_budget == NATPMP_PHASE_BUDGET_MS);
        /* The budget is the ladder's own sum, not a second literal. */
        EXPECT_TRUE("np-phase-budget",
                    phase_budget == U_LADDER_0 + U_LADDER_1 + U_LADDER_2);

        /* Room to spare: a fresh full phase. */
        EXPECT_TRUE("np-phase",
                    Natpmp_TestHook_PhaseDeadline(now + 100000, now)
                        == now + (uint64_t)phase_budget);
        /* Overall ceiling wins when the phase would overrun it. */
        EXPECT_TRUE("np-phase-clamp",
                    Natpmp_TestHook_PhaseDeadline(now + 10, now) == now + 10);
        /* Already past the overall deadline: the phase cannot resurrect it. */
        EXPECT_TRUE("np-phase-expired",
                    Natpmp_TestHook_PhaseDeadline(now - 1, now) == now - 1);
        EXPECT_TRUE("np-phase-expired",
                    Natpmp_TestHook_RemainingMs(
                        Natpmp_TestHook_PhaseDeadline(now - 1, now), now) == 0);
        /* Exactly-fits. */
        EXPECT_TRUE("np-phase-exact",
                    Natpmp_TestHook_PhaseDeadline(now + (uint64_t)phase_budget, now)
                        == now + (uint64_t)phase_budget);

        /* THREE phases must fit inside NATPMP_PROBE_BUDGET_MS exactly —
         * the relationship natpmp.h:267 asserts by construction and which
         * nothing checked against the ladder's real sum. */
        EXPECT_TRUE("np-probe-budget",
                    NATPMP_PROBE_BUDGET_MS == 3 * phase_budget);
        EXPECT_TRUE("np-renew-budget",
                    NATPMP_RENEW_BUDGET_MS == 2 * phase_budget);

        /* Walk three back-to-back phases against one overall budget and
         * check none of them can overrun it — the H-6 property, in
         * microseconds instead of the 3896 ms it took to measure. */
        uint64_t t = now;
        const uint64_t overall = now + (uint64_t)NATPMP_PROBE_BUDGET_MS;
        int phases = 0;
        for (int i = 0; i < 3; i++) {
            const uint64_t pd = Natpmp_TestHook_PhaseDeadline(overall, t);
            EXPECT_TRUE("np-phase-chain", pd <= overall);
            EXPECT_TRUE("np-phase-chain", pd == t + (uint64_t)phase_budget);
            t = pd; /* the next phase starts where this one ended */
            phases++;
        }
        EXPECT_TRUE("np-phase-chain", phases == 3);
        EXPECT_TRUE("np-phase-chain", t == overall);
        /* A fourth phase gets nothing at all, which is the point. */
        EXPECT_TRUE("np-phase-chain",
                    Natpmp_TestHook_PhaseDeadline(overall, t) == overall);
        EXPECT_TRUE("np-phase-chain",
                    Natpmp_TestHook_RemainingMs(overall, t) == 0);
    }

    return (fail_count == fails_before) ? 0 : 1;
}

/* ---------------------------------------------------------------- */
/* #145 — the deferred menu-exit decision math.
 *
 * VS_Result case 7 (menu.c) runs inside the rollback simulation, so a
 * RUNNING-state Netplay_HandleMenuExit only LATCHES a request stamped
 * with the simulated frame. Two pure predicates then decide its fate:
 *
 *   confirmed(head, R, W):  fire once head - R >= W. GekkoNet @ 7be848c
 *     caps predicted remote input at W consecutive frames and rolls back
 *     before every advance, so a frame R with head >= R + W can never be
 *     re-simulated. >= W (not W + 1) also matters for liveness: a peer
 *     that already quit stops sending, and prediction can carry the head
 *     exactly W frames past R — one frame shorter and a mutual clean
 *     quit would stall into the 5 s disconnect timeout.
 *
 *   erased(L, R):  a GekkoLoadEvent to frame L cancels iff L < R. A
 *     load to R restores post-R state without re-running R's sim, so
 *     the request survives it; L < R re-simulates R, and only a
 *     corrected timeline that still quits re-latches (case 7 re-executes
 *     on the replay leg). */
static int unit_menu_exit_deferral(void) {
    tests_run++;
    fprintf(stderr, "[test_netplay_units] menu_exit_deferral: #145 confirm/"
                    "cancel boundaries\n");
    const int fails_before = fail_count;

    /* No pending request (-1 sentinel): neither predicate ever acts. */
    EXPECT_FALSE("mx-none", Netplay_TestHook_MenuExitConfirmed(1000, -1, 8));
    EXPECT_FALSE("mx-none", Netplay_TestHook_MenuExitErasedByLoad(0, -1));

    /* Confirmation boundary at the default window (8): one frame short
     * holds, exactly W fires, beyond W fires. */
    EXPECT_FALSE("mx-conf-edge", Netplay_TestHook_MenuExitConfirmed(507, 500, 8));
    EXPECT_TRUE("mx-conf-edge", Netplay_TestHook_MenuExitConfirmed(508, 500, 8));
    EXPECT_TRUE("mx-conf-edge", Netplay_TestHook_MenuExitConfirmed(509, 500, 8));
    /* Same frame as the request: never (a request cannot be older than
     * the head that latched it, but the head can equal it). */
    EXPECT_FALSE("mx-conf-same", Netplay_TestHook_MenuExitConfirmed(500, 500, 8));

    /* The bound tracks the CONFIGURED window (1..32 per configure_gekko's
     * clamp), not a constant. */
    EXPECT_TRUE("mx-conf-w1", Netplay_TestHook_MenuExitConfirmed(501, 500, 1));
    EXPECT_FALSE("mx-conf-w32", Netplay_TestHook_MenuExitConfirmed(531, 500, 32));
    EXPECT_TRUE("mx-conf-w32", Netplay_TestHook_MenuExitConfirmed(532, 500, 32));

    /* Frame 0 request (the s_sim_frame < 0 fallback latch). */
    EXPECT_FALSE("mx-conf-zero", Netplay_TestHook_MenuExitConfirmed(7, 0, 8));
    EXPECT_TRUE("mx-conf-zero", Netplay_TestHook_MenuExitConfirmed(8, 0, 8));

    /* Load-cancel boundary: rewinding PAST the request frame erases it;
     * a load TO it (post-R state, R not re-run) or beyond does not. */
    EXPECT_TRUE("mx-erase", Netplay_TestHook_MenuExitErasedByLoad(499, 500));
    EXPECT_FALSE("mx-erase-same", Netplay_TestHook_MenuExitErasedByLoad(500, 500));
    EXPECT_FALSE("mx-erase-after", Netplay_TestHook_MenuExitErasedByLoad(501, 500));

    /* The reachable trigger, as a walk: quit latched at R=500; the
     * remote's in-flight rematch-confirm forces a rollback loading 497
     * -> request erased; the corrected timeline completes the rematch
     * (VS_Result_Move_Sub r_no[2]=6), case 7 never re-runs, and with no
     * request the head sailing past 508 fires nothing. */
    EXPECT_TRUE("mx-race", Netplay_TestHook_MenuExitErasedByLoad(497, 500));
    EXPECT_FALSE("mx-race", Netplay_TestHook_MenuExitConfirmed(520, -1, 8));

    return (fail_count == fails_before) ? 0 : 1;
}

static int unit_no_draw_frame_hold(void) {
    tests_run++;
    fprintf(stderr, "[test_netplay_units] no_draw_frame_hold: prediction stalls retain the completed canvas\n");
    const int fails_before = fail_count;

    EXPECT_TRUE("no-draw-running-empty",
                Netplay_TestHook_ShouldHoldLastFrame(NETPLAY_SESSION_RUNNING, 0));
    EXPECT_FALSE("no-draw-running-drawable",
                 Netplay_TestHook_ShouldHoldLastFrame(NETPLAY_SESSION_RUNNING, 1));
    EXPECT_TRUE("no-draw-connecting",
                Netplay_TestHook_ShouldHoldLastFrame(NETPLAY_SESSION_CONNECTING, 0));
    EXPECT_FALSE("no-draw-idle",
                 Netplay_TestHook_ShouldHoldLastFrame(NETPLAY_SESSION_IDLE, 0));

    EXPECT_TRUE("renderer-init", SoftwareRenderer_Init(true, 1));
    int w = 0;
    int h = 0;
    int pitch = 0;
    const SWCanvasPixel* canvas = SoftwareRenderer_GetCanvas(&w, &h, &pitch);
    EXPECT_TRUE("renderer-canvas", canvas != NULL && w > 0 && h > 0 && pitch > 0);
    if (canvas != NULL && w > 0 && h > 0 && pitch > 0) {
        const size_t bytes = (size_t)pitch * (size_t)h;
        unsigned char* initial = malloc(bytes);
        unsigned char* completed = malloc(bytes);
        EXPECT_TRUE("renderer-snapshots", initial != NULL && completed != NULL);
        if (initial != NULL && completed != NULL) {
            memcpy(initial, canvas, bytes);
            const uint32_t white = 0xFFFFFFFFu;
            Renderer_DrawUIBitmap(10.0f, 10.0f, 1.0f, &white, 1, 1, 0xFFFFFFFFu);
            SoftwareRenderer_RenderFrame();
            memcpy(completed, canvas, bytes);
            EXPECT_TRUE("renderer-produced-frame", memcmp(initial, completed, bytes) != 0);

            const uint32_t red = 0xFFFF0000u;
            Renderer_DrawUIBitmap(20.0f, 20.0f, 1.0f, &red, 1, 1, 0xFFFFFFFFu);
            EXPECT_TRUE("renderer-discard-count", SoftwareRenderer_HoldLastFrame() == 1);
            EXPECT_TRUE("renderer-held-canvas", memcmp(completed, canvas, bytes) == 0);
        }
        free(initial);
        free(completed);
    }
    SoftwareRenderer_Quit();

    fprintf(stderr, "[test_netplay_units] no_draw_frame_hold OK\n");
    return (fail_count == fails_before) ? 0 : 1;
}

static int unit_bg_repair_requires_source(void) {
    tests_run++;
    fprintf(stderr, "[test_netplay_units] bg_repair_requires_source: torn cache plus purged type-0x12 source\n");
    const int fails_before = fail_count;

    RCKeyWork saved_keys[RCKEY_WORK_MAX];
    Texture saved_bg_tex[4];
    const BG saved_bg = bg_w;
    const u16 saved_switch = Screen_Switch;
    const u16 saved_switch_buffer = Screen_Switch_Buffer;
    s32 saved_pal[8];
    memcpy(saved_keys, rckey_work, sizeof(saved_keys));
    memcpy(saved_bg_tex, ppgBgTex, sizeof(saved_bg_tex));
    memcpy(saved_pal, bgPalCodeOffset, sizeof(saved_pal));

    memset(rckey_work, 0, sizeof(saved_keys));
    memset(ppgBgTex, 0, sizeof(saved_bg_tex));
    memset(&bg_w, 0, sizeof(bg_w));
    bg_w.stage = 0;
    bg_w.scrno = 1;
    bg_w.bg_routine = 3;
    Screen_Switch = 0x1357u;
    Screen_Switch_Buffer = 0x2468u;
    for (int i = 0; i < 8; i++) {
        bgPalCodeOffset[i] = 1000 + i;
    }

    EXPECT_TRUE("bg-source-purged", Search_ramcnt_type(0x12) == 0);
    Bg_Texture_Rollback_Repair();
    EXPECT_TRUE("bg-repair-cache-stays-torn", ppgBgTex[0].be == 0);
    EXPECT_TRUE("bg-repair-switch-untouched", Screen_Switch == 0x1357u);
    EXPECT_TRUE("bg-repair-buffer-untouched", Screen_Switch_Buffer == 0x2468u);
    for (int i = 0; i < 8; i++) {
        EXPECT_TRUE("bg-repair-palette-untouched", bgPalCodeOffset[i] == 1000 + i);
    }

    /* Pin the loader preflight too. Before this fix the call emitted
     * Get_ramcnt_address(0), Get_size_data_ramcnt_key(0), then passed NULL/0
     * to ppgSetupTexChunk_1st: the live session's exact SIGSEGV signature. */
    Bg_Texture_Load_EX();
    EXPECT_TRUE("bg-loader-cache-stays-torn", ppgBgTex[0].be == 0);
    EXPECT_TRUE("bg-loader-switch-untouched", Screen_Switch == 0x1357u);

    memcpy(rckey_work, saved_keys, sizeof(saved_keys));
    memcpy(ppgBgTex, saved_bg_tex, sizeof(saved_bg_tex));
    bg_w = saved_bg;
    Screen_Switch = saved_switch;
    Screen_Switch_Buffer = saved_switch_buffer;
    memcpy(bgPalCodeOffset, saved_pal, sizeof(saved_pal));

    fprintf(stderr, "[test_netplay_units] bg_repair_requires_source OK\n");
    return (fail_count == fails_before) ? 0 : 1;
}

/* ================================================================== */

int Netplay_Test_NetplayUnits(void) {
    fail_count = 0;
    tests_run = 0;
    checks_run = 0;

    int rc = 0;
    rc |= unit_session_key_stability();
    rc |= unit_lan_bypass();
    rc |= unit_failure_taxonomy();
    rc |= unit_host_datagram_gate();
    rc |= unit_rendezvous_cookie_codec();
    rc |= unit_rendezvous_frame_router();
    rc |= unit_punch_gate_throttle();
    rc |= unit_race_budget_wrap_safety();
    rc |= unit_natpmp_codec();
    rc |= unit_natpmp_ladder_shape();
    rc |= unit_pcp_short_error();
    rc |= unit_nonpublic_gate();
    rc |= unit_portmap_renew_cadence();
    rc |= unit_orch_cascade();
    rc |= unit_natpmp_deadline_math();
    rc |= unit_menu_exit_deferral();
    rc |= unit_no_draw_frame_hold();
    rc |= unit_bg_repair_requires_source();

    fprintf(stderr, "[test_netplay_units] summary: %d test(s), %d assertion(s), "
                    "%d failure(s)\n", tests_run, checks_run, fail_count);

    /* Anti-vacuity. A harness that quietly stops running half of itself
     * is worse than one that fails, because it reports GREEN. */
    if (tests_run != EXPECTED_TESTS) {
        fprintf(stderr,
                "[test_netplay_units] FAIL: ran %d test(s), expected exactly %d — "
                "a test was removed from the dispatch without updating "
                "EXPECTED_TESTS\n",
                tests_run, EXPECTED_TESTS);
        fail_count++;
    }
    if (checks_run < EXPECTED_MIN_CHECKS) {
        fprintf(stderr,
                "[test_netplay_units] FAIL: only %d assertion(s) ran, floor is %d — "
                "a test body was short-circuited\n",
                checks_run, EXPECTED_MIN_CHECKS);
        fail_count++;
    }

    if (fail_count > 0 || rc != 0) {
        fprintf(stderr, "[test_netplay_units] %d failure(s)\n", fail_count);
        return 1;
    }
    fprintf(stderr, "[test_netplay_units] OK\n");
    return 0;
}

#else /* !ENABLE_NETPLAY_TESTS */

int Netplay_Test_NetplayUnits(void) {
    fprintf(stderr,
            "[test_netplay_units] not compiled in; rebuild with "
            "-DENABLE_NETPLAY_TESTS to enable.\n");
    return 2;
}

#endif /* ENABLE_NETPLAY_TESTS */
