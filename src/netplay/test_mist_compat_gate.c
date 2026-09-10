/*
 * test_mist_compat_gate.c — task #132, priority 1: the compat/desync gate
 * as a DECISION, tested directly.
 *
 * WHAT THIS COVERS AND WHY IT IS THE HIGHEST-CONSEQUENCE UNIT IN THE
 * NETPLAY TREE. classify_peer_payload (src/netplay/mist_handshake.c:314)
 * is the function that decides whether a peer is allowed to play with us.
 * It runs on ATTACKER-CONTROLLED bytes straight off a UDP socket, before
 * GekkoNet exists, and its four bounds-checked readers (:236 read_cstr,
 * :255 read_u8, :263 read_u16be, :271 read_u64be) plus the header parser
 * (:210) are the only thing between a crafted datagram and either
 *   (a) an out-of-bounds read, or
 *   (b) a WRONG ACCEPT — two builds with different sizeof(GameState) or
 *       different adapted arcade balance data connect, and desync.
 *
 * Until this file it had ZERO direct tests. Everything that touched it
 * drove it through a socket (test_mist_handshake.c) or through the runner
 * (the pump tests), where a wrong verdict on a malformed frame is
 * indistinguishable from a dropped datagram: both look like "no reply".
 * That is the exact shape this project keeps getting bitten by — a pure
 * decision whose failure mode is silence.
 *
 * WHAT IS DELIBERATELY NOT HERE. Nothing that needs two endpoints: the
 * retransmit budget, the gratuitous-ack race, the implicit-completion
 * latch, the reject round-trip. Those are interaction properties and they
 * live in test_mist_handshake.c over real loopback sockets. This file is
 * strictly the pure half.
 *
 * HOW IT CANNOT PASS VACUOUSLY. The three devices established by
 * src/netplay/test_rendezvous_wire.c:73-82, for the same reasons:
 *
 *   1. EXPECTED_TESTS — a LITERAL, never a count of a registry, so a test
 *      that is deleted or never called fails the run instead of vanishing
 *      from it.
 *   2. EXPECTED_MIN_CHECKS — an assertion floor. A run whose bodies stop
 *      executing (an #ifdef, an early return) cannot clear it.
 *   3. Per-sweep case floors — the truncation sweep asserts it visited
 *      every byte offset, the reason matrix asserts it visited every row.
 *      A table that loses a row is a failure, not a smaller green run.
 *
 * And the stub at the bottom says "not compiled in", spelled exactly that
 * way, because tools/gates/run-gates.sh:204 greps for that phrase to turn
 * an exit-2 misbuild into a RED gate rather than a pass.
 *
 * NO -DNETPLAY_TEST_HOOKS NEEDED. Every symbol used here comes from
 * mist_handshake.c, which is in the shipped netplay build unconditionally;
 * the DirectP2P_TestHook_* seams are not touched. Enable with:
 *   EXTRA_CMAKE_ARGS="-DENABLE_NETPLAY=ON \
 *                     -DCMAKE_C_FLAGS=-DENABLE_NETPLAY_TESTS"
 */

#include <stdio.h>

#ifdef ENABLE_NETPLAY_TESTS

#include "netplay/mist_handshake.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* --- counters: the anti-vacuous machinery ------------------------------ */

static int fail_count = 0;
static int tests_run = 0;
static int checks_run = 0;

/* A LITERAL. Never a count of an array of function pointers. */
#define EXPECTED_TESTS 9

/* Assertion floor. The real figure at the time of writing is 1294 and is
 * printed in the summary; this sits comfortably below it and comfortably
 * above what any executing SUBSET could produce (the two largest single
 * tests are the 256-value msg_type sweep and the 459-assertion
 * (off, len) reader sweep). Not an exact count — that invites the habit
 * of bumping the number instead of asking why it moved. */
#define EXPECTED_MIN_CHECKS 900

static void check(const char* tag, bool ok, const char* what) {
    checks_run++;
    if (!ok) {
        fail_count++;
        fprintf(stderr, "[test_mist_compat_gate] FAIL: %s: %s\n", tag, what);
    }
}

#define CHECK(tag, cond) check((tag), (cond), #cond)

static void check_eq_int(const char* tag, long got, long want, const char* what) {
    checks_run++;
    if (got != want) {
        fail_count++;
        fprintf(stderr,
                "[test_mist_compat_gate] FAIL: %s: %s: got %ld, want %ld\n",
                tag, what, got, want);
    }
}

static void check_eq_str(const char* tag, const char* got, const char* want,
                         const char* what) {
    checks_run++;
    if (got == NULL || strcmp(got, want) != 0) {
        fail_count++;
        fprintf(stderr,
                "[test_mist_compat_gate] FAIL: %s: %s: got \"%s\", want \"%s\"\n",
                tag, what, (got != NULL) ? got : "(null)", want);
    }
}

/* --- payload construction ----------------------------------------------
 *
 * Payloads are assembled BYTE BY BYTE here, never through build_frame().
 * mist_handshake.c owns both the encoder and the decoder, so a test that
 * fed the encoder's output to the decoder would agree with itself about a
 * wrong wire format and prove nothing. These bytes are written from the
 * layout table in mist_handshake.h:20-36 instead. */

typedef struct {
    uint8_t b[512];
    size_t  n;
} Pay;

static void put_bytes(Pay* p, const void* src, size_t n) {
    if (p->n + n > sizeof(p->b)) return; /* test bug; the floors catch it */
    memcpy(p->b + p->n, src, n);
    p->n += n;
}
static void put_str(Pay* p, const char* s) { put_bytes(p, s, strlen(s) + 1); }
static void put_u8(Pay* p, uint8_t v) { put_bytes(p, &v, 1); }
static void put_u16be(Pay* p, uint16_t v) {
    uint8_t t[2] = { (uint8_t)(v >> 8), (uint8_t)v };
    put_bytes(p, t, 2);
}
static void put_u64be(Pay* p, uint64_t v) {
    uint8_t t[8];
    for (int i = 0; i < 8; i++) t[i] = (uint8_t)(v >> (56 - 8 * i));
    put_bytes(p, t, 8);
}

/* The build_hash string every crafted payload carries. Seven characters,
 * matching the git short SHA the real build stamps in, and deliberately
 * NOT equal to it: a build_hash difference must be a WARNING and never a
 * reject (mist_handshake.c:378-383 / mist_handshake.h:38). Its length is
 * load-bearing for the offset table below. */
#define TEST_BUILD_HASH "abc1234"

/* Independently derived field offsets for the reference payload. Written
 * out from mist_handshake.h:22-36, NOT read back from the code under
 * test:
 *   "armv7\0"   -> 6 bytes,  offsets  0.. 5
 *   "mister\0"  -> 7 bytes,  offsets  6..12
 *   "abc1234\0" -> 8 bytes,  offsets 13..20
 *   proto_ver   u8           offset  21
 *   state_ver   u16 BE       offsets 22..23
 *   balance     u64 BE       offsets 24..31
 * so a well-formed reference payload is exactly 32 bytes. */
