/* fsread.h - leitura SOMENTE LEITURA de FAT12/16/32, exFAT e ext2/3/4 sobre um dispositivo bruto.
 * Independente de Windows: o app e os testes em Linux usam o mesmo código. */
#ifndef FSREAD_H
#define FSREAD_H

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <wchar.h>
#include "core.h"

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32); }

/* ------------------------------------------------------------ leitura bruta com cache */
#define DEV_WIN (128u * 1024)
#define DEV_CAP (4u * 1024 * 1024)

typedef int (*DevRaw)(void *ctx, uint64_t off, void *buf, uint32_t len); /* off/len múltiplos do setor; 1 = ok */
typedef struct {
    DevRaw rd;
    void *ctx;
    uint64_t size;
    uint32_t sec;
    uint8_t *buf;
    uint32_t win;
    uint64_t cOff;
    uint32_t cLen;
    unsigned long reads;
} Dev2;

static int Dev2Init(Dev2 *d, DevRaw rd, void *ctx, uint64_t size, uint32_t sec) {
    memset(d, 0, sizeof *d);
    d->rd = rd; d->ctx = ctx; d->size = size; d->sec = sec ? sec : 512; d->win = DEV_WIN;
    d->buf = (uint8_t *)ALLOC_ALIGNED(DEV_CAP);
    return d->buf != NULL;
}
static void Dev2Free(Dev2 *d) { if (d->buf) FREE_ALIGNED(d->buf); d->buf = NULL; }

/* leitura de qualquer posição/tamanho; 0 em falha */
static int Dev2Read(Dev2 *d, uint64_t off, void *out, uint64_t len) {
    uint8_t *o = (uint8_t *)out;
    if (off > d->size || len > d->size - off) return 0;
    uint64_t usable = (d->size / d->sec) * d->sec;
    while (len) {
        if (d->cLen && off >= d->cOff && off < d->cOff + d->cLen) {
            uint64_t n = d->cOff + d->cLen - off;
            if (n > len) n = len;
            memcpy(o, d->buf + (off - d->cOff), (size_t)n);
            o += n; off += n; len -= n;
            continue;
        }
        uint64_t a, want;
        if (len >= d->win) {
            a = off - off % d->sec;
            want = ((off + len - a + d->sec - 1) / d->sec) * d->sec;
            if (want > DEV_CAP) want = DEV_CAP;
        } else {
            a = off & ~(uint64_t)(d->win - 1);
            want = d->win;
        }
        if (a >= usable) return 0;
        if (a + want > usable) want = usable - a;
        if (off >= a + want) return 0;
        d->reads++;
        if (!d->rd(d->ctx, a, d->buf, (uint32_t)want)) { d->cLen = 0; return 0; }
        d->cOff = a; d->cLen = (uint32_t)want;
    }
    return 1;
}

/* ------------------------------------------------------------------ tipos */
enum { FST_NONE = 0, FST_FAT12, FST_FAT16, FST_FAT32, FST_EXFAT, FST_NTFS, FST_EXT2, FST_EXT3, FST_EXT4,
       FST_SWAP, FST_BITLOCKER, FST_LUKS, FST_SQUASH, FST_F2FS, FST_XFS, FST_EMPTY, FST_UNKNOWN };

static const wchar_t *FsNameW(int t) {
    switch (t) {
    case FST_FAT12: return L"FAT12"; case FST_FAT16: return L"FAT16"; case FST_FAT32: return L"FAT32";
    case FST_EXFAT: return L"exFAT"; case FST_NTFS: return L"NTFS"; case FST_EXT2: return L"ext2";
    case FST_EXT3: return L"ext3"; case FST_EXT4: return L"ext4"; case FST_SWAP: return L"swap Linux";
    case FST_BITLOCKER: return L"BitLocker (criptografada)"; case FST_LUKS: return L"LUKS (criptografada)";
    case FST_SQUASH: return L"SquashFS"; case FST_F2FS: return L"F2FS"; case FST_XFS: return L"XFS";
    case FST_EMPTY: return L"vazia (zeros)"; case FST_UNKNOWN: return L"sem sistema de arquivos reconhecido";
    }
    return L"?";
}
static int FsBrowsable(int t) { return t == FST_FAT12 || t == FST_FAT16 || t == FST_FAT32 || t == FST_EXFAT || t == FST_EXT2 || t == FST_EXT3 || t == FST_EXT4; }
static int FsWindowsReadable(int t) { return t == FST_FAT12 || t == FST_FAT16 || t == FST_FAT32 || t == FST_EXFAT || t == FST_NTFS; }

typedef struct { int type; uint64_t bytes; char label[48]; } FsInfo;

static void CopyLabel(char *dst, const uint8_t *src, int n) {
    int k = 0;
    for (int i = 0; i < n; i++) {
        uint8_t c = src[i];
        if (c == 0) break;
        dst[k++] = (c >= 32 && c < 127) ? (char)c : '?';
    }
    while (k > 0 && dst[k - 1] == ' ') k--;
    dst[k] = 0;
}

