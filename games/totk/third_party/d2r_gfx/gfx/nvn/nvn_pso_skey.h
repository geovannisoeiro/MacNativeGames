#pragma once
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "nvn_obj.h"
#include "nvn_border_id.h"

typedef struct {
    const void *vs, *fs; uint32_t cfmt[8], dfmt, sfmt, samples, blend_en, chmask, a2c; blend_st blend[8];
    va_st attr[16]; vs_st stream[16];
    uint8_t lmap[8]; const void *fsfn;
    // TOTK (2026-10-21): the identity of the sampler's border-colour variant. It is a HASH of the
    // border_key string of both stages, never a pointer: the patch caches (fs->bpatch / vs->vpatch)
    // are per NvShader INSTANCE, and the warm-up and the game do not share instances, so a pointer
    // in the key could never match. 0 = no border. The field is at the END so that the legacy rows
    // (which are, by construction, rows without a border) decode with it zeroed.
    uint64_t bid;
} pso_key;
// pso_key.a2c: bit 0 alpha-to-coverage; bit 1 (TOTK) layered pass, the vertex function writes the render target array index;
// bits 2-3 (TOTK_MFX) MetalFX copy role of the draw: 1 scene to attachment 6, 2 / 3 UI (blended / opaque) to attachment 7;
// bit 4 (TOTK) the vertex function emulates a sampler border colour Metal lacks (vs_fn_for, border_key).
#define PSO_LAYERED 2u
#define PSO_VBORDER 16u

typedef struct {
    uint8_t kind, fbmask;
    int8_t fx[32];
} pso_fz_desc;

// Runtime-translated TOTK variant keys are up to 79 bytes (SHA-256 plus answer hash and _b).
// The old 24-byte fields silently truncated them and made recorded render PSOs impossible to warm.
typedef struct { char vs[80], fs[80]; uint8_t vs_stage, fs_stage, merged, fz; pso_fz_desc fz_desc; pso_key k; } pso_skey;
// TOTK (2026-10-21): o formato gravado depende destes tamanhos — o hex tem de ter 1104 caracteres
// e o legado 1088 (a linha sem o bid). Se a struct mudar, o build para aqui em vez de o parser
// aceitar silenciosamente linhas de outro formato.
_Static_assert(sizeof(pso_key) == 352, "pso_key: 344 + o bid de 64 bits (a forma da linha gravada depende disto)");
_Static_assert(sizeof(pso_skey) == 552, "pso_skey: 544 + o bid (a forma da linha gravada depende disto)");

static inline void pso_skey_from(const pso_key *k, NvShader *vs, NvShader *fs, int merged, const pso_fz_desc *fz, pso_skey *out) {
    memset(out, 0, sizeof *out);
    memcpy(&out->k, k, sizeof out->k);
    out->k.vs = out->k.fs = out->k.fsfn = NULL;
    if (vs) { memcpy(out->vs, vs->key, sizeof out->vs); out->vs_stage = (uint8_t)vs->stage; }
    if (fs) { memcpy(out->fs, fs->key, sizeof out->fs); out->fs_stage = (uint8_t)fs->stage; }
    out->merged = merged != 0;
    out->fz = k->fsfn != NULL;
    if (fz) memcpy(&out->fz_desc, fz, sizeof out->fz_desc);
}

static inline void pso_skey_hex(const pso_skey *key, char *out) {
    static const char hex[] = "0123456789abcdef";
    const uint8_t *p = (const uint8_t *)key;
    for (size_t i = 0; i < sizeof *key; i++) { out[i * 2] = hex[p[i] >> 4]; out[i * 2 + 1] = hex[p[i] & 15]; }
    out[sizeof *key * 2] = 0;
}

static inline int pso_skey_parse(const char *hex, pso_skey *out) {
    // TOTK (2026-10-21): two lengths are valid — the current one, and the legacy one (before the
    // border identity field). A legacy row is by construction a row WITHOUT a border (the recorder
    // cleared the bit and the fsfn pointer), so decoding it with bid = 0 is factually correct and
    // not a guess. Any other length is a row this build cannot read.
    size_t n = hex ? strlen(hex) : 0;
    size_t full = sizeof *out * 2, legacy = (sizeof *out - sizeof(uint64_t)) * 2;
    if (n != full && n != legacy) return -1;
    memset(out, 0, sizeof *out);   // a linha legado nao escreve o bid: tem de ficar a zero
    uint8_t *p = (uint8_t *)out;
    for (size_t i = 0; i < n; i += 2) {
        int a = hex[i] >= '0' && hex[i] <= '9' ? hex[i] - '0' : hex[i] >= 'a' && hex[i] <= 'f' ? hex[i] - 'a' + 10 : hex[i] >= 'A' && hex[i] <= 'F' ? hex[i] - 'A' + 10 : -1;
        int b = hex[i + 1] >= '0' && hex[i + 1] <= '9' ? hex[i + 1] - '0' : hex[i + 1] >= 'a' && hex[i + 1] <= 'f' ? hex[i + 1] - 'a' + 10 : hex[i + 1] >= 'A' && hex[i + 1] <= 'F' ? hex[i + 1] - 'A' + 10 : -1;
        if (a < 0 || b < 0) return -1;
        p[i / 2] = (uint8_t)(a << 4 | b);
    }
    return 0;
}
