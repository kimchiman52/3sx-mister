/**
 * @file test_ui_text.c
 * Unit harness for the proportional-font word-wrap, `sc_sub.c` ->
 * `SSWrapStrPro()` (budgets and the audit behind them: docs/ui-text-width.md).
 *
 * WHY IT EXISTS
 * =============
 * The wrap had no test anywhere. Measured by mutation on 2026-09-05: making
 * `SSWrapStrPro()` return 0 without filling a single line broke NOTHING in the
 * tree -- not a gate, not a harness, not a build. Its two consumers cannot
 * catch that on their own. `sc_sub.c` -> `SSPutStrProWrapP()` just draws
 * `drawn = 0` lines, i.e. silently nothing, and `direct_p2p_overlay.c` ->
 * `dp2p_overlay_log_layout()` only logs. Both failures are invisible outside a
 * screenshot of a netplay refusal nobody is looking at.
 *
 * So the contract is asserted here instead, against the SAME metric the draw
 * path advances by (`SSGetDrawSizePro`) rather than a re-derivation of the
 * `ascProData` arithmetic -- a second copy of that arithmetic would agree with
 * a broken wrap for exactly the reasons the wrap would be broken.
 *
 * WHAT IT PROVES
 *   1. A string that fits is one line, that line is the whole string, and its
 *      reported width is the measured width.
 *   2. Every line a multi-line wrap produces is inside the budget, non-empty,
 *      and free of leading/trailing spaces -- and the lines put the source's
 *      words back in order with nothing dropped or duplicated.
 *   3. A budget too small to hold the text is reported by a RETURN VALUE
 *      greater than max_lines, and nothing is written past max_lines. That
 *      return is what SSPutStrProWrapP's "..." marker depends on.
 *   4. A single word wider than the budget is hard-split rather than dropped,
 *      and the split always advances (a zero-length line is an infinite loop).
 *   5. Degenerate inputs -- NULL, empty, all-spaces -- produce zero lines and
 *      no writes.
 *
 * Pure: no SDL window, no ROM, no engine global. `ascProData` is a const
 * table.
 */

#include <stdio.h>
#include <string.h>

int UiText_Test_Units(void);

#ifndef ENABLE_NETPLAY_TESTS

int UiText_Test_Units(void) {
    fprintf(stderr,
            "[test_ui_text] not compiled in; rebuild with "
            "-DCMAKE_C_FLAGS=-DENABLE_NETPLAY_TESTS.\n");
    return 2;
}

#else

#include "sf33rd/Source/Game/ui/sc_sub.h"

/* Deliberately a literal (house rule, see test_texcash_bounds.c): if a
 * sub-test is skipped or removed, coverage fails loudly instead of reporting
 * a smaller green run. */
#define EXPECTED_SUBTESTS 5

static int g_ran;
static int g_fail;

#define CHECK(cond, ...)                                                                                               \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            g_fail++;                                                                                                  \
            fprintf(stderr, "[test_ui_text] FAIL %s:%d: ", __FILE__, __LINE__);                                        \
            fprintf(stderr, __VA_ARGS__);                                                                              \
            fprintf(stderr, "\n");                                                                                     \
        }                                                                                                              \
    } while (0)

/* The canary that catches a wrap which reports lines it never wrote. Filled
 * into every slot before the call; a slot the wrap claims must not still
 * carry it. */
#define UNWRITTEN 0xBEEF

static void fill_canary(SSProLine* lines, int n) {
    int i;

    for (i = 0; i < n; i++) {
        lines[i].off = UNWRITTEN;
        lines[i].len = UNWRITTEN;
        lines[i].width = -1;
    }
}

static int line_is_canary(const SSProLine* l) {
    return l->off == UNWRITTEN && l->len == UNWRITTEN && l->width == -1;
}

/* Measured width of str[off..off+len), through the draw path's own metric. */
static s32 measure(const char* str, u16 off, u16 len) {
    char buf[SS_PRO_LINE_MAX + 1];

    if (len > SS_PRO_LINE_MAX) {
        len = SS_PRO_LINE_MAX;
    }

    memcpy(buf, str + off, len);
    buf[len] = '\0';
    return SSGetDrawSizePro((const s8*)buf);
}

/* Shared structural contract: in-budget, non-empty, no edge spaces, reported
 * width == measured width, and strictly forward-moving offsets. */
