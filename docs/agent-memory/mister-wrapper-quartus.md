# MiSTer Wrapper Quartus Agent Memory

## Purpose

- Capture the durable Quartus setup and host strategy for building `3S-ARM.rbf`.
- Prevent future agents from repeating the Lite-vs-Standard and Apple Silicon Docker dead ends.

## When To Load This

- When touching [tools/mister-wrapper/build-core.sh](/Users/sb/Developer/3s-mister-arm/tools/mister-wrapper/build-core.sh).
- When touching [tools/mister-wrapper/build-quartus-image.sh](/Users/sb/Developer/3s-mister-arm/tools/mister-wrapper/build-quartus-image.sh) or [tools/mister-wrapper/fetch-quartus17-installer.sh](/Users/sb/Developer/3s-mister-arm/tools/mister-wrapper/fetch-quartus17-installer.sh).
- When diagnosing `.rbf` build failures on Apple Silicon hosts.
- Skip for HPS-only work in [tools/mister-wrapper/build-hps.sh](/Users/sb/Developer/3s-mister-arm/tools/mister-wrapper/build-hps.sh) or ARMv7 runtime packaging work.

## Fast Path

- Preferred wrapper-core seed: `menu` via `tools/mister-wrapper/build-core.sh --seed menu`.
- Preferred edition: Quartus Prime Lite 17.0 with both `cyclone-17.0.0.595.qdz` and `cyclonev-17.0.0.595.qdz`.
- Preferred host on this Mac: x86_64 Colima/QEMU VM, not Docker Desktop `linux/amd64`.
- Validated VM profile: `quartus2` in `~/.colima/quartus2/colima.yaml` with `arch: x86_64`, `vmType: qemu`, `cpu: 4`, `memory: 8`, `disk: 20`, `mountType: sshfs`.
- Validated in-VM install path: `/home/sb.linux/intelFPGA_lite/17.0`.
- Local installer cache: [build/quartus17-installer](/Users/sb/Developer/3s-mister-arm/build/quartus17-installer).

### Known-Good Files

- `QuartusLiteSetup-17.0.0.595-linux.run`
- `cyclone-17.0.0.595.qdz`
- `cyclonev-17.0.0.595.qdz`

### Known-Good Command Sequence

```sh
tools/mister-wrapper/fetch-quartus17-installer.sh
```

```sh
# Dev iteration (fast compile, ~50-70% faster):
colima --profile quartus2 ssh -- bash -lc '
  export PATH=/home/sb.linux/intelFPGA_lite/17.0/quartus/bin:$PATH LC_ALL=C LANG=C &&
  cd /Users/sb/Developer/3s-mister-arm &&
  OUTPUT_DIR=/home/sb.linux/build/mister-wrapper-core \
  bash tools/mister-wrapper/build-core.sh --fast --seed menu
'
```

```sh
# Release (full optimization):
colima --profile quartus2 ssh -- bash -lc '
  export PATH=/home/sb.linux/intelFPGA_lite/17.0/quartus/bin:$PATH LC_ALL=C LANG=C &&
  cd /Users/sb/Developer/3s-mister-arm &&
  OUTPUT_DIR=/home/sb.linux/build/mister-wrapper-core \
  bash tools/mister-wrapper/build-core.sh --seed menu
'
```

### Minimal Verification

- `colima --profile quartus2 ssh -- bash -lc 'quartus_sh --version'`
- `colima --profile quartus2 ssh -- pgrep -af 'quartus_(fit|asm|sta)|quartus_sh --flow compile'`
- Check for `output_files/3S-ARM.rbf` under `/home/sb.linux/build/mister-wrapper-core/src` or the host mirror under [build/mister-wrapper-core/src/output_files](/Users/sb/Developer/3s-mister-arm/build/mister-wrapper-core/src/output_files).
- Copy the finished artifact back to the host mirror with:

