# Architecture

This is a **source modification**, not a clone. The guiding rule: the original
Wolfenstein 3D code remains the source of truth for the game simulation; the new
renderer visualises the state that code produces. The original files are edited
only where they need hooks.

## Layers

```
┌──────────────────────────────────────────────────────────────────────┐
│ Original game simulation  (src/wl_*.cpp, src/id_*.cpp)                 │
│   player, collision, actors/AI, doors, pushwalls, pickups, secrets,   │
│   levels, scoring, menus, save/load — UNCHANGED.                      │
│   Still rasterises the classic view into the 8-bit `screenBuffer`.    │
└───────────────┬──────────────────────────────────────────────────────┘
                │  RENDER_Frame3D()  (one hook in ThreeDRefresh)
                ▼
┌──────────────────────────────────────────────────────────────────────┐
│ Scene extraction   src/rt/rt_scene_extract.cpp                        │
│   Reads live globals (tilemap, actorat, objlist, statobjlist,         │
│   doorobjlist/doorposition, pwall*, player, viewx/viewy, weapon) into │
│   a renderer-friendly `rt::Scene` (rt_scene.h). No second simulation. │
└───────────────┬──────────────────────────────────────────────────────┘
                ▼
┌──────────────────────────────────────────────────────────────────────┐
│ Renderer backends                                                     │
│   Classic  — the 8-bit software frame, palette-converted and blitted  │
│              to the swapchain (rt_vulkan.cpp).                         │
│   Path tr. — rt_pathtrace.cpp: VK_KHR_ray_query compute tracer.       │
│              rt_materials.cpp (VSWAP -> RGBA atlas), rt_lights.cpp.    │
└───────────────┬──────────────────────────────────────────────────────┘
                ▼
┌──────────────────────────────────────────────────────────────────────┐
│ Compositor   src/compare/  +  rt_renderer.cpp                         │
│   Split / moving wipe / freeze; PT 3D + classic HUD strip + weapon.   │
│   Bloom, screenshots (src/compare/screenshot_tools.cpp).              │
└───────────────┬──────────────────────────────────────────────────────┘
                ▼
┌──────────────────────────────────────────────────────────────────────┐
│ Vulkan present   src/rt/rt_vulkan.cpp                                 │
│   Instance/device/swapchain; final RGBA blitted 4:3 letterboxed.      │
└──────────────────────────────────────────────────────────────────────┘
```

## Source-modification boundaries

| Concern | Treatment |
|---------|-----------|
| Original gameplay logic | **Preserved** (edited only for hooks) |
| Original renderer | **Preserved** as the "classic" mode |
| DOS/VGA video, input, timing, audio | **Replaced** with an SDL2 platform layer |
| SDL 1.2 API surface | Bridged (`src/platform/sdl_compat.h`) — keys ported to SDL2 scancodes |
| SDL_mixer | Replaced by a tiny own bridge (`src/platform/platform_audio.cpp`) |
| Path tracer, scene extraction, compositor | **New**, added beside the original |

### The hooks (grep for these)

* `wl_draw.cpp` `ThreeDRefresh()` → one `RENDER_Frame3D()` call after the classic
  view is drawn.
* `wl_def.h` → `#define SDL_Flip(x) PLAT_PresentClassic()` routes every present
  through the Vulkan renderer.
* `id_vl.cpp` `VL_SetVGAPlaneMode()` → creates the SDL2 Vulkan window and inits
  the renderer; `PLAT_PresentClassic()` palette-converts `screenBuffer` and hands
  it to `RENDER_PresentClassic()`.
* `wl_main.cpp` `main()` → `PLAT_ResolveAndEnterDataDir()` before data load.
* `id_in.cpp` → SDL2 scancode input + renderer hotkeys + autopilot tick.

## The path tracer (rt_pathtrace.cpp + shaders/pathtrace.comp)

* **Acceleration structures.** Two static BLASes (a unit cube, a unit quad) are
  instanced by a TLAS that is rebuilt every frame from `rt::Scene`:
  * walls / doors / pushwalls → cube instances (opaque),
  * floor / ceiling → large quad instances (opaque),
  * sprites → quad instances flagged `FORCE_NO_OPAQUE` so the shader's alpha test
    runs (transparent texels are skipped, giving correct silhouettes **and**
    sprite shadows).
  Each instance carries a `customIndex` into a per-instance data buffer
  (type, texture page(s), material, sprite billboard axes).
* **Textures.** VSWAP wall pages and column-compressed sprites are decoded
  (`rt_materials.cpp`) into a single `sampler2DArray` atlas (one layer per page).
* **Shading.** Primary ray → analytic UV/normal per surface type → texture
  fetch → per-light shading with hard shadow rays that respect sprite alpha →
  ambient occlusion → a glossy Fresnel reflection bounce on the floor only.
  The tracer is **deterministic** — fixed 4× sub-pixel AA, fixed AO directions,
  hard shadows, no RNG and no temporal accumulation — so a static scene renders
  byte-identical frames and there is zero shimmer (no denoiser needed). A GPU
  post pass (`shaders/post.comp`) adds bloom, ACES tone mapping and a vignette.
* **Camera.** Uses the raycaster focal point (`viewx/viewy`) and the exact
  classic horizontal FOV `2·atan((VIEWGLOBAL/2)/(FOCALLENGTH+MINDIST))`. Because
  the classic projection is isotropic in world space, the path-traced framing
  matches the classic view and the two line up in comparison modes.

## A note on `#pragma pack(1)`

`wl_def.h` sets `#pragma pack(1)` for the whole translation unit (the original
on-disk map/save struct layouts depend on it). Renderer/platform headers shared
across the game↔renderer boundary (`platform.h`, `mixer_compat.h`, `rt_scene.h`)
therefore guard their structs so their layout is consistent whether or not
`wl_def.h` is in scope — otherwise the mismatched layouts corrupt memory.

## File map

```
src/
  wl_*.cpp, id_*.cpp        original game (authoritative simulation)
  rt/
    rt_scene.h              the renderer-facing scene description
    rt_scene_extract.cpp    live globals -> rt::Scene
    rt_vulkan.{h,cpp}       Vulkan device/swapchain/present + helpers
    rt_pathtrace.{h,cpp}    ray-query compute path tracer (BLAS/TLAS/dispatch)
    rt_materials.{h,cpp}    VSWAP -> RGBA texture/sprite atlas + materials
    rt_lights.{h,cpp}       renderer-only lighting layer
    rt_renderer.cpp         mode dispatch, compositor, bloom, screenshots glue
    render_api.h            the C interface the game code calls
  platform/
    platform_sdl.cpp        SDL2 window (Vulkan) + services
    platform_audio.cpp      SDL2 audio + Mix_* bridge (replaces SDL_mixer)
    platform_files.cpp      data directory resolution
    autopilot.{h,cpp}       scripted input + screenshots for headless testing
    sdl_compat.h            SDL 1.2 -> SDL2 shims
    mixer_compat.h          Mix_* API surface
  compare/
    compare_renderer.{h,cpp} split / wipe / freeze compositing
    screenshot_tools.cpp     classic / PT / split BMP export
shaders/
  pathtrace.comp            the GLSL ray-query compute path tracer
  post.comp                 GPU bloom + ACES tone map + vignette
```
