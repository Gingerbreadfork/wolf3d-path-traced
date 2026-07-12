// rt_lights.cpp
//
// Builds the renderer-only lighting for the current frame. The design goal
// (per the plan) is a playable, readable campaign — not a horror darkness pass.
// So we combine a soft ambient/ceiling fill with punchy local lights for lamps,
// pickups, exits and the muzzle flash.

#include "rt_lights.h"
#include "render_api.h"
#include <math.h>

// --dark launch flag: no world lights at all (see render_api.h).
extern "C" { int rt_dark = 0; }

namespace lights {

// Ceiling-lamp static sprite pages (shareware/registered). These glow.
static bool IsCeilingLamp(int page) {
    // SPR_STAT_27 (green ceiling light) and a couple of common lamp props.
    // Refined against the actual stat table; err toward visibility.
    switch (page) {
        case 27:   // green ceiling light
        case 29:   // chandelier-ish
        case 34:   // floor lamp
            return true;
        default:
            return false;
    }
}

void Build(rt::Scene &s) {
    s.lights.clear();

    // Darkness mode (--dark): no ambient, no lamps. The muzzle flash below is
    // kept — firing is the only way the player makes light.
    if (!rt_dark) {
        // Subtle ambient fill so shadowed areas keep a little readable colour.
        rt::LightInst amb{};
        amb.x = s.cam.x; amb.y = s.cam.y; amb.z = 0.9f;
        amb.r = 0.30f; amb.g = 0.32f; amb.b = 0.40f;   // cool fill
        amb.intensity = 1.0f;
        amb.radius = 0.0f;              // 0 => global ambient in the shader
        amb.type = LT_AMBIENT_FILL;
        amb.flicker = 0;
        s.lights.push_back(amb);

        // Lamp statics -> warm ceiling lights.
        for (const auto &sp : s.sprites) {
            if (sp.kind != 0) continue;
            if (!IsCeilingLamp(sp.texPage)) continue;
            rt::LightInst L{};
            L.x = sp.x; L.y = sp.y; L.z = 0.92f;
            L.r = 1.0f; L.g = 0.92f; L.b = 0.70f;
            L.intensity = 2.2f;
            L.radius = 6.0f;
            L.type = LT_CEILING;
            L.flicker = 0;
            s.lights.push_back(L);
        }
    }

    // Muzzle flash at the camera when firing (driven by the extractor's per-frame
    // weapon state).
    if (s.muzzleFlash > 0.01f) {
        float m = s.muzzleFlash;
        rt::LightInst L{};
        // slightly in front of the camera
        L.x = s.cam.x + cosf(s.cam.angleRad) * 0.5f;
        L.y = s.cam.y - sinf(s.cam.angleRad) * 0.5f;
        L.z = 0.5f;
        L.r = 1.0f; L.g = 0.82f; L.b = 0.5f;
        L.intensity = 3.0f * m;
        L.radius = 4.5f;
        L.type = LT_MUZZLE;
        L.flicker = 0;
        s.lights.push_back(L);
    }
}

} // namespace lights
