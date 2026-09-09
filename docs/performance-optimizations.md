# Performance Optimizations Since v0.1.0

> **Post-Phase-C update (2026-05-03):** `src/port/sdl/fbdev_presenter.{c,h}`
> have been removed. Per-section references below are historical (each
> records the file that held a given optimization at the time it landed).
> The current MiSTer present path is `src/port/sdl/native_video_writer.c`,
> and the FPS overlay rasterizer lives in
> `src/port/sdl/fps_overlay_compositor.c` (`FPSOverlay_*` API).

Street Fighter III: 3rd Strike (CPS3) on MiSTer FPGA — ARM Cortex-A9 @ 800MHz

**Baseline (pre-optimization):** ~28 FPS on heavy stages, ~40 FPS character select
**Current (all optimizations):** ~60 FPS normal gameplay, ~45-55 FPS super art bursts

---

## 1. Rendering Infrastructure (Initial Port)

These optimizations were baked into the initial MiSTer rendering architecture, establishing the foundation all subsequent work builds on.

### 1.1 Software Frame Rasterizer with Fast-Path Classification

- **What**: Purpose-built software rasterizer that classifies every render task into one of four paths: EXACT copy (memcpy-like), SCALED copy, NON_INTEGER (gather rasterizer), or COLOR_MOD. Each path is optimized for its specific case.
- **Why**: The ARM Cortex-A9 has no GPU. All pixel compositing must happen in software on a 800MHz single-issue in-order core with 32KB L1 D-cache.
- **Impact**: EXACT path runs at ~0.5 cycles/pixel, SCALED at ~1.0, vs ~8+ cycles/pixel for the generic non-integer gather loop. This classification system makes all subsequent optimizations possible.
- **Commit**: `93762472`
- **Files**: `src/port/sdl/sdl_game_renderer.c`, `src/port/sdl/software_frame_non_integer.c`, `src/port/sdl/software_frame_parity.c`

### 1.2 fbdev Direct Scanout + Native Video DDR3 Writer

- **What**: Bypass SDL2's broken fbdev backend entirely. Write frames directly to `/dev/fb0` (fbdev presenter) or to DDR3 shared memory for FPGA-side native video scanout.
- **Why**: SDL2's fbdev support is non-functional on MiSTer. Direct framebuffer access eliminates the SDL overhead and enables the FPGA native video bypass path.
- **Impact**: Eliminates SDL video pipeline overhead entirely. Native video path avoids HDMI scaler latency.
- **Commit**: `93762472`
- **Files**: `src/port/sdl/fbdev_presenter.c`, `src/port/sdl/native_video_writer.c`

### 1.3 Compare-Dirty Partial Texture Refresh

- **What**: When unlocking INDEX8 textures (the CPS3 tile format), compare old and new pixel data byte-by-byte. Only mark the bounding rectangle of changed pixels as dirty, rather than re-uploading the full 64KB texture.
- **Why**: CPS3 textures are 256x256 INDEX8. Most frames only modify a small region (character animation tiles). Full-blit wastes bandwidth on unchanged data.
- **Impact**: Reduces texture upload cost from full 64KB to typically 2-8KB of changed region. Foundational for later dirty-rect refinements.
- **Commit**: `93762472` (infrastructure), `121b87cd` (INDEX8 tracking in `flps2vram.c`)
- **Files**: `src/sf33rd/AcrSDK/ps2/flps2vram.c`, `src/port/sdl/sdl_game_renderer.c`

---

## 2. Game Engine Optimizations (Initial Batch)

Bundled in a single commit targeting the CPS3 game engine code. These address CPU-side bottlenecks in the game logic and sprite dispatch pipeline.

### 2.1 Super Art Background Caching

- **What**: Cache the composited background surface during super art freeze frames and drop all background render tasks from the task list. Only re-render when the camera moves or the super effect ends.
- **Why**: Super art activation triggers a screen freeze + flash sequence. During this time, 100-150 background tile tasks are rendered identically every frame, wasting 3-5ms.
- **Impact**: Eliminates ~100 background render tasks during SA freeze. Later refined in `8ce22184` to preserve solid overlay tasks (darkening panels).
- **Commit**: `121b87cd`
- **Files**: `src/port/sdl/sdl_game_renderer.c`, `src/sf33rd/Source/Game/stage/bg.c`

### 2.2 Background Tile Fast Path (scr_sc == 1.0)

- **What**: When zoom factor is 1.0 (normal gameplay, no super art zoom), bypass the full matrix chain (14+ nj* calls per background layer) and compute viewport bounds and tile positions with simple integer arithmetic.
- **Why**: The `scr_trans()` function builds a complex matrix pipeline for each of the 4+ background layers. During normal gameplay (scr_sc == 1.0), this reduces to trivial translations.
- **Impact**: Saves ~0.5ms/frame by eliminating unnecessary matrix operations for background tile positioning. Also enables the `ppgCalScrPosition` fast path.
- **Commit**: `121b87cd`
- **Files**: `src/sf33rd/Source/Game/stage/bg.c`

### 2.3 njTranslate Inlining + njTranslateZ Specialization

- **What**: Replace `njTranslate`'s full 4x4 matrix multiply with direct row accumulation (4 multiply-adds instead of 64). Add `njTranslateZ` for Z-only translation (4 operations instead of 16).
- **Why**: `njTranslate` was building a temporary identity matrix, setting the translation row, then doing a full `matmul` -- 64 multiplies + 48 adds for what is mathematically a rank-1 update.
- **Impact**: ~8x fewer FP operations per `njTranslate` call. Called hundreds of times per frame for sprite and tile positioning.
- **Commit**: `121b87cd`
- **Files**: `src/sf33rd/Source/Game/rendering/dc_ghost.c`

### 2.4 Ghost Sprite Count Limiting

- **What**: Cap simultaneously-alive ghost/afterimage sprites per player at 4 (vanilla CPS3 cap). Excess spawn requests are silently suppressed in `effect_E7_init` / `effect_E8_init`.
- **Why**: Ghost sprites (Genei-Jin trails, afterimages) are the single most expensive effect. Each ghost is a full character sprite re-render. Uncapped, they can reach 8-12 simultaneous instances.
- **Impact**: Caps worst-case ghost sprite load at the arcade default while preventing runaway render cost.
- **Commits**: `121b87cd` (originally configurable via `ghost_count_max` / `CFG_KEY_GHOST_COUNT`); `d9dfe736` (knob retired, cap hardcoded to 4 once perf was stable enough that tuning it was unnecessary).
- **Files**: `src/sf33rd/Source/Game/effect/effe5.c`, `src/sf33rd/Source/Game/effect/effe7.c`, `src/sf33rd/Source/Game/effect/effe8.c`

