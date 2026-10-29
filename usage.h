/* usage.h - o que tem dentro do cartão: ocupação, tipos de arquivo e perfil de uso.
 * Só LÊ (usa fsread.h). Independente de Windows: o app e os testes em Linux usam o mesmo código. */
#ifndef USAGE_H
#define USAGE_H

#include "fsread.h"

enum { CT_GAME = 0, CT_SYS, CT_MUSIC, CT_VIDEO, CT_PHOTO, CT_DOC, CT_APP, CT_OTHER, CT_N };
enum { UP_EMPTY = 0, UP_CONSOLE, UP_SYSTEM, UP_GAMES, UP_CAMERA, UP_VIDEO, UP_MUSIC, UP_DOCS, UP_MIXED };

#define US_MAXEXT 96
#define US_MAXFILES 600000u
#define US_MAXDEPTH 24

typedef struct { char ext[12]; uint32_t files; uint64_t bytes; int cat; } ExtStat;

typedef struct {
    uint64_t catBytes[CT_N];
    uint32_t catFiles[CT_N];
    uint64_t usedBytes;
    uint32_t files, dirs;
    ExtStat ext[US_MAXEXT];
    int next;
    int hasDcim, hasRoms, hasEmu, hasLinux, hasMusicDir;
    int partial;            /* parou no limite ou foi cancelado */
    int scannedParts;       /* partições lidas */
    int skippedParts;       /* partições que o app não sabe abrir (NTFS, criptografadas...) */
    uint64_t scannedBytes;  /* tamanho das partições lidas */
    char topDirs[8][40];    /* pastas da raiz */
    int ntop;
} UsageStat;

/* retorna != 0 para cancelar */
typedef int (*UsageTick)(void *ctx, const UsageStat *u);

static const char *US_GAME_EXT =
    " nes fds unf unif sfc smc fig swc gb gbc gba agb sgb nds dsi 3ds cia n64 z64 v64 ndd gen md smd sms gg sg 32x pce sgx tg16 cue ccd chd cso pbp mdf ngp ngc ws wsc "
    " a26 a52 a78 lnx jag col int vec vb vboy mgw o2 min pico p8 gdi cdi m3u mame fba neo ";
static const char *US_SYS_EXT =
    " so elf sh cfg ini conf srm sav state st0 st1 st2 st3 sta dtb dts ko img-bak bak db sqlite lua py pyc dll sys log pak dat map xml json yml yaml bios rom-bak ";
static const char *US_MUSIC_EXT = " mp3 flac wav ogg oga m4a aac wma opus ape aiff aif mid midi mka ";
static const char *US_VIDEO_EXT = " mp4 mkv avi mov mpg mpeg wmv m4v ts mts m2ts webm 3gp flv vob ";
static const char *US_PHOTO_EXT = " jpg jpeg png gif bmp heic heif webp tif tiff raw arw cr2 cr3 nef dng orf rw2 raf srw ";
static const char *US_DOC_EXT = " txt pdf doc docx xls xlsx ppt pptx odt ods odp csv md rtf htm html epub mobi ";
static const char *US_APP_EXT = " exe msi apk zip 7z rar tar gz xz bz2 deb iso img bin ";
static const char *US_AMBIG_GAME_EXT = " zip 7z rar iso img bin rom ";

static int UsInList(const char *list, const char *ext) {
    char t[16];
    size_t n = strlen(ext);
    if (n == 0 || n > 10) return 0;
    t[0] = ' ';
    memcpy(t + 1, ext, n);
    t[n + 1] = ' ';
    t[n + 2] = 0;
    return strstr(list, t) != NULL;
}

static void UsLower(char *dst, const char *src, size_t cap) {
    size_t i = 0;
    for (; src[i] && i + 1 < cap; i++) dst[i] = (src[i] >= 'A' && src[i] <= 'Z') ? (char)(src[i] + 32) : src[i];
    dst[i] = 0;
}

static int UsDirIn(const char *lname, const char *set) { return UsInList(set, lname); }

static const char *US_ROM_DIRS =
    " roms rom games jogos psx ps1 psone psp snes nes gba gbc gb n64 nds genesis megadrive mastersystem saturn dreamcast arcade mame fbneo fba neogeo pcengine atari2600 atari7800 gamegear sega3do ports sfc fds msx scummvm ";
static const char *US_EMU_DIRS =
    " retroarch emulationstation .emulationstation es-de bios cores saves savestates states themes retropie batocera recalbox arkos jelos muos emuelec ppsspp drastic easyroms emulators ";
static const char *US_CAM_DIRS = " dcim avchd 100canon 100nikon 101msdcf 100msdcf 100andro camera ";
static const char *US_LINUX_DIRS = " etc usr var bin sbin lib lib64 boot dev proc sys opt ";

static int UsClassify(const char *ext, int inRoms, int inEmu, int ext4) {
    int c;
    if (UsInList(US_GAME_EXT, ext)) return CT_GAME;
    if (UsInList(US_MUSIC_EXT, ext)) return CT_MUSIC;
    if (UsInList(US_VIDEO_EXT, ext)) return CT_VIDEO;
    if (UsInList(US_PHOTO_EXT, ext)) return CT_PHOTO;
    if (inRoms && UsInList(US_AMBIG_GAME_EXT, ext)) return CT_GAME;
    if (UsInList(US_SYS_EXT, ext)) return (UsInList(US_DOC_EXT, ext) ? CT_DOC : CT_SYS);
    if (UsInList(US_DOC_EXT, ext)) return CT_DOC;
    if (UsInList(US_APP_EXT, ext)) return CT_APP;
    c = CT_OTHER;
    if (inRoms) c = CT_GAME;
    else if (inEmu || ext4) c = CT_SYS;
    return c;
}

