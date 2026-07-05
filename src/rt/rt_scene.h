// rt_scene.h
//
// The renderer-facing scene description. Each frame the extractor
// (rt_scene_extract.cpp) reads the live original game globals and fills one of
// these; the renderer consumes it. There is only ever ONE simulation.
//
// World convention: one map tile == 1.0 world unit. Wolf3D fixed-point world
// positions (16.16) are divided by 65536 to get world units. Walls occupy the
// unit cube [tx,tx+1] x [ty,ty+1] with floor at z=0 and ceiling at z=1. The
// camera rides at z=0.5.

#ifndef WOLFPT_RT_SCENE_H
#define WOLFPT_RT_SCENE_H

#include <stdint.h>
#include <vector>

namespace rt {

constexpr int   kMapSize   = 64;

// A solid wall block occupying one tile.
struct WallInst {
    float   tx, ty;        // tile coordinate (integer as float)
    int32_t texEW;         // texture page for east/west (vertical) faces
    int32_t texNS;         // texture page for north/south (horizontal) faces
};

// A sliding door slab.
struct DoorInst {
    float   tx, ty;
    int     vertical;      // 1 = slides along Y wall gap (vertical door)
    float   open;          // 0 = shut, 1 = fully retracted
    int32_t texPage;       // VSWAP door texture page
    int     lock;          // dr_normal / dr_lockN / dr_elevator
};

// A moving pushwall block (secret wall being shoved).
struct PushwallInst {
    float   x, y;          // current world position of the block origin
    int32_t texEW, texNS;
};

// A billboard sprite (static object or actor). Rendered as an alpha-tested
// card that faces the camera and casts real shadows.
struct SpriteInst {
    float   x, y;          // world position (tile center for statics)
    int32_t texPage;       // sprite page (relative index into sprite atlas)
    int     kind;          // 0 static, 1 actor, 2 projectile
    float   emissive;      // >0 for glowing pickups/effects
};

// A point/area light attached to the map (lamps, signs, muzzle, etc).
struct LightInst {
    float   x, y, z;
    float   r, g, b;
    float   intensity;
    float   radius;
    int     type;          // rt_lights.h LightType
    int     flicker;
};

struct Camera {
    float   x, y, z;
    float   angleRad;      // yaw (Wolf3D angle, 0 = +X, CCW)
    float   fovRad;
    float   pitch;         // usually 0 (no vertical aim in classic)
};

struct Scene {
    Camera                    cam;
    std::vector<WallInst>     walls;
    std::vector<DoorInst>     doors;
    std::vector<PushwallInst> pushwalls;
    std::vector<SpriteInst>   sprites;
    std::vector<LightInst>    lights;

    int      levelNumber = -1;
    int      weaponFrame = -1;  // active weapon sprite page, -1 none
    float    muzzleFlash = 0.f; // 0..1 muzzle-flash strength this frame

    void clearDynamic() {
        doors.clear();
        pushwalls.clear();
        sprites.clear();
        lights.clear();
    }
};

} // namespace rt

#endif // WOLFPT_RT_SCENE_H
