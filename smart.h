/* smart.h - cópia inteligente: só os blocos usados, partição por partição (como o Acronis faz).
 *
 * Monta um "plano" a partir do cartão (ou de uma imagem) de origem:
 *   - a área antes da primeira partição (tabela de partições e carregador de boot dos consoles);
 *   - pequenas áreas entre partições (EBRs de partições lógicas);
 *   - em cada partição FAT12/16/32, exFAT ou ext2/3/4, só os blocos que o sistema de arquivos usa;
 *     partições de outro tipo (NTFS, desconhecidas) vão inteiras;
 *   - a cópia de segurança da GPT no fim do disco.
 * Se o destino for menor, a última partição é reduzida (FAT16/32 e exFAT, sem mover dados), desde que
 * todos os dados usados caibam. A tabela (MBR ou GPT) é ajustada para o novo tamanho.
 *
 * Só usa a origem para LER. Independente de Windows: o app e os testes em Linux usam o mesmo código. */
#ifndef SMART_H
#define SMART_H

#include <stdio.h>
#include <stdarg.h>
#include "diag.h"

/* ------------------------------------------------------------ lista de trechos */
typedef struct { uint64_t off, len; } SmExt;
typedef struct { SmExt *v; int n, cap; } SmList;

static int SmAdd(SmList *l, uint64_t off, uint64_t len) {
    if (len == 0) return 1;
    if (l->n > 0) { /* junta com o anterior quando encosta (caso comum: blocos em sequência) */
        SmExt *p = &l->v[l->n - 1];
        if (off >= p->off && off <= p->off + p->len) {
            if (off + len > p->off + p->len) p->len = off + len - p->off;
            return 1;
        }
    }
    if (l->n == l->cap) {
        int nc = l->cap ? l->cap * 2 : 1024;
        SmExt *nv = (SmExt *)realloc(l->v, (size_t)nc * sizeof(SmExt));
        if (!nv) return 0;
        l->v = nv; l->cap = nc;
    }
    l->v[l->n].off = off; l->v[l->n].len = len; l->n++;
    return 1;
}
static void SmFree(SmList *l) { free(l->v); l->v = NULL; l->n = l->cap = 0; }
static int SmCmp(const void *a, const void *b) {
    const SmExt *x = (const SmExt *)a, *y = (const SmExt *)b;
    return x->off < y->off ? -1 : (x->off > y->off);
}
/* arredonda para setores, ordena, junta e corta em [0, limit) */
static void SmNormalize(SmList *l, uint32_t sec, uint64_t limit) {
    for (int i = 0; i < l->n; i++) {
        uint64_t a = l->v[i].off - l->v[i].off % sec, b = l->v[i].off + l->v[i].len;
        b = ((b + sec - 1) / sec) * sec;
        if (b > limit) b = limit;
        l->v[i].off = a; l->v[i].len = b > a ? b - a : 0;
    }
    qsort(l->v, (size_t)l->n, sizeof(SmExt), SmCmp);
    int k = 0;
    for (int i = 0; i < l->n; i++) {
        if (l->v[i].len == 0) continue;
        if (k > 0 && l->v[i].off <= l->v[k - 1].off + l->v[k - 1].len) {
            uint64_t e = l->v[i].off + l->v[i].len;
            if (e > l->v[k - 1].off + l->v[k - 1].len) l->v[k - 1].len = e - l->v[k - 1].off;
        } else l->v[k++] = l->v[i];
    }
    l->n = k;
}
static uint64_t SmTotal(const SmList *l) {
    uint64_t t = 0;
    for (int i = 0; i < l->n; i++) t += l->v[i].len;
    return t;
}

/* ------------------------------------------------------------ blocos usados por sistema de arquivos */
/* usados: trechos absolutos no disco. *usedEnd = fim do último byte usado, relativo ao início da partição */

