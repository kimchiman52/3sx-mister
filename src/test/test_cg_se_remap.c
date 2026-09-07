/**
 * @file test_cg_se_remap.c
 * Unit harness for the per-character cg_se sound-code remap (doc §8.Q / §21,
 * docs/research-arcade-cg-data-accuracy.md).
 *
 * cg_se was the second byte-passed namespace: read_char_table copied it
 * straight out of the CPS3 ROM while the audio samples are PS2 assets
 * (SF33RD.AFS), so six arcade codes resolved to the wrong -- or empty --
 * TSB note under arcade balance. remap_cg_se now translates exactly six
 * (character, code) pairs at parse time.
 *
 * What this harness proves (via the ArcadeCharData_TestRemapCgSe seam):
 *
 *   1. The six pairs map as specified, for ALL 16 values of the low
 *      flip/priority nibble, which must survive untouched (charset.c ->
 *      check_cgd_patdat does `wk->cg_se >>= 4;` before dispatch).
 *      check_cgd_patdat2 does the same shift but never dispatches a
 *      sound, so the dispatch this rests on is check_cgd_patdat's.
 *   2. The remap is keyed per character: a full sweep of every
 *      (character, code, nibble) triple finds exactly the six mappings and
 *      nothing else. In particular Urien's 0x2FB and Twelve's 0x3DF --
 *      legitimate voices that collide numerically with Alex's and Oro's
 *      wrong codes -- pass through unchanged (§21.8).
 *   3. The deliberate non-actions of §21.9 stay non-actions: Yang 0x27F,
 *      Yun 0x268 (mod-32 no-ops), Yang 0x269, Q 0x108, shoto 0x10A.
 *   4. No chaining: a pair's TARGET code is not itself remapped for the
 *      same character (Makoto 0x1DF is Yang's wrong code, not Makoto's).
 *
 * What it cannot prove: that exactly 29 ROM script cells carry the six
 * source codes. That is a property of rom.bin, asserted by
 * tools/arcade-audit/cg_se_audit.py against the regenerated ROM.
 */

#include <stdint.h>
#include <stdio.h>

#include "constants.h"

int CgSe_Test_Remap(void);

#ifndef ENABLE_NETPLAY_TESTS

int CgSe_Test_Remap(void) {
    fprintf(stderr,
            "[test_cg_se_remap] not compiled in; rebuild with "
            "-DCMAKE_C_FLAGS=-DENABLE_NETPLAY_TESTS.\n");
    return 2;
}

#else

#include "arcade/arcade_char_data.h"

/* Deliberately a literal (house rule, see test_texcash_bounds.c): if a
 * sub-test is skipped or removed, coverage fails loudly instead of
 * reporting a smaller green run. */
#define EXPECTED_SUBTESTS 4

static int g_ran;
static int g_fail;

#define CHECK(cond, ...)                                                                                               \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            g_fail++;                                                                                                  \
            printf("FAIL: ");                                                                                          \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
        }                                                                                                              \
    } while (0)

/* The six pairs of doc §21.7 -- upper-12-bit codes. This table is the
 * harness's own copy on purpose: it must disagree loudly if someone edits
 * cg_se_maps in arcade_char_data.c. */
typedef struct ExpectedPair {
    Character character;
    uint16_t from;
    uint16_t to;
} ExpectedPair;

static const ExpectedPair expected_pairs[] = {
    { CHAR_MAKOTO, 0x27E, 0x1DF }, /* Hayate voice: wrong line ("Hayate" vs "Chesuto") */
    { CHAR_ALEX, 0x2FB, 0x3BF },   /* silent today: TSB_PL01[27] is cmd=0 */
    { CHAR_ORO, 0x2F8, 0x25C },    /* silent today: TSB_PL09[24] is cmd=0 */
    { CHAR_ORO, 0x3DF, 0x25D },    /* wrong voice */
    { CHAR_YANG, 0x1DF, 0x29E },   /* wrong voice */
    { CHAR_AKUMA, 0x37E, 0x130 },  /* wrong class: voice note vs common SE (Se_Let) */
};

#define EXPECTED_PAIR_COUNT 6

/* 1. The six pairs map as specified, low nibble preserved, all 16 nibbles. */
static void sub_a_pairs_and_nibble(void) {
    g_ran++;

    for (int i = 0; i < EXPECTED_PAIR_COUNT; i++) {
        const ExpectedPair* e = &expected_pairs[i];

        for (uint16_t nib = 0; nib < 16; nib++) {
            const uint16_t raw = (uint16_t)((e->from << 4) | nib);
            const uint16_t want = (uint16_t)((e->to << 4) | nib);
            const uint16_t got = ArcadeCharData_TestRemapCgSe(raw, e->character);
            CHECK(got == want,
                  "pair %d char %d: 0x%04X -> 0x%04X, expected 0x%04X",
                  i,
                  (int)e->character,
                  raw,
                  got,
                  want);
        }
    }
}

/* 2. Full-domain sweep: exactly the six mappings exist, nothing else moves.
 *    This is the per-character-keying proof -- it covers Urien 0x2FB and
 *    Twelve 0x3DF as strict subsets, but asserts the whole domain. */
