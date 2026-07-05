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
#include <vector>
#include <string.h>
#include <stdio.h>
#include <math.h>

// Implemented in rt_scene_extract.cpp
namespace rt { void ExtractScene(Scene &s); }

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

// Final composite resolution (3x the classic 320x200).
constexpr int kFinalW = 960, kFinalH = 600;
constexpr int kScale  = kFinalW / 320;       // 3
constexpr int kHudLines = 40;                // classic status bar height (px)
constexpr int kPtViewH = kFinalH - kHudLines * kScale;   // 480 (PT 3D region)

std::vector<uint32_t> g_ptComposited;        // PT 3D + classic HUD + weapon

// Overlay the classic weapon sprite onto the PT viewport region.
void OverlayWeapon(std::vector<uint32_t> &dst) {
    if (g_scene.weaponFrame < 0) return;
    static uint32_t wpn[64 * 64];
    mat::DecodeSprite(g_scene.weaponFrame, wpn);
    int S = kPtViewH;                        // scale weapon to fill viewport height
    int ox = kFinalW / 2 - S / 2;
    int oy = kPtViewH - S;                   // bottom-aligned within the 3D region
    for (int dy = 0; dy < S; ++dy) {
        int fy = oy + dy;
        if (fy < 0 || fy >= kPtViewH) continue;
        int sy = dy * 64 / S;
        for (int dx = 0; dx < S; ++dx) {
            uint32_t p = wpn[sy * 64 + (dx * 64 / S)];
            if ((p >> 24) < 128) continue;   // transparent
            int fx = ox + dx;
            if (fx < 0 || fx >= kFinalW) continue;
            dst[fy * kFinalW + fx] = p;
        }
    }
}

// Build the 960x600 path-traced frame: PT 3D world on top, classic HUD strip
// and player weapon composited so the frame is complete and aligns with classic.
void BuildPTComposite(const uint32_t *classic, int cw, int ch) {
    g_ptComposited.assign((size_t)kFinalW * kFinalH, 0xFF000000u);

    // top: PT 3D (g_ptFrame is kFinalW x kPtViewH)
    if (g_ptW == kFinalW && g_ptH == kPtViewH)
        memcpy(g_ptComposited.data(), g_ptFrame.data(), (size_t)kFinalW * kPtViewH * 4);

    OverlayWeapon(g_ptComposited);

    // bottom: classic HUD strip (classic rows [ch-kHudLines, ch)) upscaled
    int hudRows = kFinalH - kPtViewH;
    for (int y = 0; y < hudRows; ++y) {
        int cy = (ch - kHudLines) + y * kHudLines / hudRows;
        if (cy < 0 || cy >= ch) continue;
        for (int x = 0; x < kFinalW; ++x) {
            int cx = x * cw / kFinalW;
            g_ptComposited[(size_t)(kPtViewH + y) * kFinalW + x] = classic[cy * cw + cx];
        }
    }
}

// Compose according to the active render mode and present.
void Compose(const uint32_t *classic, int cw, int ch) {
    bool havePT = g_have3D && !g_ptFrame.empty() &&
                  (g_mode == RM_PATHTRACED || g_mode == RM_SPLIT ||
                   g_mode == RM_WIPE || g_mode == RM_FREEZE);

    if (!havePT) {
        g_compose.assign(classic, classic + (size_t)cw * ch);
        rtvk::PresentRGBA(g_compose.data(), cw, ch);
        return;
    }

    BuildPTComposite(classic, cw, ch);

    if (g_mode == RM_PATHTRACED) {
        rtvk::PresentRGBA(g_ptComposited.data(), kFinalW, kFinalH);
        return;
    }

    int layout = (g_mode == RM_SPLIT) ? compare::SPLIT
               : (g_mode == RM_WIPE)  ? compare::WIPE
                                      : compare::FREEZE;
    int ow, oh;
    compare::Composite(layout, classic, cw, ch, g_ptComposited.data(),
                       kFinalW, kFinalH, g_wipePos, g_compose, &ow, &oh);
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

    bool needPT = (g_mode != RM_CLASSIC) && rtpt::Ready();
    if (needPT) {
        rtpt::DesiredResolution(&g_ptW, &g_ptH);
        g_ptFrame.resize((size_t)g_ptW * g_ptH);
        rtpt::Render(g_scene, g_ptFrame.data(), g_ptW, g_ptH);
    }
    g_have3D = true;

    if (g_mode == RM_WIPE) {
        g_wipePos += g_wipeDir;
        if (g_wipePos > 0.95f) { g_wipePos = 0.95f; g_wipeDir = -fabsf(g_wipeDir); }
        if (g_wipePos < 0.05f) { g_wipePos = 0.05f; g_wipeDir =  fabsf(g_wipeDir); }
    }
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
                 pt, pt ? kFinalW : 0, pt ? kFinalH : 0);
}

} // extern "C"