### 2.5 Static Jump Tables

- **What**: Move all function pointer dispatch tables (`Management_Jmp_Tbl`, `Game_Jmp_Tbl`, `SC*_Jmp_Tbl`) from stack-local to `static const` file scope.
- **Why**: Stack-allocated function pointer arrays are rebuilt every call, writing ~100-400 bytes to the stack each time. With `static const`, they live in read-only data and are cached after first access.
- **Impact**: Eliminates repeated stack writes across dozens of dispatch functions. Small per-call savings that add up over thousands of calls per frame.
- **Commit**: `121b87cd`
- **Files**: `src/sf33rd/Source/Game/game.c`, `src/sf33rd/Source/Game/engine/manage.c`

### 2.6 memset/memcpy Replacements

- **What**: Replace manual byte-by-byte and element-by-element loops with `memset`, `memcpy`, and `SDL_memset`/`SDL_memcpy` for bulk data operations.
- **Why**: The compiler cannot always auto-vectorize hand-written loops. `memcpy`/`memset` dispatch to NEON-optimized implementations on ARM.
- **Impact**: Minor per-call improvement, but applies across color tables, hit queues, priority arrays, palette copies, and other hot paths.
- **Commit**: `121b87cd`
- **Files**: `src/sf33rd/Source/Game/rendering/color3rd.c`, `src/sf33rd/Source/Game/engine/hitcheck.c`, `src/sf33rd/Source/Game/rendering/mtrans.c`

### 2.7 Z-Conversion Caching

- **What**: Cache `flPS2ConvScreenFZ` precomputed scale/bias values. Only recompute when `ZBuffMax` changes (which is never during gameplay).
- **Why**: Called per-sprite to convert Z-depth. The original code did 3 floating-point operations per call.
- **Impact**: Reduces to 1 multiply + 1 add per call after first invocation.
- **Commit**: `121b87cd`
- **Files**: `src/sf33rd/AcrSDK/ps2/flps2render.c`

### 2.8 PPG Dirty-Rect Tracking Infrastructure

- **What**: Built the full infrastructure for per-texture dirty-rect and tile-mask tracking during `ppgRenewTexChunkSeqs`. Tracks which 32x32 tiles within a 256x256 texture are actually modified, enabling partial upload.
- **Why**: Foundation for the later dirty-rect optimizations (commits `32650399`, `7d3a9631`). Initially disabled due to corruption issues.
- **Impact**: No immediate impact (disabled). Sets up the 7ms->1ms texture refresh savings activated later.
- **Commit**: `121b87cd`
- **Files**: `src/sf33rd/Source/Common/PPGFile.c`

---

## 3. Integer-Snap Sprite Positioning

The single most impactful optimization category. Routes sprites from the expensive gather rasterizer (~8 cycles/pixel) to the fast exact-copy path (~0.5 cycles/pixel).

### 3.1 Integer-Snap MTS Character Sprites

- **What**: Round screen-space vertex coordinates to integers after the matrix transform in `seqsStoreChip` (the MTS character sprite dispatch).
- **Why**: CPS3 hardware has no sub-pixel addressing. Fractional coordinates are purely artifacts of the floating-point matrix pipeline. They force sprites through the expensive non-integer gather rasterizer.
- **Impact**: ~2-4ms/frame savings by routing 50-80% of character sprites from gather loop to exact-copy. On Remy stage: render time dropped from 6.7-8.3ms.
- **Commit**: `244074bc`
- **Files**: `src/sf33rd/Source/Game/rendering/mtrans.c`

### 3.2 Static Lookup Buffers

- **What**: Replace stack-allocated src_x/src_y/pair lookup arrays (2.8KB per call) with `static` buffers in the non-integer rasterizer.
- **Why**: With ~200 calls/frame, stack allocation churns ~563KB/frame (33.8 MB/s at 60fps), thrashing the 32KB L1 D-cache.
- **Impact**: Eliminates L1 cache thrashing from lookup table allocation. Single-threaded renderer makes static buffers safe.
- **Commit**: `244074bc`
- **Files**: `src/port/sdl/software_frame_non_integer.c`

### 3.3 Integer-Snap All Rendering Paths

- **What**: Extend integer-snap from MTS sprites only to ALL rendering paths by snapping `dst_rect` at `try_setup_textured_rect_task` (the universal render task creation point). This catches background tiles (PPGFile), character select UI (sc_sub), and tile sprites (dc_ghost).
- **Why**: The initial snap only covered MTS character sprite fragments. ~40% of render tasks (backgrounds, UI) still hit the expensive non-integer path at ~8 cycles/pixel.
- **Impact**: Routes remaining 40% of tasks to fast paths. Combined with 3.1, essentially all normal-gameplay tasks now use exact-copy or scaled paths.
- **Commit**: `2ffd57a8`
- **Files**: `src/port/sdl/sdl_game_renderer.c`

---

## 4. MTS Tile Cache Hash Index

Replaced O(N) linear scan in the multi-texture sprite (MTS) tile cache with O(1) hash lookup. Implemented across 9 incremental steps.

### 4.1 Data Structures + Helper Functions (Steps 1-2)

- **What**: Define `MtsCacheIndex` (open-addressed hash table with linear probing) and `MtsFreeList` (stack-based free slot allocator). Add static inline helpers for hash lookup, insert, remove, clear, and free-list pop/push.
- **Why**: Foundation for the hash cache. Knuth multiplicative hash on composite key `(code, palt)` with 2048 or 4096 buckets (load factor <= 0.53).
- **Impact**: No behavioral change -- structures defined but not used.
- **Commits**: `67f9ee2d`, `f96cd54e`
- **Files**: `include/structs.h`, `src/sf33rd/Source/Game/rendering/mts_hash.h`

### 4.2 Allocation + Population (Steps 3-4)

