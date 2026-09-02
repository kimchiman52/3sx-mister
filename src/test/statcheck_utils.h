#if defined(STATCHECK)

#ifndef STATCHECK_UTILS_H
#define STATCHECK_UTILS_H

#include <SDL3/SDL_iostream.h>

/* Statcheck-local big-endian readers, mirroring the DEBUG harness's
 * src/test/test_runner_utils.c. That file is gated `#if DEBUG` (never
 * compiled in a STATCHECK build — plan A3b item 5), so the STATCHECK
 * translation units carry their own copies here as static inline functions
 * instead of de-gating the DEBUG file. Names match upstream's
 * test_runner_utils.h so the ported compare/runner bodies need no renames;
 * there is no link-time collision because the DEBUG symbols only exist in
 * DEBUG builds and DEBUG + STATCHECK cannot be co-compiled (CMake guard). */

static inline Uint8 read_u8(SDL_IOStream* io, Sint64 offset) {
    Uint8 result = 0;
    SDL_SeekIO(io, offset, SDL_IO_SEEK_SET);
    SDL_ReadU8(io, &result);
    return result;
}

static inline Uint16 read_u16(SDL_IOStream* io, Sint64 offset) {
    Uint16 result = 0;
    SDL_SeekIO(io, offset, SDL_IO_SEEK_SET);
    SDL_ReadU16BE(io, &result);
    return result;
}

static inline Sint16 read_s16(SDL_IOStream* io, Sint64 offset) {
    Sint16 result = 0;
    SDL_SeekIO(io, offset, SDL_IO_SEEK_SET);
    SDL_ReadS16BE(io, &result);
    return result;
}

static inline Sint32 read_s32(SDL_IOStream* io, Sint64 offset) {
    Sint32 result = 0;
    SDL_SeekIO(io, offset, SDL_IO_SEEK_SET);
    SDL_ReadS32BE(io, &result);
    return result;
}

#endif

#endif