/* identifica o sistema de arquivos que começa em base. limit = bytes disponíveis a partir de base. */
static void FsDetect(Dev2 *d, uint64_t base, FsInfo *fi) {
    uint8_t b[4096];
    memset(fi, 0, sizeof *fi);
    fi->type = FST_UNKNOWN;
    if (base >= d->size || d->size - base < 2048) return;
    uint64_t avail = d->size - base;
    uint32_t want = avail >= 4096 ? 4096 : 2048;
    memset(b, 0, sizeof b);
    if (!Dev2Read(d, base, b, want)) return;
    int zero = 1;
    for (int i = 0; i < 4096; i++) if (b[i]) { zero = 0; break; }
    if (zero) { fi->type = FST_EMPTY; return; }
    int sig = (b[510] == 0x55 && b[511] == 0xAA);
    if (sig && memcmp(b + 3, "EXFAT   ", 8) == 0) {
        fi->type = FST_EXFAT;
        if (b[0x6C] >= 9 && b[0x6C] <= 12) fi->bytes = rd64(b + 0x48) << b[0x6C];
        return;
    }
    if (memcmp(b + 3, "NTFS    ", 8) == 0) {
        uint32_t bps = rd16(b + 11);
        fi->type = FST_NTFS;
        if (bps >= 512 && bps <= 4096) fi->bytes = (rd64(b + 0x28) + 1) * bps;
        return;
    }
    if (memcmp(b + 3, "-FVE-FS-", 8) == 0) { fi->type = FST_BITLOCKER; return; }
    if (sig && (b[0] == 0xEB || b[0] == 0xE9)) {
        uint32_t bps = rd16(b + 11), spc = b[13], rsv = rd16(b + 14), nf = b[16], rootEnt = rd16(b + 17);
        uint32_t tot = rd16(b + 19) ? rd16(b + 19) : rd32(b + 32);
        uint32_t fsz = rd16(b + 22) ? rd16(b + 22) : rd32(b + 36);
        int okb = (bps == 512 || bps == 1024 || bps == 2048 || bps == 4096) && spc && (spc & (spc - 1)) == 0 && spc <= 128 &&
                  rsv >= 1 && nf >= 1 && nf <= 2 && b[21] >= 0xF0 && tot > 0 && fsz > 0;
        if (okb) {
            uint32_t rootSec = (rootEnt * 32 + bps - 1) / bps;
            uint64_t sys = (uint64_t)rsv + (uint64_t)nf * fsz + rootSec;
            if (tot > sys) {
                uint32_t clusters = (uint32_t)((tot - sys) / spc);
                fi->type = clusters < 4085 ? FST_FAT12 : (clusters < 65525 ? FST_FAT16 : FST_FAT32);
                fi->bytes = (uint64_t)tot * bps;
                CopyLabel(fi->label, b + (fi->type == FST_FAT32 ? 71 : 43), 11);
                return;
            }
        }
    }
    if (rd16(b + 1024 + 56) == 0xEF53) {
        const uint8_t *sb = b + 1024;
        uint32_t lg = rd32(sb + 24);
        if (lg <= 6) {
            uint32_t bs = 1024u << lg;
            uint32_t incompat = rd32(sb + 96), compat = rd32(sb + 92);
            uint64_t blocks = rd32(sb + 4);
            if ((incompat & 0x80) && rd16(sb + 254) >= 64) blocks |= (uint64_t)rd32(sb + 0x150) << 32;
            fi->type = (incompat & (0x40 | 0x80 | 0x200)) ? FST_EXT4 : ((compat & 0x4) ? FST_EXT3 : FST_EXT2);
            fi->bytes = blocks * bs;
            CopyLabel(fi->label, sb + 120, 16);
            return;
        }
    }
    if (avail >= 4096 && memcmp(b + 4086, "SWAPSPACE2", 10) == 0) { fi->type = FST_SWAP; return; }
    if (memcmp(b, "hsqs", 4) == 0) { fi->type = FST_SQUASH; fi->bytes = rd64(b + 40); return; }
    if (memcmp(b, "LUKS\xBA\xBE", 6) == 0) { fi->type = FST_LUKS; return; }
    if (rd32(b + 1024) == 0xF2F52010u) { fi->type = FST_F2FS; return; }
    if (memcmp(b, "XFSB", 4) == 0) { fi->type = FST_XFS; return; }
}

