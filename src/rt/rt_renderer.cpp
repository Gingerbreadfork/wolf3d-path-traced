// rt_renderer.cpp
//
// High-level renderer: owns the active render mode, extracts the scene from the
// live game state, drives the Vulkan path tracer, and composites the classic
// and path-traced frames (split / wipe / freeze) before presenting.
//
// There is only ONE simulation. The classic software frame is always produced
// by the original code (into screenBuffer); the path tracer visualises the same
// extracted state.

#include "render_api.h"
#include "rt_vulkan.h"
#include "rt_scene.h"
#include "rt_pathtrace.h"
#include "rt_materials.h"
#include "../compare/compare_renderer.h"
#include "../platform/autopilot.h"
#include "../platform/platform.h"        // PLAT_WindowSize (widescreen aspect)
#include <vector>
#include <string.h>
#include <stdio.h>
#include <math.h>

// Implemented in rt_scene_extract.cpp
namespace rt { void ExtractScene(Scene &s); }
extern "C" int   RT_ClassicViewIsFizzle(void);   // death / respawn dissolve (keep classic)
extern "C" float RT_FadeBrightness(void);        // 0..1 palette-fade level (HUD-synced)

namespace {

// Path tracing is the default experience; the classic raycaster stays one
// keypress away (F8). Menus/intermissions still present the classic frame —
// only the live 3D gameplay view is path traced.
RenderMode           g_mode = RM_PATHTRACED;
bool                 g_have3D = false;      // a gameplay 3D frame is pending
bool                 g_ready = false;

rt::Scene            g_scene;
std::vector<uint32_t> g_ptFrame;            // path-traced RGBA
int                  g_ptW = 0, g_ptH = 0;
std::vector<uint32_t> g_compose;            // final composited RGBA
std::vector<uint32_t> g_classicFrame;       // last TRUE classic frame (for screenshots)
int                  g_classicW = 0, g_classicH = 0;

float                g_wipePos = 0.5f;      // 0..1 wipe position
float                g_wipeDir = 0.20f;     // wipe animation speed (per frame)

// True when the classic buffer currently holds a freshly-rendered 3D gameplay
// view (set by RENDER_Frame3D), so a classic-only present that follows — the
// level-start palette fade-in or the death fizzle, neither of which produces a
// path-traced frame — is still composited into the widescreen layout instead of
// snapping to 4:3. Cleared by RENDER_Notify2DScreen() when the game draws a
// full-screen 2D screen (menu / "Get Psyched" / intermission).
bool                 g_classic3DView = false;

// Final composite resolution. Height is fixed at 3x the classic 320x200 (600);
// WIDTH is dynamic for Hor+ widescreen (see UpdateFinalWidth). The vertical
// layout — 480px path-traced 3D region + 120px HUD strip — never changes.
constexpr int kFinalH = 600;
constexpr int kScale  = 3;                   // 320*3 -> 960 authored HUD width
constexpr int kHudLines = 40;                // classic status bar height (px)
constexpr int kPtViewH = kFinalH - kHudLines * kScale;   // 480 (PT 3D region)
constexpr int kHudW   = 320 * kScale;        // 960: native (un-stretched) HUD width
constexpr int kFinalWMin = kHudW;            // never narrower than HUD / classic view
constexpr int kFinalWMax = 1680;             // ~21:9 cap

int g_finalW = kHudW;                         // dynamic composite width (widescreen)

// Recompute the composite width from the live SWAPCHAIN aspect. Frames are
// authored square-pixel then displayed with the classic 1.2x VGA vertical stretch
// (height 600 -> 720 on screen), so a width of 720*aspect fills the surface with no
// bars. Deriving from the swapchain extent (not the SDL window size) is what makes
// this exact: the present letterbox uses that same extent, and on Wayland the SDL
// window size can disagree with the real surface. Clamped to [4:3, ~21:9], even.
void UpdateFinalWidth() {
    int sw = 0, sh = 0;
    rtvk::SwapchainSize(&sw, &sh);
    if (sw <= 0 || sh <= 0) { g_finalW = kHudW; return; }
    int w = (int)(720.0f * (float)sw / (float)sh + 0.5f);   // 720 = kFinalH * 1.2
    if (w < kFinalWMin) w = kFinalWMin;
    if (w > kFinalWMax) w = kFinalWMax;
    g_finalW = w & ~1;                        // even
}

std::vector<uint32_t> g_ptComposited;        // PT 3D + classic HUD + weapon

// Overlay the classic weapon sprite onto the PT viewport region.
void OverlayWeapon(std::vector<uint32_t> &dst) {
    if (g_scene.weaponFrame < 0) return;
    static uint32_t wpn[64 * 64];
    mat::DecodeSprite(g_scene.weaponFrame, wpn);
    int S = kPtViewH;                        // scale weapon to fill viewport height
    int ox = g_finalW / 2 - S / 2;           // horizontally centered in the wide frame
    int oy = kPtViewH - S;                   // bottom-aligned within the 3D region
    for (int dy = 0; dy < S; ++dy) {
        int fy = oy + dy;
        if (fy < 0 || fy >= kPtViewH) continue;
        int sy = dy * 64 / S;
        for (int dx = 0; dx < S; ++dx) {
            uint32_t p = wpn[sy * 64 + (dx * 64 / S)];
            if ((p >> 24) < 128) continue;   // transparent
            int fx = ox + dx;
            if (fx < 0 || fx >= g_finalW) continue;
            dst[fy * g_finalW + fx] = p;
        }
    }
}

// Build the 960x600 path-traced frame: PT 3D world on top, classic HUD strip
// and player weapon composited so the frame is complete and aligns with classic.
void BuildPTComposite(const uint32_t *classic, int cw, int ch) {
    g_ptComposited.assign((size_t)g_finalW * kFinalH, 0xFF000000u);

    // top: PT 3D (g_ptFrame is g_finalW x kPtViewH, full width)
    if (g_ptW == g_finalW && g_ptH == kPtViewH)
        memcpy(g_ptComposited.data(), g_ptFrame.data(), (size_t)g_finalW * kPtViewH * 4);

    OverlayWeapon(g_ptComposited);

    // bottom: classic HUD strip upscaled to its NATIVE 960px width and centered;
    // the wide side margins stay black (the status bar is fixed 320px art with no
    // wider version, so centering — not stretching — is the correct widescreen
    // treatment). The status bar is STATUSLINES(40)/200 of the classic buffer, so
    // srcHud scales with the buffer height (e.g. 80 rows of a 640x400 buffer).
    int hudRows = kFinalH - kPtViewH;
    int srcHud  = ch / 5;
    int hudX0 = (g_finalW - kHudW) / 2;
    for (int y = 0; y < hudRows; ++y) {
        int cy = (ch - srcHud) + y * srcHud / hudRows;
        if (cy < 0 || cy >= ch) continue;
        uint32_t *row = &g_ptComposited[(size_t)(kPtViewH + y) * g_finalW];
        for (int x = 0; x < kHudW; ++x) {
            int cx = x * cw / kHudW;
            row[hudX0 + x] = classic[cy * cw + cx];
        }
    }
}

// Build the widescreen composite from a CLASSIC frame that still contains the
// live 3D viewport but produced no path-traced frame this present — the level-
// start palette fade-in and the death fizzle-fade both re-present / draw into the
// classic buffer directly. Uses the SAME layout as the path-traced composite —
// view stretched to full width on top, HUD centered below — so those transitions
// keep the gameplay aspect instead of snapping to 4:3.
void BuildClassicWideComposite(const uint32_t *classic, int cw, int ch) {
    g_ptComposited.assign((size_t)g_finalW * kFinalH, 0xFF000000u);

    // top: classic view region [0, ch-srcHud) upscaled to g_finalW x kPtViewH
    int srcHud = ch / 5;                 // status bar = STATUSLINES(40)/200 of buffer
    int viewRows = ch - srcHud;
    if (viewRows < 1) viewRows = ch;
    for (int y = 0; y < kPtViewH; ++y) {
        int cy = y * viewRows / kPtViewH;
        if (cy >= ch) cy = ch - 1;
        uint32_t *row = &g_ptComposited[(size_t)y * g_finalW];
        for (int x = 0; x < g_finalW; ++x)
            row[x] = classic[cy * cw + (x * cw / g_finalW)];
    }

    // bottom: classic HUD strip at native 960 width, centered (matches gameplay)
    int hudRows = kFinalH - kPtViewH;
    int hudX0 = (g_finalW - kHudW) / 2;
    for (int y = 0; y < hudRows; ++y) {
        int cy = (ch - srcHud) + y * srcHud / hudRows;
        if (cy < 0 || cy >= ch) continue;
        uint32_t *row = &g_ptComposited[(size_t)(kPtViewH + y) * g_finalW];
        for (int x = 0; x < kHudW; ++x)
            row[hudX0 + x] = classic[cy * cw + (x * cw / kHudW)];
    }
}

// Scale the 3D-view region (top kPtViewH rows) of the composite by brightness b in
// [0,1], leaving the HUD strip untouched (it fades via the classic game palette).
// Used to fade the path-traced view in at level start in sync with that palette.
void FadeViewRegion(float b) {
    if (b >= 0.999f) return;
    if (b < 0.0f) b = 0.0f;
    uint32_t bi = (uint32_t)(b * 256.0f);            // 8.8 fixed-point multiplier
    for (int y = 0; y < kPtViewH; ++y) {
        uint32_t *row = &g_ptComposited[(size_t)y * g_finalW];
        for (int x = 0; x < g_finalW; ++x) {
            uint32_t p = row[x];                     // packed 0xAABBGGRR
            uint32_t r = ((p & 0xFF) * bi) >> 8;
            uint32_t g = (((p >> 8) & 0xFF) * bi) >> 8;
            uint32_t bl = (((p >> 16) & 0xFF) * bi) >> 8;
            row[x] = 0xFF000000u | (bl << 16) | (g << 8) | r;
        }
    }
}

// Compose according to the active render mode and present.
void Compose(const uint32_t *classic, int cw, int ch) {
    bool havePT = g_have3D && !g_ptFrame.empty() &&
                  (g_mode == RM_PATHTRACED || g_mode == RM_SPLIT ||
                   g_mode == RM_WIPE || g_mode == RM_FREEZE);

    if (!havePT) {
        // A classic-only present. If it's still the gameplay 3D view (level-start
        // fade-in, death fizzle) in a path-traced mode, present it widescreen so it
        // doesn't snap to 4:3. Full-screen 2D screens (menus, "Get Psyched",
        // intermissions) cleared g_classic3DView and stay at their native 4:3, as
        // does everything in the Classic render mode.
        if (g_classic3DView && g_mode != RM_CLASSIC && cw > 0 && ch > kHudLines) {
            if (RT_ClassicViewIsFizzle()) {
                // Death fizzle (reddening) or a fizzle-in dissolve: show the classic
                // view itself, composited widescreen.
                BuildClassicWideComposite(classic, cw, ch);
            } else {
                // Level-start palette fade-in: fade the PATH-TRACED view in instead
                // of the flat classic renderer (no more "old renderer" flash). The
                // view is static during the fade, so reuse the last PT frame and
                // sync its brightness to the classic palette fade driving the HUD.
                BuildPTComposite(classic, cw, ch);
                FadeViewRegion(RT_FadeBrightness());
            }
            rtvk::PresentRGBA(g_ptComposited.data(), g_finalW, kFinalH);
            return;
        }
        g_compose.assign(classic, classic + (size_t)cw * ch);
        rtvk::PresentRGBA(g_compose.data(), cw, ch);
        return;
    }

    BuildPTComposite(classic, cw, ch);

    if (g_mode == RM_PATHTRACED) {
        rtvk::PresentRGBA(g_ptComposited.data(), g_finalW, kFinalH);
        return;
    }

    int layout = (g_mode == RM_SPLIT) ? compare::SPLIT
               : (g_mode == RM_WIPE)  ? compare::WIPE
                                      : compare::FREEZE;
    int ow, oh;
    compare::Composite(layout, classic, cw, ch, g_ptComposited.data(),
                       g_finalW, kFinalH, g_wipePos, g_compose, &ow, &oh);
    rtvk::PresentRGBA(g_compose.data(), ow, oh);
}

} // namespace

