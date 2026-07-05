// rt_pathtrace.h
//
// The Vulkan hardware path tracer (ray-query compute). Consumes an extracted
// rt::Scene and produces an RGBA frame. Acceleration structures are (re)built
// from the scene each frame; materials/textures are uploaded once per level.

#ifndef WOLFPT_RT_PATHTRACE_H
#define WOLFPT_RT_PATHTRACE_H

#include "rt_scene.h"
#include <stdint.h>

namespace rtpt {

void Init();
void Shutdown();
bool Ready();                              // false if RT unsupported / not built

void DesiredResolution(int *w, int *h);    // internal render resolution
void Render(const rt::Scene &scene, uint32_t *out, int w, int h);

// Runtime tunable: reflection-bounce count (bound to the '[' / ']' hotkeys).
enum Setting { SET_BOUNCES = 0 };
void AdjustSetting(int which, int delta);

} // namespace rtpt

#endif // WOLFPT_RT_PATHTRACE_H
