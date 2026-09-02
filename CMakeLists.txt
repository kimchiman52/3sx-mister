cmake_minimum_required(VERSION 3.24)
project(3s-arm LANGUAGES C CXX)

include(${PROJECT_SOURCE_DIR}/cmake/imgui_sources.cmake)

# ======================================
# Compiler cache
# ======================================
#
# ccache is wired as a compiler *launcher* rather than by pointing
# CMAKE_C_COMPILER at a ccache shim. That distinction is what keeps it
# compatible with this project's provenance guards:
#
#   - The compiler CMake probes, records in CMakeCache.txt and reports in the
#     build log is still the real clang, so nothing about the toolchain
#     identity becomes indirect.
#   - Every -D stays on the command line ccache hashes. MIST_BUILD_HASH is
#     attached to src/netplay/mist_handshake.c and src/netplay/netplay.c as a
#     COMPILE_DEFINITION (see the MIST_BUILD_HASH_OVERRIDE block below), so a
#     build of a different commit presents a different command line for
#     exactly those two files and cannot hit an entry cached for another
#     commit. Files that do not carry the hash are content-identical between
#     those commits and reusing their objects is correct.
#   - ccache's default sloppiness is deliberately left alone. In particular
#     `time_macros` is NOT set, so a translation unit using __DATE__/__TIME__
#     is never served from cache.
#
# The generated cmake/port_build_config.h.in carries feature flags only and no
# build identity, so it does not force a full rebuild between commits.
#
# Must be set before the first add_executable(): a target captures
# CMAKE_<LANG>_COMPILER_LAUNCHER at creation time.
option(ENABLE_CCACHE "Use ccache as the compiler launcher when it is available" ON)
if(ENABLE_CCACHE)
    find_program(CCACHE_PROGRAM ccache)
    if(CCACHE_PROGRAM)
        set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
        set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
        message(STATUS "ccache: enabled (${CCACHE_PROGRAM})")
    else()
        message(STATUS "ccache: requested but not found; building uncached")
    endif()
else()
    message(STATUS "ccache: disabled by -DENABLE_CCACHE=OFF")
endif()

# ======================================
# Options and platform setup
# ======================================

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

option(PORT_MISTER "Enable MiSTer-oriented build profile" OFF)
option(ENABLE_PERF_TELEMETRY "Enable perf telemetry capture and perf-focused CLI surface" ON)

# Task #64 measurement instrument (src/test/texgroup_window_probe.c).
# OFF by default and it must stay that way: the probe keeps file-scope
# counters that land inside the rollback-determinism harness's hashed
# data image, so an ON build reports them as DIVERGENT symbols and turns
# `tools/rollback-determinism/run.sh` red for reasons that have nothing
# to do with the simulation. Turn it on only for a measurement run:
#   cmake -B build/host -DENABLE_TEXGROUP_WINDOW_PROBE=ON
option(ENABLE_TEXGROUP_WINDOW_PROBE
       "Instrument the texgroup case-2 reclaim window and getObjectHeight ok==0 returns (task #64)" OFF)
# Stage A of docs/plan-fcade-replay-browser.md — replay-fidelity harness.
# When ON, the STATCHECK compile define lights up src/test/ram_archive.c and
# src/test/scrd_game.c (both `#if STATCHECK`), the `--ram-archive` CLI arg,
# and the statcheck smoke path in main(). Orthogonal to ENABLE_NETPLAY /
# ENABLE_PERF_TELEMETRY. OFF by default so normal builds compile no STATCHECK
# code. Incompatible with a Debug config: the fork's DEBUG-gated test harness
# (src/test/replay_game.{c,h}) declares ReplayGame/ReplayGame_Destroy, which
# are the upstream STATCHECK symbols this port renames (ScrdGame_*) precisely
# to avoid a collision — but building both harnesses at once is unsupported,
# so a Debug + THREESX_STATCHECK configure is a hard error (see below).
option(THREESX_STATCHECK "Build the statcheck replay-fidelity harness" OFF)

set(_ENABLE_NETPLAY_DEFAULT ON)
set(_ENABLE_ISO_IMPORT_DEFAULT ON)
set(_ENABLE_FFMPEG_ADX_DEFAULT ON)
set(_ENABLE_SDL_DIALOGS_DEFAULT ON)
set(_ENABLE_MISTER_ARM_HARDENING_DEFAULT OFF)

if(PORT_MISTER)
    # Netplay stays ON for MiSTer. Direct-P2P + bilateral hole-punch is a
    # shipping feature, and a netplay-OFF MiSTer package silently omits
    # libminiupnpc — which a subsequent `--delete` deploy then removes from the
    # device (the 2026-07-25 incident). We never want a netplay-off MiSTer
    # build; the exceptional case must pass -DENABLE_NETPLAY=OFF explicitly.
    set(_ENABLE_ISO_IMPORT_DEFAULT OFF)
    set(_ENABLE_FFMPEG_ADX_DEFAULT OFF)
    set(_ENABLE_SDL_DIALOGS_DEFAULT OFF)
    set(_ENABLE_MISTER_ARM_HARDENING_DEFAULT ON)
endif()

option(ENABLE_NETPLAY "Enable netplay and networking dependencies" ${_ENABLE_NETPLAY_DEFAULT})
option(ENABLE_ISO_IMPORT "Enable runtime ISO import flow via libcdio" ${_ENABLE_ISO_IMPORT_DEFAULT})
option(ENABLE_FFMPEG_ADX "Enable FFmpeg-backed ADX decode backend" ${_ENABLE_FFMPEG_ADX_DEFAULT})
option(ENABLE_SDL_DIALOGS "Enable SDL native dialogs and message boxes" ${_ENABLE_SDL_DIALOGS_DEFAULT})
option(ENABLE_MISTER_ARM_HARDENING
       "Enable conservative ARMv7 hard-float/NEON flags for MiSTer-target builds"
       ${_ENABLE_MISTER_ARM_HARDENING_DEFAULT})

# Step 6 of docs/plan-bilateral-hole-punch.md — function-pointer seam in
# direct_p2p.c so test_bilateral_punch.c can mock Stun_HolePunch /
# Rendezvous_Send. OFF by default; turn ON for the test build alongside
# -DCMAKE_C_FLAGS=-DENABLE_NETPLAY_TESTS. With OFF, direct_p2p.c emits
# byte-identical machine code to pre-Step-6 (no indirect calls).
option(NETPLAY_TEST_HOOKS "Compile in test seams for direct_p2p.c (Step 6)" OFF)

