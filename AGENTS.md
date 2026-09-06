# Agent Notes

## Safety

- Remote MiSTer filesystem mutations are high risk. Treat `rsync --delete`, `rm -rf`, broad `scp` copies, and remote config rewrites as dangerous until the destination scope is proven.
- Never target delete-capable syncs at `/media/fat`, `/media`, `/`, or another shared remote root. Limit destructive syncs to owned subtrees such as `/media/fat/games/3s-arm/`.
- For wrapper deploys, only `/media/fat/MiSTer_3S-ARM`, `/media/fat/_Other/3S-ARM.rbf`, and `/media/fat/games/3s-arm/` are owned targets. Delete scope belongs only inside `/media/fat/games/3s-arm/`.
- When the MiSTer may be shared with another agent or worktree, check `tools/mister/misterctl.sh lock-status` and `tools/mister/misterctl.sh busy-status` before deploy/probe/smoke work. Prefer local-only progress until the target is clearly idle.
- If a task truly requires a nonstandard remote root, require both a boolean unsafe override and a typed exact-path confirmation. Do not accept a bare "unsafe mode" toggle for delete-capable operations.
- When touching MiSTer deploy helpers or docs, add path validation and dry-run guidance before adding convenience shortcuts.
- Do not use `tools/mister/misterctl.sh exec` unless the task truly requires raw remote shell access; safer purpose-built subcommands are preferred.

## Source of Truth

- **Never edit files under `build/`.** The `build/` directory is gitignored and contains generated or copied artifacts. The tracked source of truth for FPGA wrapper files is `vendor/Menu_MiSTer/` (e.g. `menu.sv`, `rtl/`, `sys/`). If you see a file like `build/mister-wrapper-core/src/3S-ARM.sv`, the real source is `vendor/Menu_MiSTer/menu.sv`.

## Build

- **Always use the telemetry flavor.** The performance difference is negligible and the debug FPS overlay is worth having on every build.
- Canonical build command: `tools/mister/build-game.sh --flavor telemetry`
- Do not start with a host-local `cmake -B build/mister` flow. The build helper is the canonical path because it produces real ARM MiSTer outputs via Docker cross-compilation.
- Full build/package/deploy/probe workflow is in [docs/mister-runbook.md](docs/mister-runbook.md).
- FPGA core builds (Quartus) run in the Colima `quartus2` VM, not Docker. See [docs/agent-memory/mister-wrapper-quartus.md](docs/agent-memory/mister-wrapper-quartus.md).

## Workflow

- For implementation tasks, use the `/implement` skill (three-agent implement → review → fix loop).
- For planning tasks, use the `/plan` skill (three-agent plan → review → fix loop).
- For mature MiSTer perf queues, use [docs/agent-memory/mister-ralph-loop-v2.md](docs/agent-memory/mister-ralph-loop-v2.md) to choose the right loop type (`runtime`, `measurement`, or `workload-fidelity`) before starting another Ralph pass.
- For anything that needs to read the arcade (CPS3) program, use the `/compare-cps3-code` skill and `tools/cps3-disasm/cps3.py`. **Do not write another throwaway Capstone script** — that is how the same addresses got re-derived by hand in lane after lane. [tools/cps3-disasm/README.md](tools/cps3-disasm/README.md) has the method, the four traps it encodes, and the measured reason this is not Ghidra.

## Documentation

- **`docs/archive/` is where finished and abandoned work is kept. Read it for
  *why*.** Go there for the decision that was taken and what it was taken
  instead of, and above all for the negative results — the approach that was
  tried, measured, and did not work. Code has nowhere to record a thing that
  isn't there, so the archive is usually the only place that knowledge exists,
  and re-deriving it costs hours. **Never read it for current facts.** An
  archived document is stamped with the commit it was true at and is not
  maintained after that; for what the code does now, read the code.
- **Do not repoint citations in unenforced files.** The enforced set is
  `tools/doc-citations/baselines.txt` and nothing else — eight documents, plus
  the anchor-required rule over `src`, `include` and `tools`. If a file is not
  in that list, its line numbers are not your problem: leave them alone, in
  passing and on purpose. Repointing them is not tidying, it is 15% of one
  recent 30-hour window's commits spent changing numbers nobody reads. Fix a
  citation outside the enforced set only when you are already editing that
  passage for its content. Nothing under `docs/archive/` is scanned at all.
- **Don't write prose that restates what the code does — write the assertion
  instead.** If a fact can be verified by running something, it belongs in a
  test, where it fails loudly when it stops being true. Prose that duplicates
  code is a second copy that drifts silently and then has to be groomed.
  What legitimately stays prose is what no test can hold: negative results,
  operator procedure, external facts (router NAT behaviour, kernel
  capabilities, the CPS3 core), and decisions. None of those cite our line
  numbers, so none of them can drift.

## Memory Index