/* ------------------------------------------------------------------ UTF */
static int PutUtf8(char *dst, int cap, int *k, uint32_t cp) {
    int need = cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
    if (*k + need >= cap) return 0;
    if (need == 1) dst[(*k)++] = (char)cp;
    else if (need == 2) { dst[(*k)++] = (char)(0xC0 | (cp >> 6)); dst[(*k)++] = (char)(0x80 | (cp & 0x3F)); }
    else if (need == 3) { dst[(*k)++] = (char)(0xE0 | (cp >> 12)); dst[(*k)++] = (char)(0x80 | ((cp >> 6) & 0x3F)); dst[(*k)++] = (char)(0x80 | (cp & 0x3F)); }
    else { dst[(*k)++] = (char)(0xF0 | (cp >> 18)); dst[(*k)++] = (char)(0x80 | ((cp >> 12) & 0x3F)); dst[(*k)++] = (char)(0x80 | ((cp >> 6) & 0x3F)); dst[(*k)++] = (char)(0x80 | (cp & 0x3F)); }
    return 1;
}
static void Utf16ToUtf8(const uint16_t *u, int n, char *dst, int cap) {
    int k = 0;
    for (int i = 0; i < n; i++) {
        uint32_t c = u[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < n && u[i + 1] >= 0xDC00 && u[i + 1] < 0xE000) { c = 0x10000 + ((c - 0xD800) << 10) + (u[i + 1] - 0xDC00); i++; }
        if (!PutUtf8(dst, cap, &k, c)) break;
    }
    dst[k] = 0;
}
static void Latin1ToUtf8(const uint8_t *s, int n, char *dst, int cap) {
    int k = 0;
    for (int i = 0; i < n; i++) if (!PutUtf8(dst, cap, &k, s[i])) break;
    dst[k] = 0;
}

/* ----------------------------------------------------------------- entradas */
#define FS_NAME 400
enum { K_FILE = 0, K_DIR = 1, K_LINK = 2, K_OTHER = 3 };
typedef struct {
    char name[FS_NAME]; /* UTF-8 */
    int kind;
    uint64_t size;
    uint64_t id;  /* FAT/exFAT: primeiro cluster; ext: inode */
    uint32_t aux; /* exFAT: bit0 = sem cadeia FAT (contíguo) */
} FsEntry;

typedef int (*FsSink)(void *ctx, const uint8_t *p, uint32_t n); /* != 0 cancela */

typedef struct {
    Dev2 *d;
    uint64_t base, limit;
    int type;
    /* FAT / exFAT */
    uint32_t bps, spc, clusterBytes, clusters, rootCluster;
    uint64_t fatOff, rootOff, dataOff;
    uint32_t rootBytes;
    /* ext */
    uint32_t bs, ipg, bpg, inodeSize, descSize, firstData, incompat;
    uint64_t blocksCount;
    uint8_t *tmpBlock;
} Fs;

typedef struct { uint8_t *p; size_t n, cap, max; int over; } MemBuf;
static int MemSink(void *ctx, const uint8_t *p, uint32_t n) {
    MemBuf *m = (MemBuf *)ctx;
    if (m->n + n > m->max) { m->over = 1; return 1; }
    if (m->n + n > m->cap) {
        size_t nc = m->cap ? m->cap * 2 : 65536;
        while (nc < m->n + n) nc *= 2;
        uint8_t *q = (uint8_t *)realloc(m->p, nc);
        if (!q) { m->over = 1; return 1; }
        m->p = q; m->cap = nc;
    }
    memcpy(m->p + m->n, p, n);
    m->n += n;
    return 0;
}

#define MAX_DIR_BYTES (96u * 1024 * 1024)
#define MAX_ENTRIES 100000

static int AddEntry(FsEntry **arr, int *n, int *cap, const FsEntry *e) {
    if (*n >= MAX_ENTRIES) return 0;
    if (*n >= *cap) {
        int nc = *cap ? *cap * 2 : 256;
        FsEntry *q = (FsEntry *)realloc(*arr, sizeof(FsEntry) * (size_t)nc);
        if (!q) return 0;
        *arr = q; *cap = nc;
    }
    (*arr)[(*n)++] = *e;
    return 1;
}

/* =================================================================== FAT */
static uint32_t FatNext(Fs *fs, uint32_t c) {
    uint8_t t[4];
    uint32_t v;
    if (fs->type == FST_FAT12) {
        if (!Dev2Read(fs->d, fs->base + fs->fatOff + c + c / 2, t, 2)) return 0xFFFFFFFFu;
        v = rd16(t);
        v = (c & 1) ? (v >> 4) : (v & 0xFFF);
        return v >= 0xFF8 || v < 2 ? 0xFFFFFFFFu : v;
    }
    if (fs->type == FST_FAT16) {
        if (!Dev2Read(fs->d, fs->base + fs->fatOff + (uint64_t)c * 2, t, 2)) return 0xFFFFFFFFu;
        v = rd16(t);
        return v >= 0xFFF8 || v < 2 ? 0xFFFFFFFFu : v;
    }
    if (!Dev2Read(fs->d, fs->base + fs->fatOff + (uint64_t)c * 4, t, 4)) return 0xFFFFFFFFu;
    v = rd32(t) & 0x0FFFFFFFu;
    if (fs->type == FST_EXFAT) v = rd32(t);
    if (fs->type == FST_EXFAT) return v >= 0xFFFFFFF8u || v < 2 ? 0xFFFFFFFFu : v;
    return v >= 0x0FFFFFF8u || v < 2 ? 0xFFFFFFFFu : v;
}