static int SmUsedFat(Fs *fs, SmList *out, uint64_t *usedEnd) {
    uint64_t base = fs->base;
    SmAdd(out, base, fs->dataOff); /* setores reservados, FATs e raiz (FAT12/16) */
    *usedEnd = fs->dataOff;
    uint32_t n = fs->clusters, cb = fs->clusterBytes;
    uint64_t fatBytes = fs->type == FST_FAT12 ? ((uint64_t)(n + 2) * 3 + 1) / 2 : (uint64_t)(n + 2) * (fs->type == FST_FAT16 ? 2 : 4);
    uint8_t *fat = (uint8_t *)malloc((size_t)fatBytes + 4);
    if (!fat) return 0;
    memset(fat, 0, (size_t)fatBytes + 4);
    if (!Dev2Read(fs->d, base + fs->fatOff, fat, fatBytes)) { free(fat); return 0; }
    uint32_t runStart = 0, runLen = 0;
    for (uint32_t c = 2; c < n + 2; c++) {
        uint32_t v;
        if (fs->type == FST_FAT12) { v = rd16(fat + c + c / 2); v = (c & 1) ? (v >> 4) : (v & 0xFFF); }
        else if (fs->type == FST_FAT16) v = rd16(fat + (uint64_t)c * 2);
        else v = rd32(fat + (uint64_t)c * 4) & 0x0FFFFFFFu;
        if (v != 0) {
            if (runLen && runStart + runLen == c) runLen++;
            else {
                if (runLen) SmAdd(out, base + fs->dataOff + (uint64_t)(runStart - 2) * cb, (uint64_t)runLen * cb);
                runStart = c; runLen = 1;
            }
            uint64_t e = fs->dataOff + (uint64_t)(c - 1) * cb;
            if (e > *usedEnd) *usedEnd = e;
        }
    }
    if (runLen) SmAdd(out, base + fs->dataOff + (uint64_t)(runStart - 2) * cb, (uint64_t)runLen * cb);
    free(fat);
    return 1;
}

/* lê uma cadeia exFAT (pela FAT; se a FAT não tiver a cadeia, lê contíguo) */
static uint8_t *SmReadExfatChain(Fs *fs, uint32_t first, uint64_t len, int contiguous) {
    MemBuf m = {0, 0, 0, len + fs->clusterBytes, 0};
    if (ClusterStream(fs, first, contiguous, len, MemSink, &m) < 0 || m.n < len) {
        free(m.p);
        if (contiguous) return NULL;
        return SmReadExfatChain(fs, first, len, 1);
    }
    return m.p;
}

/* offset absoluto do byte "pos" de uma cadeia de clusters */
static uint64_t SmChainPos(Fs *fs, uint32_t first, uint64_t pos, int contiguous) {
    uint32_t c = first;
    uint64_t k = pos / fs->clusterBytes;
    for (uint64_t i = 0; i < k; i++) {
        if (contiguous) c++;
        else { uint32_t nx = FatNext(fs, c); if (nx == 0xFFFFFFFFu) return 0; c = nx; }
    }
    return fs->base + fs->dataOff + (uint64_t)(c - 2) * fs->clusterBytes + pos % fs->clusterBytes;
}

typedef struct { uint32_t bmFirst; uint64_t bmLen; uint64_t entryAbs; int ok; } ExfatBm;

static int SmFindExfatBitmap(Fs *fs, ExfatBm *bm) {
    memset(bm, 0, sizeof *bm);
    MemBuf m = {0, 0, 0, 8u * 1024 * 1024, 0};
    if (ClusterStream(fs, fs->rootCluster, 0, UINT64_MAX, MemSink, &m) < 0 || m.n == 0) { free(m.p); return 0; }
    for (size_t o = 0; o + 32 <= m.n; o += 32) {
        if (m.p[o] == 0x81) {
            bm->bmFirst = rd32(m.p + o + 20);
            bm->bmLen = rd64(m.p + o + 24);
            bm->entryAbs = SmChainPos(fs, fs->rootCluster, o, 0);
            bm->ok = bm->bmFirst >= 2 && bm->bmLen > 0 && bm->entryAbs != 0;
            break;
        }
        if (m.p[o] == 0x00) break;
    }
    free(m.p);
    return bm->ok;
}

static int SmUsedExfat(Fs *fs, SmList *out, uint64_t *usedEnd) {
    ExfatBm bm;
    uint64_t base = fs->base;
    SmAdd(out, base, fs->dataOff); /* regiões de boot (principal e reserva), FAT */
    *usedEnd = fs->dataOff;
    if (!SmFindExfatBitmap(fs, &bm)) return 0;
    uint8_t *bits = SmReadExfatChain(fs, bm.bmFirst, bm.bmLen, 0);
    if (!bits) return 0;
    uint32_t n = fs->clusters, cb = fs->clusterBytes, runStart = 0, runLen = 0;
    for (uint32_t i = 0; i < n && (uint64_t)i / 8 < bm.bmLen; i++) {
        if (bits[i >> 3] & (1u << (i & 7))) {
            uint32_t c = i + 2;
            if (runLen && runStart + runLen == c) runLen++;
            else {
                if (runLen) SmAdd(out, base + fs->dataOff + (uint64_t)(runStart - 2) * cb, (uint64_t)runLen * cb);
                runStart = c; runLen = 1;
            }
            uint64_t e = fs->dataOff + (uint64_t)(i + 1) * cb;
            if (e > *usedEnd) *usedEnd = e;
        }
    }
    if (runLen) SmAdd(out, base + fs->dataOff + (uint64_t)(runStart - 2) * cb, (uint64_t)runLen * cb);
    free(bits);
    return 1;
}