- Load [docs/mister-runbook.md](docs/mister-runbook.md) when building, packaging, deploying, probing, or perf-sampling the MiSTer runtime on device. **This is the most important doc for fresh agents doing MiSTer work.**
- Load [docs/miyoo-runbook.md](docs/miyoo-runbook.md) when building, packaging, or deploying the Miyoo Mini Plus / OnionOS port (SSH at root@192.168.1.190).
- Load [docs/building.md](docs/building.md) when you need baseline host build commands, MiSTer profile setup, or the desktop-vs-MiSTer build split.
- Load [docs/performance-optimizations.md](docs/performance-optimizations.md) when investigating performance, understanding optimization history, or planning new perf work.
- Load [docs/mister-wrapper.md](docs/mister-wrapper.md) when working on the `3S-ARM.rbf` + `MiSTer_3S-ARM` wrapper-core path, wrapper packaging, or wrapper deploy/smoke commands.
- Load [docs/config.md](docs/config.md) when changing config keys, defaults, or user-facing scale/software-frame behavior.
- Load [docs/training-select-reset.md](docs/training-select-reset.md) when touching the training-mode SELECT reset (the centre / swap / corner presets), or any in-round teardown that calls `erase_extra_plef_work` / `setup_any_data` / the `Suicide[0]` pulse. It records the defects that path hits and eight corrections to the external design doc.
- Load [docs/research-arcade-balance-desyncs.md](docs/research-arcade-balance-desyncs.md) when investigating an arcade-balance desync, changing `pow_pow.c`/`Round_Level`, touching the statcheck harness (`statcheck_compare.c`, `ScrdGame_Init`), or reading a "replays ran clean" result. It records three confirmed engine divergences from CPS3, six harness false positives with their mechanisms, and why a clean on-device log can be meaningless (the viewer never rescans). Kept separate from the ROM-data doc on purpose — that scope is why one of these defects went unfound.
- Load [docs/rollback-determinism-harness.md](docs/rollback-determinism-harness.md) when changing the rollback save/load whitelist (src/netplay/game_state.c), triaging a desync report, or running `tools/rollback-determinism/run.sh`. Any GameState/GS_SAVE change should be re-validated with the harness's fast mode.
- Load [docs/design-fpga-native-video.md](docs/design-fpga-native-video.md) when working on the FPGA native video DDR3 reader, timing generator, or ARM↔FPGA shared memory protocol.
- Load [docs/reference-native-analog-video.md](docs/reference-native-analog-video.md) when working on analog CRT output (S-Video, composite, VGA), the YC encoder, or sync signal routing.
- Load [docs/agent-memory/mister-remote-safety.md](docs/agent-memory/mister-remote-safety.md) when touching MiSTer deploy helpers, remote command wrappers, or docs that show remote file mutation.
- Load [docs/agent-memory/mister-wrapper-quartus.md](docs/agent-memory/mister-wrapper-quartus.md) when touching `3S-ARM.rbf`, Quartus setup, Apple Silicon host strategy, wrapper-core build failures, or rebuilding the Colima VM.
- Load [docs/agent-memory/mister-native-analog-crt.md](docs/agent-memory/mister-native-analog-crt.md) when revisiting scaler-off analog CRT output, `svideo`/`cvbs` color loss, or native analog wrapper/video-path cleanup.
- Load [docs/agent-memory/mister-ralph-loop-v2.md](docs/agent-memory/mister-ralph-loop-v2.md) when planning, reranking, or repairing the Ralph perf process itself.
- Load [docs/agent-memory/mister-ralph-working-brief.md](docs/agent-memory/mister-ralph-working-brief.md) first when starting a new Ralph perf loop on the active queue.
- Load [docs/agent-memory/mister-ralph-working-brief-template.md](docs/agent-memory/mister-ralph-working-brief-template.md) when creating or refreshing the small working brief for the active Ralph queue.
- Load [docs/agent-memory/mister-perf-deep-research-2026-03-21.md](docs/agent-memory/mister-perf-deep-research-2026-03-21.md) when reranking native super-art/Yun/Genei Ralph loops, validating whether a candidate is actually new, or revisiting the post-loop-149 deep-research claims.
- Load [docs/agent-memory/mister-geneijin-rendering.md](docs/agent-memory/mister-geneijin-rendering.md) when working on Yun SA3 (Genei-Jin) rendering, effect reduction, burst-window optimization, or investigating what renders the activation visual effects.
- Load [docs/agent-memory/mister-sa3-effect-reduction-handoff.md](docs/agent-memory/mister-sa3-effect-reduction-handoff.md) when picking up SA3 effect reduction work mid-session. Temporary handoff doc — delete when diagnostic phase is complete.
- Load [artifacts/mister-port/living-findings.md](artifacts/mister-port/living-findings.md) when you need archived Ralph loop evidence, exact rejection history, or old closeout details; do not treat it as the default working brief for new perf loops.
- Load [docs/archive/mister-port-plan.md](docs/archive/mister-port-plan.md) when re-evaluating stock MiSTer platform constraints, dependency strategy, or custom-image vs stock-image architecture decisions.
