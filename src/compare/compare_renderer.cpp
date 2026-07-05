// compare_renderer.cpp  — see compare_renderer.h

#include "compare_renderer.h"
#include <string.h>

namespace compare {

static inline uint32_t sampleNN(const uint32_t *src, int sw, int sh,
                                int x, int y, int dw, int dh) {
    int sx = x * sw / dw, sy = y * sh / dh;
    if (sx >= sw) sx = sw - 1;
    if (sy >= sh) sy = sh - 1;
    return src[sy * sw + sx];
}

void Composite(int layout,
               const uint32_t *classic, int cw, int ch,
               const uint32_t *pt, int pw, int ph,
               float wipePos,
               std::vector<uint32_t> &out, int *outW, int *outH) {
    out.resize((size_t)pw * ph);
    *outW = pw; *outH = ph;

    switch (layout) {
    case SPLIT: {
        int div = pw / 2;
        for (int y = 0; y < ph; ++y)
            for (int x = 0; x < pw; ++x) {
                uint32_t px = (x < div) ? sampleNN(classic, cw, ch, x, y, pw, ph)
                                        : pt[y * pw + x];
                if (x == div || x == div - 1) px = 0xFF00FF00; // green seam
                out[y * pw + x] = px;
            }
        break;
    }
    case WIPE: {
        int wx = (int)(wipePos * pw);
        for (int y = 0; y < ph; ++y)
            for (int x = 0; x < pw; ++x) {
                uint32_t px = (x < wx) ? pt[y * pw + x]
                                       : sampleNN(classic, cw, ch, x, y, pw, ph);
                if (x == wx || x == wx - 1) px = 0xFFFFFFFF; // bright wipe edge
                out[y * pw + x] = px;
            }
        break;
    }
    case FREEZE:
    default:
        memcpy(out.data(), pt, (size_t)pw * ph * 4);
        break;
    }
}

} // namespace compare
