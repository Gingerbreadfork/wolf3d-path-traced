// sdl_compat.h
//
// Thin SDL 1.2 -> SDL2 compatibility shims so the original Wolfenstein 3D
// source (which was written against SDL 1.2) compiles unchanged against SDL2.
// Only the handful of removed/renamed symbols actually used by the game are
// provided here. Everything else (surfaces, blits, locks, events, joystick,
// timers) is already SDL2-compatible.

#ifndef WOLFPT_SDL_COMPAT_H
#define WOLFPT_SDL_COMPAT_H

#include <SDL.h>

// --- Surface / video-mode creation flags (ignored by SDL2) ------------------
#ifndef SDL_HWSURFACE
#define SDL_HWSURFACE   0
#endif
#ifndef SDL_SWSURFACE
#define SDL_SWSURFACE   0
#endif
#ifndef SDL_HWPALETTE
#define SDL_HWPALETTE   0
#endif
#ifndef SDL_DOUBLEBUF
#define SDL_DOUBLEBUF   0
#endif
#ifndef SDL_FULLSCREEN
#define SDL_FULLSCREEN  0
#endif
#ifndef SDL_ASYNCBLIT
#define SDL_ASYNCBLIT   0
#endif
#ifndef SDL_ANYFORMAT
#define SDL_ANYFORMAT   0
#endif
#ifndef SDL_OPENGL
#define SDL_OPENGL      0
#endif
#ifndef SDL_OPENGLBLIT
#define SDL_OPENGLBLIT  0
#endif
#ifndef SDL_RESIZABLE
#define SDL_RESIZABLE   0
#endif

// --- Palette selectors (SDL 1.2 SDL_SetPalette flags) -----------------------
#define SDL_LOGPAL  0x01
#define SDL_PHYSPAL 0x02

// --- Input grab modes -------------------------------------------------------
#define SDL_GRAB_QUERY (-1)
#define SDL_GRAB_OFF   0
#define SDL_GRAB_ON    1

// SDL 1.2 modifier type name.
typedef SDL_Keymod SDLMod;

#ifdef __cplusplus
extern "C" {
#endif

// Implemented by the platform layer (platform_sdl.cpp).
void Plat_SetCaption(const char *title);
void Plat_SetRelativeMouse(int enabled);

#ifdef __cplusplus
}
#endif

// --- Palette helpers (SDL 1.2 API on top of SDL2 surface palettes) ----------
static inline int SDL_SetColors(SDL_Surface *surface, SDL_Color *colors,
                                int firstcolor, int ncolors) {
    if (surface && surface->format && surface->format->palette)
        return SDL_SetPaletteColors(surface->format->palette, colors,
                                    firstcolor, ncolors) == 0;
    return 0;
}

static inline int SDL_SetPalette(SDL_Surface *surface, int /*flags*/,
                                 SDL_Color *colors, int firstcolor, int ncolors) {
    if (surface && surface->format && surface->format->palette)
        return SDL_SetPaletteColors(surface->format->palette, colors,
                                    firstcolor, ncolors) == 0;
    return 1;
}

// --- Window manager helpers -------------------------------------------------
static inline void SDL_WM_SetCaption(const char *title, const char * /*icon*/) {
    Plat_SetCaption(title);
}

static inline int SDL_WM_GrabInput(int mode) {
    if (mode != SDL_GRAB_QUERY)
        Plat_SetRelativeMouse(mode == SDL_GRAB_ON);
    return mode;
}

#endif // WOLFPT_SDL_COMPAT_H
