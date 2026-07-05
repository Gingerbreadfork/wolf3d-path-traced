cmake_minimum_required(VERSION 3.16)
project(wolf3d-pt LANGUAGES C CXX)

# -----------------------------------------------------------------------------
# Wolfenstein 3D: Path Traced
#
# A source modification of the original Wolfenstein 3D (via the clean
# Chocolate-Wolfenstein-3D C++ modernization) with:
#   * a modern SDL2 platform layer replacing the DOS/SDL1.2 shell
#   * a Vulkan hardware ray-traced renderer grafted alongside the classic one
#   * scene extraction from the original live game state (single simulation)
#
# The original game files (wl_*.cpp, id_*.cpp) remain the source of truth.
# -----------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# ---- Dependencies -----------------------------------------------------------
find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2)
find_package(Vulkan REQUIRED)

# SDL_mixer is optional (classic AdLib/PC-speaker emulation is built-in).
pkg_check_modules(SDL2_MIXER SDL2_mixer)

# ---- Original game sources (kept recognizable) ------------------------------
set(GAME_SOURCES
    src/id_ca.cpp
    src/id_in.cpp
    src/id_pm.cpp
    src/id_sd.cpp
    src/id_us_1.cpp
    src/id_vh.cpp
    src/id_vl.cpp
    src/signon.cpp
    src/wl_act1.cpp
    src/wl_act2.cpp
    src/wl_agent.cpp
    src/wl_debug.cpp
    src/wl_draw.cpp
    src/wl_game.cpp
    src/wl_inter.cpp
    src/wl_main.cpp
    src/wl_menu.cpp
    src/wl_play.cpp
    src/wl_state.cpp
    src/wl_text.cpp
    src/fmopl.cpp
)

# ---- New platform + renderer sources ----------------------------------------
set(NEW_SOURCES
    src/platform/platform_sdl.cpp
    src/platform/platform_audio.cpp
    src/platform/platform_files.cpp
    src/platform/autopilot.cpp
    src/rt/rt_vulkan.cpp
    src/rt/rt_scene_extract.cpp
    src/rt/rt_materials.cpp
    src/rt/rt_lights.cpp
    src/rt/rt_renderer.cpp
    src/rt/rt_pathtrace.cpp
    src/compare/compare_renderer.cpp
    src/compare/screenshot_tools.cpp
)

add_executable(wolf3d-pt ${GAME_SOURCES} ${NEW_SOURCES})

target_include_directories(wolf3d-pt PRIVATE
    src
    src/rt
    src/platform
    src/compare
    ${SDL2_INCLUDE_DIRS}
    ${Vulkan_INCLUDE_DIRS}
)

target_link_libraries(wolf3d-pt PRIVATE
    ${SDL2_LIBRARIES}
    Vulkan::Vulkan
    m
)

if(SDL2_MIXER_FOUND)
    target_compile_definitions(wolf3d-pt PRIVATE HAVE_SDL2_MIXER=1)
    target_include_directories(wolf3d-pt PRIVATE ${SDL2_MIXER_INCLUDE_DIRS})
    target_link_libraries(wolf3d-pt PRIVATE ${SDL2_MIXER_LIBRARIES})
endif()

# The original DOS-era source needs a permissive dialect.
target_compile_options(wolf3d-pt PRIVATE
    ${SDL2_CFLAGS_OTHER}
    -fpermissive
    -fno-strict-aliasing
    -Wno-narrowing
    -Wno-write-strings
    -Wno-format-security
    -Wno-implicit-fallthrough
)

# NOTE: do NOT define VERSIONALREADYCHOSEN — that would disable version.h's
# version selection (UPLOAD / CARMACIZED for shareware v1.4).
target_compile_definitions(wolf3d-pt PRIVATE _GNU_SOURCE)

# ---- Shader compilation (GLSL -> SPIR-V) ------------------------------------
find_program(GLSLC glslc)
if(NOT GLSLC)
    message(FATAL_ERROR "glslc not found (install shaderc / Vulkan SDK)")
endif()

set(SHADER_SRC_DIR ${CMAKE_SOURCE_DIR}/shaders)
set(SHADER_OUT_DIR ${CMAKE_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${SHADER_OUT_DIR})

file(GLOB SHADER_SOURCES
    ${SHADER_SRC_DIR}/*.comp
    ${SHADER_SRC_DIR}/*.vert
    ${SHADER_SRC_DIR}/*.frag
)

set(SPV_OUTPUTS "")
foreach(shader ${SHADER_SOURCES})
    get_filename_component(fname ${shader} NAME)
    set(spv ${SHADER_OUT_DIR}/${fname}.spv)
    add_custom_command(
        OUTPUT ${spv}
        COMMAND ${GLSLC} --target-env=vulkan1.2 --target-spv=spv1.4 -O
                ${shader} -o ${spv}
        DEPENDS ${shader}
        COMMENT "Compiling shader ${fname}"
        VERBATIM
    )
    list(APPEND SPV_OUTPUTS ${spv})
endforeach()

add_custom_target(shaders DEPENDS ${SPV_OUTPUTS})
add_dependencies(wolf3d-pt shaders)

# Let the runtime find compiled shaders next to the build tree by default.
target_compile_definitions(wolf3d-pt PRIVATE
    WOLFPT_SHADER_DIR="${SHADER_OUT_DIR}"
    WOLFPT_DEFAULT_DATA_DIR="${CMAKE_SOURCE_DIR}/assets/data"
)

install(TARGETS wolf3d-pt RUNTIME DESTINATION bin)