- **What**: Allocate hash tables and free lists within the existing `Pull_ramcnt_key` memory block. Wire up `mlt_obj_trans_init`, `clear_texcash_work`, and `init_texcash_2nd` to rebuild hash indices from `PatternState` array contents on init/clear.
- **Why**: Hash tables must be initialized in sync with the tile cache state.
- **Impact**: No behavioral change -- tables maintained but not consulted.
- **Commits**: `d50a2d86`, `83c8b6f0`
- **Files**: `src/sf33rd/Source/Game/rendering/texcash.c`, `src/sf33rd/Source/Game/rendering/mtrans.c`

### 4.3 Core Lookup Replacement (Step 5)

- **What**: Replace `get_mltbuf16` and `get_mltbuf32` linear scan with hash lookup for cache hits and free-list pop for cache misses. Original linear scan preserved as fallback when probe limit (16) is exceeded.
- **Why**: The core bottleneck: scanning up to 768-2176 `PatternState` entries per tile lookup, hundreds of times per frame.
- **Impact**: Expected 2-6ms/frame savings at 800MHz on busy scenes (200+ tile lookups/frame).
- **Commit**: `e489da73`
- **Files**: `src/sf33rd/Source/Game/rendering/mtrans.c`

### 4.4 Eviction + Extended Variants (Steps 6-9)

- **What**: Wire up hash eviction on TTL expiry (`mlt_obj_trans_update`), hash lookup for extended variants (`get_mltbuf16_ext`, `get_mltbuf32_ext`, and their `_2` allocation variants), and `update_with_tpu_free` eviction.
- **Why**: The hash table must stay in sync with all cache mutation paths -- not just the primary lookup, but also TTL-based eviction and the extended (tpf/tpu) allocation path.
- **Impact**: Completes the hash cache integration. All tile cache lookups now O(1).
- **Commits**: `17c34356`, `26fae9c8`
- **Files**: `src/sf33rd/Source/Game/rendering/mtrans.c`, `src/sf33rd/Source/Game/rendering/texcash.c`

---

## 5. Texture Refresh Optimizations

### 5.1 Compare-Dirty Cap Relaxation (3/8 -> 1/2)

- **What**: Raise the partial-refresh pixel cap from 24576 (3/8 of 65536) to 32768 (1/2).
- **Why**: The hottest texture (seq 82, character portraits) needs ~25600 dirty pixels, just above the old cap. This caused 100% full-blit fallback on the most expensive single texture.
- **Impact**: Admits nearly all partial-refresh candidates. The compare-dirty path uses runtime pixel comparison (not heuristic), so relaxing the cap is safe.
- **Commit**: `7d3a9631`
- **Files**: `src/port/sdl/sdl_game_renderer.c`

### 5.2 PPG Dirty-Rect Tracking Re-enabled

- **What**: Re-enable ppg-level renew-dirty rect and tile-mask tracking for sprite textures with `texture_total >= 5` (character/effect entries). Lower page counts remain on full-refresh to avoid options menu corruption.
- **Why**: During character select and gameplay, the full PPG texture refresh (`ppgRenewTexChunkSeqs`) was copying entire 64-256KB textures even when only a few tiles changed.
- **Impact**: **Texture refresh time: 7ms -> 1ms.** Single biggest measured improvement for character select performance.
- **Commit**: `32650399`
- **Files**: `src/sf33rd/Source/Common/PPGFile.c`
- **Known issue**: Causes minor corruption on options menu text and Remy stage animated background cold-start. Tracked in `project-ppg-dirty-rect-corruption.md`.

### 5.3 Partial Texture Upload in ppgRenewTexChunkSeqs

- **What**: Copy only dirty-rect rows during texture renewal instead of full 64-256KB memmove.
- **Why**: Even with dirty-rect tracking, the actual upload was still copying the full texture buffer.
- **Impact**: Reduces per-texture upload from 64-256KB to only the rows containing changes (typically 2-16KB).
- **Commit**: `7ce70abe` (Opt 3)
- **Files**: `src/sf33rd/Source/Common/PPGFile.c`

---

## 6. Software Rasterizer Optimizations

Six render-path optimizations targeting super art frame drops, plus four additional optimizations in a follow-up commit.

### 6.1 Early Merge Rejection

- **What**: Skip expensive merge plan-building for non-mergeable render tasks (texture/color/flip mismatch detected early).
- **Why**: The task merger checks every adjacent task pair for compatibility. Most pairs are incompatible, but the full plan-building code runs before rejecting them.
- **Impact**: Reduces sort/merge overhead during high task-count frames (super arts: 300+ tasks).
- **Commit**: `7ce70abe` (Opt 1)
- **Files**: `src/port/sdl/sdl_game_renderer.c`

### 6.2 NEON Exact-Copy Blend

- **What**: New `neon_blend_4pixels()` NEON path for non-color-mod, non-flipped exact-copy tasks. Processes 4 pixels per NEON operation.
- **Why**: The existing exact-copy path handled alpha blending with scalar code. NEON can process 4 ARGB pixels simultaneously.
- **Impact**: ~4x throughput for alpha-blended exact-copy sprites (the most common task type after integer-snap).
- **Commit**: `7ce70abe` (Opt 2)
- **Files**: `src/port/sdl/sdl_game_renderer.c`

### 6.3 Binary-Alpha Skip

- **What**: Skip `modulate_argb8888` call for pixels with raw alpha == 0 (fully transparent). Test before function call overhead.
- **Why**: Many sprite textures are sparse (large transparent regions). Each transparent pixel was still going through the modulation function.
- **Impact**: Eliminates function call overhead for transparent pixels across all paths.
- **Commit**: `7ce70abe` (Opt 4)
- **Files**: `src/port/sdl/sdl_game_renderer.c`

### 6.4 COLOR_MOD Lookup Tables

- **What**: Pre-computed integer lookup tables for scaled + color-modulated tasks with integer coordinates. Replaces per-pixel floating-point UV computation.
- **Why**: COLOR_MOD tasks (super art effects, flash overlays) use per-pixel color modulation. Computing UV coordinates in floating-point per pixel is expensive on Cortex-A9.
- **Impact**: Eliminates float division in COLOR_MOD inner loop. Significant for super art frames.
- **Commit**: `7ce70abe` (Opt 5)
- **Files**: `src/port/sdl/sdl_game_renderer.c`

### 6.5 Generic Path NEON+LUT

