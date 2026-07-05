// screenshot_tools.cpp
//
// Exports classic / path-traced / split-comparison screenshots from the same
// frame. Writes 24-bit BMP files (no external image library required).

#include "../compare/compare_renderer.h"
#include <stdio.h>
#include <stdint.h>
#include <vector>
#include <string.h>
#include <time.h>

static void WriteBMP(const char *path, const uint32_t *rgba, int w, int h) {
    if (!rgba || w <= 0 || h <= 0) return;
    FILE *f = fopen(path, "wb");
    if (!f) { printf("[shot] cannot open %s\n", path); return; }

    int rowBytes = w * 3;
    int pad = (4 - (rowBytes & 3)) & 3;
    int imgSize = (rowBytes + pad) * h;
    int fileSize = 54 + imgSize;

    uint8_t hdr[54];
    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(hdr + 2, &fileSize, 4);
    int off = 54; memcpy(hdr + 10, &off, 4);
    int infoSize = 40; memcpy(hdr + 14, &infoSize, 4);
    memcpy(hdr + 18, &w, 4);
    memcpy(hdr + 22, &h, 4);
    uint16_t planes = 1, bpp = 24;
    memcpy(hdr + 26, &planes, 2);
    memcpy(hdr + 28, &bpp, 2);
    memcpy(hdr + 34, &imgSize, 4);
    fwrite(hdr, 1, 54, f);

    std::vector<uint8_t> row(rowBytes + pad, 0);
    for (int y = h - 1; y >= 0; --y) {   // BMP is bottom-up
        for (int x = 0; x < w; ++x) {
            uint32_t p = rgba[y * w + x];   // 0xAABBGGRR
            uint8_t r = p & 0xFF, g = (p >> 8) & 0xFF, b = (p >> 16) & 0xFF;
            row[x * 3 + 0] = b;             // BMP is BGR
            row[x * 3 + 1] = g;
            row[x * 3 + 2] = r;
        }
        fwrite(row.data(), 1, rowBytes + pad, f);
    }
    fclose(f);
    printf("[shot] wrote %s (%dx%d)\n", path, w, h);
}

static void Stamp(char *buf, size_t n, const char *kind) {
    static int counter = 0;
    time_t t = time(nullptr);
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &t);   // MSVC/MinGW CRT: (struct tm*, const time_t*)
#else
    localtime_r(&t, &tmv);
#endif
    snprintf(buf, n, "wolf3dpt_%02d%02d%02d_%03d_%s.bmp",
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec, counter++, kind);
}

// Called from rt_renderer.cpp RENDER_Screenshot().
extern "C" void SHOT_Capture(int which, const uint32_t *classic, int cw, int ch,
                             const uint32_t *pt, int pw, int ph) {
    char name[128];
    bool havePT = pt && pw > 0 && ph > 0;

    if ((which == 0 || which == 1) && classic) {
        Stamp(name, sizeof(name), "classic");
        WriteBMP(name, classic, cw, ch);
    }
    if ((which == 0 || which == 2) && havePT) {
        Stamp(name, sizeof(name), "pathtraced");
        WriteBMP(name, pt, pw, ph);
    }
    if ((which == 0 || which == 3) && classic && havePT) {
        std::vector<uint32_t> split; int ow, oh;
        compare::Composite(compare::SPLIT, classic, cw, ch, pt, pw, ph,
                           0.5f, split, &ow, &oh);
        Stamp(name, sizeof(name), "split");
        WriteBMP(name, split.data(), ow, oh);
    }
}
