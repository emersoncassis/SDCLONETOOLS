/* core.h - lógica pura do SD Clone (sem dependência de Windows além da alocação alinhada).
 * Usado pelo app (sdclone.c) e pelo teste em Linux (test_core.c). */
#ifndef CORE_H
#define CORE_H

#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#ifdef _WIN32
#define ALLOC_ALIGNED(n) VirtualAlloc(NULL, (n), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)
#define FREE_ALIGNED(p) VirtualFree((p), 0, MEM_RELEASE)
#else
#define ALLOC_ALIGNED(n) aligned_alloc(4096, (((n) + 4095) / 4096) * 4096)
#define FREE_ALIGNED(p) free(p)
#endif

/* ---------- padrão pseudo-aleatório para o teste de capacidade ---------- */
static uint64_t mix64(uint64_t x) {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}
/* off e len devem ser múltiplos de 8; o conteúdo depende só do deslocamento absoluto */
static void FillPattern(uint8_t *buf, uint64_t off, uint32_t len, uint64_t seed) {
    uint64_t *p = (uint64_t *)buf;
    uint64_t w = off >> 3;
    for (uint32_t i = 0; i < len / 8; i++) p[i] = mix64(seed + w + i);
}

/* ---------- fabricantes (campo MID do registro CID do cartão SD) ---------- */
typedef struct { int mid; const wchar_t *name; } MidEntry;
static const MidEntry MID_TABLE[] = {
    {0x01, L"Panasonic"}, {0x02, L"Toshiba / Kioxia"}, {0x03, L"SanDisk"},
    {0x06, L"Ritek"}, {0x09, L"ATP"}, {0x13, L"Kingmax"}, {0x1B, L"Samsung"},
    {0x1D, L"ADATA"}, {0x27, L"Phison (controlador; marca final varia)"},
    {0x28, L"Lexar / Longsys"}, {0x31, L"Silicon Power"}, {0x41, L"Kingston"},
    {0x45, L"SanDisk (Western Digital)"}, {0x74, L"Transcend"}, {0x76, L"Patriot"},
    {0x82, L"Sony"}, {-1, NULL}};
static const wchar_t *MidName(int mid) {
    for (int i = 0; MID_TABLE[i].name; i++)
        if (MID_TABLE[i].mid == mid) return MID_TABLE[i].name;
    return NULL;
}

/* ---------- motor de recuperação de arquivos (file carving) ---------- */
enum { FT_JPG = 0, FT_PNG, FT_PDF, FT_MP4, FT_MOV };

typedef int (*CarveReadFn)(void *ctx, uint64_t off, void *buf, uint32_t len); /* 1 = ok */
typedef void (*CarveEmitFn)(void *ctx, int type, uint64_t start, uint64_t len);
typedef int (*CarveProgFn)(void *ctx, uint64_t pos); /* retorna != 0 para cancelar */

#define C_WIN (4u * 1024 * 1024)
#define C_SCH (1u * 1024 * 1024)

typedef struct {
    CarveReadFn rd;
    void *rctx;
    uint64_t size;
    uint32_t sec;
    uint8_t *win, *sch, *small;
} CEng;

static int ceRead(CEng *e, uint64_t off, void *buf, uint32_t len) { return e->rd(e->rctx, off, buf, len); }

static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static uint64_t be64(const uint8_t *p) { return ((uint64_t)be32(p) << 32) | be32(p + 4); }

/* lê um trecho pequeno (<= 64 bytes) em qualquer posição, respeitando o alinhamento de setor */
static int ceReadSmall(CEng *e, uint64_t off, uint32_t len, uint8_t *out) {
    if (off + len > e->size) return 0;
    uint64_t a = off - (off % e->sec);
    uint64_t b = ((off + len + e->sec - 1) / e->sec) * e->sec;
    if (b > e->size) b = e->size;
    uint32_t n = (uint32_t)(b - a);
    if (n > 16384) return 0;
    if (!ceRead(e, a, e->small, n)) return 0;
    memcpy(out, e->small + (off - a), len);
    return 1;
}

static int findJpeg(CEng *e, uint64_t start, uint64_t *len) {
    const uint64_t MAXL = 64ull << 20;
    int depth = 0, prev = -1;
    uint64_t off = start;
    while (off < e->size && off - start < MAXL) {
        uint32_t n = (uint32_t)((e->size - off) < C_SCH ? (e->size - off) : C_SCH);
        if (!ceRead(e, off, e->sch, n)) return 0;
        for (uint32_t i = 0; i < n; i++) {
            int b = e->sch[i];
            if (prev == 0xFF) {
                if (b == 0xD8) depth++;
                else if (b == 0xD9) {
                    if (--depth <= 0) {
                        *len = off + i + 1 - start;
                        return *len >= 1024;
                    }
                }
            }
            prev = b;
        }
        off += n;
    }
    return 0;
}

static int findPng(CEng *e, uint64_t start, uint64_t *len) {
    uint64_t pos = start + 8;
    uint8_t h[8];
    for (int guard = 0; guard < 2000000; guard++) {
        if (!ceReadSmall(e, pos, 8, h)) return 0;
        uint32_t cl = be32(h);
        for (int k = 4; k < 8; k++) {
            uint8_t c = h[k];
            if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))) return 0;
        }
        if (guard == 0 && (memcmp(h + 4, "IHDR", 4) != 0 || cl != 13)) return 0;
        uint64_t next = pos + 12 + (uint64_t)cl;
        if (next > e->size || next - start > (256ull << 20)) return 0;
        if (memcmp(h + 4, "IEND", 4) == 0) { *len = next - start; return 1; }
        pos = next;
    }
    return 0;
}