- **What**: Pre-computed src_x/src_y lookup tables + NEON gather/modulate/blend for all generic fallback tasks, replacing per-pixel float UV computation.
- **Why**: The generic fallback path (non-integer, non-exact) was doing 2 float divides + 2 float multiplies per pixel for UV mapping.
- **Impact**: Replaces float computation with integer lookup + NEON blend. Major improvement for the remaining non-integer sprites.
- **Commit**: `7ce70abe` (Opt 6)
- **Files**: `src/port/sdl/sdl_game_renderer.c`

### 6.6 Opaque-Dst Blend

- **What**: New `blend_argb8888_opaque_dst()` inline that assumes `dst_a == 255` (always true for the software frame surface), eliminating dst alpha extraction, comparison, and the division-heavy generic alpha path.
- **Why**: The generic blend function handled arbitrary dst alpha, but the software frame surface is always fully opaque. The generic path includes an expensive divide-by-255 that is unnecessary.
- **Impact**: Replaces `blend_argb8888` at all 16 software frame call sites. Eliminates one multiply and one divide per blended pixel.
- **Commit**: `5d603873` (Opt 10)
- **Files**: `src/port/sdl/sdl_game_renderer.c`

### 6.7 Off-Screen Task Skip

- **What**: Early reject fully off-screen `TEXTURED_RECT` tasks in `render_frame_to_software_surface` before function call overhead.
- **Why**: Some render tasks (especially during camera scrolls and super art effects) are entirely off-screen. The existing code still set up surfaces and called the rasterizer.
- **Impact**: Eliminates function call overhead for off-screen tasks. Saves ~10-20 tasks/frame during scrolling.
- **Commit**: `5d603873` (Opt 11)
- **Files**: `src/port/sdl/sdl_game_renderer.c`

### 6.8 Contiguous NEON Fast Path

- **What**: Pre-scan lookup tables; when indices are consecutive (1:1 non-flipped sprites), use direct NEON loads instead of the 4-element gather pattern in the Opt 5/6 inner loops.
- **Why**: The gather pattern (load 4 separate pixels by index, pack into NEON register) is necessary for scaled/rotated sprites but wasteful for 1:1 sprites where pixels are already contiguous in memory.
- **Impact**: Direct `vld1q_u32` loads are ~4x faster than the gather pattern. Benefits the most common sprite type.
- **Commit**: `5d603873` (Opt 12)
- **Files**: `src/port/sdl/sdl_game_renderer.c`

---

## 7. Sprite Submission Pipeline

### 7.1 Batch Sprite Submission

- **What**: Replace per-sprite `Renderer_DrawSprite2` loop in `seqsAfterProcess` with bulk `Renderer_DrawSprites2Batch` that inlines task setup, defers dirty-tile marking, and handles texture binding internally.
- **Why**: The per-sprite path has 4+ levels of function call indirection: `seqsAfterProcess` -> `Renderer_DrawSprite2` -> `SDLGameRenderer_DrawSprite2` -> `draw_sprite_rect` -> `try_setup_textured_rect_task`. Each call crosses abstraction boundaries.
- **Impact**: Eliminates function call overhead for 200-280 sprites/frame. Batch processing enables amortized dirty-tile marking.
- **Commit**: `5d603873`
- **Files**: `src/sf33rd/Source/Game/rendering/mtrans.c`, `src/port/sdl/sdl_game_renderer.c`, `src/rendering/renderer.c`

---

## 8. Frame Timing and Video

### 8.1 ARM Frame Pacing to FPGA Refresh Rate

- **What**: Match ARM frame delivery rate to the FPGA's actual video refresh rate. When native video is active, target NV_TARGET_FPS instead of the CPS3 original rate.
- **Why**: The FPGA's integer-N PLL produces 59.6374 Hz, while the ARM targeted CPS3's 59.5995 Hz. The 0.038 Hz mismatch caused the ARM to drift one frame behind every ~26 seconds, triggering visible stutter during scrolling.
- **Impact**: Eliminates periodic ~26-second stutter on native video output.
- **Commit**: `ab80248a`
- **Files**: `src/port/sdl/sdl_app.c`

### 8.2 Dedicated Video PLL (59.5993 Hz)

- **What**: Add a separate integer-N video PLL (50 MHz × 81/5 / 26 = 31.1538 MHz) so CLK_VIDEO is no longer constrained by the system PLL. H-freq = 15,734 Hz (NTSC standard, exact). Frame rate: 59.5995 Hz (1.4 μHz error vs CPS3 target).
- **Why**: The old shared PLL produced 31.25 MHz, giving 59.6374 Hz -- 0.038 Hz off from CPS3's 59.5995 Hz, and H-freq of 15,625 Hz which some NTSC CRTs couldn't sync to.
- **Impact**: NTSC-compatible H-freq for all CRTs. Frame rate error reduced to 1.4 μHz (one stale frame per 8 days). ARM no longer needs separate NV_TARGET_FPS compensation.
- **Commit**: `dbcf340e`
- **Files**: FPGA: `pll_video.v`, `menu.sv`, `native_video_timing.sv`. ARM: `src/port/sdl/sdl_app.c`

### 8.3 Software PLL Frame Pacer

- **What**: ARM-only software PLL replaces the broken FPGA DDR3 vsync feedback approach. SCHED_FIFO real-time scheduling (priority 49) + mlockall + hybrid sleep/busy-wait (1.5ms spin with ARM `yield` instruction) + adaptive phase correction (+250us after 3 consecutive late frames, -250us decay after 60 on-time frames, capped at 2ms). SPU audio thread self-boosts to SCHED_FIFO priority 50 to prevent priority inversion on the shared soundLock mutex.
- **Why**: The FPGA DDR3 feedback approach produced broken video on all builds (Quartus fitter placement shift from 73 new signal routes). With the PLL matched to 1.4 µHz, frequency tracking is unnecessary — only OS scheduling jitter needs compensation. See `docs/archive/research-vsync-feedback-fpga-bug.md` for the full FPGA bug analysis.
- **Impact**: Eliminates sporadic frame stutter from OS scheduling jitter without any FPGA changes. Audio stays clean during heavy scenes via RT priority layering.
- **Commit**: `e65b51a8`
- **Files**: `src/port/sdl/sdl_app.c`, `src/port/sound/spu.c`

### 8.4 Stale Texture Comparison Removal