static int SmExtHasSuper(uint32_t g, uint32_t roCompat, uint32_t compat, const uint32_t *bk) {
    if (g == 0) return 1;
    if (compat & 0x200) return g == bk[0] || g == bk[1]; /* sparse_super2 */
    if (!(roCompat & 1)) return 1;                        /* sem sparse_super: todos os grupos */
    if (g == 1) return 1;
    for (uint32_t b = 3; b <= 7; b += 2) {
        uint64_t p = b;
        while (p < g) p *= b;
        if (p == g) return 1;
    }
    return 0;
}

/* 1 = ok, 0 = erro de leitura, -1 = recurso não suportado (copiar a partição inteira) */
static int SmUsedExt(Fs *fs, SmList *out, uint64_t *usedEnd) {
    uint8_t sb[1024];
    uint64_t base = fs->base;
    if (!Dev2Read(fs->d, base + 1024, sb, 1024)) return 0;
    uint32_t compat = rd32(sb + 0x5C), incompat = rd32(sb + 0x60), roCompat = rd32(sb + 0x64);
    if (incompat & 0x10) return -1;   /* meta_bg */
    if (roCompat & 0x200) return -1;  /* bigalloc */
    uint32_t rsvGdt = rd16(sb + 0xCE);
    uint32_t bk[2] = {rd32(sb + 0x24C), rd32(sb + 0x250)};
    uint64_t bs = fs->bs, blocks = fs->blocksCount;
    uint32_t bpg = fs->bpg, fd = fs->firstData;
    if (blocks <= fd) return 0;
    uint64_t ngroups = (blocks - fd + bpg - 1) / bpg;
    uint64_t gdtBlocks = (ngroups * fs->descSize + bs - 1) / bs;
    uint64_t itBlocks = ((uint64_t)fs->ipg * fs->inodeSize + bs - 1) / bs;
    /* marca blocos num mapa de bits do sistema de arquivos inteiro e depois converte em trechos */
    uint64_t mapBytes = (blocks + 7) / 8;
    uint8_t *map = (uint8_t *)calloc(1, (size_t)mapBytes);
    uint8_t *gdt = (uint8_t *)malloc((size_t)(gdtBlocks * bs));
    uint8_t *bmb = (uint8_t *)malloc((size_t)bs);
    int rc = 1;
    if (!map || !gdt || !bmb) { rc = 0; goto done; }
    if (!Dev2Read(fs->d, base + (uint64_t)(fd + 1) * bs, gdt, gdtBlocks * bs)) { rc = 0; goto done; }
#define MARK(b0, cnt) do { uint64_t _b = (b0), _e = (b0) + (cnt); if (_e > blocks) _e = blocks; for (; _b < _e; _b++) map[_b >> 3] |= (uint8_t)(1u << (_b & 7)); } while (0)
    MARK(0, (uint64_t)fd + 1 + gdtBlocks + rsvGdt); /* boot, superbloco, GDT, GDT reservada */
    for (uint64_t g = 0; g < ngroups; g++) {
        const uint8_t *d = gdt + g * fs->descSize;
        uint64_t bb = rd32(d + 0), ib = rd32(d + 4), it = rd32(d + 8);
        uint16_t flags = rd16(d + 0x12);
        if (fs->descSize >= 64) {
            bb |= (uint64_t)rd32(d + 0x20) << 32;
            ib |= (uint64_t)rd32(d + 0x24) << 32;
            it |= (uint64_t)rd32(d + 0x28) << 32;
        }
        uint64_t gs = fd + g * bpg;
        if (SmExtHasSuper((uint32_t)g, roCompat, compat, bk)) MARK(gs, 1 + gdtBlocks + rsvGdt);
        if (bb < blocks) MARK(bb, 1);
        if (ib < blocks) MARK(ib, 1);
        if (it < blocks) MARK(it, itBlocks);
        if (!(flags & 0x2) && bb < blocks) { /* bitmap inicializado */
            if (!Dev2Read(fs->d, base + bb * bs, bmb, bs)) { rc = 0; goto done; }
            for (uint64_t i = 0; i < bpg && gs + i < blocks; i++)
                if (bmb[i >> 3] & (1u << (i & 7))) map[(gs + i) >> 3] |= (uint8_t)(1u << ((gs + i) & 7));
        }
    }
#undef MARK
    *usedEnd = 0;
    {
        uint64_t rs = 0, rl = 0;
        for (uint64_t b = 0; b < blocks; b++) {
            if (!(map[b >> 3] & (1u << (b & 7)))) continue;
            if (rl && rs + rl == b) rl++;
            else {
                if (rl) SmAdd(out, base + rs * bs, rl * bs);
                rs = b; rl = 1;
            }
            *usedEnd = (b + 1) * bs;
        }
        if (rl) SmAdd(out, base + rs * bs, rl * bs);
    }
done:
    free(map); free(gdt); free(bmb);
    return rc;
}