/* percorre uma cadeia de clusters (ou região contígua) enviando bytes ao sink; limit = bytes a enviar (UINT64_MAX = até o fim da cadeia) */
static int ClusterStream(Fs *fs, uint32_t first, int contiguous, uint64_t limit, FsSink sink, void *ctx) {
    uint64_t sent = 0;
    uint32_t c = first, guard = 0;
    uint8_t *tmp = (uint8_t *)malloc(1u << 20);
    if (!tmp) return -1;
    int rc = 0;
    if (first < 2) { free(tmp); return 0; }
    while (c >= 2 && c < fs->clusters + 2 && sent < limit) {
        /* junta clusters consecutivos em uma leitura */
        uint32_t run = 1;
        uint32_t maxRun = (1u << 20) / fs->clusterBytes;
        if (maxRun < 1) maxRun = 1;
        uint32_t nextAfter = 0xFFFFFFFFu;
        if (contiguous) {
            uint64_t remainClusters = (limit == UINT64_MAX) ? maxRun : (limit - sent + fs->clusterBytes - 1) / fs->clusterBytes;
            run = (uint32_t)(remainClusters < maxRun ? remainClusters : maxRun);
            if (c + run > fs->clusters + 2) run = fs->clusters + 2 - c;
        } else {
            uint32_t cur = c;
            for (;;) {
                uint32_t nx = FatNext(fs, cur);
                if (nx == cur + 1 && run < maxRun) { run++; cur = nx; continue; }
                nextAfter = nx;
                break;
            }
        }
        uint64_t bytes = (uint64_t)run * fs->clusterBytes;
        if (limit != UINT64_MAX && bytes > limit - sent) bytes = limit - sent;
        if (!Dev2Read(fs->d, fs->base + fs->dataOff + (uint64_t)(c - 2) * fs->clusterBytes, tmp, bytes)) { rc = -1; break; }
        if (sink(ctx, tmp, (uint32_t)bytes)) { rc = 1; break; }
        sent += bytes;
        if (++guard > fs->clusters + 8) { rc = -1; break; }
        if (contiguous) { c += run; if (c >= fs->clusters + 2) break; }
        else { if (nextAfter == 0xFFFFFFFFu) break; c = nextAfter; }
    }
    free(tmp);
    return rc;
}

static int FatOpen(Fs *fs, const FsInfo *fi) {
    uint8_t b[512];
    (void)fi;
    if (!Dev2Read(fs->d, fs->base, b, 512)) return 0;
    fs->bps = rd16(b + 11); fs->spc = b[13];
    uint32_t rsv = rd16(b + 14), nf = b[16], rootEnt = rd16(b + 17);
    uint32_t tot = rd16(b + 19) ? rd16(b + 19) : rd32(b + 32);
    uint32_t fsz = rd16(b + 22) ? rd16(b + 22) : rd32(b + 36);
    fs->clusterBytes = fs->bps * fs->spc;
    fs->fatOff = (uint64_t)rsv * fs->bps;
    uint32_t rootSec = (rootEnt * 32 + fs->bps - 1) / fs->bps;
    fs->rootOff = ((uint64_t)rsv + (uint64_t)nf * fsz) * fs->bps;
    fs->rootBytes = rootSec * fs->bps;
    fs->dataOff = fs->rootOff + fs->rootBytes;
    fs->clusters = (uint32_t)(((uint64_t)tot * fs->bps - fs->dataOff) / fs->clusterBytes);
    fs->rootCluster = fs->type == FST_FAT32 ? rd32(b + 44) : 0;
    return 1;
}

static uint8_t LfnSum(const uint8_t *n11) {
    uint8_t s = 0;
    for (int i = 0; i < 11; i++) s = (uint8_t)(((s & 1) ? 0x80 : 0) + (s >> 1) + n11[i]);
    return s;
}

