/* Equivalence probe: the direct region write against the whole-level round trip.
 *
 * The port's mirrored write path takes the WHOLE level out of the pool, copies the region, and
 * puts the level back (nvn_deswizzle -> memcpy -> nvn_swizzle). It moves 8.39 MB to write 25.3 KB
 * in the real population (331x). The direct write (nvn_write_region_gob) puts each 16-byte segment
 * of the region where the block-linear layout wants it.
 *
 * This probe runs BOTH paths over the same pool and compares the pools byte for byte. It links the
 * port's own nvn_tex.c and nvn_deswizzle.c, so it tests the code that ships, not a copy of it.
 * (The first two probe rounds in this project reported mismatches that were the probe's own bugs --
 * the level's rows instead of nvn_deswizzle_geom's, and img treated as a xnz stride -- so the round
 * trip below mirrors nvn_mtl.m's loop exactly.)
 */
#include "nvn_tex.h"
#include "nvn_deswizzle.h"
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static uint32_t g_rng = 0x13579bdfu;
static uint32_t rnd(void) { g_rng = g_rng * 1664525u + 1013904223u; return g_rng >> 8; }

/* The port's round trip, exactly as mtl_write_texels does it (the mirrored branch). */
static int round_trip(const nvn_texdesc *d, unsigned level, unsigned layer, uint8_t *pool,
                      const uint8_t *src, size_t row, size_t img, size_t x0, size_t wb, size_t y0, size_t hb,
                      unsigned nz, size_t zoffset, int is3d, size_t lp, size_t lr, size_t lz) {
    uint8_t *lin = (uint8_t *)malloc(lp * lr * lz);
    if (!lin) return -1;
    for (int z = 0; z < (int)nz; z++) {
        unsigned lay = is3d ? 0u : layer + (unsigned)zoffset + (unsigned)z;
        if ((is3d && z == 0) || !is3d)
            if (nvn_deswizzle(d, level, lay, pool, lin, lp)) { free(lin); return -1; }
        uint8_t *dst = lin + (is3d ? (size_t)(zoffset + (size_t)z) * lp * lr : 0);
        for (size_t y = 0; y < hb; y++)
            memcpy(dst + (y0 + y) * lp + x0, src + (size_t)z * img + y * row, wb);
        if (!is3d || z == (int)nz - 1)
            if (nvn_swizzle(d, level, lay, lin, lp, pool)) { free(lin); return -1; }
    }
    free(lin);
    return 0;
}

/* The direct write, as the port calls it. */
static int direct_write(const nvn_texdesc *d, unsigned level, unsigned layer, uint8_t *pool,
                        const uint8_t *src, size_t row, size_t img, size_t x0, size_t wb, size_t y0, size_t hb,
                        unsigned nz, size_t zoffset, int is3d) {
    for (int z = 0; z < (int)nz; z++) {
        unsigned lay = is3d ? 0u : layer + (unsigned)zoffset + (unsigned)z;
        uint32_t slice = is3d ? (uint32_t)(zoffset + (size_t)z) : 0;
        if (nvn_write_region_gob(d, level, lay, pool, src + (size_t)z * img, row, x0, wb, y0, hb, slice)) return -1;
    }
    return 0;
}

static int g_cases = 0, g_mismatch = 0, g_skipped = 0;