/* ------------------------------------------------------------ plano */
#define SM_MAXPATCH 16
typedef struct { uint64_t off; uint32_t len; uint8_t *data; } SmPatch;

typedef struct { uint64_t start, size, newSize, used, usedEnd; int fs; int how; char label[48]; } SmPart;
enum { SMH_USED = 0, SMH_WHOLE = 1 }; /* como cada partição é copiada */

typedef struct {
    uint64_t srcSize, dstSize;
    uint32_t sec;
    int table;           /* TBL_* de diag.h */
    SmList copy;         /* trechos lidos da origem e gravados no mesmo lugar no destino */
    SmPatch patch[SM_MAXPATCH];
    int npatch;
    SmPart part[MAXP];
    int nparts;
    uint64_t copyBytes;  /* soma dos trechos */
    uint64_t needBytes;  /* menor destino possível */
    int shrunk;          /* uma partição foi reduzida */
    int ok;
    char err[600];       /* motivo (UTF-8) quando ok = 0 */
} SmPlan;

static void SmPlanFree(SmPlan *p) {
    SmFree(&p->copy);
    for (int i = 0; i < p->npatch; i++) free(p->patch[i].data);
    p->npatch = 0;
}

static uint8_t *SmAddPatch(SmPlan *p, uint64_t off, uint32_t len) {
    if (p->npatch >= SM_MAXPATCH) return NULL;
    uint8_t *b = (uint8_t *)calloc(1, len);
    if (!b) return NULL;
    p->patch[p->npatch].off = off; p->patch[p->npatch].len = len; p->patch[p->npatch].data = b;
    p->npatch++;
    return b;
}
/* patch que começa com o conteúdo atual da origem */
static uint8_t *SmPatchFromSrc(SmPlan *p, Dev2 *d, uint64_t off, uint32_t len) {
    uint8_t *b = SmAddPatch(p, off, len);
    if (b && !Dev2Read(d, off, b, len)) return NULL;
    return b;
}

static void SmErr(SmPlan *p, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(p->err, sizeof p->err, fmt, ap);
    va_end(ap);
    p->ok = 0;
}

static uint32_t SmExfatBootSum(const uint8_t *s, uint32_t bps) {
    uint32_t sum = 0;
    for (uint32_t i = 0; i < bps * 11; i++) {
        if (i == 106 || i == 107 || i == 112) continue;
        sum = ((sum & 1) ? 0x80000000u : 0) + (sum >> 1) + s[i];
    }
    return sum;
}

static void wr16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void wr64(uint8_t *p, uint64_t v) { wr32(p, (uint32_t)v); wr32(p + 4, (uint32_t)(v >> 32)); }