extern "C" {

int RENDER_Init(int windowW, int windowH) {
    if (!rtvk::Init(windowW, windowH)) {
        printf("[render] Vulkan init failed\n");
        return 0;
    }
    rtpt::Init();
    g_ready = true;
    printf("[render] ready. mode=%s  pathtracer=%s\n",
           RENDER_ModeName(g_mode), rtpt::Ready() ? "ON" : "OFF");
    return 1;
}

void RENDER_Shutdown(void) {
    if (!g_ready) return;
    rtpt::Shutdown();
    rtvk::Shutdown();
    g_ready = false;
}

int RENDER_Ready(void) { return g_ready ? 1 : 0; }

void RENDER_Frame3D(void) {
    if (!g_ready) return;

    AUTO_Tick();

    // Freeze mode keeps the last-extracted camera/scene.
    if (g_mode != RM_FREEZE)
        rt::ExtractScene(g_scene);

    UpdateFinalWidth();                       // widescreen: track live window aspect
    bool needPT = (g_mode != RM_CLASSIC) && rtpt::Ready();
    if (needPT) {
        g_ptW = g_finalW;                     // Hor+ widescreen 3D view (full width)
        g_ptH = kPtViewH;                     // 480 (fixed)
        g_ptFrame.resize((size_t)g_ptW * g_ptH);
        rtpt::Render(g_scene, g_ptFrame.data(), g_ptW, g_ptH);
        // The classic buffer now holds a live 3D view; a classic-only present that
        // follows without a fresh PT frame (fade-in, death fizzle) stays widescreen.
        g_classic3DView = true;
    }
    g_have3D = true;

    if (g_mode == RM_WIPE) {
        g_wipePos += g_wipeDir;
        if (g_wipePos > 0.95f) { g_wipePos = 0.95f; g_wipeDir = -fabsf(g_wipeDir); }
        if (g_wipePos < 0.05f) { g_wipePos = 0.05f; g_wipeDir =  fabsf(g_wipeDir); }
    }
}

void RENDER_Notify2DScreen(void) {
    // The game is about to draw a full-screen 2D screen over the classic buffer;
    // the next classic-only present is no longer the 3D view, so show it at 4:3.
    g_classic3DView = false;
}

void RENDER_PresentClassic(const uint32_t *classicRGBA, int w, int h) {
    if (!g_ready) return;
    // Keep a copy of the true classic frame for screenshots/compositing.
    g_classicFrame.assign(classicRGBA, classicRGBA + (size_t)w * h);
    g_classicW = w; g_classicH = h;
    Compose(classicRGBA, w, h);
    g_have3D = false;   // consumed
}

void RENDER_SetMode(RenderMode m) {
    if (m < 0 || m >= RM_COUNT) return;
    g_mode = m;
    printf("[render] mode -> %s\n", RENDER_ModeName(m));
}

RenderMode RENDER_GetMode(void) { return g_mode; }

void RENDER_CycleMode(int dir) {
    int m = (int)g_mode + (dir >= 0 ? 1 : -1);
    if (m < 0) m = RM_COUNT - 1;
    if (m >= RM_COUNT) m = 0;
    RENDER_SetMode((RenderMode)m);
}

const char *RENDER_ModeName(RenderMode m) {
    switch (m) {
    case RM_CLASSIC:    return "Classic";
    case RM_PATHTRACED: return "Path Traced";
    case RM_SPLIT:      return "Split Compare";
    case RM_WIPE:       return "Moving Wipe";
    case RM_FREEZE:     return "Freeze Compare";
    default:            return "?";
    }
}

void RENDER_AdjustSetting(int which, int delta) {
    rtpt::AdjustSetting(which, delta);
}

void RENDER_Screenshot(int which) {
    extern void SHOT_Capture(int which, const uint32_t *classic, int cw, int ch,
                             const uint32_t *pt, int pw, int ph);
    const uint32_t *classic = g_classicFrame.empty() ? nullptr : g_classicFrame.data();
    // Prefer the fully composited PT frame (3D + HUD + weapon).
    if (!g_ptComposited.empty() && !g_classicFrame.empty())
        BuildPTComposite(g_classicFrame.data(), g_classicW, g_classicH);
    const uint32_t *pt = g_ptComposited.empty() ? nullptr : g_ptComposited.data();
    SHOT_Capture(which, classic, g_classicW, g_classicH,
                 pt, pt ? g_finalW : 0, pt ? kFinalH : 0);
}

} // extern "C"