#define OFF_PROTO   21
#define OFF_STATE   22
#define OFF_DIGEST  24
#define REF_LEN     32

/* The local digest every test compares against, installed through the
 * PRODUCTION setter (mist_handshake.c:110) — that is what made the
 * s_balance_digest file global reachable without touching it. */
#define LOCAL_DIGEST UINT64_C(0x0123456789ABCDEF)

static void build_ref(Pay* p, const char* arch, const char* plat,
                      const char* build, uint8_t proto, uint16_t state,
                      uint64_t digest) {
    p->n = 0;
    put_str(p, arch);
    put_str(p, plat);
    put_str(p, build);
    put_u8(p, proto);
    put_u16be(p, state);
    put_u64be(p, digest);
}

/* A payload every field of which agrees with us. */
static void build_good(Pay* p) {
    build_ref(p, MIST_ARCH_TAG, MIST_PLATFORM_TAG, TEST_BUILD_HASH,
              MIST_PROTO_VER, mist_handshake_local_state_ver(), LOCAL_DIGEST);
}

/* Classify with a canary placed at EXACTLY text_cap, so every call in
 * this file proves, for free, that the reason string never runs one byte
 * past the capacity it was handed. The canary deliberately does NOT sit
 * at the end of the whole struct: a guard there only catches an overrun
 * of the allocation, not an overrun of the contract, and the contract is
 * what the caller (the direct-P2P overlay status line, netplay.c) relies
 * on when it passes a short buffer. */
typedef struct {
    char    text[96];
    uint8_t tail[32];
} Guarded;

#define GUARD_LEN 16
static const uint8_t k_canary[GUARD_LEN] = {
    0xC0, 0xDE, 0xBA, 0xBE, 0xC0, 0xDE, 0xBA, 0xBE,
    0xC0, 0xDE, 0xBA, 0xBE, 0xC0, 0xDE, 0xBA, 0xBE,
};

static uint8_t classify(const char* tag, const Pay* p, size_t len,
                        Guarded* g, size_t text_cap) {
    memset(g, 0x5A, sizeof(*g));
    memcpy((uint8_t*)g + text_cap, k_canary, GUARD_LEN);
    const uint8_t r = mist_handshake_test_classify_payload(p->b, len,
                                                           g->text, text_cap);
    checks_run++;
    if (memcmp((uint8_t*)g + text_cap, k_canary, GUARD_LEN) != 0) {
        fail_count++;
        fprintf(stderr,
                "[test_mist_compat_gate] FAIL: %s: reason text wrote past "
                "text_cap=%zu\n", tag, text_cap);
    }
    return r;
}

#define CLASSIFY(tag, p) classify((tag), (p), (p)->n, &g, sizeof(g.text))

/* ====================================================================== */
/* 1. The accept.                                                         */
/* ====================================================================== */

static void test1_accept(void) {
    const char* tag = "accept";
    tests_run++;
    Guarded g;
    Pay p;

    build_good(&p);
    check_eq_int(tag, (long)p.n, REF_LEN,
                 "the reference payload is the 32 bytes the layout table says");
    check_eq_int(tag, CLASSIFY(tag, &p), 0, "a fully-agreeing peer is accepted");

    /* build_hash difference is a WARNING and never a reject — the
     * deliberate tradeoff documented at mist_handshake.h:38 and
     * mist_handshake.c:378. TEST_BUILD_HASH is not this build's SHA, so
     * the accept above already exercised the warning path; make the
     * property explicit with a second, wildly different hash. */
    build_ref(&p, MIST_ARCH_TAG, MIST_PLATFORM_TAG, "zzzzzzz",
              MIST_PROTO_VER, mist_handshake_local_state_ver(), LOCAL_DIGEST);
    check_eq_int(tag, CLASSIFY(tag, &p), 0,
                 "a different build_hash warns, it does not reject");

    /* An EMPTY build_hash is still only a warning. */
    build_ref(&p, MIST_ARCH_TAG, MIST_PLATFORM_TAG, "",
              MIST_PROTO_VER, mist_handshake_local_state_ver(), LOCAL_DIGEST);
    check_eq_int(tag, CLASSIFY(tag, &p), 0, "an empty build_hash still accepts");

    /* Trailing garbage after the digest is ignored — the payload_len the
     * header declared may legitimately exceed what this version reads
     * (that is how a v4 field would arrive at a v3 peer). */
    build_good(&p);
    put_u8(&p, 0xFF);
    put_u8(&p, 0xFF);
    check_eq_int(tag, CLASSIFY(tag, &p), 0,
                 "unknown trailing bytes do not break the accept");
}

/* ====================================================================== */
/* 2. Truncation at EVERY field boundary.                                 */
/* ====================================================================== */

static void test2_truncation_sweep(void) {
    const char* tag = "truncate";
    tests_run++;
    Guarded g;
    Pay p;
    build_good(&p);

    /* Expected verdict per prefix length, derived from the offset table
     * above and from the ORDER of the checks in classify_peer_payload,
     * not from running it:
     *
     *   [ 0, 20]  a string's NUL is missing         -> MALFORMED
     *   [21, 23]  strings clean, proto/state short  -> LEGACY (pre-R-1)
     *   [24, 31]  proto/state clean, digest short   -> MALFORMED
     *        32   complete                          -> accept
     *
     * The LEGACY window is THREE BYTES WIDE and sits between two
     * MALFORMED regions. Get an off-by-one anywhere in the readers and a
     * pre-R-1 peer is told "malformed handshake payload" instead of
     * "Opponent build too old - both need this update", or a truncated
     * frame is misreported as an old build. Both are wrong answers that
     * no connectivity test can see. */
    int cases = 0;
    for (size_t n = 0; n < REF_LEN; n++) {
        const uint8_t want = (n <= 20)                  ? MIST_REJECT_MALFORMED
                           : (n >= 21 && n <= 23)       ? MIST_REJECT_LEGACY
                                                        : MIST_REJECT_MALFORMED;
        const uint8_t got = classify(tag, &p, n, &g, sizeof(g.text));
        if (got != want) {
            checks_run++;
            fail_count++;
            fprintf(stderr,
                    "[test_mist_compat_gate] FAIL: %s: prefix len %zu: got "
                    "reason %u, want %u\n", tag, n, (unsigned)got, (unsigned)want);
        } else {
            checks_run++;
        }
        /* Whatever the reason, a truncated frame is NEVER accepted. */
        CHECK(tag, got != 0);
        cases++;
    }
    check_eq_int(tag, cases, REF_LEN, "every prefix length was visited");

    /* Zero-length payload: the degenerate case a bare header produces. */
    check_eq_int(tag, classify(tag, &p, 0, &g, sizeof(g.text)),
                 MIST_REJECT_MALFORMED, "an empty payload is malformed");

    /* And the two boundary bytes of the LEGACY window named explicitly,
     * so the intent survives a rewrite of the loop above. */
    check_eq_int(tag, classify(tag, &p, OFF_PROTO, &g, sizeof(g.text)),
                 MIST_REJECT_LEGACY, "payload ending exactly after the strings is LEGACY");
    check_eq_int(tag, classify(tag, &p, OFF_PROTO - 1, &g, sizeof(g.text)),
                 MIST_REJECT_MALFORMED, "one byte earlier the build_hash NUL is gone");
    check_eq_int(tag, classify(tag, &p, OFF_DIGEST, &g, sizeof(g.text)),
                 MIST_REJECT_MALFORMED, "proto+state present, digest absent is MALFORMED");
    check_eq_int(tag, classify(tag, &p, REF_LEN - 1, &g, sizeof(g.text)),
                 MIST_REJECT_MALFORMED, "a digest one byte short is MALFORMED");
}