/* reduz uma FAT16/32 para newBytes (sem mover dados). 1 = ok */
static int SmShrinkFat(SmPlan *p, Dev2 *d, const SmPart *pt, uint64_t newBytes) {
    Fs fs;
    if (!FsOpen(&fs, d, pt->start)) { SmErr(p, "não foi possível abrir a partição FAT para reduzir"); return 0; }
    uint32_t bps = fs.bps;
    uint64_t tot = newBytes / bps;
    if (fs.type == FST_FAT12) { SmErr(p, "partição FAT12 não pode ser reduzida"); return 0; }
    uint64_t clusters = (tot * bps - fs.dataOff) / fs.clusterBytes;
    if (fs.type == FST_FAT32 && clusters < 65525) { SmErr(p, "a partição FAT32 ficaria pequena demais"); return 0; }
    if (fs.type == FST_FAT16 && (clusters < 4085 || tot > 0xFFFFFFFFu)) { SmErr(p, "a partição FAT16 ficaria pequena demais"); return 0; }
    if (fs.dataOff + clusters * fs.clusterBytes < pt->usedEnd) {
        SmErr(p, "os dados da última partição vão até perto do fim do cartão e não cabem no destino");
        return 0;
    }
    uint8_t *b = SmPatchFromSrc(p, d, pt->start, bps);
    if (!b) { SmErr(p, "falha ao ler o setor de boot"); return 0; }
    if (rd16(b + 19) && tot <= 0xFFFF) wr16(b + 19, (uint32_t)tot);
    else { wr16(b + 19, 0); wr32(b + 32, (uint32_t)tot); }
    if (fs.type == FST_FAT32) {
        uint32_t bk = rd16(b + 50), fsi = rd16(b + 48);
        if (bk > 0 && bk < 32) {
            uint8_t *b2 = SmAddPatch(p, pt->start + (uint64_t)bk * bps, bps);
            if (!b2) { SmErr(p, "memória"); return 0; }
            memcpy(b2, b, bps);
        }
        if (fsi > 0 && fsi < 32) {
            uint8_t *f = SmPatchFromSrc(p, d, pt->start + (uint64_t)fsi * bps, bps);
            if (f && rd32(f) == 0x41615252u) { wr32(f + 488, 0xFFFFFFFFu); wr32(f + 492, 0xFFFFFFFFu); }
        }
    }
    return 1;
}

/* reduz uma exFAT para newBytes (sem mover dados). 1 = ok */
static int SmShrinkExfat(SmPlan *p, Dev2 *d, const SmPart *pt, uint64_t newBytes) {
    Fs fs;
    if (!FsOpen(&fs, d, pt->start)) { SmErr(p, "não foi possível abrir a partição exFAT para reduzir"); return 0; }
    uint32_t bps = fs.bps;
    uint8_t *boot = (uint8_t *)malloc(bps * 12);
    if (!boot) { SmErr(p, "memória"); return 0; }
    if (!Dev2Read(d, pt->start, boot, bps * 12)) { free(boot); SmErr(p, "falha ao ler a região de boot exFAT"); return 0; }
    uint32_t bpsSh = boot[0x6C], spcSh = boot[0x6D];
    uint64_t volLen = newBytes >> bpsSh;
    uint32_t heapOff = rd32(boot + 0x58), fatLen = rd32(boot + 0x54);
    uint64_t cc = (volLen - heapOff) >> spcSh;
    uint64_t maxByFat = (uint64_t)fatLen * bps / 4 - 2;
    if (cc > maxByFat) cc = maxByFat;
    if (cc > fs.clusters) cc = fs.clusters;
    if (fs.dataOff + cc * fs.clusterBytes < pt->usedEnd) {
        free(boot);
        SmErr(p, "os dados da última partição vão até perto do fim do cartão e não cabem no destino");
        return 0;
    }
    ExfatBm bm;
    if (!SmFindExfatBitmap(&fs, &bm)) { free(boot); SmErr(p, "não achei o mapa de clusters da exFAT"); return 0; }
    wr64(boot + 0x48, volLen);
    wr32(boot + 0x5C, (uint32_t)cc);
    boot[0x70] = 0xFF; /* PercentInUse: desconhecido */
    uint32_t sum = SmExfatBootSum(boot, bps);
    for (uint32_t i = 0; i < bps; i += 4) wr32(boot + bps * 11 + i, sum);
    for (int r = 0; r < 2; r++) { /* região principal e de reserva */
        uint8_t *q = SmAddPatch(p, pt->start + (uint64_t)r * 12 * bps, bps * 12);
        if (!q) { free(boot); SmErr(p, "memória"); return 0; }
        memcpy(q, boot, bps * 12);
    }
    free(boot);
    /* tamanho do mapa de clusters na entrada do diretório raiz */
    uint64_t es = bm.entryAbs - bm.entryAbs % d->sec;
    uint8_t *e = SmPatchFromSrc(p, d, es, d->sec);
    if (!e) { SmErr(p, "falha ao ler o diretório raiz"); return 0; }
    wr64(e + (bm.entryAbs - es) + 24, (cc + 7) / 8);
    return 1;
}

static int SmPartCmp(const void *a, const void *b) {
    const SmPart *x = (const SmPart *)a, *y = (const SmPart *)b;
    return x->start < y->start ? -1 : (x->start > y->start);
}

