#define _GNU_SOURCE
#include <stdio.h>
#include <wchar.h>
#include <unistd.h>
#include <fcntl.h>
#include "core.h"

#define DISK (48u * 1024 * 1024)
#define SEC 512

static uint8_t *disk;
static uint64_t rng = 88172645463325252ULL;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t)rng; }

static void put32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

typedef struct { int type; uint64_t start, len; } Exp;
static Exp exp_[32]; static int nexp;
static Exp got[64]; static int ngot;

static int rd(void *c, uint64_t off, void *buf, uint32_t len) {
    if (off + len > DISK) return 0;
    if (off % SEC || len % SEC) { printf("ERRO: leitura desalinhada off=%llu len=%u\n", (unsigned long long)off, len); exit(2); }
    memcpy(buf, disk + off, len); return 1;
}
static void emit(void *c, int t, uint64_t s, uint64_t l) { got[ngot++] = (Exp){t, s, l}; }
static int prog(void *c, uint64_t p) { return 0; }

static void fillSafe(uint8_t *p, size_t n) { for (size_t i = 0; i < n; i++) p[i] = rnd() & 0x7F; }

static uint64_t addJpeg(uint64_t at, size_t body, int nested) {
    uint8_t *p = disk + at; size_t o = 0;
    p[o++] = 0xFF; p[o++] = 0xD8; p[o++] = 0xFF; p[o++] = 0xE1;
    size_t app = 2000; p[o++] = app >> 8; p[o++] = app & 0xFF;
    fillSafe(p + o, app - 2);
    if (nested) { p[o + 10] = 0xFF; p[o + 11] = 0xD8; p[o + 400] = 0xFF; p[o + 401] = 0xD9; }
    o += app - 2;
    for (size_t i = 0; i < body; i++) { uint8_t b = rnd() & 0xFF; if (b == 0xFF) { p[o++] = 0xFF; p[o++] = 0x00; } else p[o++] = b; }
    p[o++] = 0xFF; p[o++] = 0xD9;
    return o;
}
static uint64_t addPng(uint64_t at) {
    uint8_t *p = disk + at; size_t o = 0;
    memcpy(p, "\x89PNG\r\n\x1a\n", 8); o = 8;
    put32(p + o, 13); memcpy(p + o + 4, "IHDR", 4); fillSafe(p + o + 8, 13); o += 12 + 13;
    put32(p + o, 20000); memcpy(p + o + 4, "IDAT", 4); for (int i = 0; i < 20000; i++) p[o + 8 + i] = rnd(); o += 12 + 20000;
    put32(p + o, 0); memcpy(p + o + 4, "IEND", 4); p[o + 8] = 0xAE; p[o + 9] = 0x42; p[o + 10] = 0x60; p[o + 11] = 0x82; o += 12;
    return o;
}
static uint64_t addPdf(uint64_t at) {
    uint8_t *p = disk + at; size_t o = 0;
    memcpy(p, "%PDF-1.4\n", 9); o = 9;
    for (int i = 0; i < 5000; i++) p[o++] = 'a' + (rnd() % 20);
    memcpy(p + o, "%%EOF\n", 6); o += 6;
    return o;
}
static uint64_t addMp4(uint64_t at, const char *brand) {
    uint8_t *p = disk + at; size_t o = 0;
    put32(p, 24); memcpy(p + 4, "ftyp", 4); memcpy(p + 8, brand, 4); fillSafe(p + 12, 12); o = 24;
    put32(p + o, 108); memcpy(p + o + 4, "free", 4); o += 108;
    put32(p + o, 8 + 200000); memcpy(p + o + 4, "mdat", 4); fillSafe(p + o + 8, 200000); o += 8 + 200000;
    put32(p + o, 8 + 3000); memcpy(p + o + 4, "moov", 4); fillSafe(p + o + 8, 3000); o += 8 + 3000;
    return o;
}
static void expect(int t, uint64_t s, uint64_t l) { exp_[nexp++] = (Exp){t, s, l}; }

int main(void) {
    disk = calloc(DISK, 1);
    /* ruído sem 0xFF (cai em falsos positivos só se houver cabeçalho) */
    fillSafe(disk + 40u * 1024 * 1024, 2u * 1024 * 1024);
    uint64_t at, L;
    at = 1 * 1024 * 1024; L = addJpeg(at, 50000, 1); expect(FT_JPG, at, L);
    at = 2 * 1024 * 1024; L = addPng(at); expect(FT_PNG, at, L);
    at = 3 * 1024 * 1024; L = addPdf(at); expect(FT_PDF, at, L);
    at = 4 * 1024 * 1024; L = addMp4(at, "isom"); expect(FT_MP4, at, L);
    at = 6 * 1024 * 1024; L = addMp4(at, "qt  "); expect(FT_MOV, at, L);
    /* dois MP4 colados: o segundo começa no próximo setor depois do primeiro */
    at = 8 * 1024 * 1024; L = addMp4(at, "isom"); expect(FT_MP4, at, L);
    uint64_t at2 = at + ((L + SEC - 1) / SEC) * SEC; uint64_t L2 = addMp4(at2, "mp42"); expect(FT_MP4, at2, L2);
    /* dois JPEG colados */
    at = 10 * 1024 * 1024; L = addJpeg(at, 30000, 0); expect(FT_JPG, at, L);
    at2 = at + ((L + SEC - 1) / SEC) * SEC; L2 = addJpeg(at2, 9000, 0); expect(FT_JPG, at2, L2);
    /* cabeçalho JPEG falso sem fim: não deve emitir */
    at = 12 * 1024 * 1024; memcpy(disk + at, "\xFF\xD8\xFF\xE0", 4); fillSafe(disk + at + 4, 100000);
    /* PNG truncado (sem IEND): não deve emitir */
    at = 14 * 1024 * 1024; addPng(at); memset(disk + at + 20040, 0, 100);
    /* MP4 sem moov: não deve emitir */
    at = 16 * 1024 * 1024; { uint8_t *p = disk + at; put32(p, 24); memcpy(p + 4, "ftyp", 4); memcpy(p + 8, "isom", 4); put32(p + 24, 8 + 200000); memcpy(p + 28, "mdat", 4); }

    int n = Carve(rd, NULL, DISK, SEC, emit, NULL, prog, NULL);
    int fail = 0;
    printf("encontrados=%d esperados=%d\n", n, nexp);
    if (n != nexp) fail = 1;
    for (int i = 0; i < nexp && i < ngot; i++) {
        int okk = exp_[i].type == got[i].type && exp_[i].start == got[i].start && exp_[i].len == got[i].len;
        printf("  [%d] tipo %d/%d inicio %llu/%llu tam %llu/%llu %s\n", i, exp_[i].type, got[i].type,
               (unsigned long long)exp_[i].start, (unsigned long long)got[i].start,
               (unsigned long long)exp_[i].len, (unsigned long long)got[i].len, okk ? "OK" : "FALHOU");
        if (!okk) fail = 1;
    }
    /* padrão do teste de capacidade: determinístico e independente do pedaço */
    uint8_t *a = aligned_alloc(4096, 8192), *b = aligned_alloc(4096, 8192);
    FillPattern(a, 0, 8192, 7); FillPattern(b, 4096, 4096, 7);
    if (memcmp(a + 4096, b, 4096) != 0) { printf("padrão inconsistente\n"); fail = 1; } else printf("padrão OK\n");
    if (!MidName(0x03) || MidName(0x99)) { printf("tabela MID falhou\n"); fail = 1; }
    printf(fail ? "RESULTADO: FALHOU\n" : "RESULTADO: TUDO OK\n");
    return fail;
}