/* ====================================================================== */
/* 3. Every distinct reject reason, produced on purpose.                  */
/* ====================================================================== */

static void test3_reason_matrix(void) {
    const char* tag = "reasons";
    tests_run++;
    Guarded g;
    Pay p;
    const uint16_t local_state = mist_handshake_local_state_ver();

    int rows = 0;

    /* arch */
    build_ref(&p, "armv8", MIST_PLATFORM_TAG, TEST_BUILD_HASH,
              MIST_PROTO_VER, local_state, LOCAL_DIGEST);
    check_eq_int(tag, CLASSIFY(tag, &p), MIST_REJECT_ARCH_MISMATCH, "armv8 peer");
    check_eq_str(tag, g.text, "arch mismatch (armv8)", "arch text names the peer's tag");
    rows++;

    /* arch, case-sensitively — strcmp, not a case-folded compare. */
    build_ref(&p, "ARMV7", MIST_PLATFORM_TAG, TEST_BUILD_HASH,
              MIST_PROTO_VER, local_state, LOCAL_DIGEST);
    check_eq_int(tag, CLASSIFY(tag, &p), MIST_REJECT_ARCH_MISMATCH,
                 "the arch tag compare is case sensitive");
    rows++;

    /* arch, prefix-wise: "armv" and "armv77" must both fail, i.e. the
     * compare is whole-string and not a prefix match in either direction. */
    build_ref(&p, "armv", MIST_PLATFORM_TAG, TEST_BUILD_HASH,
              MIST_PROTO_VER, local_state, LOCAL_DIGEST);
    check_eq_int(tag, CLASSIFY(tag, &p), MIST_REJECT_ARCH_MISMATCH,
                 "a prefix of the arch tag is not the arch tag");
    rows++;
    build_ref(&p, MIST_ARCH_TAG "7", MIST_PLATFORM_TAG, TEST_BUILD_HASH,
              MIST_PROTO_VER, local_state, LOCAL_DIGEST);
    check_eq_int(tag, CLASSIFY(tag, &p), MIST_REJECT_ARCH_MISMATCH,
                 "an extension of the arch tag is not the arch tag");
    rows++;

    /* platform */
    build_ref(&p, MIST_ARCH_TAG, "linux", TEST_BUILD_HASH,
              MIST_PROTO_VER, local_state, LOCAL_DIGEST);
    check_eq_int(tag, CLASSIFY(tag, &p), MIST_REJECT_PLATFORM_MISMATCH, "linux peer");
    check_eq_str(tag, g.text, "platform mismatch (linux)", "platform text names the tag");
    rows++;

    /* legacy: the three strings and nothing else — a pre-R-1 build. */
    p.n = 0;
    put_str(&p, MIST_ARCH_TAG);
    put_str(&p, MIST_PLATFORM_TAG);
    put_str(&p, TEST_BUILD_HASH);
    check_eq_int(tag, (long)p.n, OFF_PROTO, "a legacy payload is the strings alone");
    check_eq_int(tag, CLASSIFY(tag, &p), MIST_REJECT_LEGACY, "pre-R-1 peer");
    check_eq_str(tag, g.text, "Opponent build too old - both need this update",
                 "the legacy text is the actionable one");
    rows++;

    /* proto_ver: below, above, and the extremes. */
    {
        const uint8_t protos[] = { 0, 1, 2, (uint8_t)(MIST_PROTO_VER + 1), 255 };
        for (size_t i = 0; i < sizeof(protos) / sizeof(protos[0]); i++) {
            build_ref(&p, MIST_ARCH_TAG, MIST_PLATFORM_TAG, TEST_BUILD_HASH,
                      protos[i], local_state, LOCAL_DIGEST);
            check_eq_int(tag, CLASSIFY(tag, &p), MIST_REJECT_PROTO_MISMATCH,
                         "a proto_ver that is not ours rejects");
            rows++;
        }
        build_ref(&p, MIST_ARCH_TAG, MIST_PLATFORM_TAG, TEST_BUILD_HASH,
                  2, local_state, LOCAL_DIGEST);
        (void)CLASSIFY(tag, &p);
        check_eq_str(tag, g.text, "Handshake v2 vs v5 - update one side",
                     "the proto text names both versions");
    }

    /* state_ver: the desync-preventing check. */
    {
        const uint16_t states[] = { 0, 1, (uint16_t)(local_state - 1),
                                    (uint16_t)(local_state + 1), 0xFFFF };
        for (size_t i = 0; i < sizeof(states) / sizeof(states[0]); i++) {
            if (states[i] == local_state) continue;
            build_ref(&p, MIST_ARCH_TAG, MIST_PLATFORM_TAG, TEST_BUILD_HASH,
                      MIST_PROTO_VER, states[i], LOCAL_DIGEST);
            check_eq_int(tag, CLASSIFY(tag, &p), MIST_REJECT_STATE_MISMATCH,
                         "a sizeof(GameState) that is not ours rejects");
            rows++;
        }
        char want[96];
        snprintf(want, sizeof(want), "Build state %u vs %u - update one side",
                 (unsigned)(uint16_t)(local_state + 1), (unsigned)local_state);
        build_ref(&p, MIST_ARCH_TAG, MIST_PLATFORM_TAG, TEST_BUILD_HASH,
                  MIST_PROTO_VER, (uint16_t)(local_state + 1), LOCAL_DIGEST);
        (void)CLASSIFY(tag, &p);
        check_eq_str(tag, g.text, want, "the state text is the symmetric phrasing");
    }

    /* balance digest: the second desync-preventing check. */
    build_ref(&p, MIST_ARCH_TAG, MIST_PLATFORM_TAG, TEST_BUILD_HASH,
              MIST_PROTO_VER, local_state, LOCAL_DIGEST ^ UINT64_C(1));
    check_eq_int(tag, CLASSIFY(tag, &p), MIST_REJECT_BALANCE_MISMATCH,
                 "one bit of arcade-balance divergence rejects");
    rows++;

    /* malformed: a string with no terminator anywhere in the payload. */
    p.n = 0;
    put_bytes(&p, "armv7", 5); /* NO NUL */
    check_eq_int(tag, CLASSIFY(tag, &p), MIST_REJECT_MALFORMED,
                 "an unterminated string is malformed");
    check_eq_str(tag, g.text, "malformed handshake payload", "malformed text");
    rows++;

    /* malformed, the OTHER text: proto+state fine, digest missing. */
    p.n = 0;
    put_str(&p, MIST_ARCH_TAG);
    put_str(&p, MIST_PLATFORM_TAG);
    put_str(&p, TEST_BUILD_HASH);
    put_u8(&p, MIST_PROTO_VER);
    put_u16be(&p, local_state);
    check_eq_int(tag, CLASSIFY(tag, &p), MIST_REJECT_MALFORMED,
                 "a v3 frame with no digest is malformed");
    check_eq_str(tag, g.text, "malformed handshake payload (no balance digest)",
                 "the no-digest text is distinguishable from the string one");
    rows++;

    check_eq_int(tag, rows, 19, "every reason-matrix row ran");
}