static int FatList(Fs *fs, const FsEntry *dir, FsEntry **out, int *nout) {
    MemBuf m = {0, 0, 0, MAX_DIR_BYTES, 0};
    int rc;
    if (dir->id == 0 && fs->type != FST_FAT32) {
        uint8_t *t = (uint8_t *)malloc(fs->rootBytes ? fs->rootBytes : 1);
        if (!t) return -1;
        if (!Dev2Read(fs->d, fs->base + fs->rootOff, t, fs->rootBytes)) { free(t); return -1; }
        MemSink(&m, t, fs->rootBytes);
        free(t);
    } else {
        rc = ClusterStream(fs, (uint32_t)dir->id, 0, UINT64_MAX, MemSink, &m);
        if (rc < 0 && m.n == 0) { free(m.p); return -1; }
    }
    int cap = 0, n = 0;
    FsEntry *arr = NULL;
    uint16_t lfn[20 * 13 + 1];
    int lfnCnt = 0, lfnSeen = 0;
    uint8_t lfnSumV = 0;
    memset(lfn, 0, sizeof lfn);
    for (size_t o = 0; o + 32 <= m.n; o += 32) {
        const uint8_t *e = m.p + o;
        if (e[0] == 0x00) break;
        if (e[0] == 0xE5) { lfnCnt = 0; continue; }
        uint8_t attr = e[11];
        if ((attr & 0x3F) == 0x0F) {
            int ord = e[0] & 0x1F;
            if (e[0] & 0x40) { lfnCnt = ord; lfnSeen = 0; lfnSumV = e[13]; memset(lfn, 0, sizeof lfn); }
            if (ord >= 1 && ord <= 20 && lfnCnt) {
                uint16_t *p = lfn + (ord - 1) * 13;
                for (int i = 0; i < 5; i++) p[i] = rd16(e + 1 + i * 2);
                for (int i = 0; i < 6; i++) p[5 + i] = rd16(e + 14 + i * 2);
                for (int i = 0; i < 2; i++) p[11 + i] = rd16(e + 28 + i * 2);
                lfnSeen++;
            }
            continue;
        }
        if (attr & 0x08) { lfnCnt = 0; continue; } /* rótulo do volume */
        if (e[0] == '.' ) { lfnCnt = 0; continue; }
        FsEntry fe;
        memset(&fe, 0, sizeof fe);
        int useLfn = lfnCnt && lfnSeen == lfnCnt && LfnSum(e) == lfnSumV;
        if (useLfn) {
            int len = 0;
            while (len < lfnCnt * 13 && lfn[len] != 0 && lfn[len] != 0xFFFF) len++;
            Utf16ToUtf8(lfn, len, fe.name, FS_NAME);
        } else {
            uint8_t nm[13];
            int k = 0;
            uint8_t first = e[0] == 0x05 ? 0xE5 : e[0];
            int bl = 0, el = 0;
            for (int i = 0; i < 8; i++) { uint8_t c = i == 0 ? first : e[i]; if (c == ' ') break; nm[k++] = (e[12] & 0x08) && c >= 'A' && c <= 'Z' ? (uint8_t)(c + 32) : c; bl++; }
            int hasExt = 0;
            for (int i = 8; i < 11; i++) if (e[i] != ' ') hasExt = 1;
            if (hasExt) {
                nm[k++] = '.';
                for (int i = 8; i < 11; i++) { uint8_t c = e[i]; if (c == ' ') break; nm[k++] = (e[12] & 0x10) && c >= 'A' && c <= 'Z' ? (uint8_t)(c + 32) : c; el++; }
            }
            (void)bl; (void)el;
            Latin1ToUtf8(nm, k, fe.name, FS_NAME);
        }
        lfnCnt = 0;
        if (fe.name[0] == 0) continue;
        fe.kind = (attr & 0x10) ? K_DIR : K_FILE;
        fe.size = rd32(e + 28);
        fe.id = ((uint32_t)rd16(e + 20) << 16) | rd16(e + 26);
        if (fs->type != FST_FAT32) fe.id &= 0xFFFF;
        if (!AddEntry(&arr, &n, &cap, &fe)) break;
    }
    free(m.p);
    *out = arr; *nout = n;
    return 0;
}

/* ================================================================== exFAT */
static int ExfatOpen(Fs *fs) {
    uint8_t b[512];
    if (!Dev2Read(fs->d, fs->base, b, 512)) return 0;
    uint32_t bpsSh = b[0x6C], spcSh = b[0x6D];
    if (bpsSh < 9 || bpsSh > 12 || spcSh > 25 - bpsSh) return 0;
    fs->bps = 1u << bpsSh; fs->spc = 1u << spcSh;
    fs->clusterBytes = fs->bps << spcSh;
    fs->fatOff = (uint64_t)rd32(b + 0x50) << bpsSh;
    fs->dataOff = (uint64_t)rd32(b + 0x58) << bpsSh;
    fs->clusters = rd32(b + 0x5C);
    fs->rootCluster = rd32(b + 0x60);
    if (fs->clusters == 0 || fs->rootCluster < 2) return 0;
    return 1;
}