- **What**: Remove the per-pixel `flPS2CopyIndex8TextureAndTrackDirtyRect` comparison that was tracking dirty rects at the INDEX8 texture unlock level. Revert to simple `flMemcpy`.
- **Why**: The dirty-rect tracking it fed was disabled (ppg dirty-rect was off at the time), so this was pure overhead: 0.5-3ms/frame of byte-by-byte comparison producing unused results.
- **Impact**: Saves 0.5-3ms/frame on animated stages (Remy, Elena, etc.).
- **Commit**: `9e7d69ee`
- **Files**: `src/sf33rd/AcrSDK/ps2/flps2vram.c`

### 8.4 ARM Clock Management

- **What**: Reliable sysfs-based ARM clock cycling with unbuffered writes, governor pinning, and readback verification. Startup logs report both the requested mode and the verified `scaling_max_freq`, so a rejected 1200MHz request is recorded as an actual 800MHz clock rather than a misleading success line. Default to 1200MHz on fresh install.
- **Why**: The original `fprintf`-based clock writes were unreliable (buffered I/O could be lost). The kernel governor could downclock during frame sleep, wasting headroom.
- **Impact**: Reliable overclocking to 1200MHz provides ~50% more CPU headroom. Governor pinning ensures consistent performance.
- **Commit**: `e510bc42`
- **Files**: `src/port/sdl/sdl_app.c`, `vendor/Main_MiSTer/thirdsarm_wrapper.cpp`

---

## 9. Profiling and Instrumentation

These commits do not directly improve performance but were essential for identifying and measuring bottlenecks.

### 9.1 FPS Overlay with Component Breakdown

- **What**: Enhanced show-fps overlay displaying per-component timing: `U:x.x(T:x.x) R:x.x P:x.x =x.x` with 250ms rolling averages.
- **Why**: Without component-level timing, it was impossible to tell whether frame drops came from update logic, texture refresh, rendering, or presentation.
- **Impact**: Enabled identification of the texture refresh bottleneck (T: 7ms) and render bottleneck (R: 8ms).
- **Commit**: `13af5e18`
- **Files**: `src/port/sdl/fbdev_presenter.c`, `src/port/sdl/sdl_app.c`

### 9.2 G/S/D/sort/raster Sub-Timers

- **What**: Break down Update and Render timing into sub-components: T (texture refresh), G (game logic), S (sprite submission), D (dispatch), r (rasterization).
- **Why**: The Update phase contains multiple independent bottlenecks. Needed granular timing to optimize each sub-system.
- **Impact**: Overlay format: `59 U:8.3(T2.1 G2.0 S1.5 D0.8) R:7.2(r6.8) =16.0`
- **Commit**: `b16c6f2a`
- **Files**: `src/port/sdl/sdl_app.c`, `src/sf33rd/Source/Game/game.c`

### 9.3 D Sub-Timers (Texture/Sprite Breakdown)

- **What**: Split D (dispatch) timer into t (texture renewal) and s (sprite submission) sub-timers.
- **Why**: After batch sprite submission, needed to verify whether texture renewal or sprite submission was the remaining bottleneck.
- **Impact**: Overlay format: `D4.5[t2.1 s2.0]`
- **Commit**: `e115475d`
- **Files**: `src/port/sdl/fbdev_presenter.c`, `src/port/sdl/sdl_app.c`

### 9.4 Lightweight Perf Counters

- **What**: Enable `software_frame_fast_exact_tasks`, `_scaled_tasks`, `_non_integer_tasks`, and `_candidate_tasks` counters in `--perf-basic` mode (not just extended mode).
- **Why**: Needed to measure the effectiveness of integer-snap without the overhead of full extended telemetry.
- **Impact**: Near-zero overhead (a few integer adds per task).
- **Commit**: `13af5e18`
- **Files**: `src/port/sdl/sdl_game_renderer.c`

---

## 10. Audio Latency (Indirect Performance)

### 10.1 FPGA Audio Thresholds + Buffer Reduction

- **What**: Tighten FPGA ALSA hurryup thresholds (peak sawtooth 85ms -> 43ms), reduce ADX music buffer from 400ms to 100ms, add SDL3 audio clock recovery. Note: SDL3 clock recovery later removed due to ARM artifacts.
- **Why**: Audio pipeline latency was perceptible. Not a CPU performance issue per se, but impacts perceived smoothness.
- **Impact**: Audio latency reduced from ~85ms peak to ~43ms peak.
- **Commit**: `0e88d7d6`
- **Files**: `src/port/sound/adx.c`, `src/port/sound/spu.c`, `vendor/Menu_MiSTer/sys/alsa.sv`

---

## 11. INDEX8 Rasterization

### 11.1 INDEX8 Rasterization with Per-Pixel Palette Lookup

- **What**: Keep textures in INDEX8 (1 byte/pixel) format through to the rasterizer inner loop. Apply a 256-entry ARGB8888 palette lookup per-pixel instead of pre-converting to ARGB8888 (4 bytes/pixel). Uses direct bypass (reads live `flTexture` backing buffer) rather than cache copies — avoids dirty-rect refresh bugs that caused options menu and Remy stage garbling. Task-nullify in `SDLGameRenderer_DestroyTexture` prevents use-after-free during scene transitions.
- **Why**: The software rasterizer's gather loop reads from 256KB ARGB8888 source surfaces that exceed the 32KB L1 D-cache by 8×. Keeping textures as 64KB INDEX8 reduces the working set to 2× L1. The 1KB palette table (256 × 4 bytes) fits permanently in L1. Inspired by the PSP port (demmis98/3s-psp) which uses hardware CLUT to keep textures indexed.
- **Impact**: Texture refresh eliminated for INDEX8 textures (no `SDL_ConvertSurface` needed). L1 cache pressure reduced ~4× in the gather loop. Near-stable 60 FPS on Remy and Chun-Li stages at stock 800MHz clock. Also resolved the PPG dirty-rect corruption bug (options menu text, Remy stage background) — the corruption was in the renderer-side ARGB8888 cache refresh path, which the bypass eliminates.
- **Commit**: `480d3368`, `8c1994ec`
- **Files**: `src/port/sdl/sdl_game_renderer.c`

---

## Summary Table

