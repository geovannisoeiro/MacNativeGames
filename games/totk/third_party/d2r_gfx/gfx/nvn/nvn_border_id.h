#pragma once
// TOTK (2026-10-21): the identity of a border variant, in C puro — para o port E para o probe
// usarem o mesmo codigo (o probe nao copia a implementacao: se ela mudar, o teste continua a valer).
//
// A identidade e a border_key de um estagio: "aid:r,g,b,a;" por sampler (quais argument ids, que
// cor). O hash e o que a chave de pipeline carrega; a string e o que a linha gravada carrega, e os
// dois sao conferidos um contra o outro antes de uma linha ser acreditada.
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

static inline uint64_t border_id_hash(const char *s) {   // FNV-1a 64
    uint64_t h = 1469598103934665603ull;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) { h ^= *p; h *= 1099511628211ull; }
    return h ? h : 1;   // 0 means "no border"
}
// "12:0x1p-1,0x1p-1,0x1p-1,0x1p-1;" -> the argument id and the colour of one sampler. The colour is
// written by border_key with "%a" (a hex float), which strtof reads back exactly. 0 on success.
static inline int border_parse(const char *key, float colours[][4], int aids[], int *n) {
    *n = 0;
    if (!key) return -1;
    const char *p = key;
    while (*p && *n < 8) {
        char *end = NULL;
        long aid = strtol(p, &end, 10);
        if (end == p || *end != ':') return -1;
        p = end + 1;
        for (int c = 0; c < 4; c++) {
            char *e2 = NULL;
            float v = strtof(p, &e2);
            if (e2 == p) return -1;
            colours[*n][c] = v;
            p = e2;
            if (c < 3) { if (*p != ',') return -1; p++; }
        }
        aids[*n] = (int)aid;
        ++*n;
        if (*p != ';') return -1;
        p++;
    }
    return *n > 0 ? 0 : -1;
}
// The recorded identity string -> the two stage keys. The border_key only contains ';', ',', ':',
// digits and hex floats — never '|' — so the split is unambiguous. 0 on success.
static inline int border_id_split_c(const char *id, char *vk, size_t vkcap, char *fk, size_t fkcap) {
    if (!id || id[0] != 'V') return -1;
    const char *bar = strchr(id, '|');
    if (!bar || bar[1] != 'F') return -1;   // o prefixo 'F' tambem se tira
    size_t vl = (size_t)(bar - id - 1), fl = strlen(bar + 2);
    if (vl + 1 > vkcap || fl + 1 > fkcap) return -1;
    memcpy(vk, id + 1, vl); vk[vl] = 0;
    memcpy(fk, bar + 2, fl); fk[fl] = 0;
    return 0;
}