/* ====================================================================== */
/* 4. Check ORDER — "ORDER IS LOAD-BEARING" (mist_handshake.c:305).       */
/* ====================================================================== */

static void test4_precedence(void) {
    const char* tag = "order";
    tests_run++;
    Guarded g;
    Pay p;
    const uint16_t local_state = mist_handshake_local_state_ver();

    /* arch before platform. */
    build_ref(&p, "armv8", "linux", TEST_BUILD_HASH,
              MIST_PROTO_VER, local_state, LOCAL_DIGEST);
    check_eq_int(tag, CLASSIFY(tag, &p), MIST_REJECT_ARCH_MISMATCH,
                 "arch is reported before platform");

    /* the strings before the version fields: a peer with the wrong arch
     * AND a truncated payload is an arch mismatch, not a legacy build —
     * because arch is what a user can act on. */
    p.n = 0;
    put_str(&p, "x86_64");
    put_str(&p, MIST_PLATFORM_TAG);
    put_str(&p, TEST_BUILD_HASH);
    check_eq_int(tag, CLASSIFY(tag, &p), MIST_REJECT_ARCH_MISMATCH,
                 "a legacy frame with a wrong arch reports the arch");

    /* platform before the version fields. */
    build_ref(&p, MIST_ARCH_TAG, "linux", TEST_BUILD_HASH,
              (uint8_t)(MIST_PROTO_VER + 1), (uint16_t)(local_state + 1),
              LOCAL_DIGEST ^ UINT64_C(0xFF));
    check_eq_int(tag, CLASSIFY(tag, &p), MIST_REJECT_PLATFORM_MISMATCH,
                 "platform is reported before proto/state/digest");

    /* proto before state. */
    build_ref(&p, MIST_ARCH_TAG, MIST_PLATFORM_TAG, TEST_BUILD_HASH,
              2, (uint16_t)(local_state + 1), LOCAL_DIGEST);
    check_eq_int(tag, CLASSIFY(tag, &p), MIST_REJECT_PROTO_MISMATCH,
                 "proto is reported before state");

    /* proto before the digest is even READ — the case the source comment
     * calls out by name. A v1 peer's payload has no digest field at all,
     * so if the digest were read first this frame would be reported as
     * malformed ("no balance digest"), a message about a field the
     * peer's build never sent. */
    p.n = 0;
    put_str(&p, MIST_ARCH_TAG);
    put_str(&p, MIST_PLATFORM_TAG);
    put_str(&p, TEST_BUILD_HASH);
    put_u8(&p, 1);                    /* v1 */
    put_u16be(&p, local_state);       /* v1 carried state_ver, not a digest */
    check_eq_int(tag, CLASSIFY(tag, &p), MIST_REJECT_PROTO_MISMATCH,
                 "a v1 peer is told to update, not told its frame is malformed");
    check_eq_str(tag, g.text, "Handshake v1 vs v5 - update one side",
                 "and the message is the actionable one");

    /* state before the digest. */
    build_ref(&p, MIST_ARCH_TAG, MIST_PLATFORM_TAG, TEST_BUILD_HASH,
              MIST_PROTO_VER, (uint16_t)(local_state + 1),
              LOCAL_DIGEST ^ UINT64_C(0x5555));
    check_eq_int(tag, CLASSIFY(tag, &p), MIST_REJECT_STATE_MISMATCH,
                 "state is reported before the digest");

    /* the digest before the build_hash warning: a digest mismatch must
     * still REJECT even when everything else including the hash agrees. */
    build_ref(&p, MIST_ARCH_TAG, MIST_PLATFORM_TAG, TEST_BUILD_HASH,
              MIST_PROTO_VER, local_state, LOCAL_DIGEST + 1);
    check_eq_int(tag, CLASSIFY(tag, &p), MIST_REJECT_BALANCE_MISMATCH,
                 "the digest rejects regardless of the build hash");
}

/* ====================================================================== */
/* 5. String handling: read_cstr through the classifier.                  */
/* ====================================================================== */

