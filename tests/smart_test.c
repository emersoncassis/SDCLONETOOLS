/* smart_test.c - confere smart.h e vhd.h em Linux (usado por tests/run_smart.py).
 *   smart_test <origem.img|.vhd> <destino.img|.vhd> <tamanho_destino_bytes|same>
 */
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include "../smart.h"
#include "../vhd.h"

static int sfd, dfd;
static VhdR vr; static int srcVhd;
static VhdW vw; static int dstVhd;
static int fR(void *c, uint64_t off, void *buf, uint32_t len) { return pread(*(int *)c, buf, len, (off_t)off) == (ssize_t)len; }
static int fW(void *c, uint64_t off, const void *buf, uint32_t len) { return pwrite(*(int *)c, buf, len, (off_t)off) == (ssize_t)len; }
static int srcRd(void *c, uint64_t off, void *buf, uint32_t len) { (void)c; if (off % 512 || len % 512) { fprintf(stderr, "desalinhado\n"); exit(9); } return srcVhd ? VhdRRead(&vr, off, buf, len) : fR(&sfd, off, buf, len); }
static int dstWr(void *c, uint64_t off, const void *buf, uint32_t len) { (void)c; return dstVhd ? VhdWWrite(&vw, off, buf, len) : fW(&dfd, off, buf, len); }
static int ends(const char *s, const char *e) { size_t a = strlen(s), b = strlen(e); return a >= b && strcmp(s + a - b, e) == 0; }

int main(int argc, char **argv) {
    if (argc < 4) return 2;
    sfd = open(argv[1], O_RDONLY);
    struct stat st; fstat(sfd, &st);
    uint64_t ssize = (uint64_t)st.st_size;
    srcVhd = ends(argv[1], ".vhd");
    if (srcVhd) { if (!VhdROpen(&vr, fR, &sfd, ssize)) { printf("VHD inválido\n"); return 1; } ssize = vr.size; }
    uint64_t dsize = strcmp(argv[3], "same") == 0 ? ssize : strtoull(argv[3], 0, 10);
    Dev2 d; Dev2Init(&d, srcRd, NULL, ssize, 512);
    SmPlan p;
    if (!SmBuildPlan(&d, dsize, &p, NULL, NULL)) { printf("PLANO RECUSADO: %s\n", p.err); return 3; }
    printf("tabela=%d partições=%d copiar=%llu MB de %llu MB, reduzida=%d, mínimo=%llu MB, patches=%d\n", p.table, p.nparts,
           (unsigned long long)(p.copyBytes >> 20), (unsigned long long)(ssize >> 20), p.shrunk, (unsigned long long)(p.needBytes >> 20), p.npatch);
    for (int i = 0; i < p.nparts; i++) printf("  p%d início=%llu MB tam=%llu MB novo=%llu MB fs=%d usado=%llu MB fimUsado=%llu MB modo=%d\n", i + 1,
        (unsigned long long)(p.part[i].start >> 20), (unsigned long long)(p.part[i].size >> 20), (unsigned long long)(p.part[i].newSize >> 20), p.part[i].fs,
        (unsigned long long)(p.part[i].used >> 20), (unsigned long long)(p.part[i].usedEnd >> 20), p.part[i].how);
    dstVhd = ends(argv[2], ".vhd");
    dfd = open(argv[2], O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (dstVhd) VhdWOpen(&vw, dsize, fW, fR, &dfd, 1);
    else if (ftruncate(dfd, (off_t)dsize)) return 4;
    uint8_t *buf = aligned_alloc(4096, SM_CHUNK);
    uint64_t bad = 0;
    int rc = SmRun(&p, srcRd, NULL, dstWr, NULL, buf, NULL, NULL, &bad);
    if (dstVhd && !VhdWClose(&vw)) rc = -5;
    printf("run=%d bad=%llu\n", rc, (unsigned long long)bad);
    if (dstVhd) { fstat(dfd, &st); printf("arquivo VHD: %llu MB\n", (unsigned long long)(st.st_size >> 20)); }
    free(buf); SmPlanFree(&p); Dev2Free(&d);
    return rc == 1 ? 0 : 5;
}