static void one(unsigned target, unsigned format, unsigned W, unsigned H, unsigned D, unsigned levels,
                unsigned layer0, unsigned xoff, unsigned yoff, unsigned w, unsigned h, unsigned depth) {
    nvn_texdesc d = { .target = (int)target, .format = (int)format, .samples = 1, .width = W, .height = H,
                      .depth = D, .levels = levels };
    nvn_texlayout L; nvn_tex_layout(&d, &L);
    if (!L.size) { g_skipped++; return; }
    int bw, bh; uint32_t bpb = nvn_fmt_bpb((int)format, &bw, &bh);
    if (!bpb) { g_skipped++; return; }
    int is3d = target == 2;
    unsigned level = 0;
    if (levels > 1) level = 1;                     /* a mip, where the geometry differs */
    unsigned lw = W >> level ? W >> level : 1, lh = H >> level ? H >> level : 1;
    size_t lp = (size_t)((lw + bw - 1) / bw) * bpb, lr = (lh + bh - 1) / bh;
    size_t lz = is3d ? (size_t)(D >> level ? D >> level : 1) : 1;
    /* the region must be inside the level */
    size_t wb = (size_t)((w + bw - 1) / bw) * bpb, hb = (h + bh - 1) / bh;
    size_t x0 = (size_t)(xoff / bw) * bpb, y0 = yoff / bh;
    if (x0 + wb > lp || y0 + hb > lr) { g_skipped++; return; }
    if (!is3d && layer0 + depth > L.layers) { g_skipped++; return; }
    if (is3d && (size_t)depth > lz) { g_skipped++; return; }

    size_t row = wb + 17, img = row * hb + 13;      /* the source's strides, deliberately not aligned */
    uint8_t *src = (uint8_t *)malloc(img * depth);
    if (!src) { g_skipped++; return; }
    for (size_t i = 0; i < img * depth; i++) src[i] = (uint8_t)rnd();

    uint8_t *a = (uint8_t *)malloc(L.size), *b = (uint8_t *)malloc(L.size);
    if (!a || !b) { free(src); free(a); free(b); g_skipped++; return; }
    for (size_t i = 0; i < L.size; i++) a[i] = b[i] = (uint8_t)rnd();

    if (round_trip(&d, level, layer0, a, src, row, img, x0, wb, y0, hb, depth, 0, is3d, lp, lr, lz) ||
        direct_write(&d, level, layer0, b, src, row, img, x0, wb, y0, hb, depth, 0, is3d)) {
        free(src); free(a); free(b); g_skipped++; return;
    }
    g_cases++;
    if (memcmp(a, b, L.size)) {
        g_mismatch++;
        size_t at = 0; while (at < L.size && a[at] == b[at]) at++;
        fprintf(stderr, "MISMATCH target=%u format=%#x %ux%u D=%u lv=%u levels=%u layer=%u region %ux%u+%u,%u depth=%u: byte %zu of %llu\n",
                target, format, W, H, D, level, levels, layer0, w, h, xoff, yoff, depth, at, (unsigned long long)L.size);
    }
    free(src); free(a); free(b);
}

int main(void) {
    /* the textures the game actually creates (DM_NVN_SIZELOG census), plus the formats it uses */
    static const unsigned sizes[][2] = { {320,180},{160,90},{1280,720},{80,45},{640,360},{1024,1024},{1920,1080},{256,256} };
    static const unsigned formats[] = { 0x25, 0x26, 0x2e, 0x33, 0x48, 0x4a };
    for (unsigned f = 0; f < sizeof formats / sizeof *formats; f++)
        for (unsigned s = 0; s < sizeof sizes / sizeof *sizes; s++)
            for (unsigned lv = 1; lv <= 4; lv++) {
                unsigned W = sizes[s][0], H = sizes[s][1];
                /* several region shapes, including an x offset that is not a multiple of 16 */
                one(1, formats[f], W, H, 1, lv, 0, 0, 0, W, H, 1);
                one(1, formats[f], W, H, 1, lv, 0, 1, 1, W - 2, H - 2, 1);
                one(1, formats[f], W, H, 1, lv, 0, 3, 2, W / 2, H / 2, 1);
                one(1, formats[f], W, H, 1, lv, 0, 7, 5, 13, 9, 1);
                one(1, formats[f], W, H, 1, lv, 0, 15, 1, 31, 7, 1);
            }
    /* 3D, with a z offset and a depth of slices */
    for (unsigned f = 0; f < sizeof formats / sizeof *formats; f++)
        for (unsigned s = 0; s < sizeof sizes / sizeof *sizes; s++) {
            unsigned W = sizes[s][0], H = sizes[s][1];
            one(2, formats[f], W, H, 4, 1, 0, 0, 0, W, H, 1);
            one(2, formats[f], W, H, 4, 1, 0, 0, 0, W / 2, H / 2, 2);
            one(2, formats[f], W, H, 4, 1, 0, 3, 1, 17, 11, 2);
        }
    /* 2D arrays and cubemaps: the layer is part of the address */
    for (unsigned f = 0; f < sizeof formats / sizeof *formats; f++)
        for (unsigned s = 0; s < sizeof sizes / sizeof *sizes; s++) {
            unsigned W = sizes[s][0], H = sizes[s][1];
            one(4, formats[f], W, H, 3, 1, 0, 0, 0, W, H, 1);
            one(4, formats[f], W, H, 3, 1, 1, 0, 0, W / 2, H / 2, 1);
            one(4, formats[f], W, H, 3, 1, 2, 5, 3, 23, 13, 1);
            one(8, formats[f], W, H, 1, 1, 0, 0, 0, W, H, 1);
            one(8, formats[f], W, H, 1, 1, 3, 2, 1, 19, 7, 1);
        }
    printf("%s: %d configurations compared, %d mismatches, %d skipped\n",
           g_mismatch ? "FAIL" : "PASS", g_cases, g_mismatch, g_skipped);
    if (g_mismatch) return 1;
    if (!g_cases) { puts("FAIL: nothing was compared"); return 1; }
    return 0;
}
