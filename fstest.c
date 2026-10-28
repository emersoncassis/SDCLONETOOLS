/* fstest.c - ferramenta de teste (Linux) para fsread.h e diag.h.
 *   fstest <imagem> diag [sec]            -> relatório do diagnóstico
 *   fstest <imagem> walk <inicio> [sec]   -> lista recursiva "D caminho" / "F caminho tamanho crc32"
 *   fstest <imagem> rebuild [sec]         -> mostra o MBR reconstruído (hex) a partir do diagnóstico
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include "diag.h"

static int fd;
static uint32_t SEC = 512;

static int rawrd(void *c, uint64_t off, void *buf, uint32_t len) {
    (void)c;
    if (off % SEC || len % SEC) { fprintf(stderr, "LEITURA DESALINHADA off=%llu len=%u\n", (unsigned long long)off, len); exit(9); }
    return pread(fd, buf, len, (off_t)off) == (ssize_t)len;
}

static uint32_t crc_run(uint32_t c, const uint8_t *p, size_t n) {
    Crc32((const uint8_t *)"", 0);
    for (size_t i = 0; i < n; i++) c = crc_tab[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c;
}
typedef struct { uint32_t c; uint64_t n; } CrcCtx;
static int crc_sink(void *ctx, const uint8_t *p, uint32_t n) {
    CrcCtx *k = (CrcCtx *)ctx;
    k->c = crc_run(k->c, p, n);
    k->n += n;
    return 0;
}

static int errors = 0;
static void walk(Fs *fs, const FsEntry *dir, const char *path, int depth) {
    FsEntry *arr;
    int n;
    if (depth > 20) return;
    if (FsList(fs, dir, &arr, &n) != 0) { printf("ERR list %s\n", path); errors++; return; }
    for (int i = 0; i < n; i++) {
        char p[2048];
        snprintf(p, sizeof p, "%s/%s", path, arr[i].name);
        if (arr[i].kind == K_DIR) {
            printf("D %s\n", p);
            walk(fs, &arr[i], p, depth + 1);
        } else if (arr[i].kind == K_FILE) {
            CrcCtx k = {0xFFFFFFFFu, 0};
            int rc = FsStream(fs, &arr[i], crc_sink, &k);
            if (rc != 0 || k.n != arr[i].size) { printf("ERR stream %s rc=%d got=%llu want=%llu\n", p, rc, (unsigned long long)k.n, (unsigned long long)arr[i].size); errors++; }
            else printf("F %s %llu %08x\n", p, (unsigned long long)arr[i].size, k.c ^ 0xFFFFFFFFu);
        } else printf("O %s\n", p);
    }
    free(arr);
}

static void printParts(const Diag *g) {
    printf("T table=%d mbrsig=%d entries=%d gpthdr=%d gptcrc=%d gptbak=%d disk=%llu sec=%u covered=%llu samplefail=%d\n", g->tableType, g->mbrSigOk,
           g->mbrEntries, g->gptHeaderOk, g->gptCrcOk, g->gptBackupOk, (unsigned long long)g->diskBytes, g->sec, (unsigned long long)g->coveredBytes, g->sampleFail);
    for (int i = 0; i < g->nparts; i++) {
        const Part *p = &g->parts[i];
        printf("P src=%d start=%llu size=%llu mbrtype=%02x fs=%d fsbytes=%llu flags=%d label=[%s]\n", p->src, (unsigned long long)p->start,
               (unsigned long long)p->size, p->mbrType, p->fs, (unsigned long long)p->fsBytes, p->flags, p->label);
    }
    for (int i = 0; i < g->nfound; i++) {
        const Part *p = &g->found[i];
        printf("L start=%llu size=%llu fs=%d fsbytes=%llu flags=%d label=[%s]\n", (unsigned long long)p->start, (unsigned long long)p->size, p->fs,
               (unsigned long long)p->fsBytes, p->flags, p->label);
    }
    printf("V %x\n", g->verdict);
}

int main(int argc, char **argv) {
    if (argc < 3) return 2;
    fd = open(argv[1], O_RDONLY);
    if (fd < 0) { perror("open"); return 2; }
    struct stat st;
    fstat(fd, &st);
    const char *cmd = argv[2];
    int si = strcmp(cmd, "walk") == 0 ? 4 : 3;
    if (argc > si) SEC = (uint32_t)atoi(argv[si]);
    Dev2 d;
    if (!Dev2Init(&d, rawrd, NULL, (uint64_t)st.st_size, SEC)) return 3;
    if (strcmp(cmd, "diag") == 0) {
        Diag *g = (Diag *)malloc(sizeof(Diag));
        DiagAnalyze(&d, g, 1, NULL, NULL);
        printParts(g);
        free(g);
    } else if (strcmp(cmd, "walk") == 0) {
        uint64_t base = strtoull(argv[3], NULL, 10);
        Fs fs;
        int t = FsOpen(&fs, &d, base);
        if (!t) { printf("ERR open\n"); return 1; }
        printf("FS %d\n", t);
        FsEntry root;
        FsRoot(&fs, &root);
        walk(&fs, &root, "", 0);
        FsClose(&fs);
        printf("ERRORS %d\n", errors);
    } else if (strcmp(cmd, "rebuild") == 0) {
        Diag *g = (Diag *)malloc(sizeof(Diag));
        DiagAnalyze(&d, g, 1, NULL, NULL);
        Part list[8];
        int dropped;
        int n = BuildRecoveryList(g, list, 8, &dropped);
        uint8_t s0[4096], old[4096];
        Dev2Read(&d, 0, old, SEC);
        printf("N %d dropped=%d\n", n, dropped);
        if (n >= 0 && BuildMbr(s0, SEC, old, list, n) == 0) {
            for (int i = 0; i < n; i++) printf("R start=%llu size=%llu type=%02x\n", (unsigned long long)list[i].start, (unsigned long long)list[i].size, MbrTypeFor(&list[i]));
            FILE *f = fopen("rebuilt_s0.bin", "wb");
            fwrite(s0, 1, SEC, f);
            fclose(f);
        }
        free(g);
    }
    Dev2Free(&d);
    return 0;
}