```sh
colima --profile quartus2 ssh -- bash -lc '
  cp /home/sb.linux/build/mister-wrapper-core/3S-ARM.rbf \
    /Users/sb/Developer/3s-mister-arm/build/mister-wrapper-core/3S-ARM.rbf
'
```

## VM Recreation

If the `quartus2` Colima VM needs to be rebuilt (disk shrink, corruption, new machine), use the automated setup script:

```sh
# 1. Fetch Quartus installer files (~3.5 GB, skip if already in build/quartus17-installer/)
tools/mister-wrapper/fetch-quartus17-installer.sh

# 2. Create VM (disk: 20 GiB) + install Quartus + set up build container
tools/mister/setup-colima-vm.sh
```

To shrink an existing VM (e.g. was created with disk: 80):
```sh
colima stop -p quartus2
colima delete -p quartus2
tools/mister/setup-colima-vm.sh
```

**ARM/HPS build container** (`3s-mister-arm-build`): rebuilt automatically by `setup-colima-vm.sh` via `tools/mister/setup-build-container.sh`. Contains: ARM cross-compiler (gcc-arm-linux-gnueabihf), Clang 20 (from apt.llvm.org/bullseye), cmake 3.25 (bullseye-backports), libasound2-dev (amd64 + armhf), libstdc++-10-dev-armhf-cross.

**Do not skip `prepare_source`**: Both `build-hps.sh` and `build-core.sh` call `prepare_source` unconditionally at startup — it clones/rsyncs fresh source. Agents must invoke the build scripts (not `make` or `quartus_sh` directly) to guarantee a clean source tree.

## Research Starting Points