static void sub_b_domain_sweep(void) {
    g_ran++;
    int remapped = 0;

    for (int character = 0; character < NUM_CHARS; character++) {
        for (uint32_t code = 0; code < 0x1000; code++) {
            const uint16_t raw = (uint16_t)(code << 4);
            const uint16_t got = ArcadeCharData_TestRemapCgSe(raw, (Character)character);

            int expected_to = -1;

            for (int i = 0; i < EXPECTED_PAIR_COUNT; i++) {
                if (expected_pairs[i].character == (Character)character && expected_pairs[i].from == code) {
                    expected_to = expected_pairs[i].to;
                    break;
                }
            }

            if (expected_to >= 0) {
                remapped++;
                CHECK(got == (uint16_t)(expected_to << 4),
                      "char %d code 0x%03X: got 0x%04X, expected 0x%04X",
                      character,
                      (unsigned)code,
                      got,
                      (uint16_t)(expected_to << 4));
            } else {
                CHECK(got == raw, "char %d code 0x%03X moved: 0x%04X -> 0x%04X", character, (unsigned)code, raw, got);
            }
        }
    }

    CHECK(remapped == EXPECTED_PAIR_COUNT,
          "sweep hit %d mapped (character, code) pairs, expected exactly %d",
          remapped,
          EXPECTED_PAIR_COUNT);
}

/* 3. Deliberate non-actions (§21.9) -- each one a recorded decision. The
 *    domain sweep already covers these; asserting them by name means a
 *    future "fix" of one of them turns a NAMED line red, not a generic
 *    sweep line. */
static void sub_c_non_actions(void) {
    g_ran++;

    static const struct {
        Character character;
        uint16_t code;
        const char* why;
    } non_actions[] = {
        { CHAR_YANG, 0x27F, "mod-32 equivalent to PS2 0x29F -- no-op" },
        { CHAR_YUN, 0x268, "mod-32 equivalent to PS2 0x288 -- no-op" },
        { CHAR_YANG, 0x269, "PS2 silenced it; 0x269 is also used correctly elsewhere" },
        { CHAR_Q, 0x108, "Q's caca[4..7] silence is authentic arcade data (SE added by PS2)" },
        { CHAR_RYU, 0x10A, "shoto dmca[3] SE: valid common SE PS2 removed" },
        { CHAR_KEN, 0x10A, "shoto dmca[3] SE: valid common SE PS2 removed" },
        { CHAR_SEAN, 0x10A, "shoto dmca[3] SE: valid common SE PS2 removed" },
        { CHAR_AKUMA, 0x10A, "shoto dmca[3] SE: valid common SE PS2 removed" },
        { CHAR_URIEN, 0x2FB, "legitimate Urien voice; only ALEX's 0x2FB is wrong" },
        { CHAR_TWELVE, 0x3DF, "legitimate Twelve voice; only ORO's 0x3DF is wrong" },
    };

    for (size_t i = 0; i < sizeof(non_actions) / sizeof(non_actions[0]); i++) {
        const uint16_t raw = (uint16_t)(non_actions[i].code << 4);
        const uint16_t got = ArcadeCharData_TestRemapCgSe(raw, non_actions[i].character);
        CHECK(got == raw,
              "non-action char %d code 0x%03X was remapped to 0x%04X (%s)",
              (int)non_actions[i].character,
              non_actions[i].code,
              got,
              non_actions[i].why);
    }
}

/* 4. No chaining: each pair's target passes through unchanged for the same
 *    character (single-pass remap; Makoto's target 0x1DF doubles as Yang's
 *    source, so this is not vacuous). */
static void sub_d_no_chaining(void) {
    g_ran++;

    for (int i = 0; i < EXPECTED_PAIR_COUNT; i++) {
        const ExpectedPair* e = &expected_pairs[i];
        const uint16_t raw = (uint16_t)(e->to << 4);
        const uint16_t got = ArcadeCharData_TestRemapCgSe(raw, e->character);
        CHECK(got == raw, "pair %d char %d target 0x%03X chained to 0x%04X", i, (int)e->character, e->to, got);
    }
}

int CgSe_Test_Remap(void) {
    printf("=== cg_se sound-code remap harness (doc item Q, §21) ===\n");

    sub_a_pairs_and_nibble();
    sub_b_domain_sweep();
    sub_c_non_actions();
    sub_d_no_chaining();

    if (g_ran != EXPECTED_SUBTESTS) {
        printf("COVERAGE FAIL: ran %d sub-test(s), expected exactly %d\n", g_ran, EXPECTED_SUBTESTS);
        return 2;
    }

    if (g_fail != 0) {
        printf("RESULT: FAIL (%d assertion(s))\n", g_fail);
        return 1;
    }

    printf("RESULT: PASS (%d sub-tests; %d pairs x 16 nibbles, %d x 0x1000-code domain sweep)\n",
           g_ran,
           EXPECTED_PAIR_COUNT,
           NUM_CHARS);
    return 0;
}

#endif /* ENABLE_NETPLAY_TESTS */
