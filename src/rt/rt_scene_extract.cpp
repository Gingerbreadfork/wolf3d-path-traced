// rt_scene_extract.cpp
//
// Reads the LIVE original Wolfenstein 3D game globals each frame and fills an
// rt::Scene. This is the single seam between the untouched game simulation and
// the renderer: walls/doors/pushwalls/statics/actors are all read from the same
// authoritative structures the classic renderer uses. No second simulation.

// IMPORTANT: include the renderer/STL headers FIRST, at default struct packing.
// wl_def.h ends with an un-reset "#pragma pack(1)"; including it before rt_scene.h
// would compile rt::Scene and std::vector with packing 1 here but not in the
// other translation units -> ODR layout mismatch and heap corruption.
#include "rt_scene.h"
#include "rt_lights.h"
#include "render_api.h"
#include <math.h>
#include "wl_def.h"          // original game globals (sets #pragma pack(1))

// Weapon sprite scale table (global in wl_draw.cpp).
extern int weaponscale[];

// Horizontal field of view of the classic raycaster:
//   2 * atan( (VIEWGLOBAL/2) / (FOCALLENGTH + MINDIST) )
static const float kClassicHFov =
    2.0f * atanf(32768.0f / (float)(0x5700 + 0x5800));

// Door texture pages (mirrors wl_draw.cpp door page selection).
//   DOORWALL = PMSpriteStart - 8
static inline int DoorPage(const doorobj_t &d) {
    int base = PMSpriteStart - 8;   // DOORWALL
    if (d.vertical) {
        switch (d.lock) {
            case dr_normal:   return base + 1;
            case dr_elevator: return base + 5;
            default:          return base + 7;   // locked
        }
    } else {
        switch (d.lock) {
            case dr_normal:   return base + 0;
            case dr_elevator: return base + 4;
            default:          return base + 6;   // locked
        }
    }
}

// 8-direction sprite rotation, independent of DrawScaleds ordering.
static int CalcRotateWorld(objtype *ob) {
    int viewangle = player->angle;   // world-space (drop the sub-pixel skew)
    int angle;
    if (ob->obclass == rocketobj || ob->obclass == hrocketobj)
        angle = (viewangle - 180) - ob->angle;
    else
        angle = (viewangle - 180) - dirangle[ob->dir];
    angle += ANGLES / 16;
    while (angle >= ANGLES) angle -= ANGLES;
    while (angle < 0)       angle += ANGLES;
    if (ob->state->rotate == 2) return 0;
    return angle / (ANGLES / 8);
}

// Projectiles (rockets, fireballs, needles, sparks) glow and cast a moving
// coloured light as they fly. Returns false for ordinary actors; otherwise fills
// the projectile's glow colour.
static bool ProjectileGlow(classtype oc, float &r, float &g, float &b) {
    switch (oc) {
        case rocketobj: case hrocketobj: r=1.00f; g=0.55f; b=0.22f; return true; // rocket exhaust
        case fireobj:                    r=1.00f; g=0.42f; b=0.14f; return true; // fireball
        case needleobj:                  r=0.45f; g=1.00f; b=0.45f; return true; // syringe
        case sparkobj:                   r=0.55f; g=0.75f; b=1.00f; return true; // spark
        default:                         return false;
    }
}