static void test5_strings(void) {
    const char* tag = "strings";
    tests_run++;
    Guarded g;
    Pay p;
    const uint16_t local_state = mist_handshake_local_state_ver();

    /* Zero-length strings. Empty is a legal C string and must simply not
     * match the tag — not crash, not be treated as "absent". */
    build_ref(&p, "", MIST_PLATFORM_TAG, TEST_BUILD_HASH,
              MIST_PROTO_VER, local_state, LOCAL_DIGEST);
    check_eq_int(tag, CLASSIFY(tag, &p), MIST_REJECT_ARCH_MISMATCH,
                 "an empty arch string is an arch mismatch");
    check_eq_str(tag, g.text, "arch mismatch ()", "and the text shows it empty");

    build_ref(&p, MIST_ARCH_TAG, "", TEST_BUILD_HASH,
              MIST_PROTO_VER, local_state, LOCAL_DIGEST);
    check_eq_int(tag, CLASSIFY(tag, &p), MIST_REJECT_PLATFORM_MISMATCH,
                 "an empty platform string is a platform mismatch");

    /* All three empty: still positional, so proto/state/digest are read
     * from the right place and the verdict is the arch mismatch. */
    build_ref(&p, "", "", "", MIST_PROTO_VER, local_state, LOCAL_DIGEST);
    check_eq_int(tag, (long)p.n, 3 + 11, "three empty strings are three NULs");
    check_eq_int(tag, CLASSIFY(tag, &p), MIST_REJECT_ARCH_MISMATCH,
                 "three empty strings still parse positionally");

    /* THE OVERSIZED-STRING CASE. read_cstr copies at most out_cap-1 = 31
     * bytes into classify_peer_payload's 32-byte buffers but must advance
     * *off past the WHOLE string. If it advanced by the truncated copy
     * length instead, every field after the long string would be read
     * from the wrong offset — and the symptom would be a mystery reject
     * (or, far worse, a mystery ACCEPT) for a peer whose build hash
     * happened to be long. Make the long field the build_hash, which is
     * warning-only, so the ONLY thing that can fail this case is the
     * offset arithmetic: a correct read_cstr accepts, a truncating one
     * reads proto_ver out of the middle of the string. */
    {
        char longhash[64];
        memset(longhash, 'h', 48);
        longhash[48] = '\0';
        build_ref(&p, MIST_ARCH_TAG, MIST_PLATFORM_TAG, longhash,
                  MIST_PROTO_VER, local_state, LOCAL_DIGEST);
        check_eq_int(tag, CLASSIFY(tag, &p), 0,
                     "a 48-char build_hash truncates on COPY but not on ADVANCE");
    }

    /* Same shape on the arch field: over-long arch rejects as an arch
     * mismatch, and the reason text carries the 31-char truncation
     * without overrunning either buffer (the canary in classify()). */
    {
        char longarch[64];
        memset(longarch, 'a', 48);
        longarch[48] = '\0';
        build_ref(&p, longarch, MIST_PLATFORM_TAG, TEST_BUILD_HASH,
                  MIST_PROTO_VER, local_state, LOCAL_DIGEST);
        check_eq_int(tag, CLASSIFY(tag, &p), MIST_REJECT_ARCH_MISMATCH,
                     "a 48-char arch is an arch mismatch, not an overflow");
        check_eq_int(tag, (long)strlen(g.text), (long)strlen("arch mismatch ()") + 31,
                     "the peer arch is truncated to 31 chars in the message");
    }

    /* The exact out_cap-1 boundary: 31 chars fit whole, 32 truncate. */
    {
        char s31[32], s32[33];
        memset(s31, 'q', 31); s31[31] = '\0';
        memset(s32, 'q', 32); s32[32] = '\0';
        build_ref(&p, s31, MIST_PLATFORM_TAG, TEST_BUILD_HASH,
                  MIST_PROTO_VER, local_state, LOCAL_DIGEST);
        (void)CLASSIFY(tag, &p);
        check_eq_int(tag, (long)strlen(g.text),
                     (long)strlen("arch mismatch ()") + 31, "31 chars fit exactly");
        build_ref(&p, s32, MIST_PLATFORM_TAG, TEST_BUILD_HASH,
                  MIST_PROTO_VER, local_state, LOCAL_DIGEST);
        (void)CLASSIFY(tag, &p);
        check_eq_int(tag, (long)strlen(g.text),
                     (long)strlen("arch mismatch ()") + 31, "32 chars truncate to 31");
    }

    /* A payload that is ALL NULs: three empty strings then zero version
     * fields. Reaches the arch compare, rejects, never reads past. */
    {
        Pay z;
        memset(&z, 0, sizeof(z));
        z.n = 64;
        check_eq_int(tag, CLASSIFY(tag, &z), MIST_REJECT_ARCH_MISMATCH,
                     "an all-zero payload is an arch mismatch");
    }

    /* A payload that is ALL 0xFF: no NUL anywhere -> malformed, and
     * critically NOT an out-of-bounds scan. */
    {
        Pay f;
        memset(f.b, 0xFF, sizeof(f.b));
        f.n = MIST_PAYLOAD_MAX;
        check_eq_int(tag, CLASSIFY(tag, &f), MIST_REJECT_MALFORMED,
                     "a payload with no NUL byte at all is malformed");
    }
}

/* ====================================================================== */
/* 6. parse_header — the frame gate ahead of the classifier.              */
/* ====================================================================== */

static void frame(uint8_t* out, uint8_t m0, uint8_t m1, uint8_t m2, uint8_t m3,
                  uint8_t type, uint16_t declared) {
    out[0] = m0; out[1] = m1; out[2] = m2; out[3] = m3;
    out[4] = type;
    out[5] = (uint8_t)(declared >> 8);
    out[6] = (uint8_t)declared;
}

