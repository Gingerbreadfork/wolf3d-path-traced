// render_api.h
//
// The bridge the original game code calls into. It hides whether the active
// backend is the classic software raster (blitted through Vulkan) or the
// Vulkan hardware path tracer. There is only ever ONE simulation; this module
// just visualises the current game state.

#ifndef WOLFPT_RENDER_API_H
#define WOLFPT_RENDER_API_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RM_CLASSIC = 0,   // original software raycaster, blitted to screen
    RM_PATHTRACED,    // Vulkan hardware path tracer
    RM_SPLIT,         // classic | path traced, vertical split
    RM_WIPE,          // moving vertical wipe between the two
    RM_FREEZE,        // frozen camera, inspect both
    RM_COUNT
} RenderMode;

// Lifecycle. Called from the video layer once the window exists.
int  RENDER_Init(int windowW, int windowH);
void RENDER_Shutdown(void);
int  RENDER_Ready(void);

// Submit the classic software frame (already converted to packed 0xAABBGGRR
// RGBA through the current palette). The renderer composites it with any
// path-traced frame according to the active mode and presents. Used by menus,
// intermissions, and every gameplay present (via SDL_Flip -> PLAT_PresentClassic).
void RENDER_PresentClassic(const uint32_t *classicRGBA, int w, int h);

// Hook called at the end of ThreeDRefresh() while the live view variables are
// valid. Extracts the scene from the game globals and, in a path-traced mode,
// renders the world; a subsequent present composites it.
void RENDER_Frame3D(void);

// Notify the renderer that the game is about to draw a full-screen 2D screen
// (menu, intermission, "Get Psyched", etc.) over the classic buffer, so any
// classic-only present that follows is shown at its native 4:3 rather than being
// composited into the widescreen gameplay layout. Called from VL_FadeOut and the
// in-game control panel; RENDER_Frame3D re-arms the gameplay view.
void RENDER_Notify2DScreen(void);

// Mode control.
void       RENDER_SetMode(RenderMode m);
RenderMode RENDER_GetMode(void);
void       RENDER_CycleMode(int dir);
const char *RENDER_ModeName(RenderMode m);

// Renderer tunables (bounces, samples, accumulation) toggled from menus/keys.
void RENDER_AdjustSetting(int which, int delta);

// Save screenshots as BMPs (written to the working directory). which:
// 0 = all (classic + path traced + split), 1 = classic, 2 = path traced,
// 3 = split compare.
void RENDER_Screenshot(int which);

#ifdef __cplusplus
}
#endif

#endif // WOLFPT_RENDER_API_H