# Task #116 -- decouple the deterministic-harness code from the optimisation
# level.
#
# The scripted harnesses (src/test/test_runner.c:1 and src/test/input_script.c:1
# are whole-file `#if defined(DEBUG)`, and --test-pin-rng's effect lives behind
# the same gate at src/sf33rd/Source/Game/game.c:373) only exist in a Debug
# build, and Debug is -O0. Simply adding -DDEBUG to a Release build does not
# work: `${IMGUI_SOURCES}` below is added to the target only under
# $<CONFIG:Debug>, so the link fails on the cImGui_ImplSDL3_* symbols the
# DEBUG blocks in src/port/sdl/sdl_app.c reference. The practical effect was
# that every cross-architecture determinism sweep could only ever run at -O0,
# while the configuration that actually ships is -O3.
#
# The optimisation level is not neutral for cross-architecture agreement. This
# tree passes no -ffp-contract flag, so clang's default `on` applies, and the
# measured behaviour of `float t = a * b; return t + c;` (two statements)
# differs by level: at -O0 both targets emit an unfused multiply then add
# (arm64 `fmul`/`fadd`, armv7 `vmul.f32`/`vadd.f32`), but at -O3 armv7
# -mcpu=cortex-a9 -mfpu=neon-vfpv3 contracts the pair into a single
# `vmla.f32` while arm64 leaves `fmul`/`fadd` in place. An -O0 sweep cannot
# observe that class of difference at all.
#
# ENABLE_DEBUG_HOOKS compiles exactly the same DEBUG code paths at whatever
# optimisation level the configuration selects, so an -O3 sweep runs the same
# program the -O0 sweep ran and the only variable is the optimiser.
#
# It is OFF by default and must stay that way. With OFF, `${DEBUG_HOOKS_GENEX}`
# below is $<OR:$<CONFIG:Debug>,$<BOOL:OFF>>, which evaluates identically to
# the bare $<CONFIG:Debug> each of its three use sites had before, so shipped
# builds are bit-for-bit unaffected.
option(ENABLE_DEBUG_HOOKS
       "Compile the #if DEBUG debug/harness code (and ImGui) into a non-Debug configuration (task #116)"
       OFF)
set(DEBUG_HOOKS_GENEX "$<OR:$<CONFIG:Debug>,$<BOOL:${ENABLE_DEBUG_HOOKS}>>")

# pr-243 ARM Linux display backend (fbdev + DRM/KMS) for non-MiSTer ARM Linux
# targets (Pi-class, handheld Linux, etc.). Mutually exclusive with PORT_MISTER.
# libdrm-dev must be available at build time on the target sysroot (libdrm +
# xf86drm headers).
option(PORT_ARM_FBDEV "Build pr-243's ARM Linux display backend (fbdev + DRM)" OFF)

# Miyoo Mini Plus / OnionOS port profile (SigmaStar SSD202D, Cortex-A7
# ARMv7 hard-float, Buildroot Buster glibc). Activates the MI_GFX
# hardware-blitter presenter at src/platform/app/arm/arm_display_mi_gfx.c
# and a 16bpp RGB565 software canvas. Mutually exclusive with
# PORT_MISTER and PORT_ARM_FBDEV.
option(PORT_MIYOO_MINI_PLUS "Build for Miyoo Mini Plus / OnionOS (MI_GFX presenter)" OFF)

if(PORT_ARM_FBDEV AND PORT_MISTER)
    message(FATAL_ERROR "PORT_ARM_FBDEV and PORT_MISTER are mutually exclusive.")
endif()

if(PORT_MIYOO_MINI_PLUS AND (PORT_MISTER OR PORT_ARM_FBDEV))
    message(FATAL_ERROR "PORT_MIYOO_MINI_PLUS is mutually exclusive with PORT_MISTER and PORT_ARM_FBDEV.")
endif()

# Symbol-collision guard (§2.5): the DEBUG harness and the STATCHECK harness
# both define ReplayGame_Destroy / ScrdGame_Destroy lineage code and are not
# meant to coexist in one binary. Fail loudly at configure time rather than
# hand the user a confusing link/compile error.
if(THREESX_STATCHECK AND CMAKE_BUILD_TYPE STREQUAL "Debug")
    message(FATAL_ERROR
        "THREESX_STATCHECK is incompatible with a Debug build: the Debug config "
        "compiles the fork's DEBUG-gated test harness (src/test/replay_game.*), "
        "which collides with the STATCHECK replay harness. Use a non-Debug "
        "CMAKE_BUILD_TYPE (e.g. Release) with -DTHREESX_STATCHECK=ON.")
endif()

message(STATUS "PORT_MISTER=${PORT_MISTER}")
message(STATUS "ENABLE_PERF_TELEMETRY=${ENABLE_PERF_TELEMETRY}")
message(STATUS "ENABLE_TEXGROUP_WINDOW_PROBE=${ENABLE_TEXGROUP_WINDOW_PROBE}")
message(STATUS "THREESX_STATCHECK=${THREESX_STATCHECK}")
message(STATUS "ENABLE_NETPLAY=${ENABLE_NETPLAY}")
message(STATUS "ENABLE_ISO_IMPORT=${ENABLE_ISO_IMPORT}")
message(STATUS "ENABLE_FFMPEG_ADX=${ENABLE_FFMPEG_ADX}")
message(STATUS "ENABLE_SDL_DIALOGS=${ENABLE_SDL_DIALOGS}")
message(STATUS "ENABLE_MISTER_ARM_HARDENING=${ENABLE_MISTER_ARM_HARDENING}")
message(STATUS "NETPLAY_TEST_HOOKS=${NETPLAY_TEST_HOOKS}")
message(STATUS "ENABLE_DEBUG_HOOKS=${ENABLE_DEBUG_HOOKS}")
message(STATUS "PORT_ARM_FBDEV=${PORT_ARM_FBDEV}")
message(STATUS "PORT_MIYOO_MINI_PLUS=${PORT_MIYOO_MINI_PLUS}")

# ======================================
# Include directories
# ======================================

include_directories(
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/src/imgui/imgui
    ${PROJECT_SOURCE_DIR}/src/imgui/dcimgui
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/include/sdk
)

# ======================================
# Source collection
# ======================================

file(GLOB_RECURSE GAME_SRC CONFIGURE_DEPENDS src/*.c)
list(FILTER GAME_SRC EXCLUDE REGEX "^.*/src/netplay/netplay_stub\\.c$")
# Task #157: src/launcher/ holds the desktop netplay launcher — a separate
# executable with its own main(); it must never be compiled into the game.
list(FILTER GAME_SRC EXCLUDE REGEX "^.*/src/launcher/.*\\.c$")

if(NOT ENABLE_NETPLAY)
    list(FILTER GAME_SRC EXCLUDE REGEX "^.*/src/netplay/.*\\.c$")
    list(APPEND GAME_SRC
        "${CMAKE_SOURCE_DIR}/src/netplay/netplay_stub.c"
    )
endif()

add_executable(3s-arm MACOSX_BUNDLE
    ${GAME_SRC}
    $<${DEBUG_HOOKS_GENEX}:${IMGUI_SOURCES}>
)

# The rollback-determinism capture (src/test/rollback_determinism.c) hashes
# the entire ~17 MB writable data/bss image once per frame. At Debug -O0 the
# byte loop is ~10x slower, which multiplies the whole harness runtime, so
# compile just this TU with -O2 in every configuration. Semantics are
# unaffected: the TU is self-contained bookkeeping, not simulation code.
set_source_files_properties(src/test/rollback_determinism.c
    PROPERTIES COMPILE_OPTIONS "-O2")

# R-1: wire the MIST netplay handshake's build_hash to the real git short
# SHA, derived at configure time. Falls back to mist_handshake.c's baked-in
# "0000000" when git or the repo is unavailable (tarball builds, containers
# without .git access). Scoped as a per-source define so a SHA change only
# recompiles mist_handshake.c, not the whole tree. The extra -c
# safe.directory covers builds where the repo is owned by a different uid
# ("dubious ownership").
#
# MIST_BUILD_HASH_OVERRIDE: the MiSTer Docker cross-build rsyncs the source
# tree into the container WITHOUT .git (tools/mister/build-game.sh), so the
# git probe below cannot work there. build-game.sh derives the SHA on the
# host and passes it in explicitly via this cache variable.
set(MIST_BUILD_HASH_OVERRIDE "" CACHE STRING
    "Explicit git short SHA for the MIST handshake build_hash (for builds without .git access)")
set(MIST_BUILD_HASH "${MIST_BUILD_HASH_OVERRIDE}")
if(MIST_BUILD_HASH STREQUAL "")
    find_package(Git QUIET)
    if(GIT_FOUND)
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" -c "safe.directory=${CMAKE_SOURCE_DIR}"
                    -C "${CMAKE_SOURCE_DIR}" rev-parse --short=7 HEAD
            OUTPUT_VARIABLE MIST_BUILD_HASH
            OUTPUT_STRIP_TRAILING_WHITESPACE
            RESULT_VARIABLE _mist_git_rc
            ERROR_QUIET)
        if(NOT _mist_git_rc EQUAL 0)
            set(MIST_BUILD_HASH "")
        endif()
    endif()