typedef int (*SmProg)(void *ctx, int part, int nparts); /* != 0 cancela */

/* monta o plano. dstSize = tamanho do destino (cartão ou imagem). */
static int SmBuildPlan(Dev2 *d, uint64_t dstSize, SmPlan *p, SmProg prog, void *pctx) {
    Diag *g = (Diag *)calloc(1, sizeof(Diag));
    memset(p, 0, sizeof *p);
    p->ok = 1;
    p->srcSize = d->size; p->dstSize = dstSize; p->sec = d->sec;
    if (!g) { SmErr(p, "memória"); return 0; }
    DiagAnalyze(d, g, 0, NULL, NULL);
    p->table = g->tableType;
    uint32_t sec = d->sec;
    if (g->tableType == TBL_NONE || g->nparts == 0) {
        free(g);
        SmErr(p, "o cartão não tem tabela de partições reconhecida; use a cópia setor a setor");
        return 0;
    }
    for (int i = 0; i < g->nparts; i++) {
        const Part *q = &g->parts[i];
        if (q->flags & PF_EXTENDED) continue;
        if (q->start >= d->size) continue;
        SmPart *s = &p->part[p->nparts++];
        memset(s, 0, sizeof *s);
        s->start = q->start; s->size = q->size; s->fs = q->fs; s->newSize = q->size;
        strncpy(s->label, q->label, sizeof s->label - 1);
        if (s->start + s->size > d->size) s->size = s->newSize = d->size - s->start;
    }
    qsort(p->part, (size_t)p->nparts, sizeof(SmPart), SmPartCmp);
    if (p->nparts == 0) { free(g); SmErr(p, "nenhuma partição dentro do cartão"); return 0; }

    /* 1. início do disco até a primeira partição (tabela + carregador de boot) */
    SmAdd(&p->copy, 0, p->part[0].start ? p->part[0].start : sec);
    /* 2. espaços entre partições: pequenos vão inteiros; grandes, só o 1º e o último MiB (EBR) */
    for (int i = 0; i + 1 < p->nparts; i++) {
        uint64_t e = p->part[i].start + p->part[i].size, s2 = p->part[i + 1].start;
        if (s2 <= e) continue;
        uint64_t gap = s2 - e;
        if (gap <= 16 * MIB) SmAdd(&p->copy, e, gap);
        else { SmAdd(&p->copy, e, MIB); SmAdd(&p->copy, s2 - MIB, MIB); }
    }
    /* para MBR com partições lógicas, os EBRs ficam no início de cada lógica - 1 MiB, coberto acima */

    /* 3. cada partição */
    for (int i = 0; i < p->nparts; i++) {
        SmPart *s = &p->part[i];
        if (prog && prog(pctx, i, p->nparts)) { free(g); SmErr(p, "cancelado"); return 0; }
        SmList u = {0, 0, 0};
        int r = 0;
        Fs fs;
        if (FsBrowsable(s->fs) && FsOpen(&fs, d, s->start)) {
            if (s->fs == FST_EXFAT) r = SmUsedExfat(&fs, &u, &s->usedEnd);
            else if (s->fs == FST_EXT2 || s->fs == FST_EXT3 || s->fs == FST_EXT4) r = SmUsedExt(&fs, &u, &s->usedEnd);
            else r = SmUsedFat(&fs, &u, &s->usedEnd);
            FsClose(&fs);
        }
        if (r == 1) {
            s->how = SMH_USED;
            SmAdd(&u, s->start, s->size < MIB ? s->size : MIB); /* setores de boot e reservados */
            SmNormalize(&u, sec, s->start + s->size);
            for (int k = 0; k < u.n; k++) SmAdd(&p->copy, u.v[k].off, u.v[k].len);
            s->used = SmTotal(&u);
            if (s->usedEnd < MIB && s->size >= MIB) s->usedEnd = MIB;
        } else {
            s->how = SMH_WHOLE;
            SmAdd(&p->copy, s->start, s->size);
            s->used = s->size;
            s->usedEnd = s->size;
        }
        SmFree(&u);
    }
    /* 4. GPT: tabela de reserva no fim */
    uint64_t gptTail = 0;
    if (g->tableType == TBL_GPT) {
        uint8_t h[512];
        uint32_t n = 128, es = 128;
        if (Dev2Read(d, sec, h, 512) && memcmp(h, "EFI PART", 8) == 0) { n = rd32(h + 80); es = rd32(h + 84); }
        gptTail = (((uint64_t)n * es + sec - 1) / sec + 1) * sec;
    }

    /* 5. destino menor: reduz a última partição */
    uint64_t limit = dstSize - gptTail;
    limit -= limit % sec;
    SmPart *last = &p->part[p->nparts - 1];
    for (int i = 0; i < p->nparts - 1; i++)
        if (p->part[i].start + p->part[i].size > limit) {
            free(g);
            SmErr(p, "a partição %d termina depois do fim do destino e não é a última; só a última partição pode ser reduzida", i + 1);
            return 0;
        }
    p->needBytes = last->start + last->usedEnd + gptTail;
    if (last->start + last->size > limit) {
        if (last->start >= limit) { free(g); SmErr(p, "o destino é pequeno demais: a última partição começa depois do fim dele"); return 0; }
        uint64_t newSize = limit - last->start;
        newSize -= newSize % MIB;
        if (newSize == 0) newSize = limit - last->start;
        FsInfo fi;
        FsDetect(d, last->start, &fi);
        int ok = 1;
        if (fi.bytes && fi.bytes <= newSize) {
            /* o sistema de arquivos já é menor que a partição: só a tabela muda */
        } else if (last->how == SMH_USED && (last->fs == FST_FAT16 || last->fs == FST_FAT32)) ok = SmShrinkFat(p, d, last, newSize);
        else if (last->how == SMH_USED && last->fs == FST_EXFAT) ok = SmShrinkExfat(p, d, last, newSize);
        else {
            free(g);
            SmErr(p, "a última partição é %s e não pode ser reduzida (só FAT16, FAT32 e exFAT); use um destino de pelo menos %llu MB",
                  last->fs == FST_EXT2 || last->fs == FST_EXT3 || last->fs == FST_EXT4 ? "Linux (ext)" : (last->fs == FST_NTFS ? "NTFS" : "de tipo desconhecido"),
                  (unsigned long long)((last->start + last->size + gptTail + MIB - 1) / MIB));
            return 0;
        }
        if (!ok) { free(g); return 0; }
        last->newSize = newSize;
        p->shrunk = 1;
    }
    SmNormalize(&p->copy, sec, d->size);
    /* nada pode passar do limite do destino */
    uint64_t lastNewEnd = last->start + last->newSize;
    {
        int k = 0;
        for (int i = 0; i < p->copy.n; i++) {
            SmExt e = p->copy.v[i];
            if (e.off >= lastNewEnd && e.off < d->size - gptTail) continue; /* área livre depois da última partição */
            if (e.off + e.len > lastNewEnd && e.off < lastNewEnd) e.len = lastNewEnd - e.off;
            if (e.off + e.len > limit) {
                if (e.off >= limit) continue;
                e.len = limit - e.off;
            }
            p->copy.v[k++] = e;
        }
        p->copy.n = k;
    }

    /* 6. tabela de partições */
    if (g->tableType == TBL_MBR && p->shrunk) {
        uint8_t *m = SmPatchFromSrc(p, d, 0, sec);
        int found = 0;
        if (m)
            for (int k = 0; k < 4; k++) {
                uint8_t *e = m + 446 + 16 * k;
                if (!e[4]) continue;
                if ((uint64_t)rd32(e + 8) * sec == last->start) { wr32(e + 12, (uint32_t)(last->newSize / sec)); found = 1; }
            }
        if (!found) { free(g); SmErr(p, "a última partição é lógica (dentro de uma estendida) e não pode ser reduzida"); return 0; }
    }
    if (g->tableType == TBL_SUPERFLOPPY && p->shrunk) { /* sem tabela: o próprio sistema de arquivos já foi ajustado */ }
    if (g->tableType == TBL_GPT) {
        uint8_t *h = SmPatchFromSrc(p, d, sec, sec);
        if (!h || memcmp(h, "EFI PART", 8) != 0) { free(g); SmErr(p, "cabeçalho GPT ilegível"); return 0; }
        uint32_t n = rd32(h + 80), es = rd32(h + 84), hs = rd32(h + 12);
        uint64_t entLba = rd64(h + 72), dstLbas = dstSize / sec;
        uint32_t entBytes = n * es, entSecs = (entBytes + sec - 1) / sec;
        if (hs < 92 || hs > sec || es < 128 || entBytes > 1024 * 1024) { free(g); SmErr(p, "GPT fora do padrão"); return 0; }
        uint8_t *ent = SmPatchFromSrc(p, d, entLba * sec, entSecs * sec);
        if (!ent) { free(g); SmErr(p, "entradas GPT ilegíveis"); return 0; }
        if (p->shrunk)
            for (uint32_t k = 0; k < n; k++) {
                uint8_t *e = ent + (uint64_t)k * es;
                if (rd64(e + 32) * sec == last->start) wr64(e + 40, (last->start + last->newSize) / sec - 1);
            }
        uint32_t ecrc = Crc32(ent, entBytes);
        wr64(h + 32, dstLbas - 1);                 /* LBA da cópia de reserva */
        wr64(h + 48, dstLbas - 1 - entSecs - 1);   /* último LBA utilizável */
        wr32(h + 88, ecrc);
        wr32(h + 16, 0);
        wr32(h + 16, Crc32(h, hs));
        /* cópia de reserva no fim do destino */
        uint8_t *be = SmAddPatch(p, (dstLbas - 1 - entSecs) * sec, entSecs * sec);
        uint8_t *bh = SmAddPatch(p, (dstLbas - 1) * sec, sec);
        if (!be || !bh) { free(g); SmErr(p, "memória"); return 0; }
        memcpy(be, ent, entSecs * sec);
        memcpy(bh, h, sec);
        wr64(bh + 24, dstLbas - 1);
        wr64(bh + 32, 1);
        wr64(bh + 72, dstLbas - 1 - entSecs);
        wr32(bh + 16, 0);
        wr32(bh + 16, Crc32(bh, hs));
        /* MBR de proteção */
        uint8_t *m = SmPatchFromSrc(p, d, 0, sec);
        if (m)
            for (int k = 0; k < 4; k++) {
                uint8_t *e = m + 446 + 16 * k;
                if (e[4] == 0xEE) wr32(e + 12, dstLbas - 1 > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)(dstLbas - 1));
            }
        /* não copiar a GPT de reserva antiga da origem (fica em outro lugar se o tamanho mudou) */
        int k = 0;
        for (int i = 0; i < p->copy.n; i++) {
            SmExt e = p->copy.v[i];
            uint64_t tail = d->size - gptTail;
            if (e.off >= tail) continue;
            if (e.off + e.len > tail) e.len = tail - e.off;
            p->copy.v[k++] = e;
        }
        p->copy.n = k;
    }
    p->copyBytes = SmTotal(&p->copy);
    free(g);
    return p->ok;
}

