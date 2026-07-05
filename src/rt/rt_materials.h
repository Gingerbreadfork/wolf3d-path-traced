// rt_materials.h
//
// Material + texture layer that sits on top of the original VSWAP texture
// references. Decodes the classic 8-bit wall textures and column-compressed
// sprites into RGBA (with alpha for sprites) so the path tracer can sample
// them, and assigns PBR-ish material parameters per wall/sprite class.

#ifndef WOLFPT_RT_MATERIALS_H
#define WOLFPT_RT_MATERIALS_H

#include <stdint.h>

namespace mat {

constexpr int kTexSize = 64;   // all Wolf3D textures/sprites are 64x64

int WallPageCount();     // number of wall texture pages (== PMSpriteStart)
int SpritePageCount();   // number of sprite pages

// Decode a wall texture page into a 64x64 RGBA (0xAABBGGRR) image (opaque).
void DecodeWall(int wallPage, uint32_t *out /*64*64*/);

// Decode a sprite page (index relative to PMSpriteStart) into 64x64 RGBA with
// alpha (0 where transparent). Used for alpha-tested billboards + sprite shadows.
void DecodeSprite(int spritePage, uint32_t *out /*64*64*/);

// PBR material description for a wall texture page.
struct WallMaterial {
    float roughness;
    float metallic;
    float reflectivity;
    float emissive;      // emissive strength multiplier
};
WallMaterial WallMat(int wallPage);

} // namespace mat

#endif // WOLFPT_RT_MATERIALS_H