endif()
if(MIST_BUILD_HASH STREQUAL "")
    message(STATUS "MIST_BUILD_HASH: git unavailable, using fallback 0000000")
else()
    message(STATUS "MIST_BUILD_HASH=${MIST_BUILD_HASH}")
    # #44: netplay.c stamps the same hash into the header of
    # <PrefPath>logs/netplay-report.txt, the one file a tester sends back.
    # It must be THIS macro and not a second source of build identity: a
    # report that disagrees with the MIST handshake's build_hash about
    # which binary produced it is worse than no report. netplay.c keeps an
    # #ifdef fallback so removing it here degrades to build=unknown rather
    # than failing to compile.
    set_property(SOURCE "${CMAKE_SOURCE_DIR}/src/netplay/mist_handshake.c"
                        "${CMAKE_SOURCE_DIR}/src/netplay/netplay.c"
                 APPEND PROPERTY COMPILE_DEFINITIONS
                 MIST_BUILD_HASH="${MIST_BUILD_HASH}")
endif()

configure_file(
    "${CMAKE_SOURCE_DIR}/cmake/port_build_config.h.in"
    "${CMAKE_BINARY_DIR}/generated/port/build_config.h"
)

target_include_directories(3s-arm PRIVATE
    "${CMAKE_BINARY_DIR}/generated"
)

# ======================================
# Compiler and linker flags
# ======================================

target_compile_definitions(3s-arm PRIVATE
    $<${DEBUG_HOOKS_GENEX}:DEBUG>

    $<$<CONFIG:Release>:RELEASE>
    # CHECKSUM = SHA256 verify SF33RD.AFS at startup. Skip on PORT_MISTER
    # (Cortex-A9 stock 800MHz is borderline) AND on PORT_MIYOO_MINI_PLUS
    # (Cortex-A7 @ 1.2GHz on slow SD takes 60+ seconds, looks like a hang).
    # Also suppressed under THREESX_STATCHECK (mirrors upstream:CMakeLists.txt:73):
    # a statcheck run must not stall on the SF33RD.AFS SHA256 verify.
    $<$<AND:$<CONFIG:Release>,$<NOT:$<OR:$<BOOL:${PORT_MISTER}>,$<BOOL:${PORT_MIYOO_MINI_PLUS}>,$<BOOL:${THREESX_STATCHECK}>>>>:CHECKSUM>

    _POSIX_C_SOURCE=200809L
)

# Feature toggles
target_compile_definitions(3s-arm PRIVATE
    # CPS3
)

if(WIN32)
    # include/structs.h defines a `Polygon` typedef (a vertex, per its own
    # FIXME) and Windows' wingdi.h declares a Polygon() function, which
    # every netplay TU pulls in via winsock2.h -> windows.h. NOGDI drops the
    # GDI declarations and the collision with them. Safe here because this
    # project renders through SDL and references no GDI type anywhere --
    # renaming the typedef instead would touch 9 files of decompiled code
    # to work around a header we do not use.
    target_compile_definitions(3s-arm PRIVATE NOGDI)
endif()

if(PORT_MISTER)
    target_compile_definitions(3s-arm PRIVATE PORT_MISTER)
    target_include_directories(3s-arm PRIVATE ${PROJECT_SOURCE_DIR}/vendor/Main_MiSTer)
endif()

if(ENABLE_NETPLAY)
    target_compile_definitions(3s-arm PRIVATE NETPLAY_ENABLED GEKKONET_STATIC GEKKONET_NO_ASIO ENABLE_NETPLAY)
endif()

if(NETPLAY_TEST_HOOKS)
    target_compile_definitions(3s-arm PRIVATE NETPLAY_TEST_HOOKS)
endif()

if(THREESX_STATCHECK)
    target_compile_definitions(3s-arm PRIVATE STATCHECK)
endif()

if(ENABLE_ISO_IMPORT)
    target_compile_definitions(3s-arm PRIVATE ENABLE_ISO_IMPORT)
endif()

if(ENABLE_FFMPEG_ADX)
    target_compile_definitions(3s-arm PRIVATE ENABLE_FFMPEG_ADX)
endif()

if(ENABLE_SDL_DIALOGS)
    target_compile_definitions(3s-arm PRIVATE ENABLE_SDL_DIALOGS)
endif()

target_compile_definitions(3s-arm PRIVATE
    CRS_VIDEO_DRIVER_SOFTWARE=1
    CRS_SW_INDEXED8_KERNEL_DEFAULT='b')
if(PORT_MISTER)
    # 16bpp RGB565 canvas — matches the FPGA DDR3 native_video_writer
    # 16bpp scanout. Other targets (Mac, PORT_ARM_FBDEV) use the default
    # 32bpp ARGB8888 canvas.
    target_compile_definitions(3s-arm PRIVATE CRS_SW_CANVAS_16BPP=1)
endif()

if(PORT_ARM_FBDEV)
    # pr-243 ARM Linux display backend. Files live under src/platform/app/arm/
    # and are gated by `#if CRS_APP_DRIVER_ARM` — they compile to empty TUs
    # on every other build profile.
    target_compile_definitions(3s-arm PRIVATE
        CRS_APP_DRIVER_ARM=1
        CRS_ARM_HAVE_DRM=1)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBDRM REQUIRED IMPORTED_TARGET libdrm)
    target_link_libraries(3s-arm PRIVATE PkgConfig::LIBDRM)
    message(STATUS "PORT_ARM_FBDEV=ON (pr-243 ARM Linux display backend; fbdev + DRM)")
endif()

