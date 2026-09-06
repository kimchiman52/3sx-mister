# Fixture: the six citation defects found by hand on 2026-08-24

NOT A REAL DOCUMENT. Every citation below is deliberately broken. This file is
excluded from the default scan (SKIP_SCAN_DIRS) and is reached only by
`--include-testdata`, which is how test_doc_citations.py drives it.

These six are the linter's acceptance set. They were all found by a human
reading carefully; the tool exists to find the next six mechanically. If a
change to the linter stops it catching any of them, the test goes red.

Each case below is annotated with the finding code it must produce.

## 1 -- a symbol asserted by a comment and existing nowhere  [phantom-identifier]

The texture cache teardown path here mirrors `kill_texcash_work`, which
releases the same slots in the same order.

## 2 -- evidence cited to a document that never existed  [unresolvable-evidence]

Every assertion was proven able to go red by neutralising the code under test
and observing the failure (19 neutralisations; see the S7 task report for the
patch-to-red table).

## 3 -- a line citation that drifted when lines were inserted above it  [drift]

| Symbol | Declared |
| --- | --- |
| `spmv_ng_save[2]` (u32) | `sf33rd/Source/Game/effect/effl8.c:11` |

## 4 -- a comment block whose own patch moved the lines it cites  [degenerate-target]

Reconstructed from the real comment now at texcash.c:506-510, with the line
numbers put back to their pre-shift values (the patch that wrote the comment
moved its own targets down by 60):

    MOST consumers tolerate that -- aboutspr.c:265,
    texcash.c:301/516/554 and PPGWork.c:177 all skip on
    be == 0.

## 5 -- a purge ordering cited at a lone closing brace  [degenerate-target]

Reconstructed from the real comment now at texgroup.c:284-287, with the
citation put back to the value it drifted from:

    The nested purge is bounded: purge_texture_group clears ok
    before calling Push_ramcnt_key (texgroup.c:440-443), so the
    purge_texture_group(group_num) re-entry inside
    Push_ramcnt_key_original_2 (ramcnt.c:102) sees ok == 0.

## 6 -- a path that has never existed at that location  [wrong-path]

Desktop dependencies are installed by `tools/mister/build-deps.sh`.

## 7 -- a bare line citation into an anchor-required file  [unanchored-citation]

Task #110. This is the shape that was invisible before anchor-required.txt
existed: a line number into a dense macro table, with prose that names nothing
the checker can test. It must be reported, not silently accepted.

    The rollback save set pins this value; see
    src/netplay/game_state.c:580.

(Deliberately anchor-free. If a future edit adds a symbol name to the sentence
above, this case stops testing what it is for -- the citation would then be
checked by the ordinary drift path instead.)

## 8 -- a symbol deleted from a file that still exists  [stale-identifier]

Real history, immutable: `RELAY_REQ` was the S5 relay's request message. It
was code in src/netplay/direct_p2p.c, rendezvous.h and
tools/rendezvous-server/rendezvous-server.js, and commit 2c63adc7 ("remove the
S5 relay entirely") deleted it from all of them while every one of those files
lived on. Before the fix this was reported as `phantom-identifier` with the
text "has never existed", because the history corpus skipped every path still
in the tree. It must be reported as stale, naming that commit.
