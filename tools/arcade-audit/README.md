# Arcade (CPS3) char-data audit

Static audit of the arcade-vs-PS2 character-data mapping. Decodes every arcade
script cell for all 20 characters out of the decrypted CPS3 ROM, pairs each
against its PS2 counterpart from `SF33RD.AFS`, and reports every cell whose
remapped CG number is out of bounds, lands in a table gap, or disagrees with
PS2.

This is the tooling behind the root cause of upstream issue #363 ("Ryu's Denjin
Hadoken crashes the game"). **Full findings, worklist and repro recipe:**
[`docs/research-arcade-cg-data-accuracy.md`](../../docs/research-arcade-cg-data-accuracy.md).

## Run it

```sh
python3 tools/arcade-audit/cg_audit.py        # ~40 s -> cg_audit.json + table
python3 tools/arcade-audit/data_audit.py      # ~1 s  -> data_audit.json + table
python3 tools/arcade-audit/residual_audit.py  # ~1 s  -> residual_audit.json + report
python3 tools/arcade-audit/counterfactual.py  # re-runs with historical fixes reverted
python3 tools/arcade-audit/cg_se_audit.py     # ~1 s  -> cg_se_audit.json + table
```

`cg_se_audit.py` covers the **sound-code remap** (doc item Q, §21): it parses
`cg_se_maps[]` out of `src/arcade/arcade_char_data.c` and asserts against the
ROM that the per-character `cg_se` exceptions touch exactly the six pairs / 29
script cells of §21.7 and nothing else. It imports `cg_audit.py` for the
shared constants and does not modify it. Its invariant:

```
TOTAL script cells 29 (expected 29), distinct buffer positions 17 (expected 17)
PASS
```

`cg_audit.py` covers sprite indices in the 10 script tables (plus OVCT/OVIX
coverage). `data_audit.py` covers **the other 13 sections** — STXY MVXY SERND
RICT HIIT BODA HANA CATA CAUA ATTA HOSA ATIT PROT — where hitboxes, throw
placement and attack properties live (upstream issue #325). It imports
`cg_audit.py` for the shared constants and does not modify it. Its invariant:

```
bounds hazards ... verdict ARCADE_ONLY: 0
```

None of those 13 sections is translated, so an arcade-vs-PS2 content difference
there is the balance change itself, not a defect. See the doc, §15.

`residual_audit.py` covers the **second door**: `cg_audit.py` only checks the
raw `cg_number` against `obj_group_table`'s bound. Past that, the renderer
computes an unchecked residual into the resolved group's own offset table
(`n -= texgrpdat[i].num_of_1st`) and dereferences it — a second, larger index
that `cg_audit.py` never checks. It re-derives every group's offset-table
length statically from `SF33RD.AFS`, bounds-checks the residual for all
133,901 cells plus the OVCT `parts_char` path, and derives which texture
groups are reachable from `ldreq_tbl[]`/`ldreq_ix[]`. It imports `cg_audit.py`
for shared constants and does not modify it. Its invariant:

```
residual < 0                    : 0
residual >= offset-table length : 0
```

**That invariant covers the script-cell residual only.** The same script also
runs a separate bounds check over the OVCT `parts_char -> cg_number` path
(`eff01.c:169`, printed as "R2b" in its output) and, as of this writing,
reports **6** post-adaptation violations there — all Elena parts 85-90. That
is doc worklist item **B** (§18) and it is still **OPEN**; "0/0" on the two
assertions above does not mean every out-of-bounds door in this tool is shut.
See the doc, §17.

Expected at the time of writing (`fix/arcade-cg-mapping` branch point):

```
TOTAL | (a) 66  (b) 0  (c)wg 949  (c)og 745  manu 316
cells audited: 133901
```

`(a)` is the crash class — remapped CG >= 37664, past the end of
`obj_group_table` (`src/sf33rd/Source/Game/rendering/chren3rd.c:8`). All 66 are
Elena's; see the doc, §8.A. **A fix for that item should drive `(a)` to 0.**

## Inputs

`rom.bin` is **gitignored** (8 MiB decrypted ROM derivative). Rebuild it with:

```sh
python3 tools/arcade-audit/decrypt.py   # prints SIMM sha256s; compare to rom_load.c:41-45
```

Paths resolve automatically — the repo root is derived from this directory, so
it works from any worktree. Override with env vars if needed:

| Var | Default |
|---|---|
| `ARCADE_AUDIT_REPO` | repo root (derived from this file's location) |
| `ARCADE_AUDIT_AFS`  | `~/Library/Application Support/CrowdedStreet/3S-ARM/resources/SF33RD.AFS` |
| `ARCADE_AUDIT_ROM`  | `rom.bin` beside these scripts |
| `ARCADE_AUDIT_ROMZIP` | `~/Library/Application Support/CrowdedStreet/3S-ARM/resources/sfiii3nr1.zip` |

Note the `3SX/` (not `3S-ARM/`) copy of `SF33RD.AFS` is a dangling symlink — do
not point at it.

## Constants are parsed from source, not hand-copied

`cg_audit.py` reads `location_data[]`, `cg_maps[]` and `remap_cg_number` out of
`src/arcade/arcade_char_data.c`, `texgrpdat[]` out of `texgroup.c`,
`obj_group_table` out of `chren3rd.c`, and the effect/sound/command table sizes
out of their own sources at run time. Editing those tables changes the audit's
answer automatically — which is the point.

## Files

| File | Purpose |
|---|---|
| `cg_audit.py` | the CG audit; writes `cg_audit.json` |
| `data_audit.py` | the 13-section audit; writes `data_audit.json` |
| `residual_audit.py` | the second-door residual-bounds audit; writes `residual_audit.json` |
| `counterfactual.py` | reverts #290/#359/#360 in-memory to prove the audit catches them |
| `cg_se_audit.py` | the cg_se sound-code remap audit (doc item Q); writes `cg_se_audit.json` |
| `decrypt.py` | rebuilds `rom.bin` from the ROM zip |
| `afs.py` | AFS container parser |
| `parse.py`, `scan.py`, `cgscan.py` | arcade-side script decoders / sweeps |
| `ps2scan.py`, `cmpovct.py`, `fulldiff.py` | PS2-side decode, OVCT compare, full script diff |
| `cg_audit.json`, `cg_counterfactual.json`, `data_audit.json`, `residual_audit.json`, `cg_se_audit.json` | machine-readable results |
| `audit_summary.txt`, `audit_run.txt` | human-readable results |
| `denjin_oobcount.log` | the 19 OOB hits from the ASan repro |
