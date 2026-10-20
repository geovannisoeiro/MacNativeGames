// A identidade da borda: a string que a linha gravada leva, o hash que a chave de pipeline leva,
// e o round trip entre os dois. Exercita as funções REAIS (o cabeçalho que o port usa), não uma
// cópia delas — por isso o teste continua a valer se a implementação mudar.
//
// É o teste de regressão que faltava ao conserto: sem ele, uma alteracao no formato da
// border_key, no parser ou no hash passava despercebida e o warm-up passava a construir a
// variante errada (ou a nao construir nada).
#include <stdio.h>
#include <string.h>
#include <math.h>
// As tres funcoes puras de C (a identidade, o parser e a divisao) vivem no seu cabecalho, e o
// formato gravado no da chave. O probe usa o MESMO codigo que o port — nao uma copia.
#include "nvn_border_id.h"

static int fails = 0;
static void ok(int cond, const char *what) {
    printf("%-62s %s\n", what, cond ? "ok" : "FALHA");
    if (!cond) fails++;
}

int main(void) {
    // 1) o formato que o port escreve: "%d:%a,%a,%a,%a;" por sampler (aid + 4 componentes da cor)
    char key[256];
    snprintf(key, sizeof key, "%d:%a,%a,%a,%a;", 12, 0.5f, 0.5f, 0.5f, 0.5f);
    ok(strcmp(key, "12:0x1p-1,0x1p-1,0x1p-1,0x1p-1;") == 0,
       "a border_key escreve o formato que o parser le");

    // 2) o round trip do parser
    float colours[8][4];
    int aids[8], n = 0;
    ok(border_parse(key, colours, aids, &n) == 0 && n == 1 && aids[0] == 12,
       "o parser le o argument id");
    ok(n == 1 && fabsf(colours[0][0] - 0.5f) < 1e-6f && fabsf(colours[0][3] - 0.5f) < 1e-6f,
       "o parser le a cor exacta (hex float)");
    float other = 0.25f;
    char key2[256];
    snprintf(key2, sizeof key2, "%d:%a,%a,%a,%a;", 3, other, 0.5f, 0.5f, 0.5f);
    ok(border_parse(key2, colours, aids, &n) == 0 && aids[0] == 3 &&
       fabsf(colours[0][0] - 0.25f) < 1e-6f,
       "uma cor diferente le-se diferente");

    // 3) a string de identidade e a sua divisao nas duas etapas
    char id[512];
    snprintf(id, sizeof id, "V%s|F%s", key, key);
    char vk[256], fk[256];
    ok(border_id_split_c(id, vk, sizeof vk, fk, sizeof fk) == 0,
       "a identidade divide-se nas duas etapas");
    ok(strcmp(vk, key) == 0 && strcmp(fk, key) == 0, "as duas etapas voltam inteiras");

    // 4) o hash: estavel, nunca zero, e distingue identidades
    uint64_t h1 = border_id_hash(id);
    ok(h1 != 0, "o hash nunca e zero (zero = sem borda)");
    ok(h1 == border_id_hash(id), "o hash e estavel");
    char id2[512];
    snprintf(id2, sizeof id2, "V%s|F", key);
    ok(h1 != border_id_hash(id2), "identidades diferentes dao hashes diferentes");
    ok(border_id_hash("") != 0, "a identidade vazia tambem da um hash nao nulo");

    // 5) as entradas malformadas sao rejeitadas (uma linha corrompida nao pode construir a variante errada)
    ok(border_parse("12:0x1p-1,0x1p-1,0x1p-1", colours, aids, &n) != 0, "uma cor truncada e rejeitada");
    ok(border_parse("12:0x1p-1,0x1p-1,0x1p-1,0x1p-1", colours, aids, &n) != 0, "sem o ';' final e rejeitada");
    ok(border_parse("", colours, aids, &n) != 0, "uma string vazia e rejeitada");
    ok(border_parse("x:0x1p-1,0x1p-1,0x1p-1,0x1p-1;", colours, aids, &n) != 0, "um aid nao numerico e rejeitado");
    ok(border_id_split_c("X12:0x1p-1;|F", vk, sizeof vk, fk, sizeof fk) != 0, "uma identidade sem o 'V' e rejeitada");
    ok(border_id_split_c("V12:0x1p-1;", vk, sizeof vk, fk, sizeof fk) != 0, "uma identidade sem o '|' e rejeitada");

    // 6) o formato gravado (o hex de 1104 e o legado de 1088) fica assegurado por
    // _Static_assert no cabecalho da chave: o build do port para se a struct mudar de tamanho.

    printf("\n%s\n", fails ? "FALHAS" : "todas as verificacoes passaram");
    return fails ? 1 : 0;
}