/* ------------------------------------------------------------ execução */
typedef int (*SmRead)(void *ctx, uint64_t off, void *buf, uint32_t len);       /* 1 = ok */
typedef int (*SmWrite)(void *ctx, uint64_t off, const void *buf, uint32_t len); /* 1 = ok */
typedef int (*SmTick)(void *ctx, uint64_t done);                                /* != 0 cancela */

#define SM_CHUNK (4u * 1024 * 1024)
/* buf: área alinhada de SM_CHUNK bytes. Retorna 1 = ok, 0 = erro de leitura, -1 = erro de gravação, 2 = cancelado */
static int SmRun(const SmPlan *p, SmRead rd, void *rctx, SmWrite wr, void *wctx, uint8_t *buf, SmTick tick, void *tctx, uint64_t *badBytes) {
    uint64_t done = 0;
    for (int i = 0; i < p->copy.n; i++) {
        uint64_t off = p->copy.v[i].off, left = p->copy.v[i].len;
        while (left) {
            uint32_t n = left > SM_CHUNK ? SM_CHUNK : (uint32_t)left;
            if (!rd(rctx, off, buf, n)) {
                /* setor a setor: ilegíveis viram zeros */
                for (uint32_t o = 0; o < n; o += p->sec)
                    if (!rd(rctx, off + o, buf + o, p->sec)) { memset(buf + o, 0, p->sec); if (badBytes) *badBytes += p->sec; }
            }
            if (!wr(wctx, off, buf, n)) return -1;
            off += n; left -= n; done += n;
            if (tick && tick(tctx, done)) return 2;
        }
    }
    for (int i = 0; i < p->npatch; i++)
        for (uint32_t o = 0; o < p->patch[i].len; o += SM_CHUNK) {
            uint32_t n = p->patch[i].len - o > SM_CHUNK ? SM_CHUNK : p->patch[i].len - o;
            memcpy(buf, p->patch[i].data + o, n);
            if (!wr(wctx, p->patch[i].off + o, buf, n)) return -1;
        }
    return 1;
}

#endif