static void check_lines(const char* what, const char* str, u16 max_w, const SSProLine* lines, s32 count) {
    s32 i;
    u16 prev_end = 0;

    for (i = 0; i < count; i++) {
        const SSProLine* l = &lines[i];

        CHECK(!line_is_canary(l), "%s: line %d was never written", what, (int)i);
        CHECK(l->len > 0, "%s: line %d is empty", what, (int)i);
        CHECK(l->off + l->len <= (u16)strlen(str), "%s: line %d runs past the string", what, (int)i);
        CHECK(l->off >= prev_end, "%s: line %d offset %u goes backwards (previous ended at %u)", what, (int)i, l->off,
              prev_end);
        CHECK(str[l->off] != ' ', "%s: line %d starts on a space", what, (int)i);
        CHECK(str[l->off + l->len - 1] != ' ', "%s: line %d ends on a space", what, (int)i);
        CHECK(l->width == measure(str, l->off, l->len), "%s: line %d reports width %d, measures %d", what, (int)i,
              l->width, measure(str, l->off, l->len));
        CHECK(l->width <= (s32)max_w, "%s: line %d is %d px, budget is %u", what, (int)i, l->width, max_w);

        prev_end = (u16)(l->off + l->len);
    }
}

/* 1. A string inside the budget is exactly one line covering all of it. */
static void test_single_line(void) {
    static const char* str = "HOST";
    SSProLine lines[SS_PRO_WRAP_MAX_LINES];
    s32 n;

    g_ran++;
    fill_canary(lines, SS_PRO_WRAP_MAX_LINES);
    n = SSWrapStrPro(str, 300, lines, SS_PRO_WRAP_MAX_LINES);

    CHECK(n == 1, "a string that fits wrapped to %d lines, expected 1", (int)n);

    if (n != 1) {
        return;
    }

    CHECK(lines[0].off == 0, "single line starts at %u, expected 0", lines[0].off);
    CHECK(lines[0].len == (u16)strlen(str), "single line is %u glyphs, expected %u", lines[0].len,
          (unsigned)strlen(str));
    CHECK(lines[0].width == SSGetDrawSizePro((const s8*)str), "single line reports %d px, string measures %d px",
          lines[0].width, SSGetDrawSizePro((const s8*)str));
    check_lines("single-line", str, 300, lines, n);
}

/* 2. A real multi-line wrap: structure, plus word-for-word reconstruction. */
static void test_multi_line_keeps_every_word(void) {
    /* The shape the netplay refusal overlay actually carries. */
    static const char* str = "WAITING FOR THE OTHER PLAYER TO CONFIRM THE ROOM CODE";
    SSProLine lines[SS_PRO_WRAP_MAX_LINES];
    s32 n;
    s32 i;
    char rebuilt[256];
    size_t r = 0;

    g_ran++;
    fill_canary(lines, SS_PRO_WRAP_MAX_LINES);
    n = SSWrapStrPro(str, 96, lines, SS_PRO_WRAP_MAX_LINES);

    CHECK(n > 1, "a %d px string in a 96 px budget wrapped to %d line(s), expected more than 1",
          SSGetDrawSizePro((const s8*)str), (int)n);
    CHECK(n <= SS_PRO_WRAP_MAX_LINES, "wrapped to %d lines, more than the %d the caller offered", (int)n,
          SS_PRO_WRAP_MAX_LINES);

    if (n < 1 || n > SS_PRO_WRAP_MAX_LINES) {
        return;
    }

    check_lines("multi-line", str, 96, lines, n);

    /* Joining the lines with single spaces must give the source back with its
     * (single) spaces intact. This is the assertion that a wrap returning
     * nothing, or dropping the tail, cannot survive. */
    for (i = 0; i < n; i++) {
        if (r != 0 && r + 1 < sizeof(rebuilt)) {
            rebuilt[r++] = ' ';
        }

        if (r + lines[i].len < sizeof(rebuilt)) {
            memcpy(rebuilt + r, str + lines[i].off, lines[i].len);
            r += lines[i].len;
        }
    }

    rebuilt[r] = '\0';
    CHECK(strcmp(rebuilt, str) == 0, "rejoined lines are \"%s\", source is \"%s\"", rebuilt, str);
}

/* 3. Over budget: the RETURN reports the true need, and nothing is written
 *    past max_lines. SSPutStrProWrapP's "..." marker is driven by exactly
 *    this difference. */