if(PORT_MIYOO_MINI_PLUS)
    # Miyoo Mini Plus profile: SigmaStar MI_GFX hardware-blitter presenter
    # at src/platform/app/arm/arm_display_mi_gfx.c. Source canvas is
    # RGB565 to match SDL3's giblet 16bpp canvas (CRS_SW_CANVAS_16BPP=1
    # is set here, NOT in the shared block above — that block is
    # PORT_MISTER-only). The SigmaStar BSP headers and prebuilt libs
    # are vendored at vendor/sigmastar/{inc,lib}/. The deployed package
    # does NOT bundle libmi_*.so; runtime resolves them from the
    # device's /config/lib/ provided by the OnionOS firmware BSP.
    target_compile_definitions(3s-arm PRIVATE
        CRS_APP_DRIVER_ARM=1
        CRS_ARM_HAVE_MI_GFX=1
        CRS_SW_CANVAS_16BPP=1
        PORT_MIYOO_MINI_PLUS)
    target_include_directories(3s-arm PRIVATE
        "${PROJECT_SOURCE_DIR}/vendor/sigmastar/inc")
    # Link against the bare `libmi_*.so` filenames. The Miyoo Mini Plus
    # OnionOS BSP exposes the runtime libs at `/config/lib/libmi_*.so`
    # (verified on a v0.4-class Onion install via on-device probe) — no
    # `.so.0` SONAME suffix on either the on-disk filenames or the
    # working community ports' NEEDED entries. The vendored link-time
    # stubs in vendor/sigmastar/lib/ have no DT_SONAME, so the linker
    # writes the file basename `libmi_sys.so` (etc.) into NEEDED, which
    # matches what the runtime loader will find under /config/lib/.
    # libmi_ao.so is added for the direct MI_AO audio backend at
    # src/port/sound/mi_ao_backend.c. Same link-time/runtime split as
    # the other libmi_*.so: the vendored stub satisfies the link
    # against MI_AO_* symbols and the runtime loader resolves
    # libmi_ao.so from /config/lib/ on the device.
    target_link_libraries(3s-arm PRIVATE
        "${PROJECT_SOURCE_DIR}/vendor/sigmastar/lib/libmi_sys.so"
        "${PROJECT_SOURCE_DIR}/vendor/sigmastar/lib/libmi_gfx.so"
        "${PROJECT_SOURCE_DIR}/vendor/sigmastar/lib/libmi_common.so"
        "${PROJECT_SOURCE_DIR}/vendor/sigmastar/lib/libmi_ao.so")
    # pthread for the MI_AO audio backend's dedicated send thread.
    # SDL3 already pulls libpthread transitively, but we link
    # explicitly so the dependency is reflected in NEEDED entries
    # regardless of SDL3 build options.
    find_package(Threads REQUIRED)
    target_link_libraries(3s-arm PRIVATE Threads::Threads)
    message(STATUS "PORT_MIYOO_MINI_PLUS=ON (MI_GFX presenter; direct MI_AO audio backend; SDL3 dummy video; SigmaStar SDK vendored at vendor/sigmastar/)")
endif()

if(CMAKE_C_COMPILER_ID STREQUAL "Clang" OR CMAKE_C_COMPILER_ID STREQUAL "AppleClang")
    set(DISABLED_WARNINGS
        -Wpointer-to-int-cast
        -Wno-deprecated-non-prototype
        -Wno-tautological-overlap-compare
        -Wno-gnu-folding-constant
        -Wno-c2x-extensions
        -Wno-incompatible-pointer-types
        -Wno-pointer-sign
    )
elseif(CMAKE_C_COMPILER_ID STREQUAL "GNU")
    set(DISABLED_WARNINGS
        -Wno-unused-but-set-variable
        -Wno-pointer-sign
        -Wno-parentheses
        -Wno-incompatible-pointer-types
        -Wno-discarded-qualifiers
        -Wno-format-overflow
        -Wno-comment
        -Wno-maybe-uninitialized
        -Wno-array-bounds
        -Wno-aggressive-loop-optimizations
        -Wno-stringop-overflow
        -Wno-restrict
        -Wno-stringop-truncation
    )
endif()

target_compile_options(3s-arm PRIVATE
    -Wall
    -Werror

    # An undefined identifier in an #if is not an error in C -- it folds to 0
    # silently, so a feature gate that is misspelled, renamed, or simply never
    # reached its compile definition goes permanently dead with no diagnostic
    # at all. Two shipped bugs in this tree had exactly that shape: task #62
    # (ENABLE_PERF_TELEMETRY blocks dead in mtrans.c/pls03.c) and task #58
    # (Screen_Mode left UI-unreachable). -Wundef is the warning that catches
    # the class, and under -Werror above it is a build failure rather than a
    # line of scrollback. Task #67.
    #
    # Consequence for new code: test an OPTIONAL macro with #ifdef/defined(),
    # and test a macro that is always defined to 0 or 1 -- everything coming
    # out of cmake/port_build_config.h.in via #cmakedefine01 -- with a bare
    # #if, making sure the translation unit actually includes the header that
    # defines it. The two forms are not interchangeable; see the comment at
    # the top of src/sf33rd/Source/Game/io/gd3rd.h for the case where getting
    # that wrong silently dropped a declaration.
    -Wundef

    -fno-strict-aliasing

    ${DISABLED_WARNINGS}

    $<${DEBUG_HOOKS_GENEX}:-Wno-unused-function>
    $<${DEBUG_HOOKS_GENEX}:-Wno-unused-variable>
    $<${DEBUG_HOOKS_GENEX}:-Wno-unused-but-set-variable>
)

if(PORT_MISTER AND ENABLE_MISTER_ARM_HARDENING)
    string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _mister_system_processor_lc)
    string(TOLOWER "${CMAKE_C_COMPILER_TARGET}" _mister_compiler_target_lc)
    set(_mister_armv7_target_detected OFF)

    if((_mister_compiler_target_lc MATCHES "armv7") OR (_mister_compiler_target_lc MATCHES "arm-linux-gnueabihf"))
        set(_mister_armv7_target_detected ON)
    elseif((_mister_system_processor_lc MATCHES "armv7") OR (_mister_system_processor_lc MATCHES "armhf"))
        set(_mister_armv7_target_detected ON)
    endif()

    if((CMAKE_C_COMPILER_ID STREQUAL "Clang") OR (CMAKE_C_COMPILER_ID STREQUAL "AppleClang") OR
       (CMAKE_C_COMPILER_ID STREQUAL "GNU"))
        if(_mister_armv7_target_detected)
            target_compile_options(3s-arm PRIVATE
                -mcpu=cortex-a9
                -mfpu=neon-vfpv3
                -mfloat-abi=hard
            )
            target_link_options(3s-arm PRIVATE
                -mcpu=cortex-a9
                -mfpu=neon-vfpv3
                -mfloat-abi=hard
            )
            message(STATUS "MiSTer ARM hardening flags enabled: -mcpu=cortex-a9 -mfpu=neon-vfpv3 -mfloat-abi=hard")
        else()
            message(WARNING
                    "PORT_MISTER hardening requested but ARMv7 hard-float target not detected "
                    "(system='${CMAKE_SYSTEM_PROCESSOR}', "
                    "compiler_target='${CMAKE_C_COMPILER_TARGET}'). Skipping ARM hardening flags.")
        endif()
    else()
        message(WARNING "PORT_MISTER ARM hardening supports Clang/GNU compilers only. Skipping hardening flags.")
    endif()
endif()

