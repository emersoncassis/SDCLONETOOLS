/* vhd.h - imagem .vhd dinâmica (formato do Windows/Virtual PC).
 * Só os blocos gravados ocupam espaço no arquivo: a imagem fica do tamanho dos dados usados do cartão.
 * O Windows abre .vhd com dois cliques (Montar). Independente de Windows. */
#ifndef VHD_H
#define VHD_H

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#define VHD_BLOCK (2u * 1024 * 1024)
#define VHD_BMP 512u /* bitmap de setores de um bloco de 2 MiB (4096 setores) */

typedef int (*VhFileW)(void *ctx, uint64_t off, const void *buf, uint32_t len); /* 1 = ok */
typedef int (*VhFileR)(void *ctx, uint64_t off, void *buf, uint32_t len);       /* 1 = ok */

static void vbe32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static void vbe64(uint8_t *p, uint64_t v) { vbe32(p, (uint32_t)(v >> 32)); vbe32(p + 4, (uint32_t)v); }
static uint32_t vrd32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static uint64_t vrd64(const uint8_t *p) { return ((uint64_t)vrd32(p) << 32) | vrd32(p + 4); }

static uint32_t VhdSum(const uint8_t *p, int n) {
    uint32_t s = 0;
    for (int i = 0; i < n; i++) s += p[i];
    return ~s;
}

static void VhdGeometry(uint64_t size, uint8_t *g) { /* algoritmo da especificação */
    uint64_t ts = size / 512;
    uint32_t spt, hds, cth, cyl;
    if (ts > 65535ull * 16 * 255) ts = 65535ull * 16 * 255;
    if (ts >= 65535ull * 16 * 63) { spt = 255; hds = 16; cth = (uint32_t)(ts / spt); }
    else {
        spt = 17; cth = (uint32_t)(ts / spt);
        hds = (cth + 1023) / 1024;
        if (hds < 4) hds = 4;
        if (cth >= hds * 1024 || hds > 16) { spt = 31; hds = 16; cth = (uint32_t)(ts / spt); }
        if (cth >= hds * 1024) { spt = 63; hds = 16; cth = (uint32_t)(ts / spt); }
    }
    cyl = cth / hds;
    g[0] = (uint8_t)(cyl >> 8); g[1] = (uint8_t)cyl; g[2] = (uint8_t)hds; g[3] = (uint8_t)spt;
}

static void VhdFooter(uint8_t *f, uint64_t size, uint32_t type, uint64_t dataOff, const uint8_t *uid) {
    memset(f, 0, 512);
    memcpy(f, "conectix", 8);
    vbe32(f + 8, 2);
    vbe32(f + 12, 0x00010000);
    vbe64(f + 16, dataOff);
    vbe32(f + 24, (uint32_t)((uint64_t)time(NULL) - 946684800ull));
    memcpy(f + 28, "win ", 4); /* com "win " o tamanho vale pelo campo Current Size (Windows, qemu e outros) */
    vbe32(f + 32, 0x00060001);
    memcpy(f + 36, "Wi2k", 4);
    vbe64(f + 40, size);
    vbe64(f + 48, size);
    VhdGeometry(size, f + 56);
    vbe32(f + 60, type);
    memcpy(f + 68, uid, 16);
    vbe32(f + 64, VhdSum(f, 512));
}

/* ------------------------------------------------------------ gravação */
typedef struct {
    VhFileW fw; VhFileR fr; void *ctx;
    uint64_t size, fileEnd;
    uint32_t nblocks, batBytes;
    uint32_t *bat;
    int64_t cur;
    int dirty;
    uint8_t *blk, uid[16];
} VhdW;

static int VhdWOpen(VhdW *w, uint64_t size, VhFileW fw, VhFileR fr, void *ctx, uint32_t seed) {
    memset(w, 0, sizeof *w);
    w->fw = fw; w->fr = fr; w->ctx = ctx;
    w->size = (size + 511) / 512 * 512;
    w->nblocks = (uint32_t)((w->size + VHD_BLOCK - 1) / VHD_BLOCK);
    w->batBytes = (w->nblocks * 4 + 511) / 512 * 512;
    w->bat = (uint32_t *)malloc(w->batBytes);
    w->blk = (uint8_t *)malloc(VHD_BLOCK);
    if (!w->bat || !w->blk) return 0;
    memset(w->bat, 0xFF, w->batBytes);
    w->fileEnd = 512 + 1024 + w->batBytes;
    w->cur = -1;
    uint32_t x = seed ^ (uint32_t)time(NULL) ^ 0x9E3779B9u;
    for (int i = 0; i < 16; i++) { x = x * 1664525u + 1013904223u; w->uid[i] = (uint8_t)(x >> 24); }
    return 1;
}

static int VhdWFlush(VhdW *w) {
    if (w->cur < 0 || !w->dirty) return 1;
    uint32_t b = (uint32_t)w->cur;
    if (w->bat[b] == 0xFFFFFFFFu) {
        uint8_t bm[VHD_BMP];
        memset(bm, 0xFF, sizeof bm);
        w->bat[b] = (uint32_t)(w->fileEnd / 512);
        if (!w->fw(w->ctx, w->fileEnd, bm, VHD_BMP)) return 0;
        if (!w->fw(w->ctx, w->fileEnd + VHD_BMP, w->blk, VHD_BLOCK)) return 0;
        w->fileEnd += VHD_BMP + VHD_BLOCK;
    } else if (!w->fw(w->ctx, (uint64_t)w->bat[b] * 512 + VHD_BMP, w->blk, VHD_BLOCK)) return 0;
    w->dirty = 0;
    return 1;
}

