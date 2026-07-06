// platform.h
//
// Modern SDL2 platform layer that replaces the DOS/SDL-1.2 shell. Owns the
// window (created for Vulkan), event pump, timing, and relative-mouse control.
// The original id_in.cpp still consumes SDL2 events directly; this layer only
// creates the window and the small helpers the game expects.

#ifndef WOLFPT_PLATFORM_H
#define WOLFPT_PLATFORM_H

#include <SDL.h>
#include <stdint.h>

// This header may be included both by game files (where wl_def.h has set
// "#pragma pack(1)") and by pure platform files (default packing). Force a
// consistent layout for the shared struct so it matches across the boundary.
#pragma pack(push, 8)

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int         windowW;
    int         windowH;
    int         fullscreen;
    const char *title;
} PlatConfig;

// Create the SDL2 window (SDL_WINDOW_VULKAN) and initialise the Vulkan
// renderer. Returns 1 on success.
int  PLAT_Init(const PlatConfig *cfg);
void PLAT_Shutdown(void);

SDL_Window *PLAT_Window(void);
void        PLAT_WindowSize(int *w, int *h);

// Toggle borderless desktop fullscreen at runtime (bound to Alt+Enter). Returns
// the resulting state: 1 = fullscreen, 0 = windowed. The Vulkan swapchain
// recreates itself on the next present when the drawable size changes.
int         PLAT_ToggleFullscreen(void);

// Caption / mouse helpers used by the SDL 1.2 compatibility shims.
void Plat_SetCaption(const char *title);
void Plat_SetRelativeMouse(int enabled);

// Data-file path resolution (platform_files.cpp).
void        PLAT_SetDataDir(const char *dir);
const char *PLAT_DataDir(void);

// Directory that holds the compiled shaders (*.comp.spv). Resolved at runtime so
// a relocatable/downloaded build finds shaders next to the executable: honours
// the WOLFPT_SHADER_DIR env var, then <exe-dir>/shaders, then the build-time
// default. (platform_files.cpp)
const char *PLAT_ShaderDir(void);

#ifdef __cplusplus
}
#endif

#pragma pack(pop)

#endif // WOLFPT_PLATFORM_H