# Miyoo Mini Plus hardening — Cortex-A7 (vs MiSTer's Cortex-A9) with
# VFPv4 NEON. The FPU spec differs from MiSTer because Cortex-A7's
# integrated FPU is VFPv4-D32, not VFPv3 like Cortex-A9. -mfloat-abi=hard
# is required to link against the Buildroot hardfloat sysroot. These
# flags are unconditional under PORT_MIYOO_MINI_PLUS (no
# ENABLE_MIYOO_HARDENING toggle in Cut 1).
if(PORT_MIYOO_MINI_PLUS)
    if((CMAKE_C_COMPILER_ID STREQUAL "Clang") OR (CMAKE_C_COMPILER_ID STREQUAL "AppleClang") OR
       (CMAKE_C_COMPILER_ID STREQUAL "GNU"))
        target_compile_options(3s-arm PRIVATE
            -mcpu=cortex-a7
            -mfpu=neon-vfpv4
            -mfloat-abi=hard
        )
        target_link_options(3s-arm PRIVATE
            -mcpu=cortex-a7
            -mfpu=neon-vfpv4
            -mfloat-abi=hard
        )
        message(STATUS "Miyoo ARM hardening flags enabled: -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard")
    endif()

    # The Miyoo container ships clang-13 (no `-Wdeprecated-non-prototype`).
    # Scope the meta-suppression to the Miyoo profile so Mac/MiSTer
    # builds still surface unknown-warning typos as errors. Both Clang
    # and AppleClang variants accept the option silently.
    if((CMAKE_C_COMPILER_ID STREQUAL "Clang") OR (CMAKE_C_COMPILER_ID STREQUAL "AppleClang"))
        target_compile_options(3s-arm PRIVATE -Wno-unknown-warning-option)
    endif()
endif()

# MiSTer / Miyoo Release: aggressive optimisation — -O3 (cross-build).
# Note: -flto=thin was removed because it triggers a linker segfault in
# clang-20 during the ARM cross-build (Debian bullseye toolchain).
# Cortex-A7 is slower than Cortex-A9; -O3 is desired on Miyoo too.
if(PORT_MISTER OR PORT_MIYOO_MINI_PLUS)
    target_compile_options(3s-arm PRIVATE
        $<$<CONFIG:Release>:-O3>
    )
    target_link_options(3s-arm PRIVATE
        $<$<CONFIG:Release>:-O3>
    )
endif()

set_target_properties(3s-arm PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/cmake/Info.plist"
)

if(APPLE)
    set_target_properties(3s-arm PROPERTIES
        OUTPUT_NAME "3S-ARM"
        MACOSX_BUNDLE_BUNDLE_NAME "3S-ARM"
        INSTALL_RPATH "@executable_path/../Frameworks"
    )
elseif(UNIX AND NOT APPLE)
    set_target_properties(3s-arm PROPERTIES
        INSTALL_RPATH "\$ORIGIN/../lib"
    )
endif()

# ======================================
# Dependencies
# ======================================

set(THIRD_PARTY_DIR "${CMAKE_SOURCE_DIR}/third_party")
find_package(ZLIB REQUIRED)

# Vendored cJSON — first wired in Step C2 of
# docs/plan-fcade-replay-browser.md to parse the .meta.json replay sidecar.
#
# Task #80: this used to read third_party/cJSON/cJSON.c. third_party/ is
# gitignored and build-deps.sh has no cJSON recipe, so nothing ever created
# that file -- it only existed because it was placed there by hand, and a
# clean clone therefore failed at configure time with "Cannot find source
# file". The identical source is already tracked in this repo at
# vendor/Main_MiSTer/cJSON.{c,h} (verified byte-identical, md5
# 630deb131a520d63b8913492d982b589 / a9c6c607caeee91e5fe70dfd349b4ddc), so
# using the tracked copy makes the build self-provisioning without adding a
# network fetch that could fail the way the FreeType tarball fetch did.
#
# The files are staged into the build tree rather than compiled in place
# because the include directory is PUBLIC and therefore lands on 3s-arm's
# header search path. vendor/Main_MiSTer/ holds 46 headers with generic
# names -- audio.h, debug.h, cfg.h, menu.h -- and exposing that whole
# directory would let them shadow the game's own headers. Staging exports
# exactly cJSON.h and nothing else.
set(CJSON_VENDOR_DIR "${CMAKE_SOURCE_DIR}/vendor/Main_MiSTer")
set(CJSON_STAGE_DIR "${CMAKE_BINARY_DIR}/cjson")
configure_file("${CJSON_VENDOR_DIR}/cJSON.c" "${CJSON_STAGE_DIR}/cJSON.c" COPYONLY)
configure_file("${CJSON_VENDOR_DIR}/cJSON.h" "${CJSON_STAGE_DIR}/cJSON.h" COPYONLY)

# Built as its own static library so the fork's strict `-Wall -Werror` (set
# on the 3s-arm target, not globally) does not apply to the vendored source:
# `-w` scopes a full warning-suppression to this TU only, per the plan's "do
# NOT edit vendored source" constraint. Its PUBLIC include dir propagates
# cJSON.h to 3s-arm via the link below.
add_library(cjson STATIC "${CJSON_STAGE_DIR}/cJSON.c")
target_include_directories(cjson PUBLIC "${CJSON_STAGE_DIR}")
target_compile_options(cjson PRIVATE -w)

set(FFMPEG_ROOT        "${THIRD_PARTY_DIR}/ffmpeg/build")
if(PORT_MIYOO_MINI_PLUS)
    # Miyoo Mini Plus uses a Buildroot Buster glibc cross-build of SDL3
    # at third_party/sdl3-miyoo/ (separate from the MiSTer Bullseye
    # cross-build at third_party/sdl3/). Built by build-deps.sh
    # --profile miyoo.
    set(SDL3_ROOT      "${THIRD_PARTY_DIR}/sdl3-miyoo/build")
else()
    set(SDL3_ROOT      "${THIRD_PARTY_DIR}/sdl3/build")
endif()
set(LIBCDIO_ROOT       "${THIRD_PARTY_DIR}/libcdio/build")
set(GEKKONET_ROOT      "${THIRD_PARTY_DIR}/GekkoNet/build")
set(SDL3_NET_ROOT      "${THIRD_PARTY_DIR}/SDL_net/build")
set(MINIZIP_NG_ROOT    "${THIRD_PARTY_DIR}/minizip-ng/build")
set(TF_PSA_CRYPTO_ROOT "${THIRD_PARTY_DIR}/tf-psa-crypto/build")

include_directories(
    ${SDL3_ROOT}/include
    ${MINIZIP_NG_ROOT}/include
    ${TF_PSA_CRYPTO_ROOT}/include
)

if(ENABLE_FFMPEG_ADX)
    include_directories(${FFMPEG_ROOT}/include)
endif()

if(ENABLE_ISO_IMPORT)
    include_directories(${LIBCDIO_ROOT}/include)
endif()

if(ENABLE_NETPLAY)
    include_directories(
        ${GEKKONET_ROOT}/include
        ${SDL3_NET_ROOT}/include
    )
    # Direct-P2P Step 4 (docs/plan-stun-direct-p2p.md): miniupnpc for UPnP
    # port forwarding. pkg-config in the armhf cross sysroot provides the
    # right -I / -l paths via PkgConfig::MINIUPNPC. HAVE_UPNP is set here
    # so src/netplay/upnp.c compiles the real backend (not the no-op stub).
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(MINIUPNPC REQUIRED IMPORTED_TARGET miniupnpc)
endif()

target_link_libraries(3s-arm PRIVATE
    m
    cjson
    "${MINIZIP_NG_ROOT}/lib/libminizip-ng.a"
    "${TF_PSA_CRYPTO_ROOT}/lib/libtfpsacrypto.a"
    ZLIB::ZLIB
)