static int VhdWWrite(VhdW *w, uint64_t off, const void *buf, uint32_t len) {
    const uint8_t *s = (const uint8_t *)buf;
    while (len) {
        uint32_t b = (uint32_t)(off / VHD_BLOCK), in = (uint32_t)(off % VHD_BLOCK);
        uint32_t n = VHD_BLOCK - in < len ? VHD_BLOCK - in : len;
        if (off + n > w->size || b >= w->nblocks) return 0;
        if ((int64_t)b != w->cur) {
            if (!VhdWFlush(w)) return 0;
            if (w->bat[b] != 0xFFFFFFFFu) { if (!w->fr(w->ctx, (uint64_t)w->bat[b] * 512 + VHD_BMP, w->blk, VHD_BLOCK)) return 0; }
            else memset(w->blk, 0, VHD_BLOCK);
            w->cur = b;
        }
        memcpy(w->blk + in, s, n);
        w->dirty = 1;
        off += n; s += n; len -= n;
    }
    return 1;
}

static int VhdWClose(VhdW *w) {
    int ok = VhdWFlush(w);
    uint8_t f[512], h[1024];
    VhdFooter(f, w->size, 3, 512, w->uid);
    memset(h, 0, sizeof h);
    memcpy(h, "cxsparse", 8);
    vbe64(h + 8, 0xFFFFFFFFFFFFFFFFull);
    vbe64(h + 16, 1536);
    vbe32(h + 24, 0x00010000);
    vbe32(h + 28, w->nblocks);
    vbe32(h + 32, VHD_BLOCK);
    vbe32(h + 36, VhdSum(h, 1024));
    uint8_t *bat = (uint8_t *)malloc(w->batBytes);
    if (!bat) ok = 0;
    else {
        memset(bat, 0xFF, w->batBytes);
        for (uint32_t i = 0; i < w->nblocks; i++) vbe32(bat + i * 4, w->bat[i]);
        ok = ok && w->fw(w->ctx, 0, f, 512) && w->fw(w->ctx, 512, h, 1024) && w->fw(w->ctx, 1536, bat, w->batBytes) &&
             w->fw(w->ctx, w->fileEnd, f, 512);
        free(bat);
    }
    free(w->bat); free(w->blk);
    w->bat = NULL; w->blk = NULL;
    return ok;
}
static uint64_t VhdWFileSize(const VhdW *w) { return w->fileEnd + 512; }

/* ------------------------------------------------------------ leitura (para gravar a imagem no cartão) */
typedef struct {
    VhFileR fr; void *ctx;
    uint64_t size;
    int fixed;
    uint32_t nblocks, blockSize, bmpBytes;
    uint32_t *bat;
} VhdR;

/* fileSize: tamanho do arquivo. 1 = ok */
static int VhdROpen(VhdR *r, VhFileR fr, void *ctx, uint64_t fileSize) {
    uint8_t f[512], h[1024];
    memset(r, 0, sizeof *r);
    r->fr = fr; r->ctx = ctx;
    if (fileSize < 512 || !fr(ctx, fileSize - 512, f, 512) || memcmp(f, "conectix", 8) != 0) {
        if (!fr(ctx, 0, f, 512) || memcmp(f, "conectix", 8) != 0) return 0;
    }
    uint32_t type = vrd32(f + 60);
    r->size = vrd64(f + 48);
    if (type == 2) { r->fixed = 1; return r->size <= fileSize; }
    if (type != 3 && type != 4) return 0;
    if (type == 4) return 0; /* diferencial: não suportado */
    uint64_t dOff = vrd64(f + 16);
    if (!fr(ctx, dOff, h, 1024) || memcmp(h, "cxsparse", 8) != 0) return 0;
    uint64_t tOff = vrd64(h + 16);
    r->nblocks = vrd32(h + 28);
    r->blockSize = vrd32(h + 32);
    if (r->blockSize < 512 || (r->blockSize & 511) || r->nblocks == 0 || (uint64_t)r->nblocks * r->blockSize < r->size) return 0;
    r->bmpBytes = ((r->blockSize / 512 + 7) / 8 + 511) / 512 * 512;
    uint32_t bytes = (r->nblocks * 4 + 511) / 512 * 512;
    uint8_t *raw = (uint8_t *)malloc(bytes);
    r->bat = (uint32_t *)malloc((size_t)r->nblocks * 4);
    if (!raw || !r->bat || !fr(ctx, tOff, raw, bytes)) { free(raw); return 0; }
    for (uint32_t i = 0; i < r->nblocks; i++) r->bat[i] = vrd32(raw + i * 4);
    free(raw);
    return 1;
}

static int VhdRRead(VhdR *r, uint64_t off, void *buf, uint32_t len) {
    uint8_t *d = (uint8_t *)buf;
    if (off + len > r->size) return 0;
    if (r->fixed) return r->fr(r->ctx, off, buf, len);
    while (len) {
        uint32_t b = (uint32_t)(off / r->blockSize), in = (uint32_t)(off % r->blockSize);
        uint32_t n = r->blockSize - in < len ? r->blockSize - in : len;
        if (r->bat[b] == 0xFFFFFFFFu) memset(d, 0, n);
        else if (!r->fr(r->ctx, (uint64_t)r->bat[b] * 512 + r->bmpBytes + in, d, n)) return 0;
        off += n; d += n; len -= n;
    }
    return 1;
}
static void VhdRClose(VhdR *r) { free(r->bat); r->bat = NULL; }

#endif