static void test6_parse_header(void) {
    const char* tag = "header";
    tests_run++;
    uint8_t buf[MIST_FRAME_MAX + 8];
    uint8_t type = 0;
    size_t plen = 0;

    memset(buf, 0, sizeof(buf));
    frame(buf, MIST_MAGIC_B0, MIST_MAGIC_B1, MIST_MAGIC_B2, MIST_MAGIC_B3,
          MIST_MSG_HELLO, 4);

    /* Short frames: every length below the 7-byte header refuses. */
    int shorts = 0;
    for (size_t n = 0; n < MIST_HEADER_LEN; n++) {
        CHECK(tag, !mist_handshake_test_parse_header(buf, n, &type, &plen));
        shorts++;
    }
    check_eq_int(tag, shorts, MIST_HEADER_LEN, "every short length was tried");

    /* The happy case, and the out-parameters. */
    type = 0; plen = 999;
    CHECK(tag, mist_handshake_test_parse_header(buf, MIST_HEADER_LEN + 4, &type, &plen));
    check_eq_int(tag, type, MIST_MSG_HELLO, "msg_type comes back");
    check_eq_int(tag, (long)plen, 4, "payload_len comes back");

    /* Magic: each of the four bytes, flipped independently. Byte 0 is the
     * one the GekkoNet collision argument (mist_handshake.h:53-59) rests
     * on; the other three are what stop a coincidence. */
    {
        const uint8_t good[4] = { MIST_MAGIC_B0, MIST_MAGIC_B1,
                                  MIST_MAGIC_B2, MIST_MAGIC_B3 };
        int magic_cases = 0;
        for (int i = 0; i < 4; i++) {
            uint8_t m[4];
            memcpy(m, good, 4);
            m[i] ^= 0x01;
            frame(buf, m[0], m[1], m[2], m[3], MIST_MSG_HELLO, 4);
            CHECK(tag, !mist_handshake_test_parse_header(buf, MIST_HEADER_LEN + 4,
                                                         &type, &plen));
            magic_cases++;
        }
        check_eq_int(tag, magic_cases, 4, "all four magic bytes were tested");
        /* And the exported MIST_MAGIC array is the same four bytes the
         * parser demands — netplay.c and the probes classify on it. */
        check_eq_int(tag, memcmp(MIST_MAGIC, good, 4), 0,
                     "the exported magic matches what parse_header accepts");
    }

    /* msg_type: the three legal values pass, everything else drops. */
    {
        int accepted = 0, rejected = 0;
        for (int t = 0; t < 256; t++) {
            frame(buf, MIST_MAGIC_B0, MIST_MAGIC_B1, MIST_MAGIC_B2,
                  MIST_MAGIC_B3, (uint8_t)t, 4);
            const bool ok = mist_handshake_test_parse_header(
                buf, MIST_HEADER_LEN + 4, &type, &plen);
            const bool want = (t == MIST_MSG_HELLO || t == MIST_MSG_ACK ||
                               t == MIST_MSG_REJECT);
            if (ok != want) {
                checks_run++; fail_count++;
                fprintf(stderr, "[test_mist_compat_gate] FAIL: %s: msg_type "
                        "0x%02X: got %d, want %d\n", tag, t, (int)ok, (int)want);
            } else {
                checks_run++;
            }
            if (want) accepted++; else rejected++;
        }
        check_eq_int(tag, accepted, 3, "exactly three msg_type values are legal");
        check_eq_int(tag, rejected, 253, "the other 253 are dropped");
        /* Byte 0 of a live GekkoNet datagram is a PacketType in [1,7]
         * (mist_handshake.h:53-59). Those are not our magic, so they can
         * never be mistaken for a MIST frame. */
        for (int t = MIST_GEKKO_PACKET_TYPE_MIN; t <= MIST_GEKKO_PACKET_TYPE_MAX; t++) {
            uint8_t gk[MIST_HEADER_LEN + 4];
            memset(gk, 0, sizeof(gk));
            gk[0] = (uint8_t)t;
            CHECK(tag, !mist_handshake_test_parse_header(gk, sizeof(gk), &type, &plen));
        }
    }

    /* Declared length: the bound, and the big-endian order. */
    frame(buf, MIST_MAGIC_B0, MIST_MAGIC_B1, MIST_MAGIC_B2, MIST_MAGIC_B3,
          MIST_MSG_HELLO, MIST_PAYLOAD_MAX);
    CHECK(tag, mist_handshake_test_parse_header(buf, MIST_HEADER_LEN + MIST_PAYLOAD_MAX,
                                                &type, &plen));
    check_eq_int(tag, (long)plen, MIST_PAYLOAD_MAX, "the maximum payload is accepted");
    frame(buf, MIST_MAGIC_B0, MIST_MAGIC_B1, MIST_MAGIC_B2, MIST_MAGIC_B3,
          MIST_MSG_HELLO, MIST_PAYLOAD_MAX + 1);
    CHECK(tag, !mist_handshake_test_parse_header(buf, sizeof(buf), &type, &plen));

    /* THE TRUNCATION ATTACK: a header that declares more payload than the
     * datagram carries. Accepting it would hand classify_peer_payload a
     * length that runs past the received bytes — the readers bound
     * themselves by the DECLARED length, so this check in parse_header is
     * what ties the declared length to reality. */
    frame(buf, MIST_MAGIC_B0, MIST_MAGIC_B1, MIST_MAGIC_B2, MIST_MAGIC_B3,
          MIST_MSG_HELLO, 32);
    int lie_cases = 0;
    for (size_t got = MIST_HEADER_LEN; got < MIST_HEADER_LEN + 32; got++) {
        CHECK(tag, !mist_handshake_test_parse_header(buf, got, &type, &plen));
        lie_cases++;
    }
    check_eq_int(tag, lie_cases, 32, "every short-by-N truncation was tried");
    CHECK(tag, mist_handshake_test_parse_header(buf, MIST_HEADER_LEN + 32, &type, &plen));

    /* A frame carrying MORE bytes than it declares parses, reporting the
     * declared length: trailing bytes are simply not payload. */
    plen = 999;
    CHECK(tag, mist_handshake_test_parse_header(buf, MIST_HEADER_LEN + 64, &type, &plen));
    check_eq_int(tag, (long)plen, 32, "the declared length wins over the received one");

    /* Big-endian: buf[5] is the HIGH byte. 0x0001 is 1, not 256. */
    buf[5] = 0x00; buf[6] = 0x01;
    CHECK(tag, mist_handshake_test_parse_header(buf, MIST_HEADER_LEN + 1, &type, &plen));
    check_eq_int(tag, (long)plen, 1, "0x00,0x01 is one byte");
    buf[5] = 0x01; buf[6] = 0x00;
    CHECK(tag, !mist_handshake_test_parse_header(buf, sizeof(buf), &type, &plen));
}

/* ====================================================================== */
/* 7. The fixed-width readers, driven directly.                           */
/* ====================================================================== */

static void test7_readers(void) {
    const char* tag = "readers";
    tests_run++;

    static const uint8_t data[] = {
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
        0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10,
    };

    /* Byte ORDER. A silent endianness flip in read_u16be would move
     * state_ver by a factor of 256 and reject every peer; in read_u64be it
     * would reject every peer on the digest. Both are "netplay stopped
     * working" with no other symptom. */
    {
        size_t off = 0;
        uint8_t u8v = 0;
        uint16_t u16v = 0;
        uint64_t u64v = 0;
        CHECK(tag, mist_handshake_test_read_u8(data, sizeof(data), &off, &u8v));
        check_eq_int(tag, u8v, 0x01, "read_u8 takes the byte at *off");
        check_eq_int(tag, (long)off, 1, "and advances by one");
        CHECK(tag, mist_handshake_test_read_u16be(data, sizeof(data), &off, &u16v));
        check_eq_int(tag, u16v, 0x0203, "read_u16be is big-endian");
        check_eq_int(tag, (long)off, 3, "and advances by two");
        CHECK(tag, mist_handshake_test_read_u64be(data, sizeof(data), &off, &u64v));
        CHECK(tag, u64v == UINT64_C(0x0405060708090A0B));
        checks_run++;
        check_eq_int(tag, (long)off, 11, "and advances by eight");
    }

    /* EXACT-FIT boundaries. Each reader must succeed when the field ends
     * precisely at payload_len and fail one byte earlier — the classic
     * off-by-one that turns a legal short frame into an out-of-bounds
     * read, or a legal frame into a spurious reject. */
    {
        size_t off;
        uint8_t u8v; uint16_t u16v; uint64_t u64v;

        off = 7;  CHECK(tag, mist_handshake_test_read_u8(data, 8, &off, &u8v));
        off = 8;  CHECK(tag, !mist_handshake_test_read_u8(data, 8, &off, &u8v));
        check_eq_int(tag, (long)off, 8, "a refused read does not advance *off");

        off = 6;  CHECK(tag, mist_handshake_test_read_u16be(data, 8, &off, &u16v));
        off = 7;  CHECK(tag, !mist_handshake_test_read_u16be(data, 8, &off, &u16v));

        off = 8;  CHECK(tag, mist_handshake_test_read_u64be(data, 16, &off, &u64v));
        off = 9;  CHECK(tag, !mist_handshake_test_read_u64be(data, 16, &off, &u64v));

        /* payload_len 0: nothing can be read at all. */
        off = 0;  CHECK(tag, !mist_handshake_test_read_u8(data, 0, &off, &u8v));
        off = 0;  CHECK(tag, !mist_handshake_test_read_u16be(data, 0, &off, &u16v));
        off = 0;  CHECK(tag, !mist_handshake_test_read_u64be(data, 0, &off, &u64v));

        /* A full sweep of every (off, payload_len) pair the classifier can
         * ever produce: off is only ever advanced by these readers and by
         * read_cstr, both of which keep it in [0, payload_len]. Outside
         * that invariant the `*off + n > payload_len` form would wrap on a
         * near-SIZE_MAX offset, which is why the invariant matters and why
         * it is swept here rather than assumed. */
        int pairs = 0;
        for (size_t len = 0; len <= 16; len++) {
            for (size_t o = 0; o <= len; o++) {
                size_t t;
                t = o; check_eq_int(tag,
                                    mist_handshake_test_read_u8(data, len, &t, &u8v),
                                    (o + 1 <= len), "read_u8 fits iff 1 byte remains");
                t = o; check_eq_int(tag,
                                    mist_handshake_test_read_u16be(data, len, &t, &u16v),
                                    (o + 2 <= len), "read_u16be fits iff 2 remain");
                t = o; check_eq_int(tag,
                                    mist_handshake_test_read_u64be(data, len, &t, &u64v),
                                    (o + 8 <= len), "read_u64be fits iff 8 remain");
                pairs++;
            }
        }
        check_eq_int(tag, pairs, 153, "every (off, len) pair in the invariant ran");
    }

    /* read_cstr directly: terminator handling, truncation, advance. */
    {
        static const uint8_t s[] = { 'a', 'b', 0, 'c', 'd', 'e', 0, 'f' };
        size_t off = 0;
        char out[4];

        CHECK(tag, mist_handshake_test_read_cstr(s, sizeof(s), &off, out, sizeof(out)));
        check_eq_str(tag, out, "ab", "the first string comes back");
        check_eq_int(tag, (long)off, 3, "*off lands past the NUL");
        CHECK(tag, mist_handshake_test_read_cstr(s, sizeof(s), &off, out, sizeof(out)));
        check_eq_str(tag, out, "cde", "the second string exactly fills out_cap-1");
        check_eq_int(tag, (long)off, 7, "*off lands past the second NUL");
        /* "f" has no terminator inside the payload. */
        CHECK(tag, !mist_handshake_test_read_cstr(s, sizeof(s), &off, out, sizeof(out)));

        /* Truncation into a small buffer: NUL-terminated, and *off still
         * advances past the WHOLE string. */
        off = 3;
        char tiny[2];
        CHECK(tag, mist_handshake_test_read_cstr(s, sizeof(s), &off, tiny, sizeof(tiny)));
        check_eq_str(tag, tiny, "c", "truncated to out_cap-1 and terminated");
        check_eq_int(tag, (long)off, 7, "but *off advanced past the whole string");

        /* out_cap 0 is refused rather than written to. */
        off = 0;
        CHECK(tag, !mist_handshake_test_read_cstr(s, sizeof(s), &off, out, 0));

        /* An empty string is a legal read of zero bytes. */
        static const uint8_t e[] = { 0, 'z', 0 };
        off = 0;
        CHECK(tag, mist_handshake_test_read_cstr(e, sizeof(e), &off, out, sizeof(out)));
        check_eq_str(tag, out, "", "an empty string reads as empty");
        check_eq_int(tag, (long)off, 1, "and advances one byte");

        /* payload_len 0. */
        off = 0;
        CHECK(tag, !mist_handshake_test_read_cstr(e, 0, &off, out, sizeof(out)));
    }
}