| # | Optimization | Category | Estimated Impact | Commit |
|---|---|---|---|---|
| 1.1 | Software frame rasterizer fast-path classification | Rendering | Foundation (enables all below) | `93762472` |
| 1.2 | fbdev direct scanout + native video DDR3 | Rendering | Eliminates SDL overhead | `93762472` |
| 1.3 | Compare-dirty partial texture refresh | Texture | Variable (up to 60KB/frame saved) | `93762472` |
| 2.1 | Super art background caching | Game Engine | ~3-5ms during SA freeze | `121b87cd` |
| 2.2 | Background tile fast path (scr_sc==1.0) | Game Engine | ~0.5ms/frame | `121b87cd` |
| 2.3 | njTranslate inlining + njTranslateZ | Game Engine | ~8x fewer FP ops/call | `121b87cd` |
| 2.4 | Ghost sprite count limiting | Game Engine | Caps worst-case cost | `121b87cd` |
| 2.5 | Static jump tables | Game Engine | Minor (reduced stack writes) | `121b87cd` |
| 2.6 | memset/memcpy replacements | Game Engine | Minor (NEON-optimized bulk ops) | `121b87cd` |
| 2.7 | Z-conversion caching | Game Engine | Minor (~0.1ms/frame) | `121b87cd` |
| 2.8 | PPG dirty-rect infrastructure | Texture | None (initially disabled) | `121b87cd` |
| 3.1 | Integer-snap MTS character sprites | Rendering | ~2-4ms/frame | `244074bc` |
| 3.2 | Static lookup buffers | Rendering | Eliminates 563KB/frame stack churn | `244074bc` |
| 3.3 | Integer-snap all rendering paths | Rendering | Routes remaining 40% to fast path | `2ffd57a8` |
| 4.1-4.4 | MTS hash cache (steps 1-9) | Tile Cache | ~2-6ms/frame at 800MHz | `67f9ee2d`..`26fae9c8` |
| 5.1 | Compare-dirty cap relaxation (3/8->1/2) | Texture | Admits hottest texture to partial refresh | `7d3a9631` |
| 5.2 | PPG dirty-rect tracking re-enabled | Texture | **T: 7ms -> 1ms** | `32650399` |
| 5.3 | Partial texture upload in ppgRenewTexChunkSeqs | Texture | 64-256KB -> 2-16KB per upload | `7ce70abe` |
| 6.1 | Early merge rejection | Rendering | Reduces sort/merge overhead | `7ce70abe` |
| 6.2 | NEON exact-copy blend | Rendering | ~4x throughput for blended sprites | `7ce70abe` |
| 6.3 | Binary-alpha skip | Rendering | Eliminates work for transparent pixels | `7ce70abe` |
| 6.4 | COLOR_MOD lookup tables | Rendering | Eliminates float div in COLOR_MOD loop | `7ce70abe` |
| 6.5 | Generic path NEON+LUT | Rendering | Replaces float UV with integer+NEON | `7ce70abe` |
| 6.6 | Opaque-dst blend | Rendering | Eliminates div-by-255 at 16 call sites | `5d603873` |
| 6.7 | Off-screen task skip | Rendering | ~10-20 tasks/frame during scrolls | `5d603873` |
| 6.8 | Contiguous NEON fast path | Rendering | ~4x faster for 1:1 non-flipped sprites | `5d603873` |
| 7.1 | Batch sprite submission | Submission | Eliminates 4+ indirection levels | `5d603873` |
| 8.1 | ARM frame pacing match | Timing | Eliminates 26-second stutter | `ab80248a` |
| 8.2 | Dedicated video PLL (59.5993 Hz) | Timing | 245x more accurate frame rate | `dbcf340e` |
| 8.3 | Software PLL frame pacer | Timing | Eliminates OS jitter stutter | `e65b51a8` |
| 8.4 | Stale texture comparison removal | Texture | 0.5-3ms/frame on animated stages | `9e7d69ee` |
| 8.5 | ARM clock management (user-selectable overclock) | System | ~50% CPU headroom at 1200MHz | `e510bc42` |
| 9.1 | Texture group load race → skip frame | Stability | Eliminates SIGABRT crash in attract mode at 800MHz | |
| 9.2 | CG cache full / decode error → graceful fallback | Stability | Eliminates infinite CPU spin on long idle (animated stages) | |
| 11.1 | INDEX8 rasterization with per-pixel palette lookup | Rendering | 4× smaller source textures, eliminates ARGB8888 conversion | `480d3368`, `8c1994ec` |
| 12.1 | frame_trace opt-in via env var | Overlay/I-O | Eliminates per-frame trace fprintf+fflush | `bdb5a4cd` |
| 12.2 | njdp2d prim buffer raised + overflow log rate-limited | Overlay/I-O | Eliminates per-dropped-prim I/O storm | `ff0fbaf7` |
| 12.3 | Perf telemetry counters (trace/njdp2d/quads/overlay-submit) | Instrumentation | Enables direct per-condition measurement | `dc567776` |
| 12.4 | Hoist glyph texture bind out of SSPutStr_Bigger | Overlay/Rendering | ~39->~13 register sends/player/frame (input history) | `05f5364f` |
| 12.5 | njdp2d sort-at-draw (replace O(n²) insertion walk) | Overlay/Rendering | O(n log n) vs O(n²); ~10k walk steps/frame removed | `4cf7eef1` |
| 12.6 | NEON kernel for 16bpp partial-alpha solid fill | Overlay/Rendering | Bit-exact NEON path for hitbox alpha fills | `45da0d4c` |

## 12. Training-Mode Overlay Perf Pass (Frame Meter + Input History)

Frame drops reported during heavy training scenes with the frame-data meter, input history, and hitboxes all on. Root-caused in `scratchpad` investigation report (per-frame `frame_trace` file I/O, `njdp2d` prim-buffer overflow logging, redundant draw-call/texture-bind overhead, O(n²) 2D sort). All changes are display/measurement-side only; none touch rollback-saved gameplay state (`Disp_Frame_Data`/`Disp_Input_History` semantics unchanged).

### 12.1 `frame_trace` Opt-In via Env Var

- **What**: Gate `frame_trace_tick()`/`frame_trace_annotate()` on a new `frame_trace_opt_in()` check (`FRAME_TRACE_PATH` env var set and non-empty), in addition to the existing training-mode + `Disp_Frame_Data` checks.
- **Why**: `frame_trace_tick()` ran every active training-mode frame regardless of whether anything consumed the trace file, writing a ~69-field `fprintf` + `fflush()` to `/tmp/3sx-frame-trace.log` whenever either player was "active" — i.e. during exactly the heavy scenes where drops were reported.
- **Impact**: Zero per-frame trace I/O for ordinary players with the meter on. The frame-data test harness already exports `FRAME_TRACE_PATH`, so `tools/frame-data/run-suite.sh` keeps producing trace logs with no harness changes.
- **Commit**: `bdb5a4cd`
- **Files**: `src/sf33rd/Source/Game/ui/frame_trace.c`

