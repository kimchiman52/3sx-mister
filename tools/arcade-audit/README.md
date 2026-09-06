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

`cg_audit.py` covers sprite indices in the 10 script tables, plus OVCT/OVIX
coverage, the OVCT **reachability** model (`ovct_reachability()`; table
column `ovct a/p reach`, verdicts `ok` / `tail-unreached(n)` /
`walk>end-unreached[exit:hold<=H/N]` / `walk>end[...]` / `TAIL-REACHED(n)!`),
the dangling-walk **hold** model (`ovct_dangling_hold()`, doc §25) and the
X.C.O.P.Y. **reverse-swap gate** (`k7_swap_gate()` / `k7_foreign_cells()`,
doc §26; trailing `xcopy:` column, verdicts `none` / `gated(n)` /
`unmodelled(n,in-range)` / `FOREIGN-OOB(k/n)!`) and the **shape-mismatch
gate** (`manu_delta_gate()`, doc §29; trailing `manu:` column). That last one
re-asks the class-(c) question for the scripts whose shape mismatch made
`audit()` skip it, per raw `cg_number` rather than per cell index — verdicts
`manu:C/N(dD+bB+zZ)` (confirmed direct / bracketed / nothing to adjudicate),
`?n` for the unconfirmed residue, and `DIVERGENT(s,c)!` where the PS2 oracle
contradicts a remap delta. `data_audit.py` covers **the other 13 sections** — STXY MVXY SERND
`unmodelled(n,in-range)` / `FOREIGN-OOB(k/n)!`). Every per-cell violation record also carries two independent verdict fields.
`"dead"` is `cg_audit.py` -> `k7_entry_walk()`: no entry point can reach the
cell (doc §28; the seven OOB columns read `live+dead`). `"grid"` is
`grid_phase()` (doc §29): the two releases' bytes for a script are compared
4-byte block by 4-byte block against the **per-field** cross-release transform
(word 0/1/4/5 per-u16 byte swap, word 2 a full reversal because the releases
store `cg_hit_ix`/`cg_att_ix` in opposite order, word 3's four `u8`s
byte-identical), and the walk reports whether the decoder's cell boundary is
the data's own. Verdicts: `aligned`, `phantom` (positively off-grid — the only
one that excuses a finding), `unmodelled`, `past_prefix` (the arcade span
outruns the PS2 one and the PS2 has no bytes there), `no_oracle`. The run
prints the census, the shape-mismatch split, and by name every OOB row the
grid does **not** explain; it exits non-zero if the u32 byte signature ever
fires on a cell the walk calls `aligned`. This replaces §21.6's "PS2 converter
artifact" class, which does not exist.

`data_audit.py` covers **the other 13 sections** — STXY MVXY SERND
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
(`eff01.c:169`, printed as "R2b" in its output). R2b checks **every table
slot**, reachable or not, and reports **6** post-adaptation violations — all
Elena parts 85-90, every one `part_reachable=False`. The invariant there is
the third line:

```
on a REACHABLE part         : 0
```

`part_reachable` comes from `cg_audit.py` -> `ovct_reachability()`: the set of
OVCT parts any writer of the part index can land on (the cell's `olc >> 4`
through the OVIX, `plcnt_init`'s 0, `exdm_ix_data[*][character][3]`, and the
closure of `eff01.c`'s timer walk over `parts_nix` with no timing constraint).
Elena's reachable set is parts 1-16; parts 17-90 are cold (doc §24, worklist
item **B**, CLOSED 2026-09-06). The same model finds **Dudley**'s arcade
entry 177 pointing one past the table (`ovct_walk_past_end = [178]`, doc
§24.6(i)); `ovct_dangling_hold()` then bounds how long the master can hold the
selecting `olc` — the run's script frames plus one positive `hit_stop` per
renewal cell, the per-renewal value taken from `hitcheck.c`'s parry constant
and the ATITs — against the frames the walk needs, so the row reads
`walk>end-unreached[178:hold<=179/297]` (item **R**, CLOSED 2026-09-06, doc
§25). A run the model cannot read (a C cell inside it, or a script boundary)
is reported `unmodelled` and keeps the exit flagged `walk>end[...]`.

The `xcopy:` column is the cross-character question (doc §26): `effk7.c`
rebinds a morphed Twelve's tables back to his own on a `cg_type 30` cell, and
a cell that also selects a live `olc` would hand the *target's* part index to
Twelve's OVCT. `k7_swap_gate()` re-derives, per target, that the rebind is
armed only by a path that ends on the rebirth script's own `olc 0` marker
three frames later — cell types, hit indices and cancel bits before the
marker, `hiit[0]`'s box rows, and the cells that can be current at arming —
and reports every reason it cannot close the gate as `unmodelled`, which
keeps it open. `k7_foreign_cells()` lists the protected cells and what a swap
there would consume on Twelve's tables (arcade and PS2), so a character reads
`FOREIGN-OOB(...)!` only when the gate is open **and** the consequence leaves
the table. At the time of writing the gate is closed for the five targets
with live foreign cells (Gill, Dudley, Hugo, Ibuki, Remy); Ken is
`unmodelled(6,in-range)` and Yang's four are post-terminator data.

Expected at the time of writing (`fix/arcade-cg-mapping` branch point):

```
TOTAL | (a) 66  (b) 0  (c)wg 949  (c)og 745  manu 316
cells audited: 133901
```

`(a)` is the crash class — remapped CG >= 37664, past the end of
`obj_group_table` (`src/sf33rd/Source/Game/rendering/chren3rd.c:8`). All 66 are
Elena's; see the doc, §8.A. **A fix for that item should drive `(a)` to 0.**

`manu` is **not** a violation count — it counts scripts whose shape mismatch
made `audit()` skip class (c), i.e. cells nothing looked at. `manu_delta_gate()`
(doc §29) is what looks at them, and its two summary lines are the invariant:

```
shape-mismatched (manu) scripts: 316 = 225 direct + 60 bracketed + 20 no-live-cells + 4 unresolved + 7 DIVERGENT
  their live L-cells: 3320 = 2710 direct + 465 bracketed + 94 bracket-disagree + 0 unbracketed + 51 DIVERGENT
```

The five script classes sum to `manu` by construction. The 51 divergent cells
are Twelve's 44 (doc §8.S — a hole in `twelve_cg_ranges`, reported not fixed),
Remy's 5 (§8.N) and Akuma's 2 (§8.P); the last two groups were already
enumerated by hand, and the gate rediscovering exactly them is what validates
it. **A fix for §8.S should drive `DIVERGENT` to 7 cells / 4 scripts.**

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
