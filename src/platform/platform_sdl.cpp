// platform_sdl.cpp
//
// SDL2 platform shell. Creates the Vulkan-capable window and provides the
// small helpers the original game expects. Input events are still pumped by
// the original id_in.cpp (which is already SDL2-compatible); this file only
// owns the window and window-level services.

#include "platform.h"
#include "../rt/render_api.h"
#include <SDL.h>
#include <stdio.h>
#include <string.h>

static SDL_Window *g_window = nullptr;
static int g_winW = 1280, g_winH = 960;

int PLAT_Init(const PlatConfig *cfg) {
    if (SDL_WasInit(SDL_INIT_VIDEO) == 0) {
        if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
            printf("PLAT_Init: SDL video init failed: %s\n", SDL_GetError());
            return 0;
        }
    }

    g_winW = cfg->windowW > 0 ? cfg->windowW : 1280;
    g_winH = cfg->windowH > 0 ? cfg->windowH : 960;

    Uint32 flags = SDL_WINDOW_VULKAN | SDL_WINDOW_SHOWN;
    if (cfg->fullscreen) flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;

    g_window = SDL_CreateWindow(cfg->title ? cfg->title : "Wolfenstein 3D: Path Traced",
                                SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                g_winW, g_winH, flags);
    if (!g_window) {
        printf("PLAT_Init: SDL_CreateWindow (Vulkan) failed: %s\n", SDL_GetError());
        return 0;
    }

    SDL_ShowCursor(SDL_DISABLE);

    if (!RENDER_Init(g_winW, g_winH)) {
        printf("PLAT_Init: renderer init failed\n");
        return 0;
    }
    return 1;
}

void PLAT_Shutdown(void) {
    RENDER_Shutdown();
    if (g_window) { SDL_DestroyWindow(g_window); g_window = nullptr; }
}

SDL_Window *PLAT_Window(void) { return g_window; }

void PLAT_WindowSize(int *w, int *h) {
    if (g_window)
        SDL_GetWindowSize(g_window, w, h);
    else { if (w) *w = g_winW; if (h) *h = g_winH; }
}

void Plat_SetCaption(const char *title) {
    if (g_window && title) SDL_SetWindowTitle(g_window, title);
}

void Plat_SetRelativeMouse(int enabled) {
    SDL_SetRelativeMouseMode(enabled ? SDL_TRUE : SDL_FALSE);
}