namespace rt {

void ExtractScene(Scene &s) {
    // Projectile glow lights, collected during the actor scan and added after
    // lights::Build() (which clears s.lights).
    struct ProjLight { float x, y, r, g, b; };
    std::vector<ProjLight> projGlow;
    int level = gamestate.mapon;

    // --- Camera --------------------------------------------------------------
    // Use the raycaster's focal point (viewx/viewy) so the pinhole camera
    // reproduces the classic projection exactly.
    s.cam.x = viewx / 65536.0f;
    s.cam.y = viewy / 65536.0f;
    s.cam.z = 0.5f;
    s.cam.angleRad = player->angle * (float)(M_PI / 180.0);
    s.cam.fovRad = kClassicHFov;
    s.cam.pitch = 0.0f;

    s.levelNumber = level;

    // --- Static geometry: solid wall tiles -----------------------------------
    // Refilled every frame (a 64x64 scan is cheap); the path tracer rebuilds its
    // acceleration structures from this each frame anyway.
    s.walls.clear();
    const int doorwall = PMSpriteStart - 8;   // DOORWALL (matches wl_draw.cpp)
    for (int x = 0; x < kMapSize; ++x) {
        for (int y = 0; y < kMapSize; ++y) {
            unsigned v = tilemap[x][y];
            if (v == 0 || (v & 0x80)) continue;   // empty or door tile
            // SpawnDoor ORs 0x40 into the two solid walls flanking every door
            // (wl_act1.cpp); the classic renderer reads them as walls via
            // `tilehit & ~0x40`. Strip that bit rather than dropping the tile —
            // otherwise every doorway loses its side walls (see-through walls).
            unsigned wallv = v & 0x3f;
            if (wallv == 0) continue;             // 64 = pushwall-in-motion marker
            WallInst w;
            w.tx = (float)x;
            w.ty = (float)y;
            w.texNS = horizwall[wallv];
            w.texEW = vertwall[wallv];
            if (v & 0x40) {
                // Door-side wall: use the classic door-track (jamb) texture on
                // the face that borders the door tile.
                if ((x > 0 && (tilemap[x-1][y] & 0x80)) ||
                    (x < kMapSize-1 && (tilemap[x+1][y] & 0x80)))
                    w.texEW = doorwall + 3;        // vertical (x-facing) faces
                if ((y > 0 && (tilemap[x][y-1] & 0x80)) ||
                    (y < kMapSize-1 && (tilemap[x][y+1] & 0x80)))
                    w.texNS = doorwall + 2;        // horizontal (y-facing) faces
            }
            s.walls.push_back(w);
        }
    }

    // --- Dynamic geometry ----------------------------------------------------
    s.clearDynamic();

    // Doors
    for (doorobj_t *d = &doorobjlist[0]; d != lastdoorobj; ++d) {
        int idx = (int)(d - &doorobjlist[0]);
        DoorInst di;
        di.tx = (float)d->tilex;
        di.ty = (float)d->tiley;
        di.vertical = d->vertical ? 1 : 0;
        di.open = doorposition[idx] / 65535.0f;
        di.texPage = DoorPage(*d);
        di.lock = d->lock;
        s.doors.push_back(di);
    }

    // Pushwall (secret wall being shoved). pwallpos is 0..63 tiles*64.
    if (pwallstate) {
        PushwallInst pw;
        float move = pwallpos / 64.0f;
        float dx = 0, dy = 0;
        switch (pwalldir) {
            case di_north: dy = -move; break;
            case di_south: dy =  move; break;
            case di_east:  dx =  move; break;
            case di_west:  dx = -move; break;
            default: break;
        }
        pw.x = pwallx + dx;
        pw.y = pwally + dy;
        int v = pwalltile ? pwalltile : 1;
        if (v >= MAXWALLTILES) v = MAXWALLTILES - 1;
        pw.texNS = horizwall[v];
        pw.texEW = vertwall[v];
        s.pushwalls.push_back(pw);
    }

    // Static objects (treasure, lamps, pickups, decorations)
    for (statobj_t *st = &statobjlist[0]; st != laststatobj; ++st) {
        if (st->shapenum == -1) continue;
        SpriteInst sp;
        sp.x = st->tilex + 0.5f;
        sp.y = st->tiley + 0.5f;
        sp.texPage = st->shapenum;
        sp.kind = 0;
        sp.emissive = 0.0f;
        s.sprites.push_back(sp);
    }

    // Active actors (enemies, bosses, projectiles)
    for (objtype *ob = player->next; ob; ob = ob->next) {
        if (!ob->state) continue;
        int shape = ob->state->shapenum;
        if (shape == -1) shape = ob->temp1;   // special (e.g. spawned shape) — substitute first
        if (shape <= 0) continue;             // then skip if there's no sprite this frame
        if (ob->state->rotate) shape += CalcRotateWorld(ob);
        SpriteInst sp;
        sp.x = ob->x / 65536.0f;
        sp.y = ob->y / 65536.0f;
        sp.texPage = shape;
        sp.kind = 1;
        sp.emissive = 0.0f;
        float gr, gg, gb;
        if (ProjectileGlow(ob->obclass, gr, gg, gb)) {
            sp.kind = 2;            // projectile
            sp.emissive = 0.9f;     // the sprite itself glows (blooms in post)
            projGlow.push_back({sp.x, sp.y, gr, gg, gb});
        }
        s.sprites.push_back(sp);
    }

    // --- Weapon / muzzle flash ----------------------------------------------
    if (gamestate.weapon != -1)
        s.weaponFrame = weaponscale[gamestate.weapon] + gamestate.weaponframe;
    else
        s.weaponFrame = -1;
    // Muzzle flash: the attack frames (weaponframe 1..2) are the firing frames.
    s.muzzleFlash = (gamestate.weapon != -1 && gamestate.weaponframe >= 1
                     && gamestate.weaponframe <= 2) ? 1.0f : 0.0f;

    // --- Lights (lamps, signs, muzzle, exit, etc.) --------------------------
    lights::Build(s);   // ambient fill + lamp statics + muzzle flash

    // Overhead ceiling lights on a coarse grid near the camera so every room is
    // readable and sprites cast real downward shadows. Limited to a radius so
    // the shader's per-hit light loop stays cheap. Skipped in --dark mode.
    int cx = (int)s.cam.x, cy = (int)s.cam.y;
    const int R = 9, STEP = 4;
    if (!rt_dark)
    for (int gx = ((cx - R) / STEP) * STEP; gx <= cx + R; gx += STEP) {
        for (int gy = ((cy - R) / STEP) * STEP; gy <= cy + R; gy += STEP) {
            if (gx < 1 || gy < 1 || gx >= kMapSize - 1 || gy >= kMapSize - 1) continue;
            unsigned v = tilemap[gx][gy];
            if (v != 0 && !(v & 0x80)) continue;      // solid wall tile -> no light here
            float dx = gx + 0.5f - s.cam.x, dy = gy + 0.5f - s.cam.y;
            if (dx * dx + dy * dy > (float)(R * R)) continue;
            rt::LightInst L{};
            L.x = gx + 0.5f; L.y = gy + 0.5f; L.z = 0.97f;
            L.r = 1.0f; L.g = 0.86f; L.b = 0.62f;   // warm tungsten
            L.intensity = 2.4f;
            L.radius = 5.5f;
            L.type = lights::LT_CEILING;
            s.lights.push_back(L);
        }
    }

    // Dynamic projectile lights: a rocket / fireball / needle lights the corridor
    // and the reflective floor as it flies past — the same moving-point-light trick
    // as the muzzle flash, attached to the projectile actor.
    for (const auto &pg : projGlow) {
        rt::LightInst L{};
        L.x = pg.x; L.y = pg.y; L.z = 0.5f;   // travels at ~eye height
        L.r = pg.r; L.g = pg.g; L.b = pg.b;
        L.intensity = 2.6f;
        L.radius = 4.5f;
        L.type = lights::LT_MUZZLE;           // bright moving point light
        L.flicker = 0;
        s.lights.push_back(L);
    }
}

} // namespace rt

// True when a classic-only present of the 3D view is a DISSOLVE that must show the
// classic view itself: the death fizzle (reddening) or a respawn/secret fizzle-in.
// A plain palette fade-in (fizzlein == 0, not dying) returns false, so the renderer
// fades the path-traced view in instead of the flat classic one.
extern "C" int RT_ClassicViewIsFizzle(void) {
    return (playstate == ex_died || fizzlein) ? 1 : 0;
}