/* ====================================================================== */
/* 8. The balance digest.                                                 */
/* ====================================================================== */

static void test8_digest(void) {
    const char* tag = "digest";
    tests_run++;
    Guarded g;
    Pay p;
    const uint16_t local_state = mist_handshake_local_state_ver();

    check_eq_int(tag, (long)(mist_handshake_local_balance_digest() == LOCAL_DIGEST),
                 1, "the production setter installed the local digest");

    /* THE FULL 64 BITS. The reject TEXT folds the digest to 32 bits
     * (mist_handshake.c:372-374) for display. Pick a peer digest whose
     * fold is IDENTICAL to ours but whose value is not: if anyone ever
     * "simplified" the comparison to the displayed value, two different
     * ROM sets would connect and desync, and the log line would look
     * perfectly consistent. */
    {
        const uint64_t twin = LOCAL_DIGEST ^ UINT64_C(0x0000000100000001);
        const uint32_t fold_local = (uint32_t)(LOCAL_DIGEST >> 32) ^ (uint32_t)LOCAL_DIGEST;
        const uint32_t fold_twin  = (uint32_t)(twin >> 32) ^ (uint32_t)twin;
        CHECK(tag, twin != LOCAL_DIGEST);
        check_eq_int(tag, (long)fold_twin, (long)fold_local,
                     "the two digests are indistinguishable in the message");
        build_ref(&p, MIST_ARCH_TAG, MIST_PLATFORM_TAG, TEST_BUILD_HASH,
                  MIST_PROTO_VER, local_state, twin);
        check_eq_int(tag, CLASSIFY(tag, &p), MIST_REJECT_BALANCE_MISMATCH,
                     "a fold-colliding digest still rejects");
    }

    /* Every single-bit difference rejects, across all 64 positions. */
    {
        int bits = 0;
        for (int i = 0; i < 64; i++) {
            build_ref(&p, MIST_ARCH_TAG, MIST_PLATFORM_TAG, TEST_BUILD_HASH,
                      MIST_PROTO_VER, local_state,
                      LOCAL_DIGEST ^ (UINT64_C(1) << i));
            const uint8_t r = CLASSIFY(tag, &p);
            if (r != MIST_REJECT_BALANCE_MISMATCH) {
                checks_run++; fail_count++;
                fprintf(stderr, "[test_mist_compat_gate] FAIL: %s: digest bit "
                        "%d flipped: got reason %u\n", tag, i, (unsigned)r);
            } else {
                checks_run++;
            }
            bits++;
        }
        check_eq_int(tag, bits, 64, "all 64 digest bits were flipped");
    }

    /* WIRE BYTE ORDER. The digest is big-endian on the wire
     * (mist_handshake.h:29). A peer that byte-swapped it must be
     * rejected — otherwise two builds that disagree about the encoding
     * would connect and desync. */
    {
        uint64_t swapped = 0;
        for (int i = 0; i < 8; i++) {
            swapped |= ((LOCAL_DIGEST >> (8 * i)) & 0xFF) << (56 - 8 * i);
        }
        CHECK(tag, swapped != LOCAL_DIGEST);
        build_ref(&p, MIST_ARCH_TAG, MIST_PLATFORM_TAG, TEST_BUILD_HASH,
                  MIST_PROTO_VER, local_state, swapped);
        check_eq_int(tag, CLASSIFY(tag, &p), MIST_REJECT_BALANCE_MISMATCH,
                     "a byte-swapped digest is not our digest");
        /* ...and the bytes on the wire really are most-significant first:
         * the reference payload's first digest byte is the top octet. */
        build_good(&p);
        check_eq_int(tag, p.b[OFF_DIGEST], (long)(uint8_t)(LOCAL_DIGEST >> 56),
                     "the test's own encoder puts the MSB first");
        check_eq_int(tag, CLASSIFY(tag, &p), 0,
                     "and the classifier agrees with that encoding");
    }

    /* The unwired pair: two peers that never called the setter both
     * advertise 0 and must still match, which is what keeps every
     * digest-agnostic harness in this tree working. */
    mist_handshake_set_balance_digest(0);
    build_ref(&p, MIST_ARCH_TAG, MIST_PLATFORM_TAG, TEST_BUILD_HASH,
              MIST_PROTO_VER, local_state, 0);
    check_eq_int(tag, CLASSIFY(tag, &p), 0, "0 == 0 accepts");
    build_ref(&p, MIST_ARCH_TAG, MIST_PLATFORM_TAG, TEST_BUILD_HASH,
              MIST_PROTO_VER, local_state, 1);
    check_eq_int(tag, CLASSIFY(tag, &p), MIST_REJECT_BALANCE_MISMATCH,
                 "but an unwired local side still rejects a wired peer");
    mist_handshake_set_balance_digest(LOCAL_DIGEST);
    check_eq_int(tag, (long)(mist_handshake_local_balance_digest() == LOCAL_DIGEST),
                 1, "and the local digest is restored for later tests");
}