static int findPdf(CEng *e, uint64_t start, uint64_t *len) {
    static const uint8_t pat[5] = {'%', '%', 'E', 'O', 'F'};
    static const int pi[5] = {0, 1, 0, 0, 0};
    const uint64_t MAXL = 128ull << 20;
    int k = 0;
    uint64_t off = start;
    while (off < e->size && off - start < MAXL) {
        uint32_t n = (uint32_t)((e->size - off) < C_SCH ? (e->size - off) : C_SCH);
        if (!ceRead(e, off, e->sch, n)) return 0;
        for (uint32_t i = 0; i < n; i++) {
            uint8_t b = e->sch[i];
            while (k > 0 && b != pat[k]) k = pi[k - 1];
            if (b == pat[k]) k++;
            if (k == 5) {
                uint64_t end = off + i + 1;
                uint8_t t[2];
                if (ceReadSmall(e, end, 2, t)) {
                    if (t[0] == '\r' && t[1] == '\n') end += 2;
                    else if (t[0] == '\n' || t[0] == '\r') end += 1;
                }
                *len = end - start;
                return *len >= 256;
            }
        }
        off += n;
    }
    return 0;
}

static int isBoxType(const uint8_t *t) {
    static const char *k[] = {"ftyp", "moov", "mdat", "free", "skip", "wide", "uuid", "pnot", "junk",
                              "moof", "mfra", "styp", "sidx", "meta", "pdin", "prft", "emsg", NULL};
    for (int i = 0; k[i]; i++)
        if (memcmp(t, k[i], 4) == 0) return 1;
    return 0;
}

static int findMp4(CEng *e, uint64_t start, uint64_t *len) {
    uint64_t pos = start;
    int haveMoov = 0, haveMdat = 0, boxes = 0;
    uint8_t h[16];
    while (boxes < 200000) {
        if (!ceReadSmall(e, pos, 16, h)) break;
        uint64_t sz = be32(h);
        uint32_t hdr = 8;
        if (sz == 1) { sz = be64(h + 8); hdr = 16; }
        else if (sz == 0) break;
        if (sz < hdr) break;
        if (boxes == 0 && memcmp(h + 4, "ftyp", 4) != 0) return 0;
        if (boxes > 0 && memcmp(h + 4, "ftyp", 4) == 0) break; /* próximo arquivo */
        if (!isBoxType(h + 4)) break;
        if (pos + sz > e->size || sz > (1ull << 40)) break;
        if (memcmp(h + 4, "moov", 4) == 0) haveMoov = 1;
        if (memcmp(h + 4, "mdat", 4) == 0) haveMdat = 1;
        pos += sz;
        boxes++;
    }
    *len = pos - start;
    return haveMoov && haveMdat && *len >= 65536;
}

/* retorna o tipo (FT_*) se o setor começa com uma assinatura conhecida, senão -1 */
static int detectHeader(const uint8_t *p) {
    if (p[0] == 0xFF && p[1] == 0xD8 && p[2] == 0xFF && ((p[3] >= 0xE0 && p[3] <= 0xEF) || p[3] == 0xDB))
        return FT_JPG;
    if (p[0] == 0x89 && memcmp(p + 1, "PNG\r\n\x1a\n", 7) == 0) return FT_PNG;
    if (memcmp(p, "%PDF-", 5) == 0 && p[5] >= '1' && p[5] <= '2') return FT_PDF;
    if (memcmp(p + 4, "ftyp", 4) == 0) {
        uint32_t s = be32(p);
        if (s >= 12 && s <= 512) return memcmp(p + 8, "qt  ", 4) == 0 ? FT_MOV : FT_MP4;
    }
    return -1;
}

/* Varre o dispositivo e chama emit() para cada arquivo encontrado. Retorna nº de arquivos, ou -1 se cancelado. */
static int Carve(CarveReadFn rd, void *rctx, uint64_t size, uint32_t sec, CarveEmitFn emit, void *ectx,
                 CarveProgFn prog, void *pctx) {
    CEng e;
    e.rd = rd; e.rctx = rctx; e.size = size; e.sec = sec;
    e.win = (uint8_t *)ALLOC_ALIGNED(C_WIN);
    e.sch = (uint8_t *)ALLOC_ALIGNED(C_SCH);
    e.small = (uint8_t *)ALLOC_ALIGNED(16384);
    if (!e.win || !e.sch || !e.small) return -2;
    uint64_t pos = 0, winOff = 0, winLen = 0;
    int found = 0, cancelled = 0;
    while (pos + sec <= size) {
        if (pos < winOff || pos + sec > winOff + winLen) {
            if (prog && prog(pctx, pos)) { cancelled = 1; break; }
            winOff = pos;
            uint64_t rem = size - pos;
            winLen = rem < C_WIN ? rem : C_WIN;
            if (!ceRead(&e, winOff, e.win, (uint32_t)winLen)) memset(e.win, 0, (size_t)winLen);
        }
        const uint8_t *p = e.win + (pos - winOff);
        int t = detectHeader(p);
        if (t >= 0) {
            uint64_t L = 0;
            int ok = 0;
            switch (t) {
            case FT_JPG: ok = findJpeg(&e, pos, &L); break;
            case FT_PNG: ok = findPng(&e, pos, &L); break;
            case FT_PDF: ok = findPdf(&e, pos, &L); break;
            default: ok = findMp4(&e, pos, &L); break;
            }
            if (ok) {
                emit(ectx, t, pos, L);
                found++;
                pos += ((L + sec - 1) / sec) * sec;
                if (prog && prog(pctx, pos)) { cancelled = 1; break; }
                continue;
            }
        }
        pos += sec;
    }
    FREE_ALIGNED(e.win); FREE_ALIGNED(e.sch); FREE_ALIGNED(e.small);
    return cancelled ? -1 : found;
}

#endif