if(ENABLE_ISO_IMPORT)
    target_link_libraries(3s-arm PRIVATE
        "${LIBCDIO_ROOT}/lib/libiso9660.a"
        "${LIBCDIO_ROOT}/lib/libcdio.a"
    )
endif()

if(ENABLE_NETPLAY)
    target_link_libraries(3s-arm PRIVATE
        "${GEKKONET_ROOT}/lib/libGekkoNet.a"
        "${SDL3_NET_ROOT}/lib/libSDL3_net.a"
        PkgConfig::MINIUPNPC
    )
    # Direct-P2P Step 4: HAVE_UPNP flips src/netplay/upnp.c from the no-op
    # stubs to the miniupnpc-backed implementation. Gated inside the
    # ENABLE_NETPLAY block so a non-netplay build neither links miniupnpc
    # nor compiles upnp.c (glob filter excludes src/netplay/*.c above).
    target_compile_definitions(3s-arm PRIVATE HAVE_UPNP)

    # S7 (docs/plan-netplay-connection.md §9): src/netplay/natpmp.c is the
    # second mapping backend (NAT-PMP RFC 6886 / PCP RFC 6887). It is
    # picked up by the same GLOB_RECURSE CONFIGURE_DEPENDS src/*.c above
    # as upnp.c is, and there is DELIBERATELY nothing to link and nothing
    # to add to build-deps.sh: it is raw UDP on the existing socket API,
    # not libnatpmp. Please keep it that way.

endif()

if(APPLE)
    if(ENABLE_FFMPEG_ADX)
        target_link_libraries(3s-arm PRIVATE
            ${FFMPEG_ROOT}/lib/libavcodec.dylib
            ${FFMPEG_ROOT}/lib/libavformat.dylib
            ${FFMPEG_ROOT}/lib/libavutil.dylib
            ${FFMPEG_ROOT}/lib/libswresample.dylib
        )
    endif()

    target_link_libraries(3s-arm PRIVATE
        ${SDL3_ROOT}/lib/libSDL3.0.dylib
        iconv
    )
elseif(WIN32)
    if(ENABLE_FFMPEG_ADX)
        target_link_libraries(3s-arm PRIVATE
            ${FFMPEG_ROOT}/lib/libavcodec.dll.a
            ${FFMPEG_ROOT}/lib/libavformat.dll.a
            ${FFMPEG_ROOT}/lib/libavutil.dll.a
            ${FFMPEG_ROOT}/lib/libswresample.dll.a
        )
    endif()

    target_link_libraries(3s-arm PRIVATE
        ${SDL3_ROOT}/lib/libSDL3.dll.a
        iconv
        bcrypt
        dbghelp
        ws2_32
        iphlpapi
    )
    target_link_options(3s-arm PRIVATE
        "$<$<OR:$<CONFIG:Debug>,$<CONFIG:RelWithDebInfo>>:--for-linker>"
        "$<$<OR:$<CONFIG:Debug>,$<CONFIG:RelWithDebInfo>>:--pdb=3s-arm.pdb>"
    )
elseif(UNIX)
    if(ENABLE_FFMPEG_ADX)
        target_link_libraries(3s-arm PRIVATE
            ${FFMPEG_ROOT}/lib/libavcodec.so
            ${FFMPEG_ROOT}/lib/libavformat.so
            ${FFMPEG_ROOT}/lib/libavutil.so
            ${FFMPEG_ROOT}/lib/libswresample.so
        )
    endif()

    target_link_libraries(3s-arm PRIVATE
        ${SDL3_ROOT}/lib/libSDL3.so
    )
endif()

# The C++ runtime has to come LAST on the link line, after every static
# archive that might need it. GNU ld resolves archives strictly in order, so
# with stdc++ listed before libGekkoNet.a -- where it used to be, up with
# zlib and minizip -- GekkoNet's zpp serializer left a wall of undefined
# references to std::runtime_error and std::logic_error at link time on
# MinGW. macOS and the ARM cross-build never showed it: neither linker cares
# about archive order here, so this only ever surfaced on Windows.
target_link_libraries(3s-arm PRIVATE stdc++)

# ======================================
# Desktop netplay launcher (task #157)
# ======================================
#
# Second executable, desktop only: hosts/joins direct-P2P netplay by
# writing the handoff file and spawning the game (src/launcher/
# netplay_launcher.c has the full rationale). Everything it needs is
# already vendored — SDL3 for the window/spawn/clipboard, and three
# self-contained game TUs: room_code.c (pre-spawn code validation),
# csprng.c (room_code.c's only dependency), paths.c (so the pref dir
# — where it reads netplay.status and writes launcher.handoff — resolves
# IDENTICALLY to the game's, THIRDSARM_HOME override included), and
# keymap.c + config_helpers.c (so the key-binding screen edits the same
# table the game reads, defaults included, rather than a second copy).
# Zero new runtime dependencies by construction.
#
# Not built for the embedded ports: MiSTer hosts/joins through the OSD
# (which writes the same handoff file), and the Miyoo has no netplay
# use case for a second windowed process.
if(ENABLE_NETPLAY AND NOT PORT_MISTER AND NOT PORT_MIYOO_MINI_PLUS AND NOT PORT_ARM_FBDEV)
    add_executable(3s-netplay-launcher
        src/launcher/netplay_launcher.c
        src/netplay/room_code.c
        src/port/config/config_helpers.c
        src/port/config/keymap.c
        src/port/paths.c
        src/utils/csprng.c
    )
    target_compile_options(3s-netplay-launcher PRIVATE
        -Wall
        -Werror
        -Wundef
        ${DISABLED_WARNINGS}
    )
    if(APPLE)
        target_link_libraries(3s-netplay-launcher PRIVATE
            ${SDL3_ROOT}/lib/libSDL3.0.dylib
        )
        # Named apart from the game binary it lands beside in
        # Contents/MacOS, and matching cmake/Info.plist's CFBundleExecutable
        # byte for byte -- that key is what makes opening 3S-ARM.app show
        # this launcher rather than dropping the player straight into a
        # match they have no way to configure.
        set_target_properties(3s-netplay-launcher PROPERTIES
            OUTPUT_NAME "3S-ARM-Launcher"
            INSTALL_RPATH "@executable_path/../Frameworks;@executable_path"
        )
    elseif(WIN32)
        target_link_libraries(3s-netplay-launcher PRIVATE
            ${SDL3_ROOT}/lib/libSDL3.dll.a
        )
    elseif(UNIX)
        target_link_libraries(3s-netplay-launcher PRIVATE
            ${SDL3_ROOT}/lib/libSDL3.so
        )
        set_target_properties(3s-netplay-launcher PROPERTIES
            INSTALL_RPATH "\$ORIGIN/../lib;\$ORIGIN"
        )
    endif()
endif()

# ======================================
# Windows icon
# ======================================