/* ====================================================================== */
/* 9. The reason string never overruns its buffer.                        */
/* ====================================================================== */

static void test9_text_bounds(void) {
    const char* tag = "text";
    tests_run++;
    Pay p;
    const uint16_t local_state = mist_handshake_local_state_ver();

    /* Every reject path, through progressively smaller text buffers,
     * including 1 and 0. The reason string is the only attacker-influenced
     * value classify_peer_payload writes (it interpolates the peer's own
     * arch/platform strings), so this is where a %s of a 31-char peer tag
     * into a short overlay buffer would land. */
    static const size_t caps[] = { 0, 1, 2, 8, 16, 32, 64 };

    Pay cases[6];
    /* arch mismatch, with a maximal peer tag. */
    {
        char longarch[64];
        memset(longarch, 'A', 48);
        longarch[48] = '\0';
        build_ref(&cases[0], longarch, MIST_PLATFORM_TAG, TEST_BUILD_HASH,
                  MIST_PROTO_VER, local_state, LOCAL_DIGEST);
        char longplat[64];
        memset(longplat, 'P', 48);
        longplat[48] = '\0';
        build_ref(&cases[1], MIST_ARCH_TAG, longplat, TEST_BUILD_HASH,
                  MIST_PROTO_VER, local_state, LOCAL_DIGEST);
    }
    build_ref(&cases[2], MIST_ARCH_TAG, MIST_PLATFORM_TAG, TEST_BUILD_HASH,
              (uint8_t)(MIST_PROTO_VER + 1), local_state, LOCAL_DIGEST);
    build_ref(&cases[3], MIST_ARCH_TAG, MIST_PLATFORM_TAG, TEST_BUILD_HASH,
              MIST_PROTO_VER, (uint16_t)(local_state + 1), LOCAL_DIGEST);
    build_ref(&cases[4], MIST_ARCH_TAG, MIST_PLATFORM_TAG, TEST_BUILD_HASH,
              MIST_PROTO_VER, local_state, LOCAL_DIGEST ^ UINT64_C(0xDEAD));
    cases[5].n = 0;
    put_bytes(&cases[5], "armv7", 5); /* unterminated -> malformed */

    int combos = 0;
    for (size_t ci = 0; ci < sizeof(cases) / sizeof(cases[0]); ci++) {
        for (size_t k = 0; k < sizeof(caps) / sizeof(caps[0]); k++) {
            Guarded g;
            const uint8_t r = classify(tag, &cases[ci], cases[ci].n, &g, caps[k]);
            CHECK(tag, r != 0);
            if (caps[k] > 0) {
                /* snprintf always terminates within the cap it was given. */
                CHECK(tag, memchr(g.text, '\0', caps[k]) != NULL);
            } else {
                /* text_cap 0: nothing may be written at all — which the
                 * canary at offset 0 already proved; assert the verdict
                 * still came back so the case is not a silent no-op. */
                CHECK(tag, r == MIST_REJECT_ARCH_MISMATCH ||
                           r == MIST_REJECT_PLATFORM_MISMATCH ||
                           r == MIST_REJECT_PROTO_MISMATCH ||
                           r == MIST_REJECT_STATE_MISMATCH ||
                           r == MIST_REJECT_BALANCE_MISMATCH ||
                           r == MIST_REJECT_MALFORMED);
            }
            combos++;
        }
    }
    check_eq_int(tag, combos,
                 (int)((sizeof(cases) / sizeof(cases[0])) *
                       (sizeof(caps) / sizeof(caps[0]))),
                 "every (reject path x text_cap) combination ran");

    /* A NULL payload with a nonzero length would be a caller bug, but the
     * degenerate len==0 case is reachable (a header declaring zero
     * payload) and must classify, not crash. */
    {
        Guarded g;
        p.n = 0;
        check_eq_int(tag, classify(tag, &p, 0, &g, sizeof(g.text)),
                     MIST_REJECT_MALFORMED, "a zero-length payload is malformed");
    }
}

/* ====================================================================== */

int Netplay_Test_MistCompatGate(void) {
    /* The local side of the digest comparison, installed once through the
     * production setter. Every test below assumes it. */
    mist_handshake_set_balance_digest(LOCAL_DIGEST);

    test1_accept();
    test2_truncation_sweep();
    test3_reason_matrix();
    test4_precedence();
    test5_strings();
    test6_parse_header();
    test7_readers();
    test8_digest();
    test9_text_bounds();

    /* --- the anti-vacuous gates, evaluated LAST. Hard failures, not
     * warnings: a harness reporting "0 failures" after running nothing is
     * exactly the shape these exist to make impossible. */
    if (tests_run != EXPECTED_TESTS) {
        fprintf(stderr,
                "[test_mist_compat_gate] COVERAGE FAIL: ran %d test(s), expected "
                "exactly %d — a test was removed, skipped, or never registered\n",
                tests_run, EXPECTED_TESTS);
        fail_count++;
    }
    if (checks_run < EXPECTED_MIN_CHECKS) {
        fprintf(stderr,
                "[test_mist_compat_gate] COVERAGE FAIL: only %d assertion(s) ran, "
                "floor is %d — the bodies are not executing\n",
                checks_run, EXPECTED_MIN_CHECKS);
        fail_count++;
    }

    fprintf(stderr,
            "[test_mist_compat_gate] summary: %d test(s), %d assertion(s), "
            "%d failure(s)\n",
            tests_run, checks_run, fail_count);
    if (fail_count == 0) {
        fprintf(stderr, "[test_mist_compat_gate] OK\n");
    }
    return fail_count == 0 ? 0 : 1;
}

#else /* !ENABLE_NETPLAY_TESTS */

int Netplay_Test_MistCompatGate(void) {
    /* "not compiled in", spelled exactly that way: tools/gates/run-gates.sh
     * greps for that phrase so an exit-2 misbuild is recorded RED instead
     * of being mistaken for a pass. */
    fprintf(stderr,
            "[test_mist_compat_gate] not compiled in; rebuild with "
            "EXTRA_CMAKE_ARGS=\"-DENABLE_NETPLAY=ON "
            "-DCMAKE_C_FLAGS=-DENABLE_NETPLAY_TESTS\".\n");
    return 2;
}

#endif /* ENABLE_NETPLAY_TESTS */
