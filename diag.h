/* diag.h - diagnóstico de cartões: tabela de partições (MBR/GPT), sistemas de arquivos,
 * partições perdidas e reconstrução da tabela. Independente de Windows. */
#ifndef DIAG_H
#define DIAG_H

#include "fsread.h"

static uint32_t crc_tab[256];
static int crc_ready = 0;
static uint32_t Crc32(const uint8_t *p, size_t n) {
    if (!crc_ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            crc_tab[i] = c;
        }
        crc_ready = 1;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = crc_tab[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

enum { TBL_NONE = 0, TBL_MBR, TBL_GPT, TBL_SUPERFLOPPY };
enum { PF_BEYOND = 1, PF_ACTIVE = 2, PF_OVERLAP = 4, PF_EXTENDED = 8, PF_FSBIGGER = 16 };
enum { SRC_MBR = 0, SRC_LOGICAL = 1, SRC_GPT = 2, SRC_FOUND = 3 };

typedef struct {
    int src;
    uint64_t start, size; /* bytes */
    int mbrType;
    int fs;
    char label[48];
    uint64_t fsBytes;
    int flags;
} Part;

#define MAXP 64
#define MAXF 32
typedef struct {
    int tableType;
    int mbrSigOk, mbrEntries;
    int gptHeaderOk, gptCrcOk, gptBackupOk;
    uint64_t diskBytes;
    uint32_t sec;
    int nparts;
    Part parts[MAXP];
    int nfound;
    Part found[MAXF];
    uint64_t coveredBytes;
    int sampleFail, sampleTotal;
    unsigned verdict;
} Diag;

enum {
    V_HEALTHY = 1u << 0, V_TINY = 1u << 1, V_READERR = 1u << 2, V_BEYOND = 1u << 3, V_NOTABLE = 1u << 4,
    V_RAW = 1u << 5, V_BADGPT = 1u << 6, V_SMALLPART = 1u << 7, V_LINUX = 1u << 8, V_ENCRYPTED = 1u << 9,
    V_SUPERFLOPPY = 1u << 10, V_UNKNOWNPART = 1u << 11
};

#define MIB (1024ull * 1024ull)
#define GIB (1024ull * MIB)
#define TINY_LIMIT (128ull * MIB) /* abaixo disso o cartão está informando uma capacidade absurda (ex.: 32 MB) */

static int AddPart(Diag *g, int src, uint64_t start, uint64_t size, int mbrType, int active) {
    if (g->nparts >= MAXP) return 0;
    Part *p = &g->parts[g->nparts++];
    memset(p, 0, sizeof *p);
    p->src = src; p->start = start; p->size = size; p->mbrType = mbrType;
    if (active) p->flags |= PF_ACTIVE;
    if (mbrType == 0x05 || mbrType == 0x0F || mbrType == 0x85) p->flags |= PF_EXTENDED;
    if (start + size > g->diskBytes) p->flags |= PF_BEYOND;
    return 1;
}

static void ParseEbrChain(Dev2 *d, Diag *g, uint64_t extBase, uint64_t extSize) {
    uint64_t ebr = extBase;
    uint8_t b[512];
    for (int i = 0; i < 64; i++) {
        if (ebr + 512 > g->diskBytes) break;
        if (!Dev2Read(d, ebr, b, 512) || b[510] != 0x55 || b[511] != 0xAA) break;
        uint32_t lba0 = rd32(b + 446 + 8), cnt0 = rd32(b + 446 + 12);
        int t0 = b[446 + 4];
        if (t0 != 0 && cnt0 != 0) AddPart(g, SRC_LOGICAL, ebr + (uint64_t)lba0 * g->sec, (uint64_t)cnt0 * g->sec, t0, 0);
        int t1 = b[446 + 16 + 4];
        uint32_t lba1 = rd32(b + 446 + 16 + 8);
        if (!(t1 == 0x05 || t1 == 0x0F || t1 == 0x85) || lba1 == 0) break;
        ebr = extBase + (uint64_t)lba1 * g->sec;
        (void)extSize;
    }
}

/* tenta ler um cabeçalho GPT em hdrOff; preenche entradas se ok. retorna: 0 inválido, 1 cabeçalho ok */
static int ParseGptAt(Dev2 *d, Diag *g, uint64_t hdrOff, int addParts, int *crcOk) {
    uint8_t h[512];
    *crcOk = 0;
    if (hdrOff + g->sec > g->diskBytes) return 0;
    if (!Dev2Read(d, hdrOff, h, 512)) return 0;
    if (memcmp(h, "EFI PART", 8) != 0) return 0;
    uint32_t hs = rd32(h + 12);
    if (hs < 92 || hs > 512) return 0;
    uint32_t crc = rd32(h + 16);
    uint8_t tmp[512];
    memcpy(tmp, h, hs);
    memset(tmp + 16, 0, 4);
    int hdrCrc = (Crc32(tmp, hs) == crc);
    uint64_t eLba = rd64(h + 72);
    uint32_t ne = rd32(h + 80), es = rd32(h + 84);
    if (ne == 0 || ne > 1024 || es < 128 || es > 512 || (es & (es - 1))) return 0;
    uint64_t eOff = eLba * g->sec, eLen = (uint64_t)ne * es;
    if (eOff + eLen > g->diskBytes) return 0;
    uint8_t *e = (uint8_t *)malloc((size_t)eLen);
    if (!e) return 0;
    if (!Dev2Read(d, eOff, e, eLen)) { free(e); return 0; }
    int entCrc = (Crc32(e, (size_t)eLen) == rd32(h + 88));
    *crcOk = hdrCrc && entCrc;
    if (addParts) {
        for (uint32_t i = 0; i < ne; i++) {
            const uint8_t *x = e + (size_t)i * es;
            int zero = 1;
            for (int k = 0; k < 16; k++) if (x[k]) { zero = 0; break; }
            if (zero) continue;
            uint64_t f = rd64(x + 32), l = rd64(x + 40);
            if (l < f) continue;
            AddPart(g, SRC_GPT, f * g->sec, (l - f + 1) * g->sec, 0, 0);
        }
    }
    free(e);
    return 1;
}

static void ParseTable(Dev2 *d, Diag *g) {
    uint8_t b[512];
    FsInfo fi;
    g->tableType = TBL_NONE;
    if (!Dev2Read(d, 0, b, 512)) return;
    FsDetect(d, 0, &fi);
    if (fi.type != FST_EMPTY && fi.type != FST_UNKNOWN && fi.type != FST_NONE) {
        g->tableType = TBL_SUPERFLOPPY;
        AddPart(g, SRC_MBR, 0, fi.bytes ? fi.bytes : g->diskBytes, 0, 0);
        return;
    }
    g->mbrSigOk = (b[510] == 0x55 && b[511] == 0xAA);
    if (!g->mbrSigOk) return;
    int protective = 0;
    for (int i = 0; i < 4; i++) {
        const uint8_t *x = b + 446 + i * 16;
        int t = x[4];
        uint32_t lba = rd32(x + 8), cnt = rd32(x + 12);
        if (t == 0 || cnt == 0) continue;
        if (t == 0xEE) { protective = 1; continue; }
        g->mbrEntries++;
        if (g->tableType == TBL_NONE) g->tableType = TBL_MBR;
        AddPart(g, SRC_MBR, (uint64_t)lba * g->sec, (uint64_t)cnt * g->sec, t, x[0] == 0x80);
        if (t == 0x05 || t == 0x0F || t == 0x85) ParseEbrChain(d, g, (uint64_t)lba * g->sec, (uint64_t)cnt * g->sec);
    }
    if (protective) {
        g->tableType = TBL_GPT;
        int crcOk = 0;
        g->nparts = 0;
        g->gptHeaderOk = ParseGptAt(d, g, g->sec, 1, &crcOk);
        g->gptCrcOk = crcOk;
        int bcrc = 0;
        uint64_t last = (g->diskBytes / g->sec) * g->sec - g->sec;
        int bak = ParseGptAt(d, g, last, 0, &bcrc);
        g->gptBackupOk = bak && bcrc;
        if (!g->gptHeaderOk && bak) {
            /* cabeçalho principal ruim, mas o backup existe: lê as partições pelo backup */
            int c2 = 0;
            g->nparts = 0;
            ParseGptAt(d, g, last, 1, &c2);
        }
    }
}

static int DiagSample(Dev2 *d, Diag *g, uint64_t off) {
    uint8_t b[4096];
    g->sampleTotal++;
    off = (off / g->sec) * g->sec;
    if (off + 4096 > g->diskBytes) off = g->diskBytes >= 4096 ? ((g->diskBytes - 4096) / g->sec) * g->sec : 0;
    if (!Dev2Read(d, off, b, g->diskBytes >= 4096 ? 4096 : g->sec)) { g->sampleFail++; return 0; }
    return 1;
}

static int PartCmp(const void *a, const void *b) {
    const Part *x = (const Part *)a, *y = (const Part *)b;
    return x->start < y->start ? -1 : x->start > y->start;
}

static uint64_t NextMiB(uint64_t x) { return ((x + MIB - 1) / MIB) * MIB; }

typedef int (*DiagProg)(void *ctx, uint64_t pos, int found); /* != 0 cancela */

static int InTable(const Diag *g, uint64_t pos, uint64_t *end) {
    for (int i = 0; i < g->nparts; i++) {
        const Part *p = &g->parts[i];
        if (p->flags & PF_EXTENDED) continue;
        if (pos >= p->start && pos < p->start + p->size) { *end = p->start + p->size; return 1; }
    }
    return 0;
}

static void ScanLost(Dev2 *d, Diag *g, DiagProg prog, void *pctx) {
    uint32_t oldWin = d->win;
    d->win = 4096; /* leituras pequenas: uma por candidato */
    d->cLen = 0;
    uint64_t pos = 63ull * g->sec;
    unsigned loops = 0;
    g->nfound = 0;
    while (pos + 4096 <= g->diskBytes) {
        uint64_t end;
        if (InTable(g, pos, &end)) { pos = NextMiB(end); continue; }
        if ((++loops & 63) == 0 && prog && prog(pctx, pos, g->nfound)) break;
        FsInfo fi;
        FsDetect(d, pos, &fi);
        if (fi.type != FST_EMPTY && fi.type != FST_UNKNOWN && fi.type != FST_NONE) {
            int dup = 0;
            for (int i = 0; i < g->nparts; i++) if (g->parts[i].start == pos) dup = 1;
            if (!dup && g->nfound < MAXF) {
                Part *p = &g->found[g->nfound++];
                memset(p, 0, sizeof *p);
                p->src = SRC_FOUND; p->start = pos; p->fs = fi.type; p->fsBytes = fi.bytes;
                strcpy(p->label, fi.label);
                p->size = fi.bytes;
                if (fi.bytes && pos + fi.bytes > g->diskBytes) p->flags |= PF_BEYOND;
            }
            if (fi.bytes && pos + fi.bytes <= g->diskBytes) { pos = NextMiB(pos + fi.bytes); continue; }
        }
        pos = NextMiB(pos + 1);
    }
    d->win = oldWin;
    d->cLen = 0;
    /* tamanho das encontradas sem tamanho declarado: até a próxima partição ou o fim */
    for (int i = 0; i < g->nfound; i++) {
        Part *p = &g->found[i];
        if (p->size) continue;
        uint64_t lim = g->diskBytes;
        for (int k = 0; k < g->nfound; k++) if (g->found[k].start > p->start && g->found[k].start < lim) lim = g->found[k].start;
        for (int k = 0; k < g->nparts; k++) if (g->parts[k].start > p->start && g->parts[k].start < lim) lim = g->parts[k].start;
        p->size = lim - p->start;
    }
}

static void DiagAnalyze(Dev2 *d, Diag *g, int scan, DiagProg prog, void *pctx) {
    memset(g, 0, sizeof *g);
    g->diskBytes = d->size; g->sec = d->sec;
    DiagSample(d, g, 0);
    DiagSample(d, g, d->size / 2);
    DiagSample(d, g, d->size);
    ParseTable(d, g);
    /* sobreposição e sistema de arquivos de cada partição */
    for (int i = 0; i < g->nparts; i++) {
        Part *p = &g->parts[i];
        if (p->flags & PF_EXTENDED) continue;
        FsInfo fi;
        if (p->start < d->size) {
            FsDetect(d, p->start, &fi);
            p->fs = fi.type; p->fsBytes = fi.bytes;
            strcpy(p->label, fi.label);
            if (fi.bytes > p->size + g->sec) p->flags |= PF_FSBIGGER;
        } else p->fs = FST_NONE;
        for (int k = 0; k < g->nparts; k++) {
            if (k == i || (g->parts[k].flags & PF_EXTENDED)) continue;
            const Part *q = &g->parts[k];
            if (p->start < q->start + q->size && q->start < p->start + p->size) p->flags |= PF_OVERLAP;
        }
    }
    /* cobertura (união dos intervalos, cortada no fim do disco) */
    {
        Part tmp[MAXP];
        int n = 0;
        for (int i = 0; i < g->nparts; i++) if (!(g->parts[i].flags & PF_EXTENDED) && g->parts[i].start < d->size) tmp[n++] = g->parts[i];
        qsort(tmp, (size_t)n, sizeof(Part), PartCmp);
        uint64_t curEnd = 0, cov = 0;
        for (int i = 0; i < n; i++) {
            uint64_t s = tmp[i].start, e = tmp[i].start + tmp[i].size;
            if (e > d->size) e = d->size;
            if (e <= curEnd) continue;
            if (s < curEnd) s = curEnd;
            cov += e - s; curEnd = e;
        }
        g->coveredBytes = cov;
    }
    int needScan = scan && (g->tableType == TBL_NONE || g->tableType == TBL_GPT || g->tableType == TBL_MBR) && g->diskBytes >= 2 * MIB;
    if (needScan) ScanLost(d, g, prog, pctx);
    /* ---- veredito ---- */
    unsigned v = 0;
    if (g->diskBytes < TINY_LIMIT) v |= V_TINY;
    if (g->sampleFail) v |= V_READERR;
    for (int i = 0; i < g->nparts; i++) if (g->parts[i].flags & PF_BEYOND) v |= V_BEYOND;
    if (g->tableType == TBL_NONE || (g->tableType == TBL_GPT && g->nparts == 0)) v |= (g->nfound > 0 ? V_NOTABLE : V_RAW);
    if (g->tableType == TBL_GPT && (!g->gptHeaderOk || !g->gptCrcOk || !g->gptBackupOk)) v |= V_BADGPT;
    if (g->tableType == TBL_SUPERFLOPPY) v |= V_SUPERFLOPPY;
    if (g->diskBytes > g->coveredBytes && g->diskBytes - g->coveredBytes >= 64 * MIB && g->coveredBytes * 10 < g->diskBytes * 9 && g->nparts > 0) v |= V_SMALLPART;
    for (int pass = 0; pass < 2; pass++) {
        int n = pass == 0 ? g->nparts : g->nfound;
        for (int i = 0; i < n; i++) {
            const Part *p = pass == 0 ? &g->parts[i] : &g->found[i];
            if (p->flags & PF_EXTENDED) continue;
            if (p->fs == FST_EXT2 || p->fs == FST_EXT3 || p->fs == FST_EXT4 || p->fs == FST_SWAP || p->fs == FST_SQUASH || p->fs == FST_F2FS || p->fs == FST_XFS) v |= V_LINUX;
            if (p->fs == FST_BITLOCKER || p->fs == FST_LUKS) v |= V_ENCRYPTED;
            if (pass == 0 && (p->fs == FST_UNKNOWN || p->fs == FST_EMPTY) && p->start < d->size) v |= V_UNKNOWNPART;
        }
    }
    if (!(v & (V_TINY | V_READERR | V_BEYOND | V_NOTABLE | V_RAW | V_BADGPT | V_SMALLPART | V_UNKNOWNPART))) v |= V_HEALTHY;
    g->verdict = v;
}

/* ------------------------------------------------------ reconstrução do MBR */
static int MbrTypeFor(const Part *p) {
    if (p->mbrType && p->src != SRC_FOUND) return p->mbrType;
    switch (p->fs) {
    case FST_FAT12: return 0x01;
    case FST_FAT16: return 0x0E;
    case FST_FAT32: return 0x0C;
    case FST_EXFAT: case FST_NTFS: case FST_BITLOCKER: return 0x07;
    case FST_SWAP: return 0x82;
    default: return 0x83;
    }
}

/* monta a lista final: partições primárias válidas da tabela atual + as encontradas que não colidem. 0 se GPT. */
static int BuildRecoveryList(const Diag *g, Part *out, int max, int *dropped) {
    int n = 0;
    *dropped = 0;
    if (g->tableType == TBL_GPT || g->tableType == TBL_SUPERFLOPPY) return -1;
    for (int i = 0; i < g->nparts && n < max; i++) {
        const Part *p = &g->parts[i];
        if (p->src != SRC_MBR || (p->flags & (PF_EXTENDED | PF_BEYOND | PF_OVERLAP))) continue;
        out[n++] = *p;
    }
    for (int i = 0; i < g->nfound; i++) {
        Part f = g->found[i];
        if (f.flags & PF_BEYOND) continue;
        if (f.fs == FST_SWAP || f.fs == FST_EMPTY) { /* swap é recuperável como partição */ }
        int clash = 0;
        for (int k = 0; k < n; k++) if (f.start < out[k].start + out[k].size && out[k].start < f.start + f.size) clash = 1;
        if (clash) continue;
        if (n >= 4) { (*dropped)++; continue; }
        out[n++] = f;
    }
    qsort(out, (size_t)n, sizeof(Part), PartCmp);
    if (n > 4) { *dropped += n - 4; n = 4; }
    return n;
}

/* sector0: saída com sec bytes. old: setor 0 atual (para manter o código de boot) ou NULL. 0 = ok, -1 = não cabe em MBR */
static int BuildMbr(uint8_t *sector0, uint32_t sec, const uint8_t *old, const Part *list, int n) {
    memset(sector0, 0, sec);
    if (old && old[510] == 0x55 && old[511] == 0xAA) memcpy(sector0, old, 446);
    for (int i = 0; i < n; i++) {
        uint64_t lba = list[i].start / sec, cnt = list[i].size / sec;
        if (lba > 0xFFFFFFFFull || cnt > 0xFFFFFFFFull || cnt == 0) return -1;
        uint8_t *x = sector0 + 446 + i * 16;
        x[0] = (list[i].flags & PF_ACTIVE) ? 0x80 : 0x00;
        x[1] = 0xFE; x[2] = 0xFF; x[3] = 0xFF;
        x[4] = (uint8_t)MbrTypeFor(&list[i]);
        x[5] = 0xFE; x[6] = 0xFF; x[7] = 0xFF;
        x[8] = (uint8_t)lba; x[9] = (uint8_t)(lba >> 8); x[10] = (uint8_t)(lba >> 16); x[11] = (uint8_t)(lba >> 24);
        x[12] = (uint8_t)cnt; x[13] = (uint8_t)(cnt >> 8); x[14] = (uint8_t)(cnt >> 16); x[15] = (uint8_t)(cnt >> 24);
    }
    sector0[510] = 0x55; sector0[511] = 0xAA;
    return 0;
}

#endif