### 12.2 njdp2d Prim Buffer Raised + Overflow Log Rate-Limited

- **What**: Raise `NJDP2D_PRIM_MAX` 200 -> 512 (`dc_ghost.c:20`); add an `overflow_logged` one-shot latch so the overflow `flLogOut` fires at most once per drain instead of once per dropped prim; add an `overflow_drops` counter. Both are reset in `njdp2d_init()`, which runs at the end of each `njdp2d_draw()` drain — and `njdp2d_draw()` drains up to twice per game frame, so the latch permits at most two overflow log lines per frame.
- **Why**: The 144 unconditional meter-cell prims plus hitboxes/shadows/fades could exceed the old 200-prim cap during attacks. Every prim past the cap triggered `flLogOut` — a `vsprintf` plus an always-failing `open()` syscall plus `fprintf(stderr, ...)` — once per dropped prim, per drain, while also silently dropping (visually corrupting) the overflowing prims.
- **Impact**: Meter + hitboxes now fit comfortably under the raised cap in the common case; on the rare drain that still overflows, the I/O storm collapses to one log line instead of N.
- **Commit**: `ff0fbaf7`
- **Files**: `src/main.c`, `src/sf33rd/Source/Game/rendering/dc_ghost.c`

### 12.3 Perf Telemetry Counters (ENABLE_PERF_TELEMETRY)

- **What**: Four new counters/timers, all gated under `ENABLE_PERF_TELEMETRY` and surfaced via a new `[perf_p3]` line in the debug FPS overlay's existing 120-frame cadence: (1) `FrameTrace_Set/GetPerfTickNs` — ns timer around `frame_trace_tick()`'s `main.c` call site; (2) `Njdp2d_GetPerfPeakTotal`/`GetPerfDrops` — per-frame peak `njdp2d_w.total` and summed `overflow_drops`, captured in `njdp2d_draw()` before `njdp2d_init()` clears them, reset once per frame via `Njdp2d_ResetPerf()`; (3) `SoftwareRenderer_GetPerfPeakQuads` — peak `arrlen(quads)` before sort in `SoftwareRenderer_RenderFrame()`; (4) `Training_Set/GetPerfDispNs` — ns timer around `Training_Data_Disp()`'s `menu.c` call site.
- **Why**: The investigation's on-device measurement plan (below) needed direct instrumentation to distinguish "how much is trace I/O", "do we overflow the njdp2d/quad buffers, by how much", and "total overlay submission cost" per condition, rather than inferring them from the aggregate `u` bucket.
- **Impact**: Near-zero overhead when the overlay is off (getters are only read in `FPS_OVERLAY_DEBUG` mode). Makes the 5-condition protocol below directly measurable instead of inferred.
- **Commit**: `dc567776`
- **Files**: `src/main.c`, `src/sf33rd/Source/Game/ui/frame_trace.{c,h}`, `src/sf33rd/Source/Game/rendering/dc_ghost.{c,h}`, `src/platform/video/software/software_renderer.{c,h}`, `src/sf33rd/Source/Game/menu/menu.c`, `src/sf33rd/Source/Game/ui/sc_sub.{c,h}`, `src/port/sdl/sdl_app.c`

### 12.4 Hoist Glyph Texture Bind Out of `SSPutStr_Bigger`

- **What**: `SSPutStr_Bigger` rebound the glyph texture register (`njDrawTexture`) once per character even though the whole string draws from the same glyph sheet. Add a no-bind quad path (`njDrawTextureNoBind`) so the register is set once, on the first drawn glyph, and reused for the rest of the string.
- **Why**: Input-history rows (`sc_sub.c`) draw via `SSPutStr_Bigger`, each glyph doing a full `flSetRenderState(TEXSTAGE0)` texture-register set even though the source texture never changes within a string.
- **Impact**: Cuts input-history count-field register sends from ~39 to ~13 per player per frame; benefits every other `SSPutStr_Bigger` HUD caller identically. Display-side only — same glyphs, same order, same position.
- **Commit**: `05f5364f`
- **Files**: `src/sf33rd/Source/Game/ui/sc_sub.c`, `src/sf33rd/Source/Common/PPGFile.{c,h}`, `src/sf33rd/Source/Game/rendering/dc_ghost.{c,h}`

### 12.5 njdp2d Sort-at-Draw (Replace O(n²) Insertion Walk)

- **What**: Replace `njdp2d_sort`'s per-insert linked-list walk with append-then-sort-at-draw: prims are pushed in arrival order into `njdp2d_w.prim[]`, then `njdp2d_draw()` builds a `(z-key, index)` array and quicksorts it (falling back to insertion sort for small partitions), preserving z-descending order with equal-z FIFO tie-breaking (signed-zero canonicalized so `-0.0`/`+0.0` tie like the old strict `>`).
- **Why**: The old insertion sort walked the existing list linearly per prim, giving O(n²) total cost. With 144 same-priority meter prims alone this was ~10,300 list-walk steps per frame; more with hitboxes.
- **Impact**: Replaces the O(n²) insertion walk with an O(n log n) sort once per frame. Draw order is provably unchanged (verified against the equal-key FIFO contract of the prior linked-list walk).
- **Commit**: `4cf7eef1`
- **Files**: `src/sf33rd/Source/Game/rendering/dc_ghost.c`

### 12.6 NEON Kernel for 16bpp Partial-Alpha Solid Fill

