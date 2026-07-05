// compare_renderer.h
//
// Comparison compositing: combines the classic software frame and the
// path-traced frame into one image (split, moving wipe, or freeze). Comparison
// is a core feature of the project, not an afterthought.

#ifndef WOLFPT_COMPARE_RENDERER_H
#define WOLFPT_COMPARE_RENDERER_H

#include <stdint.h>
#include <vector>

namespace compare {

enum Layout { SPLIT = 0, WIPE, FREEZE };

// Composite classic + pt into `out` (sized pw*ph). Classic is nearest-upscaled
// to the PT resolution. `wipePos` in 0..1 for WIPE. Returns via out/outW/outH.
void Composite(int layout,
               const uint32_t *classic, int cw, int ch,
               const uint32_t *pt, int pw, int ph,
               float wipePos,
               std::vector<uint32_t> &out, int *outW, int *outH);

} // namespace compare

#endif // WOLFPT_COMPARE_RENDERER_H
