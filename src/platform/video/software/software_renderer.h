#if CRS_VIDEO_DRIVER_SOFTWARE

#ifndef SOFTWARE_RENDERER_H
#define SOFTWARE_RENDERER_H

#include "core/render_primitives.h"
#include "platform/video/software/sw_blit.h"
#include "rendering/game_renderer.h"

#include <stdbool.h>
#include <stdint.h>

// Public draw surface — Renderer_* declarations live in
// include/rendering/game_renderer.h. The software backend defines them in
// software_renderer.c.

// Internal

bool SoftwareRenderer_Init(bool nearest_filter, int scale);
void SoftwareRenderer_Quit();
void SoftwareRenderer_RenderFrame();
/* Drain queued geometry without changing the completed canvas. */
int SoftwareRenderer_HoldLastFrame();
int SoftwareRenderer_GetPerfPeakQuads(void);

// Canvas accessor for the host app driver to present (SDL streaming texture, DRM dumb buffer, etc.).
// Pixel layout is ARGB8888 (default) or RGB565 (with CRS_SW_CANVAS_16BPP) — see SWCanvasPixel in
// sw_blit.h. Tightly packed (pitch == width * sizeof(SWCanvasPixel)) unless pitch_bytes reports otherwise.
const SWCanvasPixel* SoftwareRenderer_GetCanvas(int* out_width, int* out_height, int* out_pitch_bytes);
bool SoftwareRenderer_UsesNearestFilter();

#endif

#endif // CRS_VIDEO_DRIVER_SOFTWARE
