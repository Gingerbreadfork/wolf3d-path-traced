// platform_files.cpp
//
// Data-file path resolution. The original game opens data files with bare
// relative names ("vswap.wl1", ...). We let the user point the engine at a
// data directory (via --data or the WOLF3D_DATA env var) and chdir there so
// the original id_ca.cpp file code keeps working unchanged.

#include "platform.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>

static char g_dataDir[1024] = {0};

void PLAT_SetDataDir(const char *dir) {
    if (!dir) return;
    strncpy(g_dataDir, dir, sizeof(g_dataDir) - 1);
    g_dataDir[sizeof(g_dataDir) - 1] = 0;
}

const char *PLAT_DataDir(void) { return g_dataDir; }

// Called early from main() before the game touches any data file.
extern "C" int PLAT_ResolveAndEnterDataDir(void) {
    const char *candidates[4];
    int n = 0;
    if (g_dataDir[0]) candidates[n++] = g_dataDir;
    const char *env = getenv("WOLF3D_DATA");
    if (env) candidates[n++] = env;
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
