// rt_lights.h
//
// The lighting layer that attaches to the original maps. Adds renderer-only
// light sources (ceiling lamps, wall lamps, signs, treasure glints, muzzle
// flashes, exit elevators). Does not affect gameplay.

#ifndef WOLFPT_RT_LIGHTS_H
#define WOLFPT_RT_LIGHTS_H

#include "rt_scene.h"

namespace lights {

enum LightType {
    LT_CEILING = 0,   // ceiling lamp / chandelier
    LT_WALL,          // wall lamp / torch
    LT_SIGN,          // emissive sign
    LT_PICKUP,        // treasure / key glow
    LT_MUZZLE,        // muzzle flash
    LT_EXIT,          // exit elevator
    LT_AMBIENT_FILL   // soft fill so the campaign stays playable
};

// Populate scene.lights from the current scene contents (statics/actors) and
// the muzzle-flash state. Keeps the full campaign visible (not a darkness pass).
void Build(rt::Scene &s);

} // namespace lights

#endif // WOLFPT_RT_LIGHTS_H