static int ExfatList(Fs *fs, const FsEntry *dir, FsEntry **out, int *nout) {
    MemBuf m = {0, 0, 0, MAX_DIR_BYTES, 0};
    int contiguous = (dir->aux & 1) != 0;
    uint64_t lim = contiguous ? dir->size : UINT64_MAX;
    int rc = ClusterStream(fs, (uint32_t)dir->id, contiguous, lim, MemSink, &m);
    if (rc < 0 && m.n == 0) { free(m.p); return -1; }
    int cap = 0, n = 0;
    FsEntry *arr = NULL;
    for (size_t o = 0; o + 32 <= m.n;) {
        const uint8_t *e = m.p + o;
        if (e[0] == 0x00) break;
        if (e[0] != 0x85) { o += 32; continue; }
        int sec = e[1];
        uint16_t attr = rd16(e + 4);
        if (sec < 2 || o + 32 * (size_t)(sec + 1) > m.n || m.p[o + 32] != 0xC0) { o += 32; continue; }
        const uint8_t *st = m.p + o + 32;
        int nameLen = st[3];
        uint16_t nm[256];
        int got = 0;
        for (int i = 2; i <= sec && got < nameLen; i++) {
            const uint8_t *ne = m.p + o + 32 * (size_t)i;
            if (ne[0] != 0xC1) break;
            for (int j = 0; j < 15 && got < nameLen; j++) nm[got++] = rd16(ne + 2 + j * 2);
        }
        FsEntry fe;
        memset(&fe, 0, sizeof fe);
        Utf16ToUtf8(nm, got, fe.name, FS_NAME);
        fe.kind = (attr & 0x10) ? K_DIR : K_FILE;
        fe.size = rd64(st + 24);
        fe.id = rd32(st + 20);
        fe.aux = (st[1] & 2) ? 1 : 0;
        o += 32 * (size_t)(sec + 1);
        if (fe.name[0] == 0) continue;
        if (!AddEntry(&arr, &n, &cap, &fe)) break;
    }
    free(m.p);
    *out = arr; *nout = n;
    return 0;
}

/* ===================================================================== ext */
static int ExtReadBlock(Fs *fs, uint64_t blk, void *buf) { return Dev2Read(fs->d, fs->base + blk * fs->bs, buf, fs->bs); }

static int ExtOpen(Fs *fs) {
    uint8_t sb[1024];
    if (!Dev2Read(fs->d, fs->base + 1024, sb, 1024)) return 0;
    if (rd16(sb + 56) != 0xEF53) return 0;
    uint32_t lg = rd32(sb + 24);
    if (lg > 6) return 0;
    fs->bs = 1024u << lg;
    fs->ipg = rd32(sb + 40); fs->bpg = rd32(sb + 32);
    fs->firstData = rd32(sb + 20);
    fs->incompat = rd32(sb + 96);
    fs->inodeSize = rd32(sb + 76) == 0 ? 128 : rd16(sb + 88);
    fs->descSize = ((fs->incompat & 0x80) && rd16(sb + 254) >= 64) ? rd16(sb + 254) : 32;
    fs->blocksCount = rd32(sb + 4);
    if ((fs->incompat & 0x80) && fs->descSize >= 64) fs->blocksCount |= (uint64_t)rd32(sb + 0x150) << 32;
    if (fs->ipg == 0 || fs->bpg == 0 || fs->inodeSize < 128 || fs->inodeSize > 1024) return 0;
    fs->tmpBlock = (uint8_t *)malloc(fs->bs);
    return fs->tmpBlock != NULL;
}

typedef struct { uint16_t mode; uint64_t size; uint32_t flags; uint8_t iblock[60]; } ExtInode;

static int ExtGetInode(Fs *fs, uint32_t ino, ExtInode *in) {
    if (ino == 0) return 0;
    uint32_t g = (ino - 1) / fs->ipg, idx = (ino - 1) % fs->ipg;
    uint64_t gdBlock = (uint64_t)fs->firstData + 1;
    uint64_t gdOff = gdBlock * fs->bs + (uint64_t)g * fs->descSize;
    uint8_t gd[64];
    memset(gd, 0, sizeof gd);
    if (!Dev2Read(fs->d, fs->base + gdOff, gd, fs->descSize < 64 ? fs->descSize : 64)) return 0;
    uint64_t itab = rd32(gd + 8);
    if (fs->descSize >= 64) itab |= (uint64_t)rd32(gd + 0x28) << 32;
    uint8_t raw[1024];
    if (!Dev2Read(fs->d, fs->base + itab * fs->bs + (uint64_t)idx * fs->inodeSize, raw, fs->inodeSize)) return 0;
    in->mode = rd16(raw);
    in->size = rd32(raw + 4);
    if ((in->mode & 0xF000) == 0x8000) in->size |= (uint64_t)rd32(raw + 108) << 32;
    in->flags = rd32(raw + 32);
    memcpy(in->iblock, raw + 40, 60);
    return 1;
}

