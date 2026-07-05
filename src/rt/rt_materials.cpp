// rt_materials.cpp
//
// Decodes classic VSWAP data into RGBA textures and assigns materials.

// rt_materials.h first (default packing), then wl_def.h which sets pack(1).
#include "rt_materials.h"
#include "wl_def.h"          // PM_*, gamepal, t_compshape

extern SDL_Color gamepal[256];

namespace mat {

static inline uint32_t PalRGBA(uint8_t idx) {
    const SDL_Color &c = gamepal[idx];
    return 0xFF000000u | (uint32_t)c.b << 16 | (uint32_t)c.g << 8 | (uint32_t)c.r;
}

int WallPageCount()   { return PMSpriteStart; }
int SpritePageCount() { return PMSoundStart - PMSpriteStart; }

void DecodeWall(int wallPage, uint32_t *out) {
    if (wallPage < 0 || wallPage >= PMSpriteStart) {
        for (int i = 0; i < kTexSize * kTexSize; ++i) out[i] = 0xFFFF00FF; // magenta = missing
        return;
    }
    const uint8_t *src = PM_GetTexture(wallPage);   // 64x64, column-major
    for (int x = 0; x < kTexSize; ++x)
        for (int y = 0; y < kTexSize; ++y)
            out[y * kTexSize + x] = PalRGBA(src[x * kTexSize + y]);
}

void DecodeSprite(int spritePage, uint32_t *out) {
    for (int i = 0; i < kTexSize * kTexSize; ++i) out[i] = 0x00000000; // transparent
    if (spritePage < 0 || spritePage >= SpritePageCount()) return;

    const t_compshape *shape = (const t_compshape *)PM_GetSprite(spritePage);
    const uint8_t *base = (const uint8_t *)shape;

    const uint16_t *cmdptr = shape->dataofs;
    for (int x = shape->leftpix; x <= shape->rightpix; ++x, ++cmdptr) {
        const uint8_t *line = base + *cmdptr;
        for (;;) {
            uint16_t endy = *(const uint16_t *)line; line += 2;
            if (endy == 0) break;
            endy >>= 1;
            int16_t newstart = *(const int16_t *)line; line += 2;
            uint16_t starty = (*(const uint16_t *)line) >> 1; line += 2;
            for (uint16_t y = starty; y < endy; ++y) {
                uint8_t idx = base[newstart + y];
                if (x >= 0 && x < kTexSize && y < kTexSize)
                    out[y * kTexSize + x] = PalRGBA(idx);
            }
        }
    }
}

WallMaterial WallMat(int wallPage) {
    (void)wallPage;
    // Walls and doors are MATTE — no reflection at all. Any reflectivity here
    // let the environment show through them at grazing angles and read as glass.
    // Only the floor is reflective (a subtle sheen), handled in the shader.
    WallMaterial m{0.9f, 0.0f, 0.0f, 0.0f};   // rough, non-reflective
    return m;
}

} // namespace mat