# The Explorer counterpart of the macOS CFBundleIconFile wiring below. A
# Windows icon is not a file beside the exe -- it is a resource compiled INTO
# it, so both binaries the player can double-click need the .rc in their own
# source list. cmake/3s-arm.ico is generated from the same 1024x1024 art as
# cmake/3s-arm.icns, so the platforms cannot drift.
#
# enable_language(RC) is required: RC is not among the languages project()
# enables by default, and without it CMake treats the .rc as an unknown
# extension and silently links an icon-less exe rather than failing.
if(WIN32)
    enable_language(RC)

    # windres resolves a relative ICON path against its include path and the
    # working directory, and the working directory here is the build tree, not
    # cmake/. Whether binutils' windres would also find it on its own varies by
    # version, so the search path is stated rather than relied upon. Kept as an
    # include path rather than absolutising the path inside the .rc, so the .rc
    # stays readable by other resource compilers.
    set(CMAKE_RC_FLAGS "${CMAKE_RC_FLAGS} -I${CMAKE_SOURCE_DIR}/cmake")

    target_sources(3s-arm PRIVATE ${CMAKE_SOURCE_DIR}/cmake/3s-arm.rc)

    if(TARGET 3s-netplay-launcher)
        target_sources(3s-netplay-launcher PRIVATE ${CMAKE_SOURCE_DIR}/cmake/3s-arm.rc)
    endif()
endif()

# ======================================
# Installation
# ======================================

set(PROJECT_LICENSE_FILES
    "${CMAKE_SOURCE_DIR}/LICENSE"
    "${CMAKE_SOURCE_DIR}/THIRD_PARTY_NOTICES.txt"
)

if(APPLE)
    install(TARGETS 3s-arm
        RUNTIME DESTINATION bin
        BUNDLE DESTINATION .
    )

    # Task #157. On Windows and Linux the launcher is a second file next to
    # the game and double-clicking it works; a bare Mach-O has no such
    # affordance, so here the launcher is instead the .app's
    # CFBundleExecutable (cmake/Info.plist) and the game becomes the file it
    # spawns. One bundle, one icon to double-click, and the tools that run
    # Contents/MacOS/3S-ARM directly are untouched. Contents/MacOS is also
    # the only directory in which the launcher's own INSTALL_RPATH
    # (@executable_path/../Frameworks) reaches the Frameworks directory
    # libSDL3.0.dylib is staged into.
    if(TARGET 3s-netplay-launcher)
        install(TARGETS 3s-netplay-launcher
            RUNTIME DESTINATION 3S-ARM.app/Contents/MacOS
        )
    endif()

    if(ENABLE_FFMPEG_ADX)
        file(GLOB FFMPEG_DYLIBS "${FFMPEG_ROOT}/lib/*.dylib")
        install(FILES ${FFMPEG_DYLIBS} DESTINATION 3S-ARM.app/Contents/Frameworks)
    endif()

    install(FILES
        ${SDL3_ROOT}/lib/libSDL3.0.dylib
        DESTINATION 3S-ARM.app/Contents/Frameworks
    )

    # The Finder icon. cmake/Info.plist's CFBundleIconFile names this file by
    # basename; Contents/Resources is the only place Finder looks for it, and a
    # missing file degrades silently to the generic icon rather than failing the
    # build -- so if the icon ever disappears from a package, check both this
    # rule and that key before looking anywhere else.
    install(FILES
        ${CMAKE_SOURCE_DIR}/cmake/3s-arm.icns
        DESTINATION 3S-ARM.app/Contents/Resources
    )

    install(FILES
        ${PROJECT_LICENSE_FILES}
        DESTINATION 3S-ARM.app/Contents/Resources/licenses
    )

    # ------------------------------------------------------------------
    # Self-containment sweep for the installed .app.
    #
    # SDL3 and FFmpeg are vendored under third_party/ and built with
    # @rpath install names, so the install(FILES) rules above plus the
    # targets' INSTALL_RPATH are all they need. A dependency resolved
    # through pkg-config is a different story: Homebrew's miniupnpc bakes
    # an absolute LC_ID_DYLIB into the dylib, the linker copies that
    # verbatim into the game's LC_LOAD_DYLIB, and the shipped bundle then
    # only runs on a machine that has that exact path on disk. The UNIX
    # branch below already stages libminiupnpc.so* next to libSDL3.so*;
    # this is the macOS counterpart plus the step Linux does not need —
    # rewriting the referring load command, because copying a dylib into
    # the bundle does not change the name the referrer asks dyld for.
    #
    # Deliberately not keyed on miniupnpc. The sweep reads the installed
    # binaries' own load commands and relocates every absolute dependency
    # that is not a macOS system library, so the next non-vendored
    # dependency someone links is covered without editing this block, and
    # nothing here hardcodes a Homebrew prefix or a soversion.
    #
    # BundleUtilities' fixup_bundle() was the considered alternative and
    # was rejected: it re-resolves and rewrites the SDL3/FFmpeg @rpath
    # entries that already work today (a larger blast radius than the
    # defect), it wants a plugin list and a directory search path this
    # project has no need for, and its result is harder to assert on than
    # `otool -L` output. The scan below is the same idea narrowed to the
    # binaries actually shipped and to references that are provably broken
    # off the build machine.
    # ------------------------------------------------------------------
    find_program(OTOOL_EXECUTABLE otool REQUIRED)
    find_program(INSTALL_NAME_TOOL_EXECUTABLE install_name_tool REQUIRED)
    find_program(CODESIGN_EXECUTABLE codesign REQUIRED)

    install(CODE "set(_3sx_otool [[${OTOOL_EXECUTABLE}]])")
    install(CODE "set(_3sx_install_name_tool [[${INSTALL_NAME_TOOL_EXECUTABLE}]])")
    install(CODE "set(_3sx_codesign [[${CODESIGN_EXECUTABLE}]])")
    install(CODE [[
        string(ASCII 10 _3sx_nl)

        set(_3sx_app "${CMAKE_INSTALL_PREFIX}/3S-ARM.app")
        set(_3sx_frameworks "${_3sx_app}/Contents/Frameworks")

        # `otool -L` prints one "<install name> (compatibility version ...)"
        # line per LC_LOAD_DYLIB under a "<file>:" header. For a dylib the
        # first of those is its own LC_ID_DYLIB, which `otool -D` reports
        # separately so it can be filtered back out.
        function(_3sx_load_commands _bin _out)
            execute_process(COMMAND "${_3sx_otool}" -D "${_bin}"
                OUTPUT_VARIABLE _id
                OUTPUT_STRIP_TRAILING_WHITESPACE
                COMMAND_ERROR_IS_FATAL ANY
            )
            string(REPLACE "${_3sx_nl}" ";" _id_lines "${_id}")
            list(GET _id_lines -1 _self)
            string(STRIP "${_self}" _self)

            execute_process(COMMAND "${_3sx_otool}" -L "${_bin}"
                OUTPUT_VARIABLE _listing
                OUTPUT_STRIP_TRAILING_WHITESPACE
                COMMAND_ERROR_IS_FATAL ANY
            )
            string(REPLACE "${_3sx_nl}" ";" _lines "${_listing}")

            set(_found "")
            foreach(_line IN LISTS _lines)
                string(FIND "${_line}" " (compatibility version" _cut)
                if(_cut EQUAL -1)
                    continue()   # the "<file>:" header, or a blank line
                endif()
                string(SUBSTRING "${_line}" 0 ${_cut} _name)
                string(STRIP "${_name}" _name)
                if(_name STREQUAL _self)
                    continue()
                endif()
                list(APPEND _found "${_name}")
            endforeach()
            set(${_out} "${_found}" PARENT_SCOPE)
        endfunction()

        file(GLOB _3sx_queue "${_3sx_app}/Contents/MacOS/*")
        set(_3sx_seen "")
        set(_3sx_modified "")

        while(_3sx_queue)
            list(POP_FRONT _3sx_queue _3sx_bin)
            if("${_3sx_bin}" IN_LIST _3sx_seen)
                continue()
            endif()
            list(APPEND _3sx_seen "${_3sx_bin}")

            _3sx_load_commands("${_3sx_bin}" _3sx_deps)
            foreach(_dep IN LISTS _3sx_deps)
                # @rpath / @loader_path / @executable_path already resolve
                # inside the bundle; /usr/lib and /System are the OS and are
                # present on every Mac.
                if(NOT _dep MATCHES "^/")
                    continue()
                endif()
                if(_dep MATCHES "^/usr/lib/" OR _dep MATCHES "^/System/")
                    continue()
                endif()

                get_filename_component(_base "${_dep}" NAME)
                set(_staged "${_3sx_frameworks}/${_base}")

                if(NOT EXISTS "${_staged}")
                    if(NOT EXISTS "${_dep}")
                        message(FATAL_ERROR
                            "${_3sx_bin} loads ${_dep}, which is not on this machine")
                    endif()
                    file(INSTALL
                        DESTINATION "${_3sx_frameworks}"
                        TYPE SHARED_LIBRARY
                        FOLLOW_SYMLINK_CHAIN
                        FILES "${_dep}"
                    )
                    # Homebrew ships its dylibs read-only; install_name_tool
                    # rewrites in place and needs write permission.
                    file(CHMOD "${_staged}"
                        PERMISSIONS
                            OWNER_READ OWNER_WRITE OWNER_EXECUTE
                            GROUP_READ GROUP_EXECUTE
                            WORLD_READ WORLD_EXECUTE
                    )
                    execute_process(
                        COMMAND "${_3sx_install_name_tool}"
                                -id "@rpath/${_base}" "${_staged}"
                        COMMAND_ERROR_IS_FATAL ANY
                    )
                    list(APPEND _3sx_modified "${_staged}")
                    # Whatever the staged dylib itself loads has to come
                    # along too, or the bundle is self-contained by one
                    # level only.
                    list(APPEND _3sx_queue "${_staged}")
                endif()

                message(STATUS "Bundled ${_base}: ${_3sx_bin} now loads @rpath/${_base}")
                execute_process(
                    COMMAND "${_3sx_install_name_tool}"
                            -change "${_dep}" "@rpath/${_base}" "${_3sx_bin}"
                    COMMAND_ERROR_IS_FATAL ANY
                )
                list(APPEND _3sx_modified "${_3sx_bin}")
            endforeach()
        endwhile()

        # Editing a Mach-O load command invalidates the code signature over
        # it. install_name_tool regenerates the ad-hoc signature the linker
        # put on our own binaries, but it cannot regenerate the one Homebrew
        # ships on libminiupnpc.21.dylib -- it prints "changes being made to
        # the file will invalidate the code signature" and leaves it broken.
        # On Apple silicon a broken signature is not cosmetic: dyld SIGKILLs
        # the process before main(), with no diagnostic on stderr at all
        # (observed: exit 137, zero output). Re-sign ad-hoc, which is the
        # same signature class the unsigned .app already ships with.
        list(REMOVE_DUPLICATES _3sx_modified)
        foreach(_bin IN LISTS _3sx_modified)
            execute_process(
                COMMAND "${_3sx_codesign}" --force --sign - "${_bin}"
                OUTPUT_QUIET ERROR_QUIET
                COMMAND_ERROR_IS_FATAL ANY
            )
        endforeach()
    ]])