/* mapeia o bloco lógico lblk. Retorna 1 = mapeado (phys, run), 0 = buraco (run), -1 = erro */
static int ExtLookup(Fs *fs, const ExtInode *in, uint64_t lblk, uint64_t *phys, uint32_t *run) {
    *run = 1;
    if (in->flags & 0x80000) { /* extents */
        uint8_t *node = (uint8_t *)malloc(fs->bs > 60 ? fs->bs : 60);
        if (!node) return -1;
        memcpy(node, in->iblock, 60);
        int rc = -1;
        for (int depth = 0; depth < 8; depth++) {
            if (rd16(node) != 0xF30A) break;
            uint32_t ne = rd16(node + 2), dep = rd16(node + 6);
            if (ne > (fs->bs - 12) / 12) break;
            if (dep == 0) {
                uint64_t nextStart = UINT64_MAX;
                rc = 0;
                *run = 0x40000000u;
                for (uint32_t i = 0; i < ne; i++) {
                    const uint8_t *x = node + 12 + i * 12;
                    uint64_t eb = rd32(x);
                    uint32_t el = rd16(x + 4);
                    if (el > 32768) el -= 32768;
                    if (lblk >= eb && lblk < eb + el) {
                        *phys = (((uint64_t)rd16(x + 6) << 32) | rd32(x + 8)) + (lblk - eb);
                        *run = (uint32_t)(eb + el - lblk);
                        rc = 1;
                        break;
                    }
                    if (eb > lblk && eb < nextStart) nextStart = eb;
                }
                if (rc == 0 && nextStart != UINT64_MAX) *run = (uint32_t)(nextStart - lblk);
                break;
            }
            /* índice: último com ei_block <= lblk */
            int pick = -1;
            for (uint32_t i = 0; i < ne; i++) {
                const uint8_t *x = node + 12 + i * 12;
                if (rd32(x) <= lblk) pick = (int)i;
            }
            if (pick < 0) { rc = 0; *run = 1; break; }
            const uint8_t *x = node + 12 + (uint32_t)pick * 12;
            uint64_t child = rd32(x + 4) | ((uint64_t)rd16(x + 8) << 32);
            if (!ExtReadBlock(fs, child, node)) break;
        }
        free(node);
        return rc;
    }
    /* mapeamento por blocos indiretos */
    uint32_t per = fs->bs / 4;
    uint32_t lb = (uint32_t)lblk;
    uint64_t tbl;
    uint8_t *tb = fs->tmpBlock;
    uint32_t ptr = 0;
    if (lb < 12) ptr = rd32(in->iblock + lb * 4);
    else {
        lb -= 12;
        int level;
        if (lb < per) level = 1;
        else if (lb - per < (uint64_t)per * per) { lb -= per; level = 2; }
        else { lb -= per; lb -= per * per; level = 3; }
        tbl = rd32(in->iblock + (11 + level) * 4);
        uint64_t idxs[3];
        if (level == 1) { idxs[0] = lb; }
        else if (level == 2) { idxs[0] = lb / per; idxs[1] = lb % per; }
        else { idxs[0] = lb / ((uint64_t)per * per); idxs[1] = (lb / per) % per; idxs[2] = lb % per; }
        for (int i = 0; i < level; i++) {
            if (tbl == 0) { ptr = 0; break; }
            if (!ExtReadBlock(fs, tbl, tb)) return -1;
            ptr = rd32(tb + idxs[i] * 4);
            tbl = ptr;
        }
    }
    if (ptr == 0) return 0;
    *phys = ptr;
    return 1;
}

static int ExtStream(Fs *fs, const ExtInode *in, uint64_t limit, FsSink sink, void *ctx) {
    uint64_t size = in->size < limit ? in->size : limit;
    uint64_t nblocks = (size + fs->bs - 1) / fs->bs, lb = 0, sent = 0;
    uint8_t *tmp = (uint8_t *)malloc(1u << 20);
    static const uint8_t zeros[65536] = {0};
    if (!tmp) return -1;
    int rc = 0;
    while (lb < nblocks) {
        uint64_t phys;
        uint32_t run;
        int r = ExtLookup(fs, in, lb, &phys, &run);
        if (r < 0) { rc = -1; break; }
        if (run == 0) run = 1;
        if (run > nblocks - lb) run = (uint32_t)(nblocks - lb);
        if (r == 1) {
            /* ext2/3: tenta juntar blocos contíguos */
            if (!(in->flags & 0x80000) && run == 1) {
                while (run < (1u << 20) / fs->bs) {
                    uint64_t p2; uint32_t r2;
                    if (lb + run >= nblocks) break;
                    if (ExtLookup(fs, in, lb + run, &p2, &r2) != 1 || p2 != phys + run) break;
                    run++;
                }
            }
            uint32_t maxRun = (1u << 20) / fs->bs;
            if (maxRun < 1) maxRun = 1;
            while (run) {
                uint32_t part = run < maxRun ? run : maxRun;
                uint64_t bytes = (uint64_t)part * fs->bs;
                if (bytes > size - sent) bytes = size - sent;
                if (!Dev2Read(fs->d, fs->base + phys * fs->bs, tmp, bytes)) { rc = -1; break; }
                if (sink(ctx, tmp, (uint32_t)bytes)) { rc = 1; break; }
                sent += bytes; phys += part; lb += part; run -= part;
            }
            if (rc) break;
        } else {
            uint64_t bytes = (uint64_t)run * fs->bs;
            if (bytes > size - sent) bytes = size - sent;
            uint64_t left = bytes;
            while (left) {
                uint32_t part = left > sizeof zeros ? (uint32_t)sizeof zeros : (uint32_t)left;
                if (sink(ctx, zeros, part)) { rc = 1; break; }
                left -= part;
            }
            if (rc) break;
            sent += bytes; lb += run;
        }
    }
    free(tmp);
    return rc;
}