static void UsAdd(UsageStat *u, const char *ext, int cat, uint64_t size) {
    u->catBytes[cat] += size;
    u->catFiles[cat]++;
    u->usedBytes += size;
    u->files++;
    if (!ext[0]) return;
    for (int i = 0; i < u->next; i++)
        if (strcmp(u->ext[i].ext, ext) == 0) {
            u->ext[i].files++;
            u->ext[i].bytes += size;
            return;
        }
    if (u->next < US_MAXEXT) {
        ExtStat *e = &u->ext[u->next++];
        memset(e, 0, sizeof *e);
        strncpy(e->ext, ext, sizeof e->ext - 1);
        e->files = 1;
        e->bytes = size;
        e->cat = cat;
    }
}

typedef struct { UsageTick tick; void *ctx; unsigned counter; int stop; } UsWalk;

static void UsWalkDir(Fs *fs, const FsEntry *dir, UsageStat *u, int depth, int inRoms, int inEmu, int ext4, UsWalk *w) {
    FsEntry *arr = NULL;
    int n = 0;
    if (w->stop || depth > US_MAXDEPTH) return;
    if (FsList(fs, dir, &arr, &n) != 0 || !arr) return;
    for (int i = 0; i < n && !w->stop; i++) {
        const FsEntry *e = &arr[i];
        if ((++w->counter & 255u) == 0 && w->tick && w->tick(w->ctx, u)) { w->stop = 1; u->partial = 1; break; }
        if (u->files >= US_MAXFILES) { w->stop = 1; u->partial = 1; break; }
        if (e->kind == K_DIR) {
            char ln[64];
            UsLower(ln, e->name, sizeof ln);
            u->dirs++;
            int r = inRoms, m = inEmu;
            if (UsDirIn(ln, US_ROM_DIRS)) { r = 1; u->hasRoms = 1; }
            if (UsDirIn(ln, US_EMU_DIRS)) { m = 1; u->hasEmu = 1; }
            if (UsDirIn(ln, US_CAM_DIRS)) u->hasDcim = 1;
            if (strcmp(ln, "music") == 0 || strcmp(ln, "musicas") == 0 || strcmp(ln, "playlists") == 0) u->hasMusicDir = 1;
            if (depth == 0 && u->ntop < 8) {
                strncpy(u->topDirs[u->ntop], e->name, 39);
                u->topDirs[u->ntop][39] = 0;
                u->ntop++;
            }
            if (ext4 && depth == 0 && UsDirIn(ln, US_LINUX_DIRS)) u->hasLinux = 1;
            UsWalkDir(fs, e, u, depth + 1, r, m, ext4, w);
        } else if (e->kind == K_FILE) {
            char ln[FS_NAME], ext[12] = "";
            UsLower(ln, e->name, sizeof ln);
            const char *dot = strrchr(ln, '.');
            if (dot && dot != ln && strlen(dot + 1) < 11) strcpy(ext, dot + 1);
            UsAdd(u, ext, UsClassify(ext, inRoms, inEmu, ext4), e->size);
        }
    }
    free(arr);
}

/* percorre uma partição (somente leitura). Retorna 1 se abriu o sistema de arquivos. */
static int UsageScanPart(Dev2 *d, uint64_t start, uint64_t size, UsageStat *u, UsageTick tick, void *ctx) {
    Fs fs;
    FsEntry root;
    UsWalk w = {tick, ctx, 0, 0};
    int t = FsOpen(&fs, d, start);
    if (!t) { u->skippedParts++; return 0; }
    FsRoot(&fs, &root);
    int ext4 = (t == FST_EXT2 || t == FST_EXT3 || t == FST_EXT4);
    UsWalkDir(&fs, &root, u, 0, 0, 0, ext4, &w);
    FsClose(&fs);
    u->scannedParts++;
    u->scannedBytes += size;
    return 1;
}

static int UsExtCmp(const void *a, const void *b) {
    const ExtStat *x = (const ExtStat *)a, *y = (const ExtStat *)b;
    if (x->bytes != y->bytes) return x->bytes < y->bytes ? 1 : -1;
    return (int)y->files - (int)x->files;
}
static void UsageSortExt(UsageStat *u) { qsort(u->ext, (size_t)u->next, sizeof(ExtStat), UsExtCmp); }

/* perfil de uso pelo conteúdo */
static int UsageProfile(const UsageStat *u) {
    double t = (double)u->usedBytes;
    if (u->files < 5 || t < 4.0 * 1024 * 1024) return u->hasLinux ? UP_SYSTEM : UP_EMPTY;
    double g = (double)u->catBytes[CT_GAME] / t, s = (double)u->catBytes[CT_SYS] / t, m = (double)u->catBytes[CT_MUSIC] / t;
    double v = (double)u->catBytes[CT_VIDEO] / t, p = (double)u->catBytes[CT_PHOTO] / t, dc = (double)u->catBytes[CT_DOC] / t;
    if (u->hasLinux && (u->hasRoms || u->hasEmu || g > 0.05)) return UP_CONSOLE;
    if (u->hasLinux) return UP_SYSTEM;
    if (u->hasRoms || u->hasEmu || g + s >= 0.5) return (g + s >= 0.3 || u->hasRoms) ? UP_GAMES : UP_MIXED;
    if (u->hasDcim || p + v >= 0.6) return (v >= 0.5 && !u->hasDcim) ? UP_VIDEO : UP_CAMERA;
    if (m >= 0.4) return UP_MUSIC;
    if (v >= 0.5) return UP_VIDEO;
    if (dc >= 0.4) return UP_DOCS;
    return UP_MIXED;
}

#endif