- **What**: Add a shared scalar-reference + NEON kernel (`sw_blit_565_fill.h`) for the 16bpp partial-alpha (`0 < a < 255`) solid-fill row used by hitbox box fills, processing 8 pixels/iteration in u32 lanes with a scalar tail. Routed in `sw_blit.c` behind `__ARM_NEON && CRS_SW_CANVAS_16BPP`; the `a==0`/`a==0xFF` short-circuits and the 32bpp path are untouched.
- **Why**: Hitbox alpha fills are the one overlay cost that is per-pixel-significant, running through the scalar 16bpp blend row over character-sized screen areas every frame hitboxes are shown.
- **Impact**: Proven bit-exact against the scalar reference over 88,531,974 comparisons in a standalone host harness (`tools/sw-blit-565/`) covering full geometry/alpha-bucket sweeps plus 20k fuzz cases. Cross-compiled clean for ARMv7 cortex-a9/neon-vfpv3 (`-Wall -Wextra -Werror`); on-device execution timing not yet measured.
- **Commit**: `45da0d4c`
- **Files**: `src/platform/video/software/sw_blit_565_fill.h`, `src/platform/video/software/sw_blit.c`, `tools/sw-blit-565/`

### 12.7 Frame Meter Batched into Cached ARGB Strips (P4)

- **What**: Replace the frame meter's 144 per-frame solid-rect prims (72 cells x 2 rows via `fd_draw_rect` -> `njColorBlendingMode` + `njDrawPolygon2D`) with two persistent 288x6 ARGB strips submitted as one UI-bitmap quad per row. Strips are regenerated only when a row's 72 visible cell kinds change (`memcmp` dirty check); cell colors, the odd-cell darken, positions, and opaque semantics on the 16bpp canvas are reproduced exactly (both paths write `sw_argb_to_canvas(argb)` for opaque pixels).
- **Why**: The meter's per-cell prim submission was one of the remaining per-frame costs identified alongside 12.2/12.5 — 144 njdp2d prims, 144 giblet quads, and 144 blend-mode calls every frame the meter is visible, regardless of whether any cell actually changed.
- **Impact**: Removes 144 njdp2d prims, 144 giblet quads, and 144 blend-mode calls per frame while the meter is visible; reviewed z-order against all `PrioBase[2]` users confirms no same-z opaque overlapper intersects the meter rectangle. Full frame-data suite `--check-golden` zero drift (94 corpora, 1,290 PASS / 59 XFAIL); ARM cross-build (telemetry) clean; all 12 lever gates asserted `==1` pre-commit. Measurement code, lever constants, and the numeric text line are untouched.
- **Commit**: `6590c818`
- **Files**: `include/rendering/game_renderer.h`, `src/platform/video/software/software_renderer.c`, `src/sf33rd/Source/Game/ui/frame_data_overlay.c`

---

## 13. On-Device Measurement Protocol (Training Overlay Perf)

Use this to measure the wins above during real gameplay once on-device time is available. All conditions use the **telemetry** build flavor (`ENABLE_PERF_TELEMETRY` ON) — headless captures give inflated FPS and are not a substitute for on-device measurement.

**Setup**: `show-fps = debug` (`config.h`) turns on the on-canvas `fps N u X r X p X t X` overlay. Training mode, same heavy scene (repeated SA + projectiles) for every condition so results are comparable.

**5 conditions** (from the investigation report §4):
1. All overlays off (baseline).
2. Frame-data meter only.
3. Input history only.
4. Meter + input history (the originally reported repro scene).
5. Meter + input history + hitboxes (goal state — the heaviest combination).

**What to record per condition**:
- `u`/`r`/`p`/`t` buckets and any FPS dips from the on-canvas overlay.
- `[perf_avg]` 120-frame rolling averages (`sdl_app.c`).
- `FRAME OUTLIER` log lines.
- The new `[perf_p3]` telemetry line (step 12.3), which reports, per 120-frame window: `frame_trace` ns sum, `Training_Data_Disp` ns sum, njdp2d peak-total/512 and drops, and quads-peak/512. This directly answers: how much does trace I/O cost now (should be ~0 with `FRAME_TRACE_PATH` unset), how close to the 512-prim/512-quad caps each condition gets, and how much time the overlay submission itself costs.
- `ls -l /tmp/3sx-frame-trace.log` during conditions 2/4 only if `FRAME_TRACE_PATH` is deliberately set — confirms the opt-in gate (12.1) is doing its job when unset (file should not exist/grow).
- Any `２Ｄポリゴン…` line in stderr/log during condition 5 — now rate-limited to at most once per `njdp2d_draw()` drain (up to twice per game frame) (12.2) instead of once per dropped prim.

Compare each condition's `[perf_p3]`/`[perf_avg]` numbers against this phase's baseline (pre-P1..P7, commit `e37d6208`) to quantify the win; the finer per-subsystem splits (`Game_GetPerfGameLogicNs/SpriteSubmitNs/DispatchNs`, `Mtrans_GetPerfTexRenewNs/SprSubmitNs`) remain available to rule out interaction with the texture-refresh/dirty-rect path.

---

### Aggregate Impact

| Metric | Before | After | Improvement |
|--------|--------|-------|-------------|
| Heavy stage FPS (Remy, 800MHz) | ~28 | ~60 (stable) | +114% |
| Character select FPS (800MHz) | ~40 | ~60 | +50% |
| Texture refresh time (char select) | ~7ms | ~1ms | -86% |
| Super art burst FPS | ~20-30 | ~45-50 | +67-100% |
| Super art sustained FPS | ~35-40 | ~50-55 | +37-43% |
| Tile cache lookup (per frame) | 2-6ms (linear) | <0.1ms (hash) | -97% |

### Key Files

| File | Role |
|------|------|
| `src/port/sdl/sdl_game_renderer.c` | Software rasterizer, render task management, texture cache |
| `src/port/sdl/software_frame_non_integer.c` | Non-integer gather rasterizer (hot path) |
| `src/port/sdl/native_video_writer.c` | FPGA native video DDR3 direct-write present path |
| `src/port/sdl/fps_overlay_compositor.c` | FPS overlay rasterizer (`FPSOverlay_*` API) |
| `src/sf33rd/Source/Common/PPGFile.c` | PPG texture system, dirty-rect tracking |
| `src/sf33rd/Source/Game/rendering/mtrans.c` | MTS tile cache lookups, sprite dispatch |
| `src/sf33rd/Source/Game/rendering/mts_hash.h` | Hash table + free-list helpers |
| `src/sf33rd/Source/Game/rendering/texcash.c` | Tile cache allocation and lifecycle |
| `src/sf33rd/Source/Game/stage/bg.c` | Background tile rendering, fast path |
| `src/sf33rd/Source/Game/rendering/dc_ghost.c` | Matrix operations, ghost sprites |
| `src/port/sdl/sdl_app.c` | Frame pacing, clock management, perf timers |