static int ExtList(Fs *fs, const FsEntry *dir, FsEntry **out, int *nout) {
    ExtInode di;
    if (!ExtGetInode(fs, (uint32_t)dir->id, &di)) return -1;
    if ((di.mode & 0xF000) != 0x4000) return -1;
    if (di.flags & 0x10000000) return -1; /* inline data: não suportado */
    MemBuf m = {0, 0, 0, MAX_DIR_BYTES, 0};
    int rc = ExtStream(fs, &di, MAX_DIR_BYTES, MemSink, &m);
    if (rc < 0 && m.n == 0) { free(m.p); return -1; }
    int cap = 0, n = 0;
    FsEntry *arr = NULL;
    size_t o = 0;
    while (o + 8 <= m.n) {
        uint32_t ino = rd32(m.p + o);
        uint32_t rl = rd16(m.p + o + 4);
        uint32_t nl = m.p[o + 6];
        if (rl < 8 || (rl & 3) || o + rl > m.n) {
            /* registro inválido: avança até o próximo bloco */
            o = (o / fs->bs + 1) * fs->bs;
            continue;
        }
        if (ino != 0 && nl > 0 && nl + 8 <= rl) {
            FsEntry fe;
            memset(&fe, 0, sizeof fe);
            int skip = (nl == 1 && m.p[o + 8] == '.') || (nl == 2 && m.p[o + 8] == '.' && m.p[o + 9] == '.');
            if (!skip) {
                /* nomes ext são bytes (normalmente UTF-8) */
                int len = nl < FS_NAME - 1 ? (int)nl : FS_NAME - 1;
                memcpy(fe.name, m.p + o + 8, (size_t)len);
                fe.name[len] = 0;
                fe.id = ino;
                ExtInode ii;
                if (ExtGetInode(fs, ino, &ii)) {
                    uint16_t t = ii.mode & 0xF000;
                    fe.kind = t == 0x4000 ? K_DIR : t == 0x8000 ? K_FILE : t == 0xA000 ? K_LINK : K_OTHER;
                    fe.size = ii.size;
                }
                if (!AddEntry(&arr, &n, &cap, &fe)) break;
            }
        }
        o += rl;
    }
    free(m.p);
    *out = arr; *nout = n;
    return 0;
}

/* ============================================================ API pública */
static int FsOpen(Fs *fs, Dev2 *d, uint64_t base) {
    FsInfo fi;
    memset(fs, 0, sizeof *fs);
    fs->d = d; fs->base = base;
    FsDetect(d, base, &fi);
    fs->type = fi.type;
    switch (fi.type) {
    case FST_FAT12: case FST_FAT16: case FST_FAT32: return FatOpen(fs, &fi) ? fi.type : 0;
    case FST_EXFAT: return ExfatOpen(fs) ? fi.type : 0;
    case FST_EXT2: case FST_EXT3: case FST_EXT4: return ExtOpen(fs) ? fi.type : 0;
    }
    return 0;
}
static void FsClose(Fs *fs) { if (fs->tmpBlock) free(fs->tmpBlock); fs->tmpBlock = NULL; }

static void FsRoot(Fs *fs, FsEntry *root) {
    memset(root, 0, sizeof *root);
    root->kind = K_DIR;
    strcpy(root->name, "/");
    if (fs->type == FST_FAT32 || fs->type == FST_EXFAT) root->id = fs->rootCluster;
    else if (fs->type == FST_FAT12 || fs->type == FST_FAT16) root->id = 0;
    else root->id = 2;
}

/* lista o conteúdo de uma pasta. *out deve ser liberado com free(). 0 = ok */
static int FsList(Fs *fs, const FsEntry *dir, FsEntry **out, int *n) {
    *out = NULL; *n = 0;
    switch (fs->type) {
    case FST_FAT12: case FST_FAT16: case FST_FAT32: return FatList(fs, dir, out, n);
    case FST_EXFAT: return ExfatList(fs, dir, out, n);
    case FST_EXT2: case FST_EXT3: case FST_EXT4: return ExtList(fs, dir, out, n);
    }
    return -1;
}

/* envia o conteúdo de um arquivo ao sink. 0 = ok, 1 = cancelado, <0 = erro */
static int FsStream(Fs *fs, const FsEntry *e, FsSink sink, void *ctx) {
    switch (fs->type) {
    case FST_FAT12: case FST_FAT16: case FST_FAT32:
        if (e->size == 0) return 0;
        return ClusterStream(fs, (uint32_t)e->id, 0, e->size, sink, ctx);
    case FST_EXFAT:
        if (e->size == 0) return 0;
        return ClusterStream(fs, (uint32_t)e->id, (e->aux & 1) != 0, e->size, sink, ctx);
    case FST_EXT2: case FST_EXT3: case FST_EXT4: {
        ExtInode in;
        if (!ExtGetInode(fs, (uint32_t)e->id, &in)) return -1;
        if ((in.mode & 0xF000) != 0x8000) return -1;
        if (in.flags & 0x10000000) return -1;
        return ExtStream(fs, &in, UINT64_MAX, sink, ctx);
    }
    }
    return -1;
}

#endif