static void test_reports_overflow_without_overrunning(void) {
    static const char* str = "WAITING FOR THE OTHER PLAYER TO CONFIRM THE ROOM CODE";
    SSProLine lines[SS_PRO_WRAP_MAX_LINES];
    const s32 budget_lines = 2;
    s32 n;
    s32 i;

    g_ran++;
    fill_canary(lines, SS_PRO_WRAP_MAX_LINES);
    n = SSWrapStrPro(str, 96, lines, budget_lines);

    CHECK(n > budget_lines, "wrap into %d lines returned %d; the caller cannot tell it lost text", (int)budget_lines,
          (int)n);

    /* Only the slots the wrap claims to have filled; a wrap that returned
     * fewer has already failed the CHECK above and re-reporting canaries as
     * bad lines just buries it. */
    check_lines("overflow", str, 96, lines, n < budget_lines ? n : budget_lines);

    for (i = budget_lines; i < SS_PRO_WRAP_MAX_LINES; i++) {
        CHECK(line_is_canary(&lines[i]), "slot %d was written past the caller's %d-line budget", (int)i,
              (int)budget_lines);
    }
}

/* 4. One word wider than the whole budget: hard-split, never dropped, and the
 *    walk always advances. */
static void test_hard_splits_an_oversized_word(void) {
    static const char* str = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
    SSProLine lines[SS_PRO_WRAP_MAX_LINES];
    s32 n;
    s32 i;
    u16 covered = 0;

    g_ran++;
    fill_canary(lines, SS_PRO_WRAP_MAX_LINES);
    n = SSWrapStrPro(str, 24, lines, SS_PRO_WRAP_MAX_LINES);

    CHECK(n > 1, "a single %d px word in a 24 px budget produced %d line(s)", SSGetDrawSizePro((const s8*)str),
          (int)n);

    if (n < 1 || n > SS_PRO_WRAP_MAX_LINES) {
        return;
    }

    check_lines("hard-split", str, 24, lines, n);

    /* No spaces in the source, so the pieces must tile it exactly. */
    for (i = 0; i < n; i++) {
        CHECK(lines[i].off == covered, "split piece %d starts at %u, expected %u", (int)i, lines[i].off, covered);
        covered = (u16)(lines[i].off + lines[i].len);
    }

    CHECK(covered == (u16)strlen(str), "split pieces cover %u of %u glyphs", covered, (unsigned)strlen(str));
}

/* 5. Degenerate inputs produce nothing and write nothing. */
static void test_degenerate_inputs(void) {
    SSProLine lines[SS_PRO_WRAP_MAX_LINES];
    s32 n;

    g_ran++;

    fill_canary(lines, SS_PRO_WRAP_MAX_LINES);
    n = SSWrapStrPro(NULL, 96, lines, SS_PRO_WRAP_MAX_LINES);
    CHECK(n == 0, "NULL wrapped to %d lines", (int)n);
    CHECK(line_is_canary(&lines[0]), "NULL wrote a line");

    fill_canary(lines, SS_PRO_WRAP_MAX_LINES);
    n = SSWrapStrPro("", 96, lines, SS_PRO_WRAP_MAX_LINES);
    CHECK(n == 0, "the empty string wrapped to %d lines", (int)n);
    CHECK(line_is_canary(&lines[0]), "the empty string wrote a line");

    fill_canary(lines, SS_PRO_WRAP_MAX_LINES);
    n = SSWrapStrPro("     ", 96, lines, SS_PRO_WRAP_MAX_LINES);
    CHECK(n == 0, "an all-spaces string wrapped to %d lines", (int)n);
    CHECK(line_is_canary(&lines[0]), "an all-spaces string wrote a line");
}

int UiText_Test_Units(void) {
    test_single_line();
    test_multi_line_keeps_every_word();
    test_reports_overflow_without_overrunning();
    test_hard_splits_an_oversized_word();
    test_degenerate_inputs();

    if (g_ran != EXPECTED_SUBTESTS) {
        fprintf(stderr, "[test_ui_text] FAIL coverage: ran %d sub-tests, expected %d\n", g_ran, EXPECTED_SUBTESTS);
        g_fail++;
    }

    if (g_fail != 0) {
        fprintf(stderr, "[test_ui_text] FAILED (%d check(s), %d sub-tests)\n", g_fail, g_ran);
        return 1;
    }

    printf("[test_ui_text] PASS (%d sub-tests)\n", g_ran);
    return 0;
}

#endif /* ENABLE_NETPLAY_TESTS */