elseif(WIN32)
    install(TARGETS 3s-arm
        RUNTIME_DEPENDENCY_SET deps
        RUNTIME DESTINATION bin
    )

    # Task #157: the launcher lands next to the game exe, which is also
    # where the runtime-dependency staging puts SDL3.dll.
    if(TARGET 3s-netplay-launcher)
        install(TARGETS 3s-netplay-launcher RUNTIME DESTINATION bin)
    endif()

    execute_process(COMMAND cygpath -m /mingw64/bin
        OUTPUT_VARIABLE binPath
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )

    set(_runtime_dep_dirs ${binPath} "${SDL3_ROOT}/bin")
    if(ENABLE_FFMPEG_ADX)
        list(APPEND _runtime_dep_dirs "${FFMPEG_ROOT}/bin")
    endif()

    install(RUNTIME_DEPENDENCY_SET deps
        DIRECTORIES ${_runtime_dep_dirs}
        PRE_EXCLUDE_REGEXES "^api"
        POST_EXCLUDE_REGEXES ".*system32/.*\\.dll"
        DESTINATION bin
    )

    install(FILES
        "$<$<OR:$<CONFIG:Debug>,$<CONFIG:RelWithDebInfo>>:${CMAKE_CURRENT_BINARY_DIR}/3s-arm.pdb>"
        DESTINATION bin
    )

    install(FILES
        ${PROJECT_LICENSE_FILES}
        DESTINATION bin/licenses
    )
elseif(UNIX)
    install(TARGETS 3s-arm
        RUNTIME DESTINATION bin
    )

    # Task #157: guarded because this UNIX branch also covers the
    # embedded cross-builds (MiSTer, Miyoo), where the launcher target
    # deliberately does not exist. Its $ORIGIN/../lib rpath finds the
    # libSDL3.so* staged into lib/ below.
    if(TARGET 3s-netplay-launcher)
        install(TARGETS 3s-netplay-launcher RUNTIME DESTINATION bin)
    endif()

    if(ENABLE_FFMPEG_ADX)
        file(GLOB FFMPEG_SO "${FFMPEG_ROOT}/lib/*.so*")
        install(FILES ${FFMPEG_SO} DESTINATION lib)
    endif()

    file(GLOB SDL3_SO "${SDL3_ROOT}/lib/libSDL3.so*")
    install(FILES ${SDL3_SO} DESTINATION lib)

    if(ENABLE_NETPLAY)
        # Direct-P2P Step 4 follow-up (docs/plan-stun-direct-p2p.md): the
        # linked binary has libminiupnpc.so.17 in its NEEDED list and the
        # MiSTer rootfs does not ship it. pkg_check_modules populated
        # MINIUPNPC_LIBRARY_DIRS with the armhf cross sysroot libdir
        # (e.g. /usr/lib/arm-linux-gnueabihf). Glob the SONAME symlink
        # plus versioned real file and stage both under lib/ so the
        # runtime sits next to libSDL3.so*.
        foreach(_mu_dir IN LISTS MINIUPNPC_LIBRARY_DIRS)
            file(GLOB _mu_so "${_mu_dir}/libminiupnpc.so*")
            if(_mu_so)
                install(FILES ${_mu_so} DESTINATION lib)
                break()
            endif()
        endforeach()
    endif()

    install(FILES
        ${PROJECT_LICENSE_FILES}
        DESTINATION share/3s-arm/licenses
    )
endif()

# ======================================
# Packaging
# ======================================

set(CPACK_PACKAGE_NAME "3S-ARM")
set(CPACK_PACKAGE_VENDOR "CrowdedStreet")
# set(CPACK_PACKAGE_VERSION "1.0.0")

if(WIN32)
    set(CPACK_GENERATOR "ZIP")
elseif(APPLE)
    set(CPACK_GENERATOR "DragNDrop")
else()
    set(CPACK_GENERATOR "TGZ")
endif()

include(CPack)