- Source: [Intel installer CLI docs](https://www.intel.com/content/www/us/en/docs/programmable/683472/25-1/using-cli-commands.html) | Use for: supported command-line installer behavior and flags like `--cli`, `--install-dir`, and `--accept-eula`.
- Source: [Intel Quartus edition comparison PDF](https://www.intel.com/content/dam/www/central-libraries/us/en/documents/quartus-prime-compare-editions-guide.pdf) | Use for: official statement that Lite is free and includes Cyclone IV/V device support.
- Source: [Altera installation/licensing guide](https://docs.altera.com/r/docs/683472/current) | Use for: installation archive, licensing flow, and cross-version install references when Intel links move.
- Source: [Wrapper operator doc](/Users/sb/Developer/3s-mister-arm/docs/mister-wrapper.md) | Use for: repo-specific packaging, deploy, and wrapper runtime behavior.

## Durable Decisions

- Decision: Prefer Quartus Lite 17.0 plus both Cyclone device packs for `3S-ARM.rbf` work. | Why: a clean Lite install passed the earlier false `Cyclone V` blocker and reached `quartus_fit`; the original failure came from incomplete device support. | Date: `2026-03-09`.
- Decision: The validated end-to-end host recipe is `build-core.sh` inside the x86_64 `quartus2` Colima/QEMU VM with local Quartus Lite on `PATH`. | Why: that exact scripted path completed `quartus_map`, `quartus_fit`, `quartus_asm`, and `quartus_sta` and produced `3S-ARM.rbf`. | Date: `2026-03-09`.
- Decision: Prefer the `menu` wrapper-core seed as the only supported path. | Why: real hardware proved the HPS framebuffer handoff, while the MemTest-derived core still produced a black CRT image; the Menu-derived seed preserves the known-good MiSTer framebuffer substrate and keeps the build surface smaller. | Date: `2026-03-10`.
- Decision: Keep Standard installer support in the scripts, but treat it as fallback only. | Why: Standard may still be useful on Linux hosts with existing installs, but it can introduce license requirements that are unnecessary for the default path. | Date: `2026-03-09`.
- Decision: Keep Quartus builds separate from the ARMv7 MiSTer runtime container. | Why: FPGA bitstream builds need x86_64 Quartus, while the runtime container targets ARMv7 userspace binaries. | Date: `2026-03-09`.
- Decision: On Apple Silicon, prefer an x86_64 VM over Docker Desktop `linux/amd64` for Quartus. | Why: the Quartus installer hit `rosetta error: bss_size overflow` in Docker Desktop builds, while the x86_64 Colima/QEMU VM worked. | Date: `2026-03-09`.

- Decision: Use `--fast` for dev iteration builds. | Why: the default QSF has every aggressive optimization maxed out (`PHYSICAL_SYNTHESIS_EFFORT EXTRA`, `FITTER_EFFORT "STANDARD FIT"`, `OPTIMIZATION_MODE "HIGH PERFORMANCE EFFORT"`, etc.), which makes compilation very slow. At 19% ALM utilization there is massive headroom, so timing should still close with relaxed settings. The fast overlay (`menu-fast.qsf`) sets `FAST FIT`, `BALANCED` mode, and disables physical synthesis/router extras. | Date: `2026-03-28`.
- Decision: VZ+Rosetta and QEMU user-mode are dead ends for Quartus 17.0 on Apple Silicon. | Why: Rosetta deadlocks after `fork()` during `quartus_map` synthesis (child completes as zombie, parent hangs permanently). QEMU user-mode 8.2 SIGSEGV during heavy computation. Tested thoroughly on macOS 26.3 / Colima 0.10.1. The x86_64 QEMU full-system VM remains the only viable local path. | Date: `2026-03-28`.

## Known Pitfalls

- Missing `cyclonev-17.0*.qdz` makes Lite look unsupported for the target device. -> Install both `cyclone` and `cyclonev` packages before concluding Lite is insufficient.
- Revisiting the old MemTest-derived seed wastes time once the CRT black-screen finding is known. -> Keep `build-core.sh` and vendor inputs focused on the `menu` seed.
- Docker Desktop `linux/amd64` on Apple Silicon can fail during Quartus install with `rosetta error: bss_size overflow`. -> Use the x86_64 Colima/QEMU VM or another real Linux x86_64 host.
- Changing the global Docker context can interfere with other worktrees. -> Prefer the VM path or use explicit `docker --context ...` instead of `docker context use ...`.
- Standard installs may fail with missing-license errors. -> Try Lite first; if Standard is required, set `LM_LICENSE_FILE` or `MISTER_QUARTUS_LICENSE_FILE`.
- `tools/mister-wrapper/build-core.sh` selecting Docker mode does not prove the image path is the right host strategy on this Mac. -> Use the local-in-VM `quartus_sh` path first for real compile diagnosis.
- VZ+Rosetta (aarch64 VM with Rosetta x86 binary translation) does not work for Quartus 17.0 compilation. -> Do not attempt; `quartus_map` deadlocks after `fork()`. Stick with x86_64 QEMU VM.
- QEMU user-mode (qemu-x86_64) inside a VZ VM segfaults during Quartus synthesis. -> Do not attempt; the QEMU 8.2 TCG translator crashes during heavy x86_64 computation.
- `~/.colima` is a symlink onto the external volume `/Volumes/KimchDrive`, so an `Include /Users/sb/.colima/ssh_config` in `~/.ssh/config` hangs **every** ssh started by any LaunchAgent on this machine, forever: macOS TCC denies launchd-spawned processes access to removable volumes by blocking `open()`/`readdir()` indefinitely rather than returning `EPERM`, and OpenSSH reads `Include` files unconditionally at parse time -- before any host is resolved, inside non-matching `Host` blocks, and in glob form. The glob form does not help; it only silences the error while still hanging. -> Do not put that `Include` in `~/.ssh/config`. The Quartus workflow does not need it: `colima ssh --profile quartus2` passes its own `-F` lima config. For a direct connection use `ssh -F ~/.colima/ssh_config colima-quartus2`.

## Update Rules

- Keep this file focused on Quartus host setup and wrapper-core build diagnosis.
- Update `Last verified` whenever the host recipe or the official Intel doc links are rechecked.
- Replace disproven claims instead of stacking contradictory history.

Last verified: `2026-03-28`
