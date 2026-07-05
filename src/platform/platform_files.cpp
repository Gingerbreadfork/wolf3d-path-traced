// platform_files.cpp
//
// Data-file path resolution. The original game opens data files with bare
// relative names ("vswap.wl1", ...). We let the user point the engine at a
// data directory (via --data or the WOLF3D_DATA env var) and chdir there so
// the original id_ca.cpp file code keeps working unchanged.

#include "platform.h"
#include <SDL.h>          // SDL_GetBasePath: portable "directory of the executable"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>       // _chdir
#define chdir _chdir
#else
#include <unistd.h>
#endif

static char g_dataDir[1024] = {0};

// Directory the executable lives in (with trailing separator), or "" if unknown.
// Cached; safe to call before SDL_Init on the platforms we target.
static const char *ExeDir(void) {
    static char dir[1024] = {0};
    static int  done = 0;
    if (!done) {
        done = 1;
        char *base = SDL_GetBasePath();
        if (base) { strncpy(dir, base, sizeof(dir) - 1); SDL_free(base); }
    }
    return dir;
}

void PLAT_SetDataDir(const char *dir) {
    if (!dir) return;
    strncpy(g_dataDir, dir, sizeof(g_dataDir) - 1);
    g_dataDir[sizeof(g_dataDir) - 1] = 0;
}

const char *PLAT_DataDir(void) { return g_dataDir; }

// Resolve the shader directory once. Order: WOLFPT_SHADER_DIR env override,
// then <exe-dir>/shaders (portable/downloaded builds), then the build-time
// default baked by CMake.
const char *PLAT_ShaderDir(void) {
    static char dir[1024] = {0};
    if (dir[0]) return dir;

    char baseShaders[1024] = {0};
    if (ExeDir()[0]) snprintf(baseShaders, sizeof(baseShaders), "%sshaders", ExeDir());

    const char *cands[3];
    int n = 0;
    const char *env = getenv("WOLFPT_SHADER_DIR");
    if (env)             cands[n++] = env;
    if (baseShaders[0])  cands[n++] = baseShaders;
#ifdef WOLFPT_SHADER_DIR
    cands[n++] = WOLFPT_SHADER_DIR;
#endif
    for (int i = 0; i < n; ++i) {
        char probe[1200];
        struct stat st;
        snprintf(probe, sizeof(probe), "%s/pathtrace.comp.spv", cands[i]);
        if (stat(probe, &st) == 0) {
            strncpy(dir, cands[i], sizeof(dir) - 1);
            return dir;
        }
    }
    // Nothing verified; fall back to the build-time default (may not exist).
#ifdef WOLFPT_SHADER_DIR
    strncpy(dir, WOLFPT_SHADER_DIR, sizeof(dir) - 1);
#else
    strncpy(dir, "shaders", sizeof(dir) - 1);
#endif
    return dir;
}

// Called early from main() before the game touches any data file.
extern "C" int PLAT_ResolveAndEnterDataDir(void) {
    const char *candidates[8];
    char baseData[1024] = {0}, baseAssets[1024] = {0};
    int n = 0;
    if (g_dataDir[0]) candidates[n++] = g_dataDir;
    const char *env = getenv("WOLF3D_DATA");
    if (env) candidates[n++] = env;
    // Next to the executable (portable/downloaded release layout).
    if (ExeDir()[0]) {
        snprintf(baseData,   sizeof(baseData),   "%sdata", ExeDir());
        snprintf(baseAssets, sizeof(baseAssets), "%sassets/data", ExeDir());
        candidates[n++] = baseData;      // <exe>/data
        candidates[n++] = baseAssets;    // <exe>/assets/data
        candidates[n++] = ExeDir();      // <exe>/
    }
#ifdef WOLFPT_DEFAULT_DATA_DIR
    candidates[n++] = WOLFPT_DEFAULT_DATA_DIR;
#endif
    candidates[n++] = ".";

    for (int i = 0; i < n; ++i) {
        char probe[1200];
        snprintf(probe, sizeof(probe), "%s/vswap.wl1", candidates[i]);
        struct stat st;
        if (stat(probe, &st) == 0) {
            if (chdir(candidates[i]) == 0) {
                PLAT_SetDataDir(candidates[i]);
                printf("[data] using %s\n", candidates[i]);
                return 1;
            }
        }
        // also try shareware/registered/SoD variants
        static const char *exts[] = {"wl6", "wl3", "sod", "sd1", "sdm"};
        for (int e = 0; e < 5; ++e) {
            snprintf(probe, sizeof(probe), "%s/vswap.%s", candidates[i], exts[e]);
            if (stat(probe, &st) == 0 && chdir(candidates[i]) == 0) {
                PLAT_SetDataDir(candidates[i]);
                printf("[data] using %s (%s)\n", candidates[i], exts[e]);
                return 1;
            }
        }
    }
    printf("[data] WARNING: no Wolfenstein data files found; game will abort on load.\n");
    return 0;
}
