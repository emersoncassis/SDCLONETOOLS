/* SDCLONETOOLS - copiar, gravar, verificar, autenticar e recuperar cartões SD.
 * Win32 puro (sem dependências). Requer administrador (acesso direto ao disco). */
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <winioctl.h>
#include <setupapi.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <dbt.h>
#include <stdio.h>
#include <stdarg.h>
#include <wchar.h>
#include <math.h>
#include "core.h"
#include "diag.h"
#include "usage.h"
#include "bench.h"
#include "smart.h"
#include "vhd.h"

#define APP_TITLE L"SDCLONETOOLS"
#define CHUNK (4u * 1024 * 1024)
#define MIN64(a, b) ((a) < (b) ? (a) : (b))

#define WM_APP_LOG (WM_APP + 1)
#define WM_APP_DONE (WM_APP + 2)

enum { OP_C2I = 0, OP_I2C, OP_C2C, OP_VERIFY, OP_RECOVER, OP_AUTH, OP_DIAG, OP_CSD, OP_BENCH, OP_COUNT };

enum {
    ID_OP0 = 100, /* 100..107 */
    ID_DESC = 120, ID_LBL1, ID_COMBO1, ID_REFRESH, ID_SHOWALL, ID_LBL2, ID_COMBO2, ID_COMBOB, ID_COMBOC,
    ID_LBLFILE, ID_EDITFILE, ID_BROWSE, ID_CHK1, ID_CHK2, ID_RADA, ID_RADB,
    ID_START, ID_CANCELBTN, ID_PROGRESS, ID_STATUS, ID_LOG, ID_HEADER, ID_TITLE, ID_WARN, ID_RESULT, ID_NEXT, ID_LOGBTN, ID_COMBOP, ID_DETECT, ID_OPTS, ID_EJECT, ID_STEPS,
    ID_BR_PART = 300, ID_BR_LBL, ID_BR_PATH, ID_BR_LIST, ID_BR_INFO, ID_BR_UP, ID_BR_COPYSEL, ID_BR_COPYALL, ID_BR_CANCEL, ID_BR_CLOSE
};

/* ------------------------------------------------------------------ dados */
typedef struct {
    int index;
    uint64_t size;
    uint32_t sec;
    int bus;
    BOOL removable, likelyCard;
    wchar_t vendor[64], product[64], letters[64], label[320];
} DiskInfo;

typedef struct {
    int op, mode, src, dst;
    uint64_t srcSize, dstSize;
    uint32_t srcSec, dstSec;
    wchar_t file[MAX_PATH], img[MAX_PATH];
    BOOL verify;
    int brand, cls;
    DiskInfo di;
} Job;

typedef struct { HANDLE h; BOOL raw; uint32_t sec; } Dev;
typedef struct { HANDLE h[26]; int n; } VolSet;
typedef struct { uint64_t good, badBytes, firstBad; double wMBs, rMBs; } CapRes;
typedef struct { int mid; wchar_t oid[8], pnm[24], rev[16]; } SdId;

static HINSTANCE g_inst;
static HWND g_hwnd;
static HFONT g_font;
static int g_dpi = 96;
static int g_op = 0;
static BOOL g_busy = FALSE, g_marq = FALSE;
static HANDLE g_thread = NULL;
static volatile LONG g_cancel = 0;
static volatile LONGLONG g_done = 0, g_total = 0;
static volatile LONG g_indet = 0;
static ULONGLONG g_t0 = 0;
static wchar_t g_phase[200];
static CRITICAL_SECTION g_cs;
static wchar_t g_summary[2400];
static UINT g_summaryIcon = MB_ICONINFORMATION;
static DiskInfo g_disks[32];
static int g_ndisks = 0, g_sysDisk = -1;
static Job g_job;
static uint64_t g_bad = 0;
static int g_badLogged = 0;

#define S(v) MulDiv((v), g_dpi, 96)

static const struct { const wchar_t *name; int mids[4]; int strong; } BRANDS[] = {
    {L"(não informar)", {-1, -1, -1, -1}, 0},
    {L"SanDisk", {0x03, 0x45, -1, -1}, 1},
    {L"Samsung", {0x1B, -1, -1, -1}, 1},
    {L"Kingston", {0x41, 0x27, -1, -1}, 0},
    {L"Lexar", {0x28, -1, -1, -1}, 0},
    {L"Sony", {0x82, -1, -1, -1}, 0},
    {L"Transcend", {0x74, -1, -1, -1}, 0},
    {L"ADATA", {0x1D, -1, -1, -1}, 0},
    {L"Silicon Power", {0x31, -1, -1, -1}, 0},
    {L"Toshiba / Kioxia", {0x02, -1, -1, -1}, 0},
    {L"Outra marca / sem marca", {-1, -1, -1, -1}, 0},
};
#define NBRANDS ((int)(sizeof(BRANDS) / sizeof(BRANDS[0])))

static const struct { const wchar_t *name; int mbs; } CLASSES[] = {
    {L"(não informar)", 0}, {L"Class 4 (4 MB/s)", 4}, {L"Class 10 / U1 (10 MB/s)", 10},
    {L"U3 / V30 (30 MB/s)", 30}, {L"V60 (60 MB/s)", 60}, {L"V90 (90 MB/s)", 90},
};
#define NCLASSES ((int)(sizeof(CLASSES) / sizeof(CLASSES[0])))

/* ----------------------------------------------------------------- utilitários */
static HWND Ctl(int id) { return GetDlgItem(g_hwnd, id); }

/* ================== TEMA: segue o modo claro/escuro do Windows (estilo Windows 11) ================== */
typedef struct {
    COLORREF bg, panel, edge, input, txt, dim, accent, accentHot, accentDown, onAccent,
        navSel, navHot, btn, btnHot, btnDown, btnEdge, disFill, disTxt, chkEdge,
        danger, warnBg, warnEdge, ok, okBg, caution, cautionBg, log, logTxt, empty;
} Pal;
static Pal g_pal;
static BOOL g_dark;
#define CL_BG     g_pal.bg
#define CL_PANEL  g_pal.panel
#define CL_EDGE   g_pal.edge
#define CL_INPUT  g_pal.input
#define CL_TXT    g_pal.txt
#define CL_DIM    g_pal.dim
#define CL_NEON   g_pal.accent
#define CL_DANGER g_pal.danger
#define CL_LOG    g_pal.log
#define CL_LOGTXT g_pal.logTxt
#define CL_NONE   0xFFFFFFFFu

static void LoadPalette(void) {
    static const Pal L = {
        RGB(243, 243, 243), RGB(251, 251, 251), RGB(229, 229, 229), RGB(255, 255, 255), RGB(27, 27, 27), RGB(96, 96, 96),
        RGB(0, 95, 184), RGB(25, 110, 191), RGB(50, 125, 198), RGB(255, 255, 255),
        RGB(233, 233, 233), RGB(237, 237, 237), RGB(253, 253, 253), RGB(246, 246, 246), RGB(240, 240, 240), RGB(213, 213, 213),
        RGB(197, 197, 197), RGB(255, 255, 255), RGB(138, 138, 138),
        RGB(196, 43, 28), RGB(253, 231, 233), RGB(240, 205, 208), RGB(15, 123, 15), RGB(223, 246, 221),
        RGB(157, 93, 0), RGB(255, 244, 206), RGB(255, 255, 255), RGB(52, 52, 52), RGB(214, 214, 214)};
    static const Pal D = {
        RGB(32, 32, 32), RGB(43, 43, 43), RGB(29, 29, 29), RGB(36, 36, 36), RGB(255, 255, 255), RGB(176, 176, 176),
        RGB(96, 205, 255), RGB(88, 186, 232), RGB(80, 166, 207), RGB(0, 0, 0),
        RGB(45, 45, 45), RGB(41, 41, 41), RGB(45, 45, 45), RGB(50, 50, 50), RGB(39, 39, 39), RGB(60, 60, 60),
        RGB(67, 67, 67), RGB(160, 160, 160), RGB(160, 160, 160),
        RGB(255, 153, 164), RGB(68, 39, 38), RGB(92, 52, 50), RGB(108, 203, 95), RGB(57, 61, 27),
        RGB(252, 225, 0), RGB(67, 53, 25), RGB(28, 28, 28), RGB(214, 214, 214), RGB(75, 75, 75)};
    DWORD v = 1, sz = sizeof v;
    g_dark = (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                           L"AppsUseLightTheme", RRF_RT_REG_DWORD, NULL, &v, &sz) == ERROR_SUCCESS && v == 0);
    g_pal = g_dark ? D : L;
}

/* geometria (pixels lógicos; S() escala) */
#define WIN_W 1000
#define WIN_H 688
#define NAV_W 240
#define MX 256
#define MW 720
#define DEV_Y 72
#define DEV_H 76
#define TASK_Y 164
#define CARD_PAD 16
#define NAV_Y0 52
#define NAV_SEP_Y 296
#define NAV_Y1 308

/* resultado do diagnóstico (preenchido pela thread de trabalho, lido pela interface ao terminar) */
enum { NX_NONE = 0, NX_BACKUP, NX_CARVE, NX_REBUILD, NX_RESTORE, NX_AUTH, NX_BENCH };
enum { MC_DATA = 0, MC_EMPTY, MC_SLOW, MC_BAD, MC_SKIP };
#define MAP_N 128
#define IS_VIEW(op) ((op) == OP_DIAG || (op) == OP_CSD || (op) == OP_BENCH) /* páginas que terminam em veredito */
typedef struct {
    BOOL valid, map;
    int op, level, next; /* level: 0 ok, 1 atenção, 2 grave */
    wchar_t title[160], cause[400], card[300], data[300], todo[300];
    uint8_t cell[MAP_N];
    int nData, nEmpty, nSlow, nBad, nSkip;
    /* conteúdo do cartão (aba "Conteúdo" do Diagnóstico) */
    int hRead, hWrite, hPart; /* saúde resumida: 0 ok, 1 atenção, 2 falha, 3 não testado */
    int uFitLevel;            /* 0 serve, 1 ressalvas, 2 não serve, 3 meça o desempenho */
    BOOL usage;
    UsageStat us;
    uint64_t uCard;
    int uProfile;
    wchar_t uTitle[160], uOcc[300], uTop[400], uUse[420], uFit[520];
    /* desempenho */
    BOOL bench;
    BenchRes br;
    BenchJudge bj;
    float curveR[64], curveW[64];
    int nCurveR, nCurveW;
} Verdict;
static Verdict g_vd;
static char g_demo[16] = ""; /* modo demonstração (--demo=...) */
static BOOL IsHero(void);
static int NextFor(void);
static BOOL g_showLog = FALSE;
static const wchar_t *NX_TEXT[] = {L"", L"Próximo: Fazer backup", L"Próximo: Recuperar arquivos", L"Próximo: Reparar partições", L"Próximo: Restaurar", L"Próximo: Autenticidade", L"Próximo: Desempenho"};

static HBRUSH g_brBg, g_brInput, g_brLog, g_brCard;
static HFONT g_fontB, g_fontMono, g_fontTitle, g_fontSmall, g_fontH, g_fontVal, g_fontValB;
static HFONT g_fontIcon, g_fontIconM, g_fontIconL; /* NULL se o Windows não tiver fonte de ícones */
static int g_hover = 0;
static BOOL g_fileOn = TRUE;
static BOOL g_chk[512];

static BOOL Checked(int id) { return g_chk[id]; }
static void SetChk(int id, BOOL v) {
    g_chk[id] = v;
    if (g_hwnd && Ctl(id)) InvalidateRect(Ctl(id), NULL, FALSE);
}

typedef HRESULT(WINAPI *PFN_SWT)(HWND, LPCWSTR, LPCWSTR);
static void SetTheme(HWND h, const wchar_t *a, const wchar_t *b) {
    static PFN_SWT f = NULL;
    static int tried = 0;
    if (!tried) {
        tried = 1;
        HMODULE m = LoadLibraryW(L"uxtheme.dll");
        if (m) f = (PFN_SWT)GetProcAddress(m, "SetWindowTheme");
    }
    if (f) f(h, a, b);
}

static void DarkTitlebar(HWND h) {
    typedef HRESULT(WINAPI * F)(HWND, DWORD, LPCVOID, DWORD);
    HMODULE m = LoadLibraryW(L"dwmapi.dll");
    if (!m) return;
    F f = (F)GetProcAddress(m, "DwmSetWindowAttribute");
    if (!f) return;
    BOOL on = g_dark;
    if (FAILED(f(h, 20, &on, sizeof on))) f(h, 19, &on, sizeof on);
    COLORREF c = CL_BG;
    f(h, 35, &c, sizeof c); /* barra de título na mesma cor da janela */
    c = CL_TXT;
    f(h, 36, &c, sizeof c);
}

/* ---------------------------------------------------------------- GDI+ (cantos arredondados suaves) */
typedef struct { UINT32 ver; void *cb; BOOL noThread, noCodecs; } GpStartIn;
static HMODULE g_gpDll;
static ULONG_PTR g_gpTok;
static BOOL g_gpOk;
static int(WINAPI *pGpStartup)(ULONG_PTR *, const GpStartIn *, void *);
static void(WINAPI *pGpShutdown)(ULONG_PTR);
static int(WINAPI *pGpFromHdc)(HDC, void **);
static int(WINAPI *pGpDelG)(void *);
static int(WINAPI *pGpSmooth)(void *, int);
static int(WINAPI *pGpNewPath)(int, void **);
static int(WINAPI *pGpDelPath)(void *);
static int(WINAPI *pGpArc)(void *, float, float, float, float, float, float);
static int(WINAPI *pGpClose)(void *);
static int(WINAPI *pGpNewBrush)(UINT32, void **);
static int(WINAPI *pGpDelBrush)(void *);
static int(WINAPI *pGpNewPen)(UINT32, float, int, void **);
static int(WINAPI *pGpDelPen)(void *);
static int(WINAPI *pGpFillPath)(void *, void *, void *);
static int(WINAPI *pGpDrawPath)(void *, void *, void *);
static int(WINAPI *pGpDrawLine)(void *, void *, float, float, float, float);
static int(WINAPI *pGpFillEll)(void *, void *, float, float, float, float);
/* opcionais (gráfico de rosca, giro da animação, ícones vetoriais) */
static int(WINAPI *pGpFillPie)(void *, void *, float, float, float, float, float, float);
static int(WINAPI *pGpDrawArc)(void *, void *, float, float, float, float, float, float);
static int(WINAPI *pGpPoly)(void *, const float *, int);

#define ARGB(c) (0xFF000000u | ((UINT32)GetRValue(c) << 16) | ((UINT32)GetGValue(c) << 8) | (UINT32)GetBValue(c))
#define GPF(v, n) (*(FARPROC *)&(v) = GetProcAddress(g_gpDll, n))

static void GpInit(void) {
    g_gpDll = LoadLibraryW(L"gdiplus.dll");
    if (!g_gpDll) return;
    GPF(pGpStartup, "GdiplusStartup"); GPF(pGpShutdown, "GdiplusShutdown");
    GPF(pGpFromHdc, "GdipCreateFromHDC"); GPF(pGpDelG, "GdipDeleteGraphics"); GPF(pGpSmooth, "GdipSetSmoothingMode");
    GPF(pGpNewPath, "GdipCreatePath"); GPF(pGpDelPath, "GdipDeletePath"); GPF(pGpArc, "GdipAddPathArc");
    GPF(pGpClose, "GdipClosePathFigure");
    GPF(pGpNewBrush, "GdipCreateSolidFill"); GPF(pGpDelBrush, "GdipDeleteBrush");
    GPF(pGpNewPen, "GdipCreatePen1"); GPF(pGpDelPen, "GdipDeletePen");
    GPF(pGpFillPath, "GdipFillPath"); GPF(pGpDrawPath, "GdipDrawPath"); GPF(pGpDrawLine, "GdipDrawLine");
    GPF(pGpFillEll, "GdipFillEllipse");
    GPF(pGpFillPie, "GdipFillPie"); GPF(pGpDrawArc, "GdipDrawArc"); GPF(pGpPoly, "GdipAddPathPolygon");
    if (!pGpStartup || !pGpShutdown || !pGpFromHdc || !pGpDelG || !pGpSmooth || !pGpNewPath || !pGpDelPath || !pGpArc ||
        !pGpClose || !pGpNewBrush || !pGpDelBrush || !pGpNewPen || !pGpDelPen || !pGpFillPath || !pGpDrawPath ||
        !pGpDrawLine || !pGpFillEll)
        return;
    GpStartIn in = {1, NULL, FALSE, FALSE};
    if (pGpStartup(&g_gpTok, &in, NULL) == 0) g_gpOk = TRUE;
}

static void GpDone(void) {
    if (g_gpOk) pGpShutdown(g_gpTok);
    g_gpOk = FALSE;
}

static void Fill(HDC dc, RECT r, COLORREF c) {
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, &r, b);
    DeleteObject(b);
}
static void Frame(HDC dc, RECT r, COLORREF c) {
    HBRUSH b = CreateSolidBrush(c);
    FrameRect(dc, &r, b);
    DeleteObject(b);
}
static void Txt(HDC dc, const wchar_t *t, RECT *r, UINT fl, COLORREF c, HFONT f) {
    HGDIOBJ o = SelectObject(dc, f);
    SetTextColor(dc, c);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, t, -1, r, fl);
    SelectObject(dc, o);
}
static void Glyph(HDC dc, wchar_t g, RECT *r, COLORREF c, HFONT f) {
    if (!f) return;
    wchar_t s[2] = {g, 0};
    Txt(dc, s, r, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX, c, f);
}

/* retângulo arredondado com preenchimento e borda opcionais (CL_NONE = sem) */
static void RRect(HDC dc, RECT r, int rad, COLORREF fill, COLORREF border) {
    if (g_gpOk) {
        void *g = NULL, *p = NULL, *br = NULL, *pn = NULL;
        if (pGpFromHdc(dc, &g) != 0) return;
        pGpSmooth(g, 4);
        float x = (float)r.left + 0.5f, y = (float)r.top + 0.5f, w = (float)(r.right - r.left) - 1.0f, h = (float)(r.bottom - r.top) - 1.0f;
        float d = (float)rad * 2.0f;
        if (d > w) d = w;
        if (d > h) d = h;
        if (d < 1.0f) d = 1.0f;
        pGpNewPath(0, &p);
        pGpArc(p, x, y, d, d, 180, 90);
        pGpArc(p, x + w - d, y, d, d, 270, 90);
        pGpArc(p, x + w - d, y + h - d, d, d, 0, 90);
        pGpArc(p, x, y + h - d, d, d, 90, 90);
        pGpClose(p);
        if (fill != CL_NONE) {
            pGpNewBrush(ARGB(fill), &br);
            pGpFillPath(g, br, p);
            pGpDelBrush(br);
        }
        if (border != CL_NONE) {
            pGpNewPen(ARGB(border), 1.0f, 2, &pn);
            pGpDrawPath(g, pn, p);
            pGpDelPen(pn);
        }
        pGpDelPath(p);
        pGpDelG(g);
    } else {
        HBRUSH b = fill != CL_NONE ? CreateSolidBrush(fill) : (HBRUSH)GetStockObject(NULL_BRUSH);
        HPEN pn = border != CL_NONE ? CreatePen(PS_SOLID, 1, border) : (HPEN)GetStockObject(NULL_PEN);
        HGDIOBJ o1 = SelectObject(dc, b), o2 = SelectObject(dc, pn);
        RoundRect(dc, r.left, r.top, r.right, r.bottom, rad * 2, rad * 2);
        SelectObject(dc, o1);
        SelectObject(dc, o2);
        if (fill != CL_NONE) DeleteObject(b);
        if (border != CL_NONE) DeleteObject(pn);
    }
}

static void Disc(HDC dc, int cx, int cy, int rad, COLORREF c) {
    if (g_gpOk) {
        void *g = NULL, *br = NULL;
        if (pGpFromHdc(dc, &g) != 0) return;
        pGpSmooth(g, 4);
        pGpNewBrush(ARGB(c), &br);
        pGpFillEll(g, br, (float)(cx - rad), (float)(cy - rad), (float)(rad * 2), (float)(rad * 2));
        pGpDelBrush(br);
        pGpDelG(g);
    } else {
        HBRUSH b = CreateSolidBrush(c);
        HPEN pn = CreatePen(PS_SOLID, 1, c);
        HGDIOBJ o1 = SelectObject(dc, b), o2 = SelectObject(dc, pn);
        Ellipse(dc, cx - rad, cy - rad, cx + rad, cy + rad);
        SelectObject(dc, o1);
        SelectObject(dc, o2);
        DeleteObject(b);
        DeleteObject(pn);
    }
}

static void Tick(HDC dc, float x1, float y1, float x2, float y2, float x3, float y3, float wpx, COLORREF c) {
    if (g_gpOk) {
        void *g = NULL, *pn = NULL;
        if (pGpFromHdc(dc, &g) != 0) return;
        pGpSmooth(g, 4);
        pGpNewPen(ARGB(c), wpx, 2, &pn);
        pGpDrawLine(g, pn, x1, y1, x2, y2);
        pGpDrawLine(g, pn, x2, y2, x3, y3);
        pGpDelPen(pn);
        pGpDelG(g);
    } else {
        HPEN pn = CreatePen(PS_SOLID, (int)wpx, c);
        HGDIOBJ o = SelectObject(dc, pn);
        MoveToEx(dc, (int)x1, (int)y1, NULL);
        LineTo(dc, (int)x2, (int)y2);
        LineTo(dc, (int)x3, (int)y3);
        SelectObject(dc, o);
        DeleteObject(pn);
    }
}


/* ---------------------------------------------- desenho vetorial (ícones, rosca, animação) */
#define PI_F 3.14159265f
/* fatia de pizza; ângulos em graus, sentido horário a partir das 3 horas (como no GDI+) */
static void PieSeg(HDC dc, float cx, float cy, float r, float a0, float sw, COLORREF c) {
    if (sw <= 0.05f) return;
    if (g_gpOk && pGpFillPie) {
        void *g = NULL, *br = NULL;
        if (pGpFromHdc(dc, &g) != 0) return;
        pGpSmooth(g, 4);
        pGpNewBrush(ARGB(c), &br);
        pGpFillPie(g, br, cx - r, cy - r, 2 * r, 2 * r, a0, sw);
        pGpDelBrush(br);
        pGpDelG(g);
        return;
    }
    POINT pts[100];
    int n = 0;
    pts[n].x = (LONG)cx; pts[n].y = (LONG)cy; n++;
    int steps = (int)(sw / 4.0f) + 2;
    if (steps > 96) steps = 96;
    for (int i = 0; i <= steps; i++) {
        float a = (a0 + sw * i / steps) * PI_F / 180.0f;
        pts[n].x = (LONG)(cx + r * cosf(a)); pts[n].y = (LONG)(cy + r * sinf(a)); n++;
    }
    HBRUSH b = CreateSolidBrush(c);
    HPEN pn = CreatePen(PS_SOLID, 1, c);
    HGDIOBJ o1 = SelectObject(dc, b), o2 = SelectObject(dc, pn);
    Polygon(dc, pts, n);
    SelectObject(dc, o1); SelectObject(dc, o2);
    DeleteObject(b); DeleteObject(pn);
}

static void ArcLine(HDC dc, float cx, float cy, float r, float a0, float sw, float wpx, COLORREF c) {
    if (g_gpOk && pGpDrawArc) {
        void *g = NULL, *pn = NULL;
        if (pGpFromHdc(dc, &g) != 0) return;
        pGpSmooth(g, 4);
        pGpNewPen(ARGB(c), wpx, 2, &pn);
        pGpDrawArc(g, pn, cx - r, cy - r, 2 * r, 2 * r, a0, sw);
        pGpDelPen(pn);
        pGpDelG(g);
        return;
    }
    HPEN pn = CreatePen(PS_SOLID, (int)wpx, c);
    HGDIOBJ o = SelectObject(dc, pn);
    int steps = (int)(sw / 6.0f) + 2;
    for (int i = 0; i <= steps; i++) {
        float a = (a0 + sw * i / steps) * PI_F / 180.0f;
        int x = (int)(cx + r * cosf(a)), y = (int)(cy + r * sinf(a));
        if (i == 0) MoveToEx(dc, x, y, NULL); else LineTo(dc, x, y);
    }
    SelectObject(dc, o);
    DeleteObject(pn);
}

/* polígono preenchido; pts = x0,y0,x1,y1,... */
static void PolyFill(HDC dc, const float *pts, int n, COLORREF fill) {
    if (g_gpOk && pGpPoly) {
        void *g = NULL, *p = NULL, *br = NULL;
        if (pGpFromHdc(dc, &g) != 0) return;
        pGpSmooth(g, 4);
        pGpNewPath(0, &p);
        pGpPoly(p, pts, n);
        pGpNewBrush(ARGB(fill), &br);
        pGpFillPath(g, br, p);
        pGpDelBrush(br);
        pGpDelPath(p);
        pGpDelG(g);
        return;
    }
    POINT q[64];
    if (n > 64) n = 64;
    for (int i = 0; i < n; i++) { q[i].x = (LONG)pts[2 * i]; q[i].y = (LONG)pts[2 * i + 1]; }
    HBRUSH b = CreateSolidBrush(fill);
    HPEN pn = CreatePen(PS_SOLID, 1, fill);
    HGDIOBJ o1 = SelectObject(dc, b), o2 = SelectObject(dc, pn);
    Polygon(dc, q, n);
    SelectObject(dc, o1); SelectObject(dc, o2);
    DeleteObject(b); DeleteObject(pn);
}

static void LineSeg(HDC dc, float x1, float y1, float x2, float y2, float wpx, COLORREF c) { Tick(dc, x1, y1, x2, y2, x2, y2, wpx, c); }

/* escudo: 0 = saudável (visto), 1 = atenção (!), 2 = defeito (X) */
static void ShieldIcon(HDC dc, float cx, float top, float w, float h, int level, COLORREF c) {
    float pts[2 * 40];
    int n = 0;
    float hw = w / 2;
#define PT(x, y) (pts[2 * n] = (x), pts[2 * n + 1] = (y), n++)
    PT(cx, top);
    PT(cx + hw, top + h * 0.16f);
    PT(cx + hw, top + h * 0.50f);
    for (int i = 1; i <= 12; i++) { /* curva até a ponta de baixo */
        float t = i / 12.0f, u = 1 - t;
        float x = u * u * (cx + hw) + 2 * u * t * (cx + hw * 0.92f) + t * t * cx;
        float y = u * u * (top + h * 0.50f) + 2 * u * t * (top + h * 0.86f) + t * t * (top + h);
        PT(x, y);
    }
    for (int i = 1; i <= 12; i++) {
        float t = i / 12.0f, u = 1 - t;
        float x = u * u * cx + 2 * u * t * (cx - hw * 0.92f) + t * t * (cx - hw);
        float y = u * u * (top + h) + 2 * u * t * (top + h * 0.86f) + t * t * (top + h * 0.50f);
        PT(x, y);
    }
    PT(cx - hw, top + h * 0.16f);
#undef PT
    PolyFill(dc, pts, n, c);
    float s = w / 64.0f, my = top + h * 0.47f;
    COLORREF wc = RGB(255, 255, 255);
    if (level == 0) Tick(dc, cx - 13 * s, my, cx - 3 * s, my + 10 * s, cx + 15 * s, my - 10 * s, 5.5f * s, wc);
    else if (level == 1) {
        LineSeg(dc, cx, my - 15 * s, cx, my + 4 * s, 6 * s, wc);
        Disc(dc, (int)cx, (int)(my + 14 * s), (int)(3.6f * s + 0.5f), wc);
    } else {
        LineSeg(dc, cx - 11 * s, my - 11 * s, cx + 11 * s, my + 11 * s, 5.5f * s, wc);
        LineSeg(dc, cx + 11 * s, my - 11 * s, cx - 11 * s, my + 11 * s, 5.5f * s, wc);
    }
}

/* cartão SD (silhueta com canto cortado e contatos) */
static void SdCardIcon(HDC dc, float l, float t, float w, float h, COLORREF body, COLORREF contact) {
    float cut = w * 0.30f;
    float pts[] = {l, t, l + w - cut, t, l + w, t + cut, l + w, t + h, l, t + h};
    PolyFill(dc, pts, 5, body);
    float cw = w * 0.09f, gap = w * 0.045f, cy = t + h * 0.07f, ch = h * 0.20f;
    for (int i = 0; i < 6; i++) {
        float x = l + w * 0.10f + i * (cw + gap);
        if (x + cw > l + w - cut * 0.55f && i > 3) break;
        RECT r = {(LONG)x, (LONG)cy, (LONG)(x + cw), (LONG)(cy + ch)};
        RRect(dc, r, (int)(cw / 3), contact, CL_NONE);
    }
}

/* círculo de status pequeno: 0 ok (visto), 1 atenção (!), 2 erro (X), 3 não testado (traço) */
static void StatusDot(HDC dc, int cx, int cy, int r, int st) {
    COLORREF c = st == 0 ? g_pal.ok : st == 1 ? g_pal.caution : st == 2 ? CL_DANGER : g_pal.btnEdge;
    Disc(dc, cx, cy, r, c);
    float s = (float)r / 10.0f;
    COLORREF wc = RGB(255, 255, 255);
    if (st == 0) Tick(dc, cx - 4.5f * s, (float)cy, cx - 1.0f * s, cy + 3.5f * s, cx + 5.0f * s, cy - 3.5f * s, 2.0f * s, wc);
    else if (st == 1) { LineSeg(dc, (float)cx, cy - 5 * s, (float)cx, cy + 1 * s, 2.2f * s, wc); Disc(dc, cx, (int)(cy + 4.5f * s), (int)(1.4f * s + 0.5f), wc); }
    else if (st == 2) { LineSeg(dc, cx - 4 * s, cy - 4 * s, cx + 4 * s, cy + 4 * s, 2.0f * s, wc); LineSeg(dc, cx + 4 * s, cy - 4 * s, cx - 4 * s, cy + 4 * s, 2.0f * s, wc); }
    else LineSeg(dc, cx - 4 * s, (float)cy, cx + 4 * s, (float)cy, 2.0f * s, wc);
}

/* indicador girando */
static void Spinner(HDC dc, int cx, int cy, int r, float wpx) {
    float a = (float)((GetTickCount64() / 3) % 360);
    ArcLine(dc, (float)cx, (float)cy, (float)r, 0, 360, wpx, g_pal.btnEdge);
    ArcLine(dc, (float)cx, (float)cy, (float)r, a, 100, wpx, CL_NEON);
}

/* ---------------------------------------------------------------- textos */
/* indexados por operação: C2I, I2C, C2C, VERIFY, RECOVER, AUTH, DIAG */
static const wchar_t *NAV_T[] = {L"Fazer backup", L"Gravar imagem", L"Clonar cartão", L"Verificar setores", L"Recuperar e reparar", L"Autenticidade", L"Diagnóstico", L"Proteção do cartão", L"Desempenho"};
static const wchar_t NAV_G[] = {0xE74E, 0xE896, 0xE8C8, 0xE721, 0xE90F, 0xEA18, 0xE9D9, 0xE72E, 0xE916};
static const int NAV_ORDER[] = {OP_DIAG, OP_BENCH, OP_CSD, OP_RECOVER, OP_AUTH, OP_VERIFY, OP_C2I, OP_I2C, OP_C2C};
static const wchar_t *START_T[] = {L"Criar backup", L"Gravar no cartão", L"Clonar cartão", L"Iniciar verificação", L"Iniciar", L"Analisar cartão", L"Analisar cartão", L"Ler proteção", L"Medir desempenho"};
static const wchar_t *RECOVER_START[] = {L"Analisar partições", L"Abrir arquivos", L"Reconstruir partições", L"Procurar arquivos", L"Restaurar cartão"};

static int NavY(int slot) { return slot < 6 ? NAV_Y0 + slot * 40 : NAV_Y1 + (slot - 6) * 40; }

static COLORREF BtnBg(int id) {
    if (id >= ID_OP0 && id < ID_OP0 + OP_COUNT) return CL_BG;
    if ((id == ID_START || id == ID_OPTS) && IsHero()) return CL_PANEL;
    if (id == ID_CHK1 || id == ID_CHK2 || id == ID_RADA || id == ID_RADB || id == ID_SHOWALL || id == ID_REFRESH || id == ID_BROWSE || id == ID_DETECT) return CL_PANEL;
    return CL_BG;
}

static void DrawButton(DRAWITEMSTRUCT *d) {
    int id = (int)d->CtlID, w = d->rcItem.right - d->rcItem.left, h = d->rcItem.bottom - d->rcItem.top;
    HDC dc = CreateCompatibleDC(d->hDC);
    HBITMAP bm = CreateCompatibleBitmap(d->hDC, w, h);
    HGDIOBJ ob = SelectObject(dc, bm);
    RECT r = {0, 0, w, h};
    BOOL dis = (d->itemState & ODS_DISABLED) != 0, sel = (d->itemState & ODS_SELECTED) != 0;
    BOOL hot = (g_hover == id) && !dis, foc = (d->itemState & ODS_FOCUS) != 0 && !dis;
    wchar_t t[200];
    GetWindowTextW(d->hwndItem, t, 200);
    Fill(dc, r, BtnBg(id));
    if (id >= ID_OP0 && id < ID_OP0 + OP_COUNT) {
        /* item do menu de navegação */
        int op = id - ID_OP0;
        BOOL on = (op == g_op);
        if (on || hot || sel) RRect(dc, r, S(4), (on || sel) ? g_pal.navSel : g_pal.navHot, CL_NONE);
        else if (foc) RRect(dc, r, S(4), CL_NONE, g_pal.btnEdge);
        if (on) {
            RECT bar = {0, h / 2 - S(8), S(3), h / 2 + S(8)};
            RRect(dc, bar, S(1), CL_NEON, CL_NONE);
        }
        COLORREF tc = dis && !on ? CL_DIM : CL_TXT;
        RECT ir = {S(12), 0, S(36), h};
        Glyph(dc, NAV_G[op], &ir, tc, g_fontIcon);
        RECT tr = {g_fontIcon ? S(46) : S(16), 0, w - S(8), h};
        Txt(dc, NAV_T[op], &tr, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS, tc, g_font);
    } else if (id == ID_EJECT) {
        /* item fixo no pé do menu */
        if (hot || sel) RRect(dc, r, S(4), g_pal.navHot, CL_NONE);
        else if (foc) RRect(dc, r, S(4), CL_NONE, g_pal.btnEdge);
        COLORREF tc = dis ? CL_DIM : CL_TXT;
        RECT ir = {S(12), 0, S(36), h};
        Glyph(dc, 0xE88E, &ir, tc, g_fontIcon);
        RECT tr = {g_fontIcon ? S(46) : S(16), 0, w - S(8), h};
        Txt(dc, t, &tr, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS, tc, g_font);
    } else if (id == ID_LOGBTN || id == ID_OPTS) {
        /* botão discreto, estilo link */
        if (hot || sel) RRect(dc, r, S(4), g_pal.navHot, CL_NONE);
        Txt(dc, t, &r, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX, dis ? CL_DIM : CL_NEON, g_font);
    } else if (id == ID_REFRESH && g_fontIcon) {
        RRect(dc, r, S(4), sel ? g_pal.btnDown : (hot ? g_pal.btnHot : g_pal.btn), g_pal.btnEdge);
        Glyph(dc, 0xE72C, &r, dis ? CL_DIM : CL_TXT, g_fontIcon);
    } else if (id == ID_START || id == ID_CANCELBTN || id == ID_REFRESH || id == ID_BROWSE || id == ID_NEXT || id == ID_DETECT || (id >= ID_BR_UP && id <= ID_BR_CLOSE)) {
        COLORREF fill, border, tc;
        BOOL prim = id == ID_NEXT || (id == ID_START && !(IS_VIEW(g_op) && g_vd.valid && g_vd.op == g_op && NextFor() != NX_NONE));
        if (prim) {
            fill = dis ? g_pal.disFill : (sel ? g_pal.accentDown : (hot ? g_pal.accentHot : CL_NEON));
            border = CL_NONE;
            tc = dis ? g_pal.disTxt : g_pal.onAccent;
        } else {
            fill = sel ? g_pal.btnDown : (hot ? g_pal.btnHot : g_pal.btn);
            border = g_pal.btnEdge;
            tc = dis ? (g_dark ? RGB(120, 120, 120) : RGB(160, 160, 160)) : CL_TXT;
        }
        RRect(dc, r, S(4), fill, border);
        if (foc && !prim) {
            RECT fr = r;
            InflateRect(&fr, -S(2), -S(2));
            RRect(dc, fr, S(3), CL_NONE, g_pal.chkEdge);
        }
        Txt(dc, t, &r, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX, tc, (id == ID_START && IsHero()) ? g_fontH : (prim ? g_fontB : g_font));
    } else {
        /* caixa de seleção */
        BOOL radio = (id == ID_RADA || id == ID_RADB), on = Checked(id);
        int bs = S(20), y0 = (h - bs) / 2;
        RECT bx = {0, y0, bs, y0 + bs};
        if (radio) {
            Disc(dc, bs / 2, y0 + bs / 2, bs / 2, on ? CL_NEON : g_pal.chkEdge);
            Disc(dc, bs / 2, y0 + bs / 2, on ? S(4) : bs / 2 - 1, on ? g_pal.onAccent : CL_INPUT);
        } else {
            if (on) {
                RRect(dc, bx, S(4), dis ? g_pal.disFill : (hot ? g_pal.accentHot : CL_NEON), CL_NONE);
                float k = (float)g_dpi / 96.0f;
                Tick(dc, 5.0f * k, (float)y0 + 10.0f * k, 8.5f * k, (float)y0 + 13.5f * k, 15.0f * k, (float)y0 + 6.5f * k, 1.5f * k, g_pal.onAccent);
            } else
                RRect(dc, bx, S(4), hot ? g_pal.btnHot : CL_INPUT, foc ? CL_TXT : g_pal.chkEdge);
        }
        RECT tr = {bs + S(10), 0, w, h};
        Txt(dc, t, &tr, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS | DT_NOPREFIX, dis ? CL_DIM : CL_TXT, g_font);
    }
    RECT sp2 = {S(20), S(WIN_H - 62), w - S(20), S(WIN_H - 62) + 1};
    Fill(dc, sp2, g_pal.btnEdge);
    BitBlt(d->hDC, d->rcItem.left, d->rcItem.top, w, h, dc, 0, 0, SRCCOPY);
    SelectObject(dc, ob);
    DeleteObject(bm);
    DeleteDC(dc);
}

static void DrawCombo(DRAWITEMSTRUCT *d) {
    RECT rc = d->rcItem;
    BOOL dis = (d->itemState & ODS_DISABLED) != 0;
    BOOL edit = (d->itemState & ODS_COMBOBOXEDIT) != 0;
    BOOL hi = (d->itemState & ODS_SELECTED) != 0 && !edit;
    Fill(d->hDC, rc, hi ? g_pal.navSel : CL_INPUT);
    if ((int)d->itemID < 0) return;
    wchar_t t[400];
    t[0] = 0;
    if (SendMessageW(d->hwndItem, CB_GETLBTEXT, d->itemID, (LPARAM)t) == CB_ERR) return;
    if (hi) {
        RECT b = {rc.left + S(2), rc.top + (rc.bottom - rc.top) / 2 - S(7), rc.left + S(5), rc.top + (rc.bottom - rc.top) / 2 + S(7)};
        RRect(d->hDC, b, S(1), CL_NEON, CL_NONE);
    }
    RECT tr = rc;
    tr.left += S(10);
    tr.right -= S(4);
    Txt(d->hDC, t, &tr, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX, dis ? CL_DIM : CL_TXT, g_font);
}

/* fundo do menu de navegação: nome do app e separador entre os grupos */
static void DrawSidebar(DRAWITEMSTRUCT *d) {
    int w = d->rcItem.right - d->rcItem.left, h = d->rcItem.bottom - d->rcItem.top;
    HDC dc = CreateCompatibleDC(d->hDC);
    HBITMAP bm = CreateCompatibleBitmap(d->hDC, w, h);
    HGDIOBJ ob = SelectObject(dc, bm);
    RECT r = {0, 0, w, h};
    Fill(dc, r, CL_BG);
    RECT gi = {S(16), S(10), S(36), S(34)};
    Glyph(dc, 0xE7F1, &gi, CL_NEON, g_fontIcon);
    RECT tt = {g_fontIcon ? S(46) : S(16), S(10), w - S(8), S(34)};
    Txt(dc, L"SD Clone Tools", &tt, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX, CL_TXT, g_fontSmall);
    RECT sp = {S(20), S(NAV_SEP_Y), w - S(20), S(NAV_SEP_Y) + 1};
    Fill(dc, sp, g_pal.btnEdge);
    BitBlt(d->hDC, d->rcItem.left, d->rcItem.top, w, h, dc, 0, 0, SRCCOPY);
    SelectObject(dc, ob);
    DeleteObject(bm);
    DeleteDC(dc);
}

/* faixa de aviso (InfoBar) quando a tarefa apaga dados */
static void DrawWarn(DRAWITEMSTRUCT *d) {
    int w = d->rcItem.right - d->rcItem.left, h = d->rcItem.bottom - d->rcItem.top;
    HDC dc = CreateCompatibleDC(d->hDC);
    HBITMAP bm = CreateCompatibleBitmap(d->hDC, w, h);
    HGDIOBJ ob = SelectObject(dc, bm);
    RECT r = {0, 0, w, h};
    wchar_t t[300];
    GetWindowTextW(d->hwndItem, t, 300);
    Fill(dc, r, CL_PANEL);
    RRect(dc, r, S(4), g_pal.warnBg, g_pal.warnEdge);
    RECT gi = {S(12), 0, S(32), h};
    if (g_fontIcon) Glyph(dc, 0xEA39, &gi, CL_DANGER, g_fontIcon);
    else Disc(dc, S(20), h / 2, S(5), CL_DANGER);
    RECT tr = {S(42), 0, w - S(12), h};
    Txt(dc, t, &tr, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS, CL_TXT, g_font);
    BitBlt(d->hDC, d->rcItem.left, d->rcItem.top, w, h, dc, 0, 0, SRCCOPY);
    SelectObject(dc, ob);
    DeleteObject(bm);
    DeleteDC(dc);
}

static COLORREF MapColor(int c) {
    switch (c) {
    case MC_DATA: return CL_NEON;
    case MC_EMPTY: return g_pal.empty;
    case MC_SLOW: return g_pal.caution;
    case MC_BAD: return CL_DANGER;
    default: return g_pal.btnEdge;
    }
}

/* painel com o veredito do diagnóstico */
static void FmtSize(uint64_t b, wchar_t *out, size_t n);
static DiskInfo *FindDisk(int n);
static int SelectedDisk(int id);

/* jornada: Diagnóstico -> Autenticidade -> Desempenho (por cartão) */
static const int JOURNEY_OP[3] = {OP_DIAG, OP_AUTH, OP_BENCH};
static int g_jdisk = -1;
static BOOL g_jdone[3];
static BOOL g_diagForm = FALSE;          /* Diagnóstico mostrando o formulário de opções */
static volatile LONG g_step = 0;         /* etapa do diagnóstico em andamento (animação) */
static volatile LONG g_mapDone = 0;      /* pontos do mapa já lidos */
static BOOL JourneyDone(int i) { return g_jdisk >= 0 && g_jdisk == SelectedDisk(ID_COMBO1) && g_jdone[i]; }

/* tela inicial do Diagnóstico: cartão escolhido, ainda sem resultado */
static BOOL IsHero(void) {
    return g_op == OP_DIAG && !g_busy && !g_diagForm && !g_showLog && !(g_vd.valid && g_vd.op == OP_DIAG);
}

static int NextFor(void) {
    if (g_busy) return NX_NONE;
    if (IS_VIEW(g_op) && g_vd.valid && g_vd.op == g_op) {
        if (g_op == OP_BENCH) return NX_NONE;
        return g_vd.next;
    }
    if (g_op == OP_AUTH && JourneyDone(1)) return NX_BENCH;
    return NX_NONE;
}

static COLORREF CatColor(int c) {
    switch (c) {
    case CT_GAME: return g_pal.accent;
    case CT_SYS: return RGB(155, 120, 230);
    case CT_MUSIC: return RGB(76, 175, 120);
    case CT_VIDEO: return RGB(240, 150, 60);
    case CT_PHOTO: return RGB(228, 196, 60);
    case CT_DOC: return RGB(150, 165, 185);
    case CT_APP: return RGB(224, 110, 150);
    default: return RGB(120, 120, 120);
    }
}
static const wchar_t *CatName(int c) {
    static const wchar_t *n[CT_N] = {L"Jogos (ROMs)", L"Sistema e emulador", L"Música", L"Vídeo", L"Fotos", L"Documentos e texto", L"Programas e compactados", L"Outros"};
    return n[c];
}
static wchar_t CatGlyph(int c) {
    static const wchar_t g[CT_N] = {0xE7FC, 0xE770, 0xE8D6, 0xE714, 0xE91B, 0xE8A5, 0xE7B8, 0xE7C3};
    return g[c];
}
static wchar_t ProfileGlyph(int p) {
    switch (p) {
    case UP_CONSOLE: case UP_GAMES: return 0xE7FC;
    case UP_SYSTEM: return 0xE770;
    case UP_CAMERA: return 0xE722;
    case UP_VIDEO: return 0xE714;
    case UP_MUSIC: return 0xE8D6;
    case UP_DOCS: return 0xE8A5;
    default: return 0xE8B7;
    }
}

static void DrawRows(HDC dc, int w, int y, int maxY, const wchar_t *const *lab, const wchar_t *const *val, int n) {
    int lx = S(20), lw = S(190), vx = lx + lw + S(12), vw = w - vx - S(20);
    for (int i = 0; i < n; i++) {
        RECT vr = {vx, y, vx + vw, y};
        HGDIOBJ o = SelectObject(dc, g_fontVal);
        DrawTextW(dc, val[i], -1, &vr, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
        SelectObject(dc, o);
        int rh = vr.bottom - vr.top;
        if (rh < S(18)) rh = S(18);
        if (y + rh > maxY) rh = maxY - y;
        if (rh <= 0) break;
        RECT lr = {lx, y, lx + lw, y + rh};
        vr.left = vx; vr.right = vx + vw; vr.top = y; vr.bottom = y + rh;
        Txt(dc, lab[i], &lr, DT_SINGLELINE | DT_NOPREFIX, CL_DIM, g_fontValB);
        Txt(dc, val[i], &vr, DT_WORDBREAK | DT_NOPREFIX, CL_TXT, g_fontVal);
        y += rh + S(10);
    }
}

static void DrawBanner(HDC dc, int w, int y, int level, const wchar_t *title) {
    COLORREF lc = level == 0 ? g_pal.ok : (level == 1 ? g_pal.caution : CL_DANGER);
    COLORREF lb = level == 0 ? g_pal.okBg : (level == 1 ? g_pal.cautionBg : g_pal.warnBg);
    RECT hb = {S(16), y, w - S(16), y + S(44)};
    RRect(dc, hb, S(4), lb, CL_NONE);
    ShieldIcon(dc, (float)(hb.left + S(24)), (float)(hb.top + S(10)), (float)S(20), (float)S(24), level, lc);
    RECT ti = {hb.left + S(46), hb.top, hb.right - S(12), hb.bottom};
    Txt(dc, title, &ti, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS, CL_TXT, g_fontH);
}

static void DrawCurve(HDC dc, RECT r, const float *c, int n, float vmax, COLORREF col) {
    if (n < 2 || vmax <= 0) return;
    for (int i = 1; i < n; i++) {
        float v0 = c[i - 1] > vmax ? vmax : c[i - 1], v1 = c[i] > vmax ? vmax : c[i];
        float x0 = r.left + (float)(r.right - r.left) * (i - 1) / (n - 1), x1 = r.left + (float)(r.right - r.left) * i / (n - 1);
        float y0 = r.bottom - (float)(r.bottom - r.top) * v0 / vmax, y1 = r.bottom - (float)(r.bottom - r.top) * v1 / vmax;
        LineSeg(dc, x0, y0, x1, y1, (float)S(2), col);
    }
}

/* barras das 4 medidas + curva de velocidade durante o teste */
static void DrawBenchChart(HDC dc, int w, int top, int h, const Verdict *v) {
    RECT sl = {S(20), top, w - S(20), top + 1};
    Fill(dc, sl, CL_EDGE);
    static const wchar_t *nm[4] = {L"Leitura sequencial", L"Gravação sequencial", L"Leitura 4 KB", L"Gravação 4 KB"};
    double val[4] = {v->br.seqR, v->br.hasW ? v->br.seqW : -1, v->br.rr, v->br.hasW ? v->br.rw : -1};
    double ref[4] = {90, 90, 2000, 1000};
    const wchar_t *un[4] = {L"MB/s", L"MB/s", L"ops/s", L"ops/s"};
    int lx = S(20), labW = S(150), bx = lx + labW, bwMax = S(150), by = top + S(14);
    for (int i = 0; i < 4; i++) {
        int y = by + i * S(26);
        RECT lr = {lx, y, lx + labW, y + S(18)};
        Txt(dc, nm[i], &lr, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX, CL_DIM, g_fontSmall);
        RECT bk = {bx, y + S(3), bx + bwMax, y + S(15)};
        RRect(dc, bk, S(3), g_pal.btnEdge, CL_NONE);
        wchar_t t[48];
        if (val[i] < 0) {
            wcscpy(t, L"não testado");
        } else {
            double f = val[i] / ref[i];
            if (f > 1) f = 1;
            RECT fl = {bx, y + S(3), bx + (int)(bwMax * f) + S(6), y + S(15)};
            RRect(dc, fl, S(3), i < 2 ? CL_NEON : CatColor(CT_VIDEO), CL_NONE);
            _snwprintf(t, 47, L"%.0f %ls", val[i], un[i]);
            t[47] = 0;
        }
        RECT tr = {bx + bwMax + S(8), y, bx + bwMax + S(100), y + S(18)};
        Txt(dc, t, &tr, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX, CL_TXT, g_fontSmall);
    }
    int cx = bx + bwMax + S(120);
    RECT cr = {cx, top + S(24), w - S(24), top + h - S(18)};
    RECT cap = {cx, top + S(6), w - S(20), top + S(22)};
    Txt(dc, L"Velocidade durante o teste (MB/s)", &cap, DT_SINGLELINE | DT_NOPREFIX, CL_DIM, g_fontSmall);
    RRect(dc, cr, S(3), g_pal.input, CL_EDGE);
    float vmax = 10;
    for (int i = 0; i < v->nCurveR; i++) if (v->curveR[i] > vmax) vmax = v->curveR[i];
    for (int i = 0; i < v->nCurveW; i++) if (v->curveW[i] > vmax) vmax = v->curveW[i];
    RECT in = {cr.left + S(4), cr.top + S(4), cr.right - S(4), cr.bottom - S(4)};
    DrawCurve(dc, in, v->curveR, v->nCurveR, vmax, CL_NEON);
    DrawCurve(dc, in, v->curveW, v->nCurveW, vmax, CatColor(CT_VIDEO));
    RECT lg1 = {cr.left, cr.bottom + S(2), cr.left + S(90), cr.bottom + S(18)};
    RECT lg2 = {cr.left + S(96), cr.bottom + S(2), cr.left + S(200), cr.bottom + S(18)};
    Txt(dc, L"— leitura", &lg1, DT_SINGLELINE | DT_NOPREFIX, CL_NEON, g_fontSmall);
    Txt(dc, L"— gravação", &lg2, DT_SINGLELINE | DT_NOPREFIX, CatColor(CT_VIDEO), g_fontSmall);
}

/* mapa de 128 pontos numa faixa */
static void DrawMiniMap(HDC dc, int x, int y, int w, int h, const Verdict *v, int upto) {
    for (int i = 0; i < MAP_N; i++) {
        RECT c = {x + w * i / MAP_N, y, x + w * (i + 1) / MAP_N, y + h};
        if (c.right - c.left > 3) c.right -= 1;
        Fill(dc, c, i < upto ? MapColor(v->cell[i]) : g_pal.btnEdge);
    }
}

static void CardBox(HDC dc, RECT r, const wchar_t *cap) {
    RRect(dc, r, S(8), CL_BG, CL_EDGE);
    RECT t = {r.left + S(16), r.top + S(10), r.right - S(16), r.top + S(28)};
    Txt(dc, cap, &t, DT_SINGLELINE | DT_NOPREFIX, CL_DIM, g_fontSmall);
}

static const wchar_t *NextHint(const Verdict *v) {
    switch (v->next) {
    case NX_BACKUP: return L"Faça um backup agora, antes que o cartão piore.";
    case NX_CARVE: return L"Recupere os arquivos para o computador.";
    case NX_REBUILD: return L"Reconstrua as partições. Não apaga os dados.";
    case NX_RESTORE: return L"Restaure o cartão para voltar a usar.";
    case NX_AUTH: return L"Confira se o cartão é original e se a capacidade é real.";
    case NX_BENCH: return L"Meça a velocidade para saber se ele serve para jogos e vídeo.";
    }
    return v->todo;
}

/* resultado do Diagnóstico: saúde (esquerda) + conteúdo (direita) + próximo passo */
static void DrawDiagResult(HDC dc, int w, int h, const Verdict *v) {
    int pad = S(16), bot = h - S(66);
    COLORREF lc = v->level == 0 ? g_pal.ok : (v->level == 1 ? g_pal.caution : CL_DANGER);

    /* ---- saúde ---- */
    RECT L = {pad, pad, pad + S(236), bot};
    CardBox(dc, L, L"Saúde");
    int cx = (L.left + L.right) / 2;
    ShieldIcon(dc, (float)cx, (float)(L.top + S(36)), (float)S(58), (float)S(68), v->level, lc);
    RECT sw = {L.left + S(8), L.top + S(110), L.right - S(8), L.top + S(138)};
    Txt(dc, v->level == 0 ? L"Saudável" : (v->level == 1 ? L"Atenção" : L"Com defeito"), &sw, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX, lc, g_fontH);
    RECT tt = {L.left + S(14), L.top + S(140), L.right - S(14), L.top + S(178)};
    Txt(dc, v->title, &tt, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX | DT_END_ELLIPSIS, CL_DIM, g_fontVal);
    static const wchar_t *hl[3] = {L"Leitura", L"Gravação", L"Partições"};
    int hs[3] = {v->hRead, v->hWrite, v->hPart};
    const wchar_t *hv[3];
    hv[0] = v->hRead == 0 ? L"ok" : (v->hRead == 1 ? L"lenta" : L"falha");
    hv[1] = v->hWrite == 0 ? L"ok" : (v->hWrite == 1 ? L"protegida" : (v->hWrite == 2 ? L"falha" : L"não testada"));
    hv[2] = v->hPart == 0 ? L"ok" : (v->hPart == 1 ? L"sem tabela" : L"ilegível");
    int ry = L.top + S(186);
    for (int i = 0; i < 3; i++) {
        int y = ry + i * S(26);
        StatusDot(dc, L.left + S(26), y + S(10), S(8), hs[i]);
        RECT a = {L.left + S(42), y, L.right - S(14), y + S(20)};
        Txt(dc, hl[i], &a, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX, CL_TXT, g_fontVal);
        Txt(dc, hv[i], &a, DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_NOPREFIX, CL_DIM, g_fontVal);
    }
    RECT ml = {L.left + S(16), bot - S(36), L.right - S(16), bot - S(20)};
    Txt(dc, L"Mapa de leitura", &ml, DT_SINGLELINE | DT_NOPREFIX, CL_DIM, g_fontSmall);
    DrawMiniMap(dc, L.left + S(16), bot - S(18), (L.right - L.left) - S(32), S(8), v, MAP_N);

    /* ---- conteúdo ---- */
    RECT R = {L.right + S(12), pad, w - pad, bot};
    CardBox(dc, R, L"Conteúdo");
    if (!v->usage) {
        int ccx = (R.left + R.right) / 2;
        RECT gi = {ccx - S(30), R.top + S(70), ccx + S(30), R.top + S(120)};
        Glyph(dc, 0xE8B7, &gi, CL_DIM, g_fontIconL);
        RECT a = {R.left + S(20), R.top + S(130), R.right - S(20), R.top + S(160)};
        Txt(dc, L"Não deu para ler o conteúdo", &a, DT_SINGLELINE | DT_CENTER | DT_NOPREFIX, CL_TXT, g_fontH);
        RECT b = {R.left + S(30), R.top + S(166), R.right - S(30), R.top + S(230)};
        Txt(dc, L"O cartão não tem uma partição FAT, exFAT ou ext legível (pode estar sem partição, em NTFS ou com o sistema de arquivos danificado).", &b, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX, CL_DIM, g_fontVal);
    } else {
        const UsageStat *u = &v->us;
        /* perfil */
        int px = R.left + S(16), py = R.top + S(34);
        Disc(dc, px + S(18), py + S(18), S(18), CL_NEON);
        RECT pg = {px, py, px + S(36), py + S(36)};
        Glyph(dc, ProfileGlyph(v->uProfile), &pg, g_pal.onAccent, g_fontIconM);
        RECT pt = {px + S(46), py - S(1), R.right - S(16), py + S(19)};
        Txt(dc, v->uTitle, &pt, DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS, CL_TXT, g_fontValB);
        static const wchar_t *fitT[4] = {L"Serve para esse uso", L"Serve com ressalvas", L"Não serve para esse uso", L"Meça o desempenho para confirmar"};
        int fl = v->uFitLevel < 0 || v->uFitLevel > 3 ? 3 : v->uFitLevel;
        HGDIOBJ o = SelectObject(dc, g_fontSmall);
        SIZE ts;
        GetTextExtentPoint32W(dc, fitT[fl], (int)wcslen(fitT[fl]), &ts);
        SelectObject(dc, o);
        RECT chip = {px + S(46), py + S(21), px + S(46) + ts.cx + S(30), py + S(41)};
        COLORREF cc = fl == 0 ? g_pal.ok : fl == 1 ? g_pal.caution : fl == 2 ? CL_DANGER : CL_DIM;
        COLORREF cb = fl == 0 ? g_pal.okBg : fl == 1 ? g_pal.cautionBg : fl == 2 ? g_pal.warnBg : g_pal.navHot;
        RRect(dc, chip, S(10), cb, CL_NONE);
        StatusDot(dc, chip.left + S(10), (chip.top + chip.bottom) / 2, S(6), fl == 3 ? 3 : fl);
        RECT ct = {chip.left + S(20), chip.top, chip.right - S(6), chip.bottom};
        Txt(dc, fitT[fl], &ct, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX, cc, g_fontSmall);

        /* rosca: o cartão inteiro */
        float r = (float)S(66), dcx = (float)(R.left + S(24)) + r, dcy = (float)(R.top + S(92)) + r;
        double cap = (double)(v->uCard ? v->uCard : 1);
        float a = -90.0f;
        int order[CT_N];
        for (int i = 0; i < CT_N; i++) order[i] = i;
        for (int i = 0; i < CT_N; i++)
            for (int k = i + 1; k < CT_N; k++)
                if (u->catBytes[order[k]] > u->catBytes[order[i]]) { int t = order[i]; order[i] = order[k]; order[k] = t; }
        PieSeg(dc, dcx, dcy, r, 0, 360, g_pal.btnEdge);
        for (int i = 0; i < CT_N; i++) {
            int c = order[i];
            if (!u->catBytes[c]) continue;
            float sw2 = (float)(360.0 * (double)u->catBytes[c] / cap);
            if (sw2 < 1.5f) sw2 = 1.5f;
            PieSeg(dc, dcx, dcy, r, a, sw2, CatColor(c));
            a += sw2;
        }
        uint64_t freeIn = u->scannedBytes > u->usedBytes ? u->scannedBytes - u->usedBytes : 0;
        float fsw = (float)(360.0 * (double)freeIn / cap);
        if (a + fsw > 270.0f) fsw = 270.0f - a;
        PieSeg(dc, dcx, dcy, r, a, fsw, g_pal.empty);
        Disc(dc, (int)dcx, (int)dcy, (int)(r * 0.64f), CL_BG);
        int pct = v->uCard ? (int)(u->usedBytes * 100 / v->uCard) : 0;
        wchar_t pc[16];
        _snwprintf(pc, 15, L"%d%%", pct);
        pc[15] = 0;
        RECT c1 = {(int)(dcx - r), (int)dcy - S(22), (int)(dcx + r), (int)dcy + S(10)};
        Txt(dc, pc, &c1, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX, CL_TXT, g_fontTitle);
        RECT c2 = {(int)(dcx - r), (int)dcy + S(8), (int)(dcx + r), (int)dcy + S(26)};
        Txt(dc, L"ocupado", &c2, DT_SINGLELINE | DT_CENTER | DT_NOPREFIX, CL_DIM, g_fontSmall);
        wchar_t a1[32], b1[32], line[120];
        FmtSize(u->usedBytes, a1, 32);
        FmtSize(v->uCard, b1, 32);
        _snwprintf(line, 119, L"%ls de %ls", a1, b1);
        line[119] = 0;
        RECT u1 = {(int)(dcx - r - S(20)), (int)(dcy + r) + S(6), (int)(dcx + r + S(20)), (int)(dcy + r) + S(24)};
        Txt(dc, line, &u1, DT_SINGLELINE | DT_CENTER | DT_NOPREFIX, CL_TXT, g_fontValB);
        _snwprintf(line, 119, L"%u arquivos · %u pastas", (unsigned)u->files, (unsigned)u->dirs);
        line[119] = 0;
        RECT u2 = {(int)(dcx - r - S(20)), (int)(dcy + r) + S(24), (int)(dcx + r + S(20)), (int)(dcy + r) + S(42)};
        Txt(dc, line, &u2, DT_SINGLELINE | DT_CENTER | DT_NOPREFIX, CL_DIM, g_fontSmall);

        /* legenda com ícones */
        int lx = (int)(dcx + r) + S(26), lw = R.right - S(16) - lx, ly = R.top + S(94), shown = 0;
        for (int i = 0; i < CT_N && shown < 6; i++) {
            int c = order[i];
            if (!u->catFiles[c]) continue;
            int y = ly + shown * S(33);
            RECT gi = {lx, y, lx + S(22), y + S(20)};
            Glyph(dc, CatGlyph(c), &gi, CatColor(c), g_fontIcon);
            wchar_t nm[96], sz[32];
            FmtSize(u->catBytes[c], sz, 32);
            _snwprintf(nm, 95, L"%ls · %u", CatName(c), (unsigned)u->catFiles[c]);
            nm[95] = 0;
            RECT tn = {lx + S(28), y, lx + lw - S(70), y + S(20)};
            Txt(dc, nm, &tn, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS, CL_TXT, g_fontVal);
            RECT tsz = {lx + lw - S(70), y, lx + lw, y + S(20)};
            Txt(dc, sz, &tsz, DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_NOPREFIX, CL_TXT, g_fontValB);
            int bw = lw - S(28);
            RECT bk = {lx + S(28), y + S(22), lx + S(28) + bw, y + S(26)};
            RRect(dc, bk, S(2), g_pal.btnEdge, CL_NONE);
            double f = u->usedBytes ? (double)u->catBytes[c] / (double)u->usedBytes : 0;
            RECT bf = {lx + S(28), y + S(22), lx + S(28) + (int)(bw * f) + S(4), y + S(26)};
            RRect(dc, bf, S(2), CatColor(c), CL_NONE);
            shown++;
        }
        /* extensões mais comuns */
        int ex = R.left + S(16), ey = bot - S(36);
        RECT el = {ex, ey - S(20), R.right - S(16), ey - S(4)};
        Txt(dc, L"Extensões mais comuns", &el, DT_SINGLELINE | DT_NOPREFIX, CL_DIM, g_fontSmall);
        o = SelectObject(dc, g_fontSmall);
        for (int i = 0; i < u->next && i < 8; i++) {
            wchar_t e[40];
            _snwprintf(e, 39, L".%hs  %u", u->ext[i].ext, (unsigned)u->ext[i].files);
            e[39] = 0;
            GetTextExtentPoint32W(dc, e, (int)wcslen(e), &ts);
            int cw = ts.cx + S(18);
            if (ex + cw > R.right - S(16)) break;
            RECT p = {ex, ey, ex + cw, ey + S(22)};
            RRect(dc, p, S(11), g_pal.navHot, CL_NONE);
            RECT d2 = {p.left + S(4), p.top, p.left + S(10), p.bottom};
            Disc(dc, p.left + S(9), (p.top + p.bottom) / 2, S(3), CatColor(u->ext[i].cat));
            (void)d2;
            RECT pt2 = {p.left + S(14), p.top, p.right, p.bottom};
            SetTextColor(dc, CL_TXT);
            SetBkMode(dc, TRANSPARENT);
            DrawTextW(dc, e, -1, &pt2, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
            ex += cw + S(6);
        }
        SelectObject(dc, o);
    }

    /* ---- próximo passo ---- */
    RECT B = {pad, h - S(54), w - pad, h - pad};
    RRect(dc, B, S(8), g_pal.navSel, CL_NONE);
    RECT ai = {B.left + S(12), B.top, B.left + S(36), B.bottom};
    Glyph(dc, 0xE72A, &ai, CL_NEON, g_fontIconM);
    RECT at = {B.left + S(44), B.top, B.left + S(160), B.bottom};
    Txt(dc, L"Próximo passo", &at, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX, CL_TXT, g_fontValB);
    RECT ah = {B.left + S(160), B.top, B.right - S(14), B.bottom};
    Txt(dc, NextHint(v), &ah, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS, CL_TXT, g_fontVal);
}

/* tela inicial do Diagnóstico */
static void DrawHero(HDC dc, int w, int h) {
    int cx = w / 2;
    DiskInfo *di = FindDisk(SelectedDisk(ID_COMBO1));
    BOOL has = di != NULL;
    float cw = (float)S(84), ch = (float)S(108);
    float pulse = 0;
    if (!has) pulse = (float)((GetTickCount64() / 20) % 100) / 100.0f;
    SdCardIcon(dc, cx - cw / 2, (float)S(30), cw, ch, has ? CL_NEON : g_pal.btnEdge, has ? g_pal.onAccent : CL_PANEL);
    if (has) StatusDot(dc, (int)(cx + cw / 2), S(30) + (int)ch, S(13), 0);
    (void)pulse;
    RECT t1 = {S(20), S(156), w - S(20), S(190)};
    Txt(dc, has ? L"Cartão pronto para o diagnóstico" : L"Insira um cartão SD", &t1, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX, CL_TXT, g_fontTitle);
    RECT t2 = {S(20), S(194), w - S(20), S(214)};
    wchar_t s[400];
    if (has) {
        wchar_t sz[32];
        FmtSize(di->size, sz, 32);
        _snwprintf(s, 399, L"%ls %ls · %ls%ls%ls%ls", di->vendor, di->product, sz, di->letters[0] ? L" · unidade " : L"", di->letters, g_ndisks > 1 ? L"   (há outro cartão: escolha na lista acima)" : L"");
    } else wcscpy(s, L"Assim que o cartão for conectado, ele aparece aqui sozinho.");
    s[399] = 0;
    Txt(dc, s, &t2, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS, CL_DIM, g_fontVal);
    if (has) {
        RECT t3 = {S(20), S(300), w - S(20), S(320)};
        Txt(dc, L"Leva cerca de 1 minuto e não apaga nada.", &t3, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX, CL_DIM, g_fontSmall);
    }
    /* o que o diagnóstico mostra */
    int fy = h - S(118), fw = (w - S(64)) / 3;
    static const wchar_t *ft[3] = {L"Saúde do cartão", L"O que tem dentro", L"O que fazer depois"};
    static const wchar_t *fd[3] = {L"Leitura, gravação e partições", L"Jogos, fotos, música e mais", L"Backup, reparo ou próximo teste"};
    RECT sep = {S(32), fy - S(14), w - S(32), fy - S(13)};
    Fill(dc, sep, CL_EDGE);
    for (int i = 0; i < 3; i++) {
        int x0 = S(32) + i * fw, ic = x0 + fw / 2;
        if (i == 0) ShieldIcon(dc, (float)ic, (float)fy, (float)S(26), (float)S(30), 0, g_pal.ok);
        else if (i == 1) {
            PieSeg(dc, (float)ic, (float)(fy + S(15)), (float)S(15), -90, 360, g_pal.btnEdge);
            PieSeg(dc, (float)ic, (float)(fy + S(15)), (float)S(15), -90, 150, CL_NEON);
            PieSeg(dc, (float)ic, (float)(fy + S(15)), (float)S(15), 60, 70, CatColor(CT_MUSIC));
            Disc(dc, ic, fy + S(15), S(8), CL_PANEL);
        } else {
            Disc(dc, ic, fy + S(15), S(15), CL_NEON);
            RECT gi = {ic - S(15), fy, ic + S(15), fy + S(30)};
            Glyph(dc, 0xE72A, &gi, g_pal.onAccent, g_fontIcon);
        }
        RECT a = {x0, fy + S(38), x0 + fw, fy + S(58)};
        Txt(dc, ft[i], &a, DT_SINGLELINE | DT_CENTER | DT_NOPREFIX, CL_TXT, g_fontValB);
        RECT b = {x0, fy + S(58), x0 + fw, fy + S(76)};
        Txt(dc, fd[i], &b, DT_SINGLELINE | DT_CENTER | DT_NOPREFIX | DT_END_ELLIPSIS, CL_DIM, g_fontSmall);
    }
}

/* animação durante o diagnóstico */
static void DrawDiagBusy(HDC dc, int w, int h, const Verdict *v) {
    (void)h;
    /* cartão com linha de leitura passando */
    float cw = (float)S(96), ch = (float)S(124), l = (float)S(70), t = (float)S(50);
    SdCardIcon(dc, l, t, cw, ch, CL_NEON, g_pal.onAccent);
    float ph = (float)((GetTickCount64() / 12) % 100) / 100.0f;
    float y = t + ch * 0.32f + (ch * 0.62f) * (ph < 0.5f ? ph * 2 : (1 - ph) * 2);
    LineSeg(dc, l - S(10), y, l + cw + S(10), y, (float)S(3), g_pal.onAccent);
    LineSeg(dc, l - S(10), y, l + cw + S(10), y, (float)S(1), CL_TXT);
    RECT tl = {S(20), S(196), S(236), S(220)};
    Txt(dc, L"Analisando...", &tl, DT_SINGLELINE | DT_CENTER | DT_NOPREFIX, CL_DIM, g_fontVal);

    int x0 = S(270), y0 = S(36);
    RECT hd = {x0, y0 - S(6), w - S(20), y0 + S(24)};
    Txt(dc, L"Diagnóstico em andamento", &hd, DT_SINGLELINE | DT_NOPREFIX, CL_TXT, g_fontH);
    static const wchar_t *st[5] = {L"Lendo a tabela de partições", L"Lendo 128 pontos do cartão", L"Testando a gravação", L"Lendo o conteúdo", L"Montando o relatório"};
    int cur = (int)g_step;
    for (int i = 0; i < 5; i++) {
        int yy = y0 + S(44) + i * S(40);
        int ccx = x0 + S(11), ccy = yy + S(10);
        if (i < cur) StatusDot(dc, ccx, ccy, S(10), 0);
        else if (i == cur) Spinner(dc, ccx, ccy, S(9), (float)S(3));
        else ArcLine(dc, (float)ccx, (float)ccy, (float)S(9), 0, 360, (float)S(2), g_pal.btnEdge);
        RECT a = {x0 + S(32), yy, w - S(20), yy + S(20)};
        Txt(dc, st[i], &a, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX, i <= cur ? CL_TXT : CL_DIM, i == cur ? g_fontValB : g_fontVal);
        if (i == cur && (i == 1 || i == 3)) {
            if (i == 1) DrawMiniMap(dc, x0 + S(32), yy + S(24), w - x0 - S(56), S(8), v, (int)g_mapDone);
            else {
                wchar_t ph2[200];
                EnterCriticalSection(&g_cs);
                wcscpy(ph2, g_phase);
                LeaveCriticalSection(&g_cs);
                RECT b = {x0 + S(32), yy + S(20), w - S(20), yy + S(38)};
                Txt(dc, ph2, &b, DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS, CL_DIM, g_fontSmall);
            }
        }
    }
}

/* animação genérica (Desempenho, Proteção) */
static void DrawBusyGeneric(HDC dc, int w, int h) {
    Spinner(dc, w / 2, h / 2 - S(30), S(22), (float)S(4));
    wchar_t ph2[200];
    EnterCriticalSection(&g_cs);
    wcscpy(ph2, g_phase);
    LeaveCriticalSection(&g_cs);
    RECT a = {S(20), h / 2 + S(6), w - S(20), h / 2 + S(32)};
    Txt(dc, ph2[0] ? ph2 : L"Trabalhando...", &a, DT_SINGLELINE | DT_CENTER | DT_NOPREFIX | DT_END_ELLIPSIS, CL_TXT, g_fontVal);
    RECT b = {S(20), h / 2 + S(34), w - S(20), h / 2 + S(54)};
    Txt(dc, g_op == OP_BENCH ? L"Leva até 1 minuto. Não retire o cartão." : L"Não retire o cartão.", &b, DT_SINGLELINE | DT_CENTER | DT_NOPREFIX, CL_DIM, g_fontSmall);
}

static void DrawResult(DRAWITEMSTRUCT *d) {
    int w = d->rcItem.right - d->rcItem.left, h = d->rcItem.bottom - d->rcItem.top;
    HDC dc = CreateCompatibleDC(d->hDC);
    HBITMAP bm = CreateCompatibleBitmap(d->hDC, w, h);
    HGDIOBJ ob = SelectObject(dc, bm);
    RECT r = {0, 0, w, h};
    Fill(dc, r, CL_BG);
    RRect(dc, r, S(8), CL_PANEL, CL_EDGE);
    const Verdict *v = &g_vd;
    if (g_busy && g_op == OP_DIAG) DrawDiagBusy(dc, w, h, v);
    else if (g_busy) DrawBusyGeneric(dc, w, h);
    else if (IsHero()) DrawHero(dc, w, h);
    else if (!v->valid || v->op != g_op) {
        RECT tr = {S(24), 0, w - S(24), h};
        Txt(dc, g_op == OP_CSD ? L"Escolha como acessar o cartão e clique em Ler proteção. O resultado aparece aqui."
                : g_op == OP_BENCH ? L"Escolha o cartão e clique em Medir desempenho. O resultado aparece aqui."
                                   : L"Escolha o cartão e clique em Analisar cartão. O resultado aparece aqui.", &tr,
            DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX, CL_DIM, g_font);
    } else if (v->op == OP_DIAG) {
        DrawDiagResult(dc, w, h, v);
    } else {
        int y0 = S(16);
        DrawBanner(dc, w, y0, v->level, v->title);
        static const wchar_t *lab[4] = {L"Causa provável", L"O cartão tem conserto?", L"Dá para salvar os dados?", L"O que fazer agora"};
        static const wchar_t *labB[4] = {L"Velocidade medida", L"Acesso aleatório (4 KB)", L"Classe estimada", L"Serve para"};
        const wchar_t *val[4] = {v->cause, v->card, v->data, v->todo};
        int mapTop = v->bench ? h - S(150) : h - S(10);
        DrawRows(dc, w, y0 + S(44) + S(14), mapTop - S(6), v->bench ? labB : lab, val, 4);
        if (v->bench) DrawBenchChart(dc, w, mapTop, S(150), v);
    }
    BitBlt(d->hDC, d->rcItem.left, d->rcItem.top, w, h, dc, 0, 0, SRCCOPY);
    SelectObject(dc, ob);
    DeleteObject(bm);
    DeleteDC(dc);
}

/* trilha da jornada no topo (Diagnóstico -> Autenticidade -> Desempenho) */
static void DrawSteps(DRAWITEMSTRUCT *d) {
    int w = d->rcItem.right - d->rcItem.left, h = d->rcItem.bottom - d->rcItem.top;
    HDC dc = CreateCompatibleDC(d->hDC);
    HBITMAP bm = CreateCompatibleBitmap(d->hDC, w, h);
    HGDIOBJ ob = SelectObject(dc, bm);
    RECT r = {0, 0, w, h};
    Fill(dc, r, CL_BG);
    static const wchar_t *nm[3] = {L"Diagnóstico", L"Autenticidade", L"Desempenho"};
    int sw = w / 3, cy = h / 2, rad = S(11);
    for (int i = 0; i < 3; i++) {
        int x = i * sw + rad + S(2);
        if (i < 2) {
            RECT ln = {x + rad + S(4) + S(96), cy, (i + 1) * sw + S(2) - S(4), cy + S(2)};
            if (ln.right > ln.left) Fill(dc, ln, JourneyDone(i) ? g_pal.ok : g_pal.btnEdge);
        }
        BOOL done = JourneyDone(i), cur = (g_op == JOURNEY_OP[i]);
        if (done) StatusDot(dc, x, cy, rad, 0);
        else {
            Disc(dc, x, cy, rad, cur ? CL_NEON : g_pal.btnEdge);
            if (!cur) Disc(dc, x, cy, rad - S(2), CL_BG);
            wchar_t n[2] = {(wchar_t)(L'1' + i), 0};
            RECT nr = {x - rad, cy - rad, x + rad, cy + rad};
            Txt(dc, n, &nr, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX, cur ? g_pal.onAccent : CL_DIM, g_fontSmall);
        }
        RECT tr = {x + rad + S(6), 0, x + rad + S(6) + S(96), h};
        Txt(dc, nm[i], &tr, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX, cur ? CL_TXT : CL_DIM, cur ? g_fontValB : g_fontVal);
    }
    BitBlt(d->hDC, d->rcItem.left, d->rcItem.top, w, h, dc, 0, 0, SRCCOPY);
    SelectObject(dc, ob);
    DeleteObject(bm);
    DeleteDC(dc);
}

static void ApplyOp(int op);
static LRESULT CALLBACK StepsSub(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR ref) {
    (void)ref; (void)id;
    if (m == WM_NCHITTEST) return HTCLIENT;
    if (m == WM_SETCURSOR) { SetCursor(LoadCursor(NULL, g_busy ? IDC_ARROW : IDC_HAND)); return TRUE; }
    if (m == WM_LBUTTONUP && !g_busy) {
        RECT rc;
        GetClientRect(h, &rc);
        int x = (short)LOWORD(l), i = x * 3 / (rc.right > 0 ? rc.right : 1);
        if (i >= 0 && i < 3 && JOURNEY_OP[i] != g_op) ApplyOp(JOURNEY_OP[i]);
        return 0;
    }
    return DefSubclassProc(h, m, w, l);
}

static LRESULT CALLBACK BtnSub(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR ref) {
    (void)ref;
    if (m == WM_MOUSEMOVE && g_hover != (int)id) {
        int old = g_hover;
        g_hover = (int)id;
        TRACKMOUSEEVENT t = {sizeof t, TME_LEAVE, h, 0};
        TrackMouseEvent(&t);
        InvalidateRect(h, NULL, FALSE);
        if (old && Ctl(old)) InvalidateRect(Ctl(old), NULL, FALSE);
    } else if (m == WM_MOUSELEAVE) {
        if (g_hover == (int)id) g_hover = 0;
        InvalidateRect(h, NULL, FALSE);
    } else if (m == WM_NCDESTROY) {
        RemoveWindowSubclass(h, BtnSub, id);
    }
    return DefSubclassProc(h, m, w, l);
}
/* ========================================================================== */

static void FmtSize(uint64_t b, wchar_t *out, size_t n) {
    static const wchar_t *u[] = {L"B", L"KB", L"MB", L"GB", L"TB"};
    double v = (double)b;
    int i = 0;
    while (v >= 1024.0 && i < 4) { v /= 1024.0; i++; }
    if (i == 0) _snwprintf(out, n, L"%u B", (unsigned)b);
    else _snwprintf(out, n, L"%.1f %ls", v, u[i]);
    out[n - 1] = 0;
    for (wchar_t *p = out; *p; p++) if (*p == L'.') *p = L',';
}

static void FmtDur(double sec, wchar_t *out, size_t n) {
    unsigned s = (unsigned)(sec + 0.5);
    if (s >= 3600) _snwprintf(out, n, L"%u h %02u min", s / 3600, (s % 3600) / 60);
    else if (s >= 60) _snwprintf(out, n, L"%u min %02u s", s / 60, s % 60);
    else _snwprintf(out, n, L"%u s", s);
    out[n - 1] = 0;
}

static double NowSec(void) {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}

static void Log(const wchar_t *fmt, ...) {
    wchar_t msg[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(msg, 1023, fmt, ap);
    va_end(ap);
    msg[1023] = 0;
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t *full = (wchar_t *)malloc(sizeof(wchar_t) * 1100);
    if (!full) return;
    _snwprintf(full, 1099, L"[%02d:%02d:%02d] %ls\r\n", st.wHour, st.wMinute, st.wSecond, msg);
    full[1099] = 0;
    if (!PostMessageW(g_hwnd, WM_APP_LOG, 0, (LPARAM)full)) free(full);
}

static void LogErr(const wchar_t *what) {
    DWORD e = GetLastError();
    wchar_t *m = NULL;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, e,
                   MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPWSTR)&m, 0, NULL);
    if (m) {
        size_t l = wcslen(m);
        while (l > 0 && (m[l - 1] == L'\r' || m[l - 1] == L'\n' || m[l - 1] == L' ')) m[--l] = 0;
    }
    Log(L"ERRO: %ls — %ls (código %lu)", what, m ? m : L"", (unsigned long)e);
    if (m) LocalFree(m);
    if (e == ERROR_WRITE_PROTECT) Log(L"Dica: o cartão está protegido contra gravação (chave lateral do cartão SD).");
    if (e == ERROR_ACCESS_DENIED) Log(L"Dica: feche programas que usam o cartão e confirme que o app está como administrador.");
}

static void SetPhase(const wchar_t *p, uint64_t total) {
    EnterCriticalSection(&g_cs);
    wcsncpy(g_phase, p, 199); g_phase[199] = 0;
    LeaveCriticalSection(&g_cs);
    g_done = 0; g_total = (LONGLONG)total; g_indet = 0; g_t0 = GetTickCount64();
}
static void SetPhaseText(const wchar_t *p) {
    EnterCriticalSection(&g_cs);
    wcsncpy(g_phase, p, 199); g_phase[199] = 0;
    LeaveCriticalSection(&g_cs);
}
static void SetIndet(const wchar_t *p) { SetPhaseText(p); g_done = 0; g_total = 0; g_indet = 1; g_t0 = GetTickCount64(); }

static void SetSummary(UINT icon, const wchar_t *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(g_summary, 2399, fmt, ap);
    va_end(ap);
    g_summary[2399] = 0;
    g_summaryIcon = icon;
}

/* ----------------------------------------------------------------- discos */
static int DiskOfLetter(wchar_t L) {
    wchar_t p[] = L"\\\\.\\X:";
    p[4] = L;
    HANDLE h = CreateFileW(p, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;
    STORAGE_DEVICE_NUMBER sdn;
    DWORD br;
    int r = -1;
    if (DeviceIoControl(h, IOCTL_STORAGE_GET_DEVICE_NUMBER, NULL, 0, &sdn, sizeof sdn, &br, NULL) &&
        sdn.DeviceType == FILE_DEVICE_DISK)
        r = (int)sdn.DeviceNumber;
    CloseHandle(h);
    return r;
}

static int SystemDisk(void) {
    wchar_t w[MAX_PATH];
    if (GetWindowsDirectoryW(w, MAX_PATH) < 2 || w[1] != L':') return -1;
    return DiskOfLetter((wchar_t)towupper(w[0]));
}

static BOOL PathOnDisk(const wchar_t *path, int disk) {
    wchar_t full[MAX_PATH];
    if (!GetFullPathNameW(path, MAX_PATH, full, NULL)) return FALSE;
    if (full[1] != L':') return FALSE;
    return DiskOfLetter((wchar_t)towupper(full[0])) == disk;
}

static HANDLE OpenDisk(int n, BOOL write) {
    wchar_t p[64];
    _snwprintf(p, 63, L"\\\\.\\PhysicalDrive%d", n);
    p[63] = 0;
    return CreateFileW(p, write ? (GENERIC_READ | GENERIC_WRITE) : GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                       OPEN_EXISTING, FILE_FLAG_NO_BUFFERING | (write ? FILE_FLAG_WRITE_THROUGH : 0), NULL);
}

static BOOL DiskLenMatches(HANDLE h, uint64_t expect) {
    GET_LENGTH_INFORMATION li;
    DWORD br;
    if (!DeviceIoControl(h, IOCTL_DISK_GET_LENGTH_INFO, NULL, 0, &li, sizeof li, &br, NULL)) return FALSE;
    return (uint64_t)li.Length.QuadPart == expect;
}

static void TrimSpaces(wchar_t *s) {
    size_t l = wcslen(s);
    while (l > 0 && s[l - 1] == L' ') s[--l] = 0;
}

static BOOL QueryDisk(int n, DiskInfo *d) {
    memset(d, 0, sizeof *d);
    HANDLE h = OpenDisk(n, FALSE);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    DWORD br;
    GET_LENGTH_INFORMATION li;
    if (!DeviceIoControl(h, IOCTL_DISK_GET_LENGTH_INFO, NULL, 0, &li, sizeof li, &br, NULL) || li.Length.QuadPart <= 0) {
        CloseHandle(h);
        return FALSE;
    }
    d->index = n;
    d->size = (uint64_t)li.Length.QuadPart;
    d->sec = 512;
    BYTE gbuf[256];
    if (DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, NULL, 0, gbuf, sizeof gbuf, &br, NULL)) {
        DWORD bps = ((DISK_GEOMETRY_EX *)gbuf)->Geometry.BytesPerSector;
        if (bps >= 512 && bps <= 4096) d->sec = bps;
    }
    d->bus = 0;
    wcscpy(d->vendor, L"");
    wcscpy(d->product, L"");
    STORAGE_PROPERTY_QUERY q;
    memset(&q, 0, sizeof q);
    q.PropertyId = StorageDeviceProperty;
    q.QueryType = PropertyStandardQuery;
    BYTE sbuf[1024];
    if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof q, sbuf, sizeof sbuf, &br, NULL)) {
        STORAGE_DEVICE_DESCRIPTOR *sd = (STORAGE_DEVICE_DESCRIPTOR *)sbuf;
        d->bus = (int)sd->BusType;
        d->removable = sd->RemovableMedia;
        if (sd->VendorIdOffset && sd->VendorIdOffset < sizeof sbuf) {
            MultiByteToWideChar(CP_ACP, 0, (char *)sbuf + sd->VendorIdOffset, -1, d->vendor, 63);
            TrimSpaces(d->vendor);
        }
        if (sd->ProductIdOffset && sd->ProductIdOffset < sizeof sbuf) {
            MultiByteToWideChar(CP_ACP, 0, (char *)sbuf + sd->ProductIdOffset, -1, d->product, 63);
            TrimSpaces(d->product);
        }
    }
    CloseHandle(h);
    d->likelyCard = d->removable || d->bus == 7 || d->bus == 12 || d->bus == 13;
    DWORD mask = GetLogicalDrives();
    d->letters[0] = 0;
    for (int i = 0; i < 26; i++) {
        if (!(mask & (1u << i))) continue;
        if (DiskOfLetter((wchar_t)(L'A' + i)) != n) continue;
        wchar_t one[8];
        _snwprintf(one, 7, L"%ls%c:", d->letters[0] ? L", " : L"", (wchar_t)(L'A' + i));
        one[7] = 0;
        if (wcslen(d->letters) + wcslen(one) < 60) wcscat(d->letters, one);
    }
    static const wchar_t *busn[] = {L"?", L"SCSI", L"ATAPI", L"ATA", L"1394", L"SSA", L"Fibre", L"USB", L"RAID", L"iSCSI",
                                    L"SAS", L"SATA", L"SD", L"MMC", L"Virtual", L"VirtualArq", L"Spaces", L"NVMe"};
    const wchar_t *bn = (d->bus >= 0 && d->bus <= 17) ? busn[d->bus] : L"?";
    wchar_t sz[32];
    FmtSize(d->size, sz, 32);
    _snwprintf(d->label, 319, L"Disco %d — %ls %ls — %ls [%ls]%ls%ls%ls", n, d->vendor, d->product, sz, bn,
               d->letters[0] ? L" (" : L"", d->letters, d->letters[0] ? L")" : L"");
    d->label[319] = 0;
    return TRUE;
}

static DiskInfo *FindDisk(int n) {
    for (int i = 0; i < g_ndisks; i++)
        if (g_disks[i].index == n) return &g_disks[i];
    return NULL;
}

static int SelectedDisk(int id) {
    int i = (int)SendMessageW(Ctl(id), CB_GETCURSEL, 0, 0);
    if (i < 0) return -1;
    return (int)SendMessageW(Ctl(id), CB_GETITEMDATA, i, 0);
}

static void FillDiskCombo(int id, int prev) {
    HWND c = Ctl(id);
    SendMessageW(c, CB_RESETCONTENT, 0, 0);
    int sel = -1;
    for (int i = 0; i < g_ndisks; i++) {
        int at = (int)SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)g_disks[i].label);
        SendMessageW(c, CB_SETITEMDATA, at, g_disks[i].index);
        if (g_disks[i].index == prev) sel = at;
    }
    if (g_ndisks == 0) {
        int at = (int)SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)L"(nenhum cartão encontrado — insira e clique em Atualizar)");
        SendMessageW(c, CB_SETITEMDATA, at, (LPARAM)-1);
        sel = at;
    }
    SendMessageW(c, CB_SETCURSEL, sel >= 0 ? sel : 0, 0);
}

static void RefreshDisks(void) {
    int p1 = SelectedDisk(ID_COMBO1), p2 = SelectedDisk(ID_COMBO2);
    BOOL all = Checked(ID_SHOWALL);
    g_sysDisk = SystemDisk();
    g_ndisks = 0;
    for (int i = 0; i < 32; i++) {
        DiskInfo d;
        if (!QueryDisk(i, &d)) continue;
        if (i == g_sysDisk) continue;
        if (!all && !d.likelyCard) continue;
        g_disks[g_ndisks++] = d;
    }
    FillDiskCombo(ID_COMBO1, p1);
    FillDiskCombo(ID_COMBO2, p2);
}

/* --------------------------------------------------------- volumes (bloqueio) */
static void UnlockVolumes(VolSet *vs) {
    for (int i = 0; i < vs->n; i++) {
        DWORD br;
        DeviceIoControl(vs->h[i], FSCTL_UNLOCK_VOLUME, NULL, 0, NULL, 0, &br, NULL);
        CloseHandle(vs->h[i]);
    }
    vs->n = 0;
}

static BOOL LockDiskVolumes(int disk, VolSet *vs) {
    vs->n = 0;
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; i++) {
        if (!(mask & (1u << i))) continue;
        wchar_t L = (wchar_t)(L'A' + i);
        if (DiskOfLetter(L) != disk) continue;
        wchar_t p[] = L"\\\\.\\X:";
        p[4] = L;
        HANDLE h = CreateFileW(p, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) {
            Log(L"ERRO: não foi possível abrir a unidade %c: para bloqueio.", L);
            UnlockVolumes(vs);
            return FALSE;
        }
        BOOL locked = FALSE;
        DWORD br;
        for (int t = 0; t < 10 && !locked; t++) {
            if (DeviceIoControl(h, FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0, &br, NULL)) locked = TRUE;
            else Sleep(500);
        }
        if (!locked) {
            Log(L"ERRO: a unidade %c: está em uso. Feche o Explorador/programas que usam o cartão e tente de novo.", L);
            CloseHandle(h);
            UnlockVolumes(vs);
            return FALSE;
        }
        DeviceIoControl(h, FSCTL_DISMOUNT_VOLUME, NULL, 0, NULL, 0, &br, NULL);
        vs->h[vs->n++] = h;
    }
    return TRUE;
}

/* remover com segurança: trava e desmonta os volumes do cartão e pede ao Windows para ejetar a mídia */
static void DoEject(void) {
    int n = SelectedDisk(ID_COMBO1);
    if (n < 0) {
        MessageBoxW(g_hwnd, L"Escolha o cartão na lista antes de removê-lo.", L"Remover com segurança", MB_ICONINFORMATION);
        return;
    }
    DiskInfo *di = FindDisk(n);
    wchar_t lt[64];
    wcscpy(lt, di ? di->letters : L"");
    HCURSOR oc = SetCursor(LoadCursor(NULL, IDC_WAIT));
    VolSet vs;
    BOOL ok = LockDiskVolumes(n, &vs);
    BOOL ejected = FALSE;
    if (ok) {
        for (int i = 0; i < vs.n; i++) {
            DWORD br;
            PREVENT_MEDIA_REMOVAL pmr = {FALSE};
            DeviceIoControl(vs.h[i], IOCTL_STORAGE_MEDIA_REMOVAL, &pmr, sizeof pmr, NULL, 0, &br, NULL);
            if (DeviceIoControl(vs.h[i], IOCTL_STORAGE_EJECT_MEDIA, NULL, 0, NULL, 0, &br, NULL)) ejected = TRUE;
        }
        if (vs.n == 0) {
            wchar_t p[40];
            _snwprintf(p, 39, L"\\\\.\\PhysicalDrive%d", n);
            HANDLE h = CreateFileW(p, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
            if (h != INVALID_HANDLE_VALUE) {
                DWORD br;
                if (DeviceIoControl(h, IOCTL_STORAGE_EJECT_MEDIA, NULL, 0, NULL, 0, &br, NULL)) ejected = TRUE;
                CloseHandle(h);
            }
        }
        UnlockVolumes(&vs);
    }
    SetCursor(oc);
    if (!ok) {
        MessageBoxW(g_hwnd, L"O cartão está em uso e não pôde ser liberado.\n\nFeche o Explorador de Arquivos e os programas que usam o cartão e tente de novo.", L"Remover com segurança", MB_ICONWARNING);
        return;
    }
    RefreshDisks();
    MessageBoxW(g_hwnd, ejected ? L"Pronto. Já pode tirar o cartão do leitor." : L"O Windows não está usando o cartão. Já pode tirá-lo do leitor.", L"Remover com segurança", MB_ICONINFORMATION);
}

static void UpdateDiskProps(HANDLE hd) {
    DWORD br;
    DeviceIoControl(hd, IOCTL_DISK_UPDATE_PROPERTIES, NULL, 0, NULL, 0, &br, NULL);
}

/* -------------------------------------------------------- leitura / escrita */
static BOOL ReadAtRaw(HANDLE h, uint64_t off, void *buf, DWORD len) {
    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG)off;
    if (!SetFilePointerEx(h, li, NULL, FILE_BEGIN)) return FALSE;
    DWORD got = 0;
    if (!ReadFile(h, buf, len, &got, NULL)) return FALSE;
    return got == len;
}

static BOOL WriteAtRaw(HANDLE h, uint64_t off, const void *buf, DWORD len) {
    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG)off;
    if (!SetFilePointerEx(h, li, NULL, FILE_BEGIN)) return FALSE;
    DWORD w = 0;
    if (!WriteFile(h, buf, len, &w, NULL)) return FALSE;
    return w == len;
}

/* len deve ser múltiplo de sec. Setores ilegíveis viram zeros e são contados em *bad. */
static void ReadChunkRobust(HANDLE h, uint64_t off, uint8_t *buf, DWORD len, DWORD sec, uint64_t *bad) {
    if (ReadAtRaw(h, off, buf, len)) return;
    /* estilo ddrescue: setor a setor; depois de 32 falhas seguidas, só tenta 1 a cada 64 setores
       até a leitura voltar, para não travar horas numa área ruim */
    int run = 0;
    for (DWORD o = 0; o < len; o += sec) {
        BOOL ok = FALSE;
        BOOL tryIt = run < 32 || ((o / sec) % 64) == 0;
        for (int t = 0; t < (run < 32 ? 2 : 1) && !ok && tryIt; t++) ok = ReadAtRaw(h, off + o, buf + o, sec);
        if (ok) run = 0;
        else run++;
        if (!ok) {
            memset(buf + o, 0, sec);
            (*bad)++;
            if (g_badLogged < 10) {
                g_badLogged++;
                Log(L"Setor ilegível na posição %I64u (setor %I64u)%ls", (unsigned long long)(off + o),
                    (unsigned long long)((off + o) / sec), g_badLogged == 10 ? L" — novos erros não serão listados" : L"");
            }
        }
    }
}

static DWORD RoundUp(DWORD v, DWORD m) { return ((v + m - 1) / m) * m; }

static BOOL DevRead(Dev *d, uint64_t off, uint8_t *buf, DWORD want, uint64_t *bad) {
    if (d->raw) {
        ReadChunkRobust(d->h, off, buf, RoundUp(want, d->sec), d->sec, bad);
        return TRUE;
    }
    return ReadAtRaw(d->h, off, buf, want);
}

static BOOL DevWrite(Dev *d, uint64_t off, uint8_t *buf, DWORD want) {
    DWORD len = want;
    if (d->raw) {
        len = RoundUp(want, d->sec);
        if (len > want) memset(buf + want, 0, len - want);
    }
    return WriteAtRaw(d->h, off, buf, len);
}

/* copia total bytes de in para out (out == NULL: só lê) */
static BOOL CopyDev(Dev *in, Dev *out, uint64_t total, const wchar_t *phase, uint64_t *bad) {
    uint8_t *buf = (uint8_t *)ALLOC_ALIGNED(CHUNK);
    if (!buf) { Log(L"ERRO: memória insuficiente."); return FALSE; }
    SetPhase(phase, total);
    uint64_t off = 0;
    BOOL ok = TRUE;
    while (off < total) {
        if (g_cancel) { Log(L"Operação cancelada pelo usuário."); ok = FALSE; break; }
        DWORD want = (DWORD)MIN64(CHUNK, total - off);
        if (!DevRead(in, off, buf, want, bad)) { LogErr(L"falha ao ler a origem"); ok = FALSE; break; }
        if (out && !DevWrite(out, off, buf, want)) { LogErr(L"falha ao gravar no destino"); ok = FALSE; break; }
        off += want;
        g_done = (LONGLONG)off;
    }
    FREE_ALIGNED(buf);
    return ok;
}

/* compara a e b; *diff = bytes diferentes, *first = primeira posição diferente */
static BOOL CompareDev(Dev *a, Dev *b, uint64_t total, const wchar_t *phase, uint64_t *diff, uint64_t *first, uint64_t *bad) {
    uint8_t *ba = (uint8_t *)ALLOC_ALIGNED(CHUNK), *bb = (uint8_t *)ALLOC_ALIGNED(CHUNK);
    if (!ba || !bb) { Log(L"ERRO: memória insuficiente."); if (ba) FREE_ALIGNED(ba); if (bb) FREE_ALIGNED(bb); return FALSE; }
    SetPhase(phase, total);
    uint64_t off = 0;
    BOOL ok = TRUE;
    *diff = 0; *first = (uint64_t)-1;
    while (off < total) {
        if (g_cancel) { Log(L"Operação cancelada pelo usuário."); ok = FALSE; break; }
        DWORD want = (DWORD)MIN64(CHUNK, total - off);
        if (!DevRead(a, off, ba, want, bad) || !DevRead(b, off, bb, want, bad)) { LogErr(L"falha na leitura durante a comparação"); ok = FALSE; break; }
        if (memcmp(ba, bb, want) != 0) {
            for (DWORD i = 0; i < want; i++)
                if (ba[i] != bb[i]) { (*diff)++; if (*first == (uint64_t)-1) *first = off + i; }
        }
        off += want;
        g_done = (LONGLONG)off;
    }
    FREE_ALIGNED(ba); FREE_ALIGNED(bb);
    return ok;
}

static void ReportCompare(uint64_t diff, uint64_t first, uint64_t bad) {
    if (diff == 0 && bad == 0) Log(L"Comparação concluída: os dados são IDÊNTICOS.");
    else {
        if (diff) Log(L"ATENÇÃO: %I64u bytes diferentes (primeira diferença na posição %I64u).", (unsigned long long)diff, (unsigned long long)first);
        if (bad) Log(L"ATENÇÃO: %I64u setores ilegíveis durante a comparação.", (unsigned long long)bad);
    }
}

/* --------------------------------------------------------------------- jobs */
static BOOL DoCardToImage(Job *j) {
    BOOL ok = FALSE;
    HANDLE hd = INVALID_HANDLE_VALUE, hf = INVALID_HANDLE_VALUE;
    wchar_t sz[32], full[MAX_PATH], root[MAX_PATH];
    uint64_t diff = 0, first = 0;
    ULARGE_INTEGER fa;
    Dev in, out, chk;
    FmtSize(j->srcSize, sz, 32);
    Log(L"Criando imagem do Disco %d (%ls) em: %ls", j->src, sz, j->file);
    hd = OpenDisk(j->src, FALSE);
    if (hd == INVALID_HANDLE_VALUE) { LogErr(L"abrir o cartão"); goto done; }
    if (!DiskLenMatches(hd, j->srcSize)) { Log(L"ERRO: o tamanho do disco mudou. Atualize a lista e tente de novo."); goto done; }
    if (GetFullPathNameW(j->file, MAX_PATH, full, NULL) && GetVolumePathNameW(full, root, MAX_PATH) &&
        GetDiskFreeSpaceExW(root, &fa, NULL, NULL) && fa.QuadPart < j->srcSize) {
        wchar_t fs[32];
        FmtSize(fa.QuadPart, fs, 32);
        Log(L"ERRO: espaço livre insuficiente no destino (%ls livres, são necessários %ls).", fs, sz);
        goto done;
    }
    hf = CreateFileW(j->file, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (hf == INVALID_HANDLE_VALUE) { LogErr(L"criar o arquivo de imagem"); goto done; }
    in.h = hd; in.raw = TRUE; in.sec = j->srcSec;
    out.h = hf; out.raw = FALSE; out.sec = 0;
    g_bad = 0;
    ok = CopyDev(&in, &out, j->srcSize, j->verify ? L"Criando a imagem — lendo o cartão (1/2)" : L"Criando a imagem — lendo o cartão", &g_bad);
    if (ok) FlushFileBuffers(hf);
    CloseHandle(hf);
    hf = INVALID_HANDLE_VALUE;
    if (ok && j->verify) {
        hf = CreateFileW(j->file, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
        if (hf == INVALID_HANDLE_VALUE) { LogErr(L"reabrir a imagem para verificar"); ok = FALSE; goto done; }
        chk.h = hf; chk.raw = FALSE; chk.sec = 0;
        uint64_t bad2 = 0;
        ok = CompareDev(&in, &chk, j->srcSize, L"Verificando a imagem contra o cartão (2/2)", &diff, &first, &bad2);
        if (ok) { ReportCompare(diff, first, bad2); if (diff || bad2) ok = FALSE; }
    }
    if (ok) {
        if (g_bad) SetSummary(MB_ICONWARNING, L"Imagem criada, mas %I64u setores do cartão estavam ilegíveis (foram gravados como zeros).\n\n%ls", (unsigned long long)g_bad, j->file);
        else SetSummary(MB_ICONINFORMATION, L"Imagem criada com sucesso.\n\nArquivo: %ls\nTamanho: %ls", j->file, sz);
    }
done:
    if (hf != INVALID_HANDLE_VALUE) CloseHandle(hf);
    if (hd != INVALID_HANDLE_VALUE) CloseHandle(hd);
    if (!ok && g_summary[0] == 0) SetSummary(MB_ICONERROR, L"A criação da imagem não foi concluída. Veja o registro para detalhes.");
    if (!ok && !j->verify) DeleteFileW(j->file);
    return ok;
}

static BOOL DoImageToCard(Job *j) {
    BOOL ok = FALSE;
    HANDLE hf = INVALID_HANDLE_VALUE, hd = INVALID_HANDLE_VALUE;
    VolSet vs;
    vs.n = 0;
    LARGE_INTEGER fsz;
    uint64_t diff = 0, first = 0, bad2 = 0;
    wchar_t sz[32];
    Dev in, out;
    Log(L"Gravando a imagem no Disco %d: %ls", j->dst, j->file);
    hf = CreateFileW(j->file, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (hf == INVALID_HANDLE_VALUE) { LogErr(L"abrir a imagem"); goto done; }
    if (!GetFileSizeEx(hf, &fsz) || fsz.QuadPart <= 0) { Log(L"ERRO: arquivo de imagem vazio."); goto done; }
    if ((uint64_t)fsz.QuadPart > j->dstSize) { Log(L"ERRO: a imagem é maior que o cartão."); goto done; }
    if (!LockDiskVolumes(j->dst, &vs)) goto done;
    hd = OpenDisk(j->dst, TRUE);
    if (hd == INVALID_HANDLE_VALUE) { LogErr(L"abrir o cartão para escrita"); goto done; }
    if (!DiskLenMatches(hd, j->dstSize)) { Log(L"ERRO: o tamanho do disco mudou. Atualize a lista e tente de novo."); goto done; }
    in.h = hf; in.raw = FALSE; in.sec = 0;
    out.h = hd; out.raw = TRUE; out.sec = j->dstSec;
    g_bad = 0;
    ok = CopyDev(&in, &out, (uint64_t)fsz.QuadPart, j->verify ? L"Gravando a imagem no cartão (1/2)" : L"Gravando a imagem no cartão", &g_bad);
    if (ok) FlushFileBuffers(hd);
    if (ok && j->verify) {
        ok = CompareDev(&in, &out, (uint64_t)fsz.QuadPart, L"Verificando o que foi gravado (2/2)", &diff, &first, &bad2);
        if (ok) { ReportCompare(diff, first, bad2); if (diff || bad2) ok = FALSE; }
    }
    UpdateDiskProps(hd);
    if (ok) {
        FmtSize((uint64_t)fsz.QuadPart, sz, 32);
        Log(L"Gravação concluída. Se o Windows pedir para formatar o disco, clique em CANCELAR.");
        SetSummary(MB_ICONINFORMATION, L"Imagem gravada com sucesso (%ls)%ls.\n\nSe o Windows pedir para formatar o disco, clique em Cancelar.", sz, j->verify ? L" e verificada" : L"");
    }
done:
    if (hd != INVALID_HANDLE_VALUE) CloseHandle(hd);
    UnlockVolumes(&vs);
    if (hf != INVALID_HANDLE_VALUE) CloseHandle(hf);
    if (!ok && g_summary[0] == 0) SetSummary(MB_ICONERROR, L"A gravação não foi concluída com sucesso. O cartão pode estar em estado incompleto — veja o registro.");
    return ok;
}

static BOOL DoCardToCard(Job *j) {
    BOOL ok = FALSE;
    HANDLE hs = INVALID_HANDLE_VALUE, hd = INVALID_HANDLE_VALUE;
    VolSet vs;
    vs.n = 0;
    uint64_t diff = 0, first = 0, bad2 = 0;
    Dev in, out;
    Log(L"Clonando o Disco %d para o Disco %d.", j->src, j->dst);
    hs = OpenDisk(j->src, FALSE);
    if (hs == INVALID_HANDLE_VALUE) { LogErr(L"abrir o cartão de origem"); goto done; }
    if (!LockDiskVolumes(j->dst, &vs)) goto done;
    hd = OpenDisk(j->dst, TRUE);
    if (hd == INVALID_HANDLE_VALUE) { LogErr(L"abrir o cartão de destino para escrita"); goto done; }
    if (!DiskLenMatches(hs, j->srcSize) || !DiskLenMatches(hd, j->dstSize)) { Log(L"ERRO: o tamanho de um dos discos mudou. Atualize a lista."); goto done; }
    in.h = hs; in.raw = TRUE; in.sec = j->srcSec;
    out.h = hd; out.raw = TRUE; out.sec = j->dstSec;
    g_bad = 0;
    ok = CopyDev(&in, &out, j->srcSize, j->verify ? L"Copiando cartão para cartão (1/2)" : L"Copiando cartão para cartão", &g_bad);
    if (ok) FlushFileBuffers(hd);
    if (ok && j->verify) {
        ok = CompareDev(&in, &out, j->srcSize, L"Verificando a cópia (2/2)", &diff, &first, &bad2);
        if (ok) { ReportCompare(diff, first, bad2); if (diff || bad2) ok = FALSE; }
    }
    UpdateDiskProps(hd);
    if (ok) {
        if (j->dstSize > j->srcSize) Log(L"O destino é maior: o espaço extra ficou sem partição.");
        if (g_bad) SetSummary(MB_ICONWARNING, L"Cópia concluída, mas %I64u setores da origem estavam ilegíveis (copiados como zeros).", (unsigned long long)g_bad);
        else SetSummary(MB_ICONINFORMATION, L"Cartão clonado com sucesso%ls.\n\nSe o Windows pedir para formatar o disco, clique em Cancelar.", j->verify ? L" e verificado" : L"");
    }
done:
    if (hd != INVALID_HANDLE_VALUE) CloseHandle(hd);
    UnlockVolumes(&vs);
    if (hs != INVALID_HANDLE_VALUE) CloseHandle(hs);
    if (!ok && g_summary[0] == 0) SetSummary(MB_ICONERROR, L"A clonagem não foi concluída com sucesso. O destino pode estar incompleto — veja o registro.");
    return ok;
}

static BOOL DoVerify(Job *j) {
    BOOL ok = FALSE;
    HANDLE hd = INVALID_HANDLE_VALUE, hf = INVALID_HANDLE_VALUE;
    Dev card, file;
    uint64_t diff = 0, first = 0;
    double t0;
    hd = OpenDisk(j->src, FALSE);
    if (hd == INVALID_HANDLE_VALUE) { LogErr(L"abrir o cartão"); goto done; }
    if (!DiskLenMatches(hd, j->srcSize)) { Log(L"ERRO: o tamanho do disco mudou. Atualize a lista."); goto done; }
    card.h = hd; card.raw = TRUE; card.sec = j->srcSec;
    g_bad = 0;
    t0 = NowSec();
    if (j->mode == 1) {
        LARGE_INTEGER fsz;
        hf = CreateFileW(j->file, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
        if (hf == INVALID_HANDLE_VALUE) { LogErr(L"abrir a imagem"); goto done; }
        if (!GetFileSizeEx(hf, &fsz) || (uint64_t)fsz.QuadPart > j->srcSize || fsz.QuadPart <= 0) { Log(L"ERRO: a imagem está vazia ou é maior que o cartão."); goto done; }
        file.h = hf; file.raw = FALSE; file.sec = 0;
        Log(L"Comparando o cartão com a imagem (%I64d bytes)...", (long long)fsz.QuadPart);
        ok = CompareDev(&card, &file, (uint64_t)fsz.QuadPart, L"Comparando o cartão com a imagem", &diff, &first, &g_bad);
        if (ok) {
            ReportCompare(diff, first, g_bad);
            if (diff || g_bad) SetSummary(MB_ICONWARNING, L"O cartão NÃO é idêntico à imagem.\n\nBytes diferentes: %I64u\nSetores ilegíveis: %I64u", (unsigned long long)diff, (unsigned long long)g_bad);
            else SetSummary(MB_ICONINFORMATION, L"O cartão é IDÊNTICO à imagem.");
        }
    } else {
        Log(L"Lendo todo o cartão para procurar setores ilegíveis...");
        ok = CopyDev(&card, NULL, j->srcSize, L"Verificando o cartão (leitura completa)", &g_bad);
        if (ok) {
            double el = NowSec() - t0;
            wchar_t sz[32];
            FmtSize(j->srcSize, sz, 32);
            Log(L"Leitura concluída: %ls em %.0f s (média %.1f MB/s).", sz, el, (double)j->srcSize / 1048576.0 / (el > 0.01 ? el : 0.01));
            if (g_bad) {
                Log(L"ATENÇÃO: %I64u setores ilegíveis. O cartão está com defeito — copie o que puder e substitua-o.", (unsigned long long)g_bad);
                SetSummary(MB_ICONWARNING, L"Verificação concluída: %I64u setores ILEGÍVEIS encontrados.\nO cartão está com defeito.", (unsigned long long)g_bad);
            } else {
                Log(L"Nenhum erro de leitura. (Esta leitura não prova que a capacidade é verdadeira: use Autenticidade para isso.)");
                SetSummary(MB_ICONINFORMATION, L"Verificação concluída: nenhum erro de leitura em %ls.", sz);
            }
        }
    }
done:
    if (hf != INVALID_HANDLE_VALUE) CloseHandle(hf);
    if (hd != INVALID_HANDLE_VALUE) CloseHandle(hd);
    if (!ok && g_summary[0] == 0) SetSummary(MB_ICONERROR, L"A verificação não foi concluída. Veja o registro.");
    return ok;
}

/* teste de capacidade real (estilo H2testw, no nível do disco): APAGA o cartão */
static BOOL RunCapacity(Job *j, CapRes *r) {
    BOOL ok = FALSE;
    VolSet vs;
    vs.n = 0;
    HANDLE hd = INVALID_HANDLE_VALUE;
    uint8_t *buf = NULL, *ref = NULL;
    uint64_t size = j->srcSize, off;
    uint32_t sec = j->srcSec;
    uint64_t seed = (uint64_t)GetTickCount64() * 0x9E3779B97F4A7C15ULL ^ (uint64_t)GetCurrentProcessId();
    double t0, t1;
    memset(r, 0, sizeof *r);
    r->firstBad = (uint64_t)-1;
    buf = (uint8_t *)ALLOC_ALIGNED(CHUNK);
    ref = (uint8_t *)ALLOC_ALIGNED(CHUNK);
    if (!buf || !ref) { Log(L"ERRO: memória insuficiente."); goto done; }
    if (!LockDiskVolumes(j->src, &vs)) goto done;
    hd = OpenDisk(j->src, TRUE);
    if (hd == INVALID_HANDLE_VALUE) { LogErr(L"abrir o cartão para escrita"); goto done; }
    if (!DiskLenMatches(hd, size)) { Log(L"ERRO: o tamanho do disco mudou. Atualize a lista."); goto done; }
    SetPhase(L"Teste de capacidade: gravando dados de teste (1/2)", size);
    t0 = NowSec();
    for (off = 0; off < size;) {
        if (g_cancel) { Log(L"Operação cancelada. O cartão ficou com dados de teste — use Recuperar > Restaurar para reformatá-lo."); goto done; }
        DWORD len = (DWORD)MIN64(CHUNK, size - off);
        FillPattern(buf, off, len, seed);
        if (!WriteAtRaw(hd, off, buf, len)) { LogErr(L"falha ao gravar dados de teste"); goto done; }
        off += len;
        g_done = (LONGLONG)off;
    }
    FlushFileBuffers(hd);
    t1 = NowSec();
    r->wMBs = (double)size / 1048576.0 / (t1 - t0 > 0.01 ? t1 - t0 : 0.01);
    SetPhase(L"Teste de capacidade: lendo e comparando (2/2)", size);
    t0 = NowSec();
    for (off = 0; off < size;) {
        if (g_cancel) { Log(L"Operação cancelada."); goto done; }
        DWORD len = (DWORD)MIN64(CHUNK, size - off);
        uint64_t b = 0;
        ReadChunkRobust(hd, off, buf, len, sec, &b);
        FillPattern(ref, off, len, seed);
        for (DWORD o = 0; o < len; o += sec) {
            if (memcmp(buf + o, ref + o, sec) == 0) r->good += sec;
            else {
                r->badBytes += sec;
                if (r->firstBad == (uint64_t)-1) r->firstBad = off + o;
            }
        }
        off += len;
        g_done = (LONGLONG)off;
    }
    t1 = NowSec();
    r->rMBs = (double)size / 1048576.0 / (t1 - t0 > 0.01 ? t1 - t0 : 0.01);
    ok = TRUE;
done:
    if (hd != INVALID_HANDLE_VALUE) { UpdateDiskProps(hd); CloseHandle(hd); }
    UnlockVolumes(&vs);
    if (buf) FREE_ALIGNED(buf);
    if (ref) FREE_ALIGNED(ref);
    return ok;
}

static BOOL DoCapacity(Job *j) {
    CapRes r;
    wchar_t a[32], b[32];
    BOOL ok = RunCapacity(j, &r);
    if (ok) {
        FmtSize(j->srcSize, a, 32);
        FmtSize(r.good, b, 32);
        Log(L"Resultado: %ls corretos de %ls. Gravação %.1f MB/s, leitura %.1f MB/s.", b, a, r.wMBs, r.rMBs);
        if (r.badBytes == 0) {
            Log(L"CAPACIDADE VERDADEIRA: todos os dados voltaram idênticos.");
            SetSummary(MB_ICONINFORMATION, L"Capacidade VERDADEIRA: %ls testados, nenhum erro.\nGravação média %.1f MB/s, leitura %.1f MB/s.\n\nO cartão foi apagado; se necessário use Recuperar > Restaurar para formatá-lo.", a, r.wMBs, r.rMBs);
        } else {
            wchar_t fb[32];
            FmtSize(r.firstBad, fb, 32);
            Log(L"CARTÃO FALSO OU DEFEITUOSO: %ls de dados corrompidos; primeiro erro em %ls.", a, fb);
            SetSummary(MB_ICONERROR, L"CARTÃO FALSO ou DEFEITUOSO!\n\nAnunciado: %ls\nRealmente utilizável: cerca de %ls\n\nO cartão foi apagado; use Recuperar > Restaurar para formatá-lo.", a, b);
        }
    } else if (g_summary[0] == 0) SetSummary(MB_ICONERROR, L"O teste de capacidade não foi concluído. Veja o registro.");
    return ok;
}

/* ---------------------------------------------------- identificação (CID) */
/* Cartões em slot SD nativo aparecem no barramento "SD" com ID de hardware SD\VID_xx&OID_xxxx&PID_nome&REV_x.x */
static int ReadSdBusCards(SdId *out, int max) {
    int n = 0;
    HDEVINFO h = SetupDiGetClassDevsW(NULL, L"SD", NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (h == INVALID_HANDLE_VALUE) return 0;
    SP_DEVINFO_DATA d;
    d.cbSize = sizeof d;
    for (DWORD i = 0; n < max && SetupDiEnumDeviceInfo(h, i, &d); i++) {
        wchar_t ids[2048];
        DWORD type = 0, need = 0;
        if (!SetupDiGetDeviceRegistryPropertyW(h, &d, SPDRP_HARDWAREID, &type, (PBYTE)ids, sizeof ids - 4, &need)) continue;
        ids[2047] = 0;
        for (wchar_t *s = ids; *s; s += wcslen(s) + 1) {
            wchar_t *v = wcsstr(s, L"VID_"), *o = wcsstr(s, L"&OID_"), *p = wcsstr(s, L"&PID_"), *r = wcsstr(s, L"&REV_");
            if (!v || !o || !p) continue;
            SdId id;
            memset(&id, 0, sizeof id);
            id.mid = (int)wcstoul(v + 4, NULL, 16);
            size_t k = 0;
            for (wchar_t *c = o + 5; *c && *c != L'&' && k < 7; c++) id.oid[k++] = *c;
            k = 0;
            for (wchar_t *c = p + 5; *c && *c != L'&' && k < 23; c++) id.pnm[k++] = *c;
            if (r) { k = 0; for (wchar_t *c = r + 5; *c && *c != L'&' && k < 15; c++) id.rev[k++] = *c; }
            out[n++] = id;
            break;
        }
    }
    SetupDiDestroyDeviceInfoList(h);
    return n;
}

static void OidToText(const wchar_t *hex4, wchar_t *out) {
    unsigned v = (unsigned)wcstoul(hex4, NULL, 16);
    wchar_t a = (wchar_t)((v >> 8) & 0xFF), b = (wchar_t)(v & 0xFF);
    out[0] = (a >= 32 && a < 127) ? a : L'?';
    out[1] = (b >= 32 && b < 127) ? b : L'?';
    out[2] = 0;
}

static double SampleRead(HANDLE h, uint64_t off, uint64_t bytes, uint32_t sec, uint8_t *buf) {
    double t0 = NowSec();
    uint64_t done = 0, bad = 0;
    while (done < bytes) {
        if (g_cancel) return -1;
        DWORD want = (DWORD)MIN64(CHUNK, bytes - done);
        ReadChunkRobust(h, off + done, buf, want, sec, &bad);
        done += want;
        g_done += want;
    }
    double el = NowSec() - t0;
    return (double)bytes / 1048576.0 / (el > 0.001 ? el : 0.001);
}

static BOOL DoAuth(Job *j) {
    BOOL ok = TRUE;
    wchar_t sz[32], line[512];
    int strongSuspect = 0, hint = 0;
    wchar_t verdictBrand[400] = L"", verdictCap[400] = L"", verdictSpeed[400] = L"", verdictMid[400] = L"";
    SdId ids[4];
    int nid;
    FmtSize(j->srcSize, sz, 32);
    Log(L"=== Dados informados pelo dispositivo ===");
    Log(L"Modelo: %ls %ls | Tamanho: %ls | Setor: %u bytes | Removível: %ls", j->di.vendor, j->di.product, sz, (unsigned)j->srcSec, j->di.removable ? L"sim" : L"não");
    if (j->di.vendor[0] == 0 || wcsstr(j->di.vendor, L"Generic") || wcsstr(j->di.product, L"STORAGE DEVICE") || wcsstr(j->di.product, L"SD/MMC"))
        Log(L"Nota: esse nome é do LEITOR de cartões (genérico), não do cartão.");

    Log(L"=== Identificação gravada no cartão (registro CID) ===");
    nid = ReadSdBusCards(ids, 4);
    if (nid == 0) {
        Log(L"Não foi possível ler o CID: leitores USB escondem esses dados. Use um slot SD nativo do notebook/PC para isso.");
        Log(L"Atenção: o fabricante do CHIP de memória (NAND) não é gravado no cartão; o CID só informa quem fabricou/etiquetou o cartão.");
        wcscpy(verdictMid, L"Fabricante gravado no cartão: não lido (leitor USB). ");
    } else {
        for (int i = 0; i < nid; i++) {
            wchar_t oid[4];
            OidToText(ids[i].oid, oid);
            const wchar_t *mn = MidName(ids[i].mid);
            Log(L"Cartão %d no barramento SD: MID=0x%02X (%ls) | OEM=%ls | Produto=%ls | Rev=%ls", i + 1, ids[i].mid,
                mn ? mn : L"fabricante NÃO listado — típico de cartão genérico/clone", oid, ids[i].pnm, ids[i].rev);
        }
        if (nid > 1) Log(L"Há mais de um cartão em slots SD; a verificação de marca usa o primeiro (%d).", 1);
        const wchar_t *mn = MidName(ids[0].mid);
        _snwprintf(verdictMid, 399, L"Fabricante gravado no cartão: %ls (MID 0x%02X). ", mn ? mn : L"não listado (genérico/clone)", ids[0].mid);
        if (j->brand > 0 && j->brand < NBRANDS - 1) {
            int match = 0;
            for (int k = 0; k < 4 && BRANDS[j->brand].mids[k] >= 0; k++)
                if (BRANDS[j->brand].mids[k] == ids[0].mid) match = 1;
            if (match) {
                _snwprintf(verdictBrand, 399, L"A marca da etiqueta (%ls) CONFERE com o fabricante gravado no cartão. ", BRANDS[j->brand].name);
            } else if (BRANDS[j->brand].strong) {
                _snwprintf(verdictBrand, 399, L"SUSPEITO: a etiqueta diz %ls, mas o cartão se identifica como %ls — comum em falsificações. ", BRANDS[j->brand].name, mn ? mn : L"fabricante desconhecido");
                strongSuspect = 1;
            } else {
                _snwprintf(verdictBrand, 399, L"Indício: a etiqueta diz %ls, mas o cartão se identifica como %ls (algumas marcas usam controladores de terceiros, confirme com o teste completo). ", BRANDS[j->brand].name, mn ? mn : L"fabricante desconhecido");
                hint = 1;
            }
        }
    }

    if (j->mode == 2) {
        Log(L"=== Teste completo de capacidade e velocidade ===");
        CapRes r;
        ok = RunCapacity(j, &r);
        if (!ok) { if (g_summary[0] == 0) SetSummary(MB_ICONERROR, L"O teste não foi concluído. Veja o registro."); return FALSE; }
        wchar_t b[32];
        FmtSize(r.good, b, 32);
        Log(L"Corretos: %ls de %ls | Gravação média %.1f MB/s | Leitura média %.1f MB/s", b, sz, r.wMBs, r.rMBs);
        if (r.badBytes) {
            _snwprintf(verdictCap, 399, L"CAPACIDADE FALSA: anunciado %ls, real cerca de %ls. ", sz, b);
            strongSuspect = 1;
        } else _snwprintf(verdictCap, 399, L"Capacidade VERDADEIRA (%ls testados sem erro). ", sz);
        if (j->cls > 0) {
            if (r.wMBs < j->cls * 0.7)
                _snwprintf(verdictSpeed, 399, L"Velocidade de gravação (%.1f MB/s) abaixo do mínimo da classe anunciada (%d MB/s) — pode ser falsificação, cartão desgastado ou leitor USB lento. ", r.wMBs, j->cls);
            else _snwprintf(verdictSpeed, 399, L"Velocidade (%.1f MB/s) compatível com a classe anunciada. ", r.wMBs);
        } else _snwprintf(verdictSpeed, 399, L"Gravação %.1f MB/s, leitura %.1f MB/s. ", r.wMBs, r.rMBs);
    } else {
        Log(L"=== Amostra de velocidade de leitura (não destrutiva) ===");
        HANDLE hd = OpenDisk(j->src, FALSE);
        uint8_t *buf = (uint8_t *)ALLOC_ALIGNED(CHUNK);
        if (hd == INVALID_HANDLE_VALUE || !buf) { LogErr(L"abrir o cartão"); if (buf) FREE_ALIGNED(buf); if (hd != INVALID_HANDLE_VALUE) CloseHandle(hd); SetSummary(MB_ICONERROR, L"Não foi possível abrir o cartão."); return FALSE; }
        uint64_t samp = MIN64(64ull << 20, j->srcSize / 3);
        samp = (samp / CHUNK) * CHUNK;
        if (samp < CHUNK) samp = CHUNK;
        SetPhase(L"Medindo velocidade de leitura (início, meio e fim)", samp * 3);
        uint64_t aligned = (j->srcSize / j->srcSec) * j->srcSec;
        uint64_t offs[3] = {0, ((aligned / 2) / CHUNK) * CHUNK, ((aligned - samp) / CHUNK) * CHUNK};
        const wchar_t *nm[3] = {L"início", L"meio", L"fim"};
        double sp[3] = {0, 0, 0};
        for (int i = 0; i < 3; i++) {
            sp[i] = SampleRead(hd, offs[i], samp, j->srcSec, buf);
            if (sp[i] < 0) { Log(L"Cancelado."); ok = FALSE; break; }
            Log(L"Leitura no %ls do cartão: %.1f MB/s", nm[i], sp[i]);
        }
        FREE_ALIGNED(buf);
        CloseHandle(hd);
        if (!ok) { SetSummary(MB_ICONWARNING, L"Teste cancelado."); return FALSE; }
        _snwprintf(verdictSpeed, 399, L"Leitura: %.1f / %.1f / %.1f MB/s (início/meio/fim). ", sp[0], sp[1], sp[2]);
        _snwprintf(verdictCap, 399, L"Capacidade NÃO verificada (o teste rápido não consegue provar isso — marque o teste completo). ");
    }

    const wchar_t *head;
    UINT icon;
    if (strongSuspect) { head = L"PROVÁVEL FALSIFICAÇÃO / CARTÃO FALSO"; icon = MB_ICONERROR; }
    else if (hint) { head = L"ATENÇÃO — há indícios, confirme com o teste completo"; icon = MB_ICONWARNING; }
    else if (j->mode == 2) { head = L"Sem indícios de falsificação"; icon = MB_ICONINFORMATION; }
    else { head = L"Teste rápido concluído (sem conclusão sobre capacidade)"; icon = MB_ICONINFORMATION; }
    _snwprintf(line, 511, L"RESULTADO: %ls", head);
    Log(L"%ls", line);
    Log(L"%ls%ls%ls%ls", verdictMid, verdictBrand, verdictCap, verdictSpeed);
    SetSummary(icon, L"%ls\n\n%ls%ls\n%ls%ls%ls\n\nObservação: o chip de memória (NAND) não é identificável por software; só o fabricante/etiqueta do cartão.", head, verdictMid, verdictBrand, verdictCap, verdictSpeed, j->mode == 2 ? L"\n\nO cartão foi apagado; use Recuperar > Restaurar para formatá-lo." : L"");
    return TRUE;
}

/* ------------------------------------------------------------- recuperação */
typedef struct {
    Dev *d;
    wchar_t folder[MAX_PATH];
    int count, next, fail, logged;
    uint64_t bytes;
    uint8_t *buf;
} RecCtx;

static int RecRead(void *c, uint64_t off, void *buf, uint32_t len) {
    RecCtx *r = (RecCtx *)c;
    ReadChunkRobust(r->d->h, off, (uint8_t *)buf, len, r->d->sec, &g_bad);
    return 1;
}

static void RecEmit(void *c, int type, uint64_t start, uint64_t len) {
    RecCtx *r = (RecCtx *)c;
    static const wchar_t *ext[] = {L"jpg", L"png", L"pdf", L"mp4", L"mov"};
    wchar_t fn[MAX_PATH];
    HANDLE hf = INVALID_HANDLE_VALUE;
    if (r->fail) return;
    for (;;) {
        _snwprintf(fn, MAX_PATH - 1, L"%ls\\recuperado_%06d.%ls", r->folder, r->next, ext[type]);
        fn[MAX_PATH - 1] = 0;
        hf = CreateFileW(fn, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hf != INVALID_HANDLE_VALUE) break;
        DWORD e = GetLastError();
        if (e == ERROR_FILE_EXISTS || e == ERROR_ALREADY_EXISTS) { r->next++; continue; }
        LogErr(L"criar arquivo recuperado");
        r->fail = 1;
        return;
    }
    r->next++;
    uint64_t done = 0;
    while (done < len) {
        DWORD want = (DWORD)MIN64(CHUNK, len - done);
        DevRead(r->d, start + done, r->buf, want, &g_bad);
        DWORD w = 0;
        if (!WriteFile(hf, r->buf, want, &w, NULL) || w != want) { LogErr(L"gravar arquivo recuperado (disco cheio?)"); r->fail = 1; break; }
        done += want;
    }
    CloseHandle(hf);
    r->count++;
    r->bytes += len;
    if (r->logged < 300) {
        r->logged++;
        wchar_t sz[32];
        FmtSize(len, sz, 32);
        Log(L"Encontrado: %ls (%ls) na posição %I64u", wcsrchr(fn, L'\\') + 1, sz, (unsigned long long)start);
    }
    wchar_t ph[200];
    _snwprintf(ph, 199, L"Procurando arquivos no cartão — %d encontrados", r->count);
    ph[199] = 0;
    SetPhaseText(ph);
}

static int RecProg(void *c, uint64_t pos) {
    g_done = (LONGLONG)pos;
    return ((RecCtx *)c)->fail || g_cancel;
}

static BOOL DoRecover(Job *j) {
    BOOL ok = FALSE, fromImg = j->img[0] != 0;
    HANDLE hd = fromImg ? CreateFileW(j->img, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL) : OpenDisk(j->src, FALSE);
    RecCtx r;
    Dev d;
    memset(&r, 0, sizeof r);
    if (hd == INVALID_HANDLE_VALUE) { LogErr(fromImg ? L"abrir o arquivo de imagem" : L"abrir o cartão"); goto done; }
    if (fromImg) {
        LARGE_INTEGER fsz;
        if (!GetFileSizeEx(hd, &fsz) || fsz.QuadPart < 512) { Log(L"ERRO: arquivo de imagem vazio ou ilegível."); goto done; }
        j->srcSize = (uint64_t)fsz.QuadPart & ~(uint64_t)511;
        j->srcSec = 512;
        Log(L"Lendo do arquivo de imagem: %ls", j->img);
    } else if (!DiskLenMatches(hd, j->srcSize)) { Log(L"ERRO: o tamanho do disco mudou. Atualize a lista."); goto done; }
    d.h = hd; d.raw = TRUE; d.sec = j->srcSec;
    r.d = &d;
    r.next = 1;
    wcsncpy(r.folder, j->file, MAX_PATH - 1);
    {
        size_t l = wcslen(r.folder);
        while (l > 3 && (r.folder[l - 1] == L'\\' || r.folder[l - 1] == L'/')) r.folder[--l] = 0;
    }
    r.buf = (uint8_t *)ALLOC_ALIGNED(CHUNK);
    if (!r.buf) { Log(L"ERRO: memória insuficiente."); goto done; }
    Log(L"Procurando JPG, PNG, PDF e vídeos MP4/MOV%ls. Os arquivos vão para: %ls", fromImg ? L" na imagem" : L" no cartão", r.folder);
    Log(L"Os nomes originais não existem mais; os arquivos recebem nomes numerados. Esta operação só LÊ o cartão.");
    SetPhase(L"Procurando arquivos no cartão — 0 encontrados", j->srcSize);
    g_bad = 0;
    int n = Carve(RecRead, &r, j->srcSize, j->srcSec, RecEmit, &r, RecProg, &r);
    if (n == -2) { Log(L"ERRO: memória insuficiente."); goto done; }
    if (n < 0 && !r.fail) { Log(L"Busca cancelada. %d arquivos já recuperados ficam na pasta.", r.count); }
    ok = (n >= 0) && !r.fail;
    {
        wchar_t sz[32];
        FmtSize(r.bytes, sz, 32);
        Log(L"Busca %ls: %d arquivos recuperados (%ls).", ok ? L"concluída" : L"interrompida", r.count, sz);
        if (g_bad) Log(L"Aviso: %I64u setores ilegíveis foram lidos como zeros.", (unsigned long long)g_bad);
        if (ok) SetSummary(r.count ? MB_ICONINFORMATION : MB_ICONWARNING,
                           r.count ? L"%d arquivos recuperados (%ls).\n\nPasta: %ls" : L"Nenhum arquivo recuperável foi encontrado (%d).%ls\n\nSe o cartão foi formatado/regravado há muito tempo, os dados podem ter sido sobrescritos.", r.count, r.count ? sz : L"", r.folder);
    }
done:
    if (r.buf) FREE_ALIGNED(r.buf);
    if (hd != INVALID_HANDLE_VALUE) CloseHandle(hd);
    if (!ok && g_summary[0] == 0) SetSummary(MB_ICONWARNING, L"A recuperação foi interrompida. Veja o registro.");
    return ok;
}

static BOOL RunHidden(wchar_t *cmd, DWORD *exitCode) {
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    HANDLE rd = NULL, wr = NULL;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return FALSE;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = INVALID_HANDLE_VALUE;
    if (!CreateProcessW(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        CloseHandle(rd); CloseHandle(wr);
        return FALSE;
    }
    CloseHandle(wr);
    char buf[2048], line[1024];
    int ll = 0;
    DWORD got;
    while (ReadFile(rd, buf, sizeof buf, &got, NULL) && got > 0) {
        for (DWORD i = 0; i < got; i++) {
            char c = buf[i];
            if (c == '\n' || ll >= 1000) {
                line[ll] = 0;
                while (ll > 0 && (line[ll - 1] == '\r' || line[ll - 1] == ' ')) line[--ll] = 0;
                if (ll > 0) {
                    wchar_t w[1100];
                    MultiByteToWideChar(CP_OEMCP, 0, line, -1, w, 1099);
                    Log(L"diskpart: %ls", w);
                }
                ll = 0;
            } else line[ll++] = c;
        }
    }
    if (ll > 0) { line[ll] = 0; wchar_t w[1100]; MultiByteToWideChar(CP_OEMCP, 0, line, -1, w, 1099); Log(L"diskpart: %ls", w); }
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, 600000);
    GetExitCodeProcess(pi.hProcess, exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return TRUE;
}

static BOOL FormatDisk(int disk, const char *fs, const char *label) {
    wchar_t tmpdir[MAX_PATH], tmp[MAX_PATH], cmd[MAX_PATH + 64];
    char script[700];
    HANDLE hf;
    DWORD w, code = 1;
    SetIndet(L"Formatando o cartão (limpar, particionar e formatar)...");
    _snprintf(script, sizeof script - 1,
              "select disk %d\r\nattributes disk clear readonly noerr\r\nclean\r\ncreate partition primary\r\nformat fs=%s quick label=%s\r\nassign\r\nexit\r\n",
              disk, fs, label);
    script[sizeof script - 1] = 0;
    GetTempPathW(MAX_PATH, tmpdir);
    GetTempFileNameW(tmpdir, L"scd", 0, tmp);
    hf = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE) { LogErr(L"criar script temporário"); return FALSE; }
    WriteFile(hf, script, (DWORD)strlen(script), &w, NULL);
    CloseHandle(hf);
    Log(L"Formatando o Disco %d com %ls (rápido). Aguarde, não remova o cartão...", disk, fs[0] == 'f' ? L"FAT32" : L"exFAT");
    _snwprintf(cmd, MAX_PATH + 63, L"diskpart.exe /s \"%ls\"", tmp);
    cmd[MAX_PATH + 63] = 0;
    if (!RunHidden(cmd, &code)) { LogErr(L"executar o diskpart"); DeleteFileW(tmp); return FALSE; }
    DeleteFileW(tmp);
    if (code != 0) Log(L"ERRO: o diskpart terminou com código %lu. Veja as mensagens acima.", (unsigned long)code);
    return code == 0;
}

static BOOL DoRestore(Job *j) {
    BOOL ok = FALSE;
    const char *fs = j->srcSize <= 34359738368ULL ? "fat32" : "exfat";
    if (j->src == g_sysDisk) { Log(L"ERRO: disco do sistema — recusado."); goto done; }
    ok = FormatDisk(j->src, fs, "SDCARD");
    if (ok) {
        Log(L"Restauração concluída. O cartão foi reparticionado e formatado com o nome SDCARD.");
        SetSummary(MB_ICONINFORMATION, L"Cartão restaurado e formatado (%ls, nome SDCARD).\n\nSe ele ainda mostrar capacidade menor que a esperada, use Autenticidade: pode ser um cartão falso.", fs[0] == 'f' ? L"FAT32" : L"exFAT");
    }
done:
    if (!ok && g_summary[0] == 0) SetSummary(MB_ICONERROR, L"Não foi possível restaurar o cartão. Veja o registro.");
    return ok;
}

#include "repair.h"

/* ------------------------------------------------------------ diagnóstico com veredito
 * 1. lê o início do cartão (MBR/GPT);
 * 2. lê 128 pontos espalhados do início ao fim e classifica cada um (com dados, vazio, lento, ilegível);
 * 3. (opcional) testa a gravação em 2 pontos: grava o conteúdo invertido, lê de volta e restaura o original.
 *    Isso separa: gravação recusada (controladora travada em somente leitura), gravação ignorada
 *    (o cartão finge que gravou) e gravação corrompida (memória com defeito);
 * 4. junta tudo num veredito em linguagem simples. */
static int IsFill(const uint8_t *b, DWORD n, uint8_t v) {
    for (DWORD i = 0; i < n; i++)
        if (b[i] != v) return 0;
    return 1;
}

static void VSet(wchar_t *dst, size_t n, const wchar_t *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(dst, n - 1, fmt, ap);
    va_end(ap);
    dst[n - 1] = 0;
}
#define VS(field, ...) VSet(v->field, sizeof v->field / sizeof(wchar_t), __VA_ARGS__)

/* ---------------------------------------------------- conteúdo do cartão (aba "Conteúdo") */
typedef struct { BOOL valid; int disk; uint64_t size; BenchRes br; BenchJudge bj; } BenchMem;
static BenchMem g_bm; /* última medição de desempenho (usada para dizer se o cartão serve para o uso encontrado) */

static void FmtNum(double v, int dec, wchar_t *out, size_t n) {
    _snwprintf(out, n - 1, dec ? L"%.1f" : L"%.0f", v);
    out[n - 1] = 0;
    for (wchar_t *c = out; *c; c++) if (*c == L'.') *c = L',';
}

static int UsTickCb(void *c, const UsageStat *u) {
    wchar_t ph[160];
    (void)c;
    _snwprintf(ph, 159, L"Lendo o conteúdo — %u arquivos, %u pastas", (unsigned)u->files, (unsigned)u->dirs);
    ph[159] = 0;
    SetPhaseText(ph);
    return g_cancel ? 1 : 0;
}

static const wchar_t *ProfileTitle(int p) {
    static const wchar_t *t[] = {L"Cartão quase vazio", L"Cartão de console ou game stick (sistema e jogos)", L"Sistema Linux de placa ou console",
                                 L"Cartão de jogos e emulação", L"Cartão de câmera (fotos e vídeos)", L"Cartão de vídeos", L"Cartão de músicas",
                                 L"Cartão de documentos", L"Uso misto, sem um tipo dominante"};
    return t[p];
}

static void BuildUsageText(Verdict *v) {
    UsageStat *u = &v->us;
    wchar_t a[32], b[32], tmp[160];
    FmtSize(u->usedBytes, a, 32);
    FmtSize(v->uCard, b, 32);
    int pct = v->uCard ? (int)(u->usedBytes * 100 / v->uCard) : 0;
    VS(uOcc, L"%ls de %ls (%d%%) em %u arquivos e %u pastas.%ls%ls", a, b, pct, (unsigned)u->files, (unsigned)u->dirs,
       u->partial ? L" Leitura parcial." : L"", u->skippedParts ? L" Há partições que o app não abre (NTFS, criptografada ou desconhecida); o conteúdo delas não entrou na conta." : L"");
    /* extensões */
    v->uTop[0] = 0;
    int nshow = 0;
    for (int i = 0; i < u->next && nshow < 8; i++) {
        wchar_t sz[32];
        FmtSize(u->ext[i].bytes, sz, 32);
        _snwprintf(tmp, 159, L"%ls.%hs (%u · %ls)", nshow ? L"   " : L"", u->ext[i].ext, (unsigned)u->ext[i].files, sz);
        tmp[159] = 0;
        if (wcslen(v->uTop) + wcslen(tmp) < 395) wcscat(v->uTop, tmp);
        nshow++;
    }
    if (!nshow) wcscpy(v->uTop, L"Nenhum arquivo com extensão encontrado.");
    /* tipo de uso */
    int idx[CT_N];
    for (int i = 0; i < CT_N; i++) idx[i] = i;
    for (int i = 0; i < CT_N; i++)
        for (int k = i + 1; k < CT_N; k++)
            if (u->catBytes[idx[k]] > u->catBytes[idx[i]]) { int t = idx[i]; idx[i] = idx[k]; idx[k] = t; }
    v->uUse[0] = 0;
    double tot = (double)u->usedBytes;
    for (int i = 0; i < 3 && tot > 0; i++) {
        if (u->catBytes[idx[i]] == 0) break;
        _snwprintf(tmp, 159, L"%ls%ls %d%%", i ? L", " : L"", CatName(idx[i]), (int)((double)u->catBytes[idx[i]] * 100.0 / tot + 0.5));
        tmp[159] = 0;
        wcscat(v->uUse, tmp);
    }
    if (v->uUse[0]) wcscat(v->uUse, L". ");
    if (u->hasRoms) wcscat(v->uUse, L"Tem pasta de ROMs. ");
    if (u->hasEmu) wcscat(v->uUse, L"Tem pasta de emulador (RetroArch, BIOS, saves...). ");
    if (u->hasLinux) wcscat(v->uUse, L"Tem sistema Linux (partição ext4). ");
    if (u->hasDcim) wcscat(v->uUse, L"Tem pasta de câmera (DCIM). ");
    if (u->hasMusicDir) wcscat(v->uUse, L"Tem pasta de música. ");
}

/* "serve para esse uso?" junta o perfil, o diagnóstico de saúde e (se houver) a medição de desempenho */
static void BuildFitText(Verdict *v, int disk, uint64_t size) {
    int p = v->uProfile;
    BOOL bm = g_bm.valid && g_bm.disk == disk && g_bm.size == size;
    wchar_t t[520] = L"", x[64];
    if (v->level >= 2) wcscat(t, L"Não. O diagnóstico encontrou um defeito grave (veja a aba Saúde); use o cartão só para ler e copiar os dados. ");
    else if (v->level == 1) wcscat(t, L"Com ressalvas: o diagnóstico encontrou um problema (veja a aba Saúde). ");
    if (p == UP_GAMES || p == UP_CONSOLE) {
        if (bm) {
            FmtNum(g_bm.br.seqR, 0, x, 64);
            if (g_bm.bj.game == 2) { wcscat(t, L"Sim para jogos. Leitura de "); wcscat(t, x); wcscat(t, L" MB/s e acesso aleatório rápido."); }
            else if (g_bm.bj.game == 1) { wcscat(t, L"Serve para jogos leves (8 e 16 bits). Pode travar em jogos pesados (PSP, Dreamcast...). Leitura de "); wcscat(t, x); wcscat(t, L" MB/s."); }
            else wcscat(t, L"Fraco para jogos: a leitura ou o acesso aleatório é lento e causa travadas e demora para carregar.");
        } else wcscat(t, L"Jogos pedem leitura rápida e acesso aleatório bom. Rode Desempenho (menu à esquerda) para confirmar.");
    } else if (p == UP_CAMERA || p == UP_VIDEO) {
        if (bm && g_bm.bj.media >= 0) {
            FmtNum(g_bm.br.seqW, 0, x, 64);
            if (g_bm.bj.media >= 3) { wcscat(t, L"Sim, inclusive vídeo 4K. Gravação de "); wcscat(t, x); wcscat(t, L" MB/s."); }
            else if (g_bm.bj.media == 2) { wcscat(t, L"Sim para fotos e vídeo Full HD; não para 4K. Gravação de "); wcscat(t, x); wcscat(t, L" MB/s."); }
            else if (g_bm.bj.media == 1) { wcscat(t, L"Só para fotos e vídeo básico. Gravação de "); wcscat(t, x); wcscat(t, L" MB/s."); }
            else wcscat(t, L"Não: a gravação é lenta demais e a câmera vai parar ou perder quadros.");
        } else wcscat(t, L"Câmera pede gravação contínua (Full HD: 10 MB/s; 4K: 30 MB/s). Rode Desempenho com o teste de gravação para confirmar.");
    } else if (p == UP_MUSIC || p == UP_DOCS) {
        if (bm && g_bm.bj.trust == BT_BAD) wcscat(t, L"Funciona para ler, mas a medição mostrou instabilidade: não confie nele para guardar arquivos únicos.");
        else wcscat(t, L"Sim. Esse uso pede pouco; qualquer cartão sem defeito serve.");
    } else {
        if (bm) wcscat(t, g_bm.bj.trust == BT_OK ? L"Desempenho dentro do esperado para uso geral." : L"A medição mostrou desempenho irregular: use só para dados que você possa perder.");
        else wcscat(t, L"Sem uso definido. Rode Desempenho para saber para que o cartão presta.");
    }
    VS(uFit, L"%ls", t);
    int fl = 3;
    if (p == UP_GAMES || p == UP_CONSOLE) { if (bm) fl = g_bm.bj.game == 2 ? 0 : (g_bm.bj.game == 1 ? 1 : 2); }
    else if (p == UP_CAMERA || p == UP_VIDEO) { if (bm && g_bm.bj.media >= 0) fl = g_bm.bj.media >= 2 ? 0 : (g_bm.bj.media == 1 ? 1 : 2); }
    else if (p == UP_MUSIC || p == UP_DOCS) fl = (bm && g_bm.bj.trust == BT_BAD) ? 1 : 0;
    else if (bm) fl = g_bm.bj.trust == BT_OK ? 0 : 1;
    if (v->level >= 2) fl = 2;
    else if (v->level == 1 && fl != 2) fl = 1;
    v->uFitLevel = fl;
}

static void ScanContent(Job *j, HANDLE hd, Verdict *v) {
    Dev2 d;
    Diag *g = (Diag *)calloc(1, sizeof *g);
    if (!g) return;
    if (!Dev2Init(&d, StrictRd, hd, j->srcSize, j->srcSec ? j->srcSec : 512)) { free(g); return; }
    SetIndet(L"Lendo o conteúdo do cartão...");
    DiagAnalyze(&d, g, 0, NULL, NULL);
    UsageStat *u = &v->us;
    memset(u, 0, sizeof *u);
    for (int i = 0; i < g->nparts && !g_cancel; i++) {
        Part *p = &g->parts[i];
        if (p->flags & PF_EXTENDED) continue;
        if (p->start >= j->srcSize) continue;
        if (!FsBrowsable(p->fs)) {
            if (p->fs == FST_NTFS || p->fs == FST_BITLOCKER || p->fs == FST_LUKS) u->skippedParts++;
            continue;
        }
        UsageScanPart(&d, p->start, p->size, u, UsTickCb, NULL);
    }
    if (g_cancel) u->partial = 1;
    if (u->scannedParts > 0) {
        UsageSortExt(u);
        v->usage = TRUE;
        v->uCard = j->srcSize;
        v->uProfile = UsageProfile(u);
        VS(uTitle, L"%ls", ProfileTitle(v->uProfile));
        BuildUsageText(v);
        wchar_t a[32];
        FmtSize(u->usedBytes, a, 32);
        Log(L"Conteúdo: %ls em %u arquivos e %u pastas. Perfil: %ls.", a, (unsigned)u->files, (unsigned)u->dirs, v->uTitle);
        for (int c = 0; c < CT_N; c++)
            if (u->catFiles[c]) {
                FmtSize(u->catBytes[c], a, 32);
                Log(L"  %ls: %u arquivos, %ls", CatName(c), (unsigned)u->catFiles[c], a);
            }
    } else Log(L"Conteúdo: nenhuma partição com sistema de arquivos legível (FAT, exFAT ou ext).");
    Dev2Free(&d);
    free(g);
}

static BOOL DoHealth(Job *j) {
    Verdict *v = &g_vd;
    memset(v, 0, sizeof *v);
    v->op = OP_DIAG;
    v->map = TRUE;
    uint32_t sec = j->srcSec ? j->srcSec : 512;
    uint64_t size = j->srcSize;
    DWORD blk = 64 * 1024;
    if (sec > 4096 || blk % sec) blk = sec * 16;
    DWORD tl = sec > 4096 ? sec : 4096;
    uint8_t *buf = (uint8_t *)ALLOC_ALIGNED(blk), *pat = (uint8_t *)ALLOC_ALIGNED(blk), *rb = (uint8_t *)ALLOC_ALIGNED(blk);
    HANDLE hd = INVALID_HANDLE_VALUE, hw = INVALID_HANDLE_VALUE;
    VolSet vs;
    memset(&vs, 0, sizeof vs);
    BOOL locked = FALSE, opened = FALSE, mbr = FALSE, gpt = FALSE, r0 = FALSE;
    int wr = -1; /* -1 não testado, 0 ok, 1 recusada, 2 ignorada, 3 corrompida */
    DWORD wrErr = 0;
    wchar_t sz[32];
    FmtSize(size, sz, 32);
    if (!buf || !pat || !rb) { Log(L"ERRO: memória insuficiente."); goto out; }
    Log(L"Diagnóstico do Disco %d: %ls, setor de %u bytes.", j->src, sz, sec);

    hd = OpenDisk(j->src, FALSE);
    if (hd != INVALID_HANDLE_VALUE && DiskLenMatches(hd, size)) opened = TRUE;
    else if (hd == INVALID_HANDLE_VALUE) LogErr(L"abrir o cartão");
    else Log(L"ERRO: o tamanho do disco mudou. Clique em Atualizar e tente de novo.");

    if (opened) {
        /* 1. tabela de partições */
        g_step = 0;
        g_mapDone = 0;
        SetIndet(L"Lendo a tabela de partições...");
        if (ReadAtRaw(hd, 0, buf, sec * 2 <= blk ? sec * 2 : blk)) {
            r0 = TRUE;
            if (buf[510] == 0x55 && buf[511] == 0xAA)
                for (int i = 0; i < 4; i++)
                    if (buf[446 + 16 * i + 4]) mbr = TRUE;
            if (memcmp(buf + sec, "EFI PART", 8) == 0) gpt = TRUE;
        }
        Log(!r0 ? L"Não foi possível ler o início do cartão." : (gpt ? L"Tabela de partições: GPT." : (mbr ? L"Tabela de partições: MBR." : L"Tabela de partições: nenhuma (cartão RAW).")));

        /* 2. leitura por amostragem */
        g_step = 1;
        SetPhase(L"Lendo pontos de todo o cartão", (uint64_t)MAP_N * blk);
        int fails = 0;
        for (int i = 0; i < MAP_N; i++) {
            if (g_cancel || fails >= 12) { v->cell[i] = MC_SKIP; continue; }
            uint64_t off = size > blk ? ((size - blk) / (MAP_N - 1)) * (uint64_t)i : 0;
            off -= off % sec;
            double t0 = NowSec();
            BOOL ok = ReadAtRaw(hd, off, buf, blk);
            double dt = NowSec() - t0;
            if (!ok) { v->cell[i] = MC_BAD; fails++; }
            else if (dt > 1.0) v->cell[i] = MC_SLOW;
            else if (IsFill(buf, blk, 0) || IsFill(buf, blk, 0xFF)) v->cell[i] = MC_EMPTY;
            else v->cell[i] = MC_DATA;
            g_done += blk;
            g_mapDone = i + 1;
        }
        if (g_cancel) { Log(L"Diagnóstico cancelado."); goto out; }
        if (fails >= 12) Log(L"Muitos pontos ilegíveis seguidos; o restante do mapa não foi lido para não travar.");
    }
    for (int i = 0; i < MAP_N; i++) {
        if (!opened) v->cell[i] = MC_SKIP;
        switch (v->cell[i]) {
        case MC_DATA: v->nData++; break;
        case MC_EMPTY: v->nEmpty++; break;
        case MC_SLOW: v->nSlow++; break;
        case MC_BAD: v->nBad++; break;
        default: v->nSkip++; break;
        }
    }
    int tested = MAP_N - v->nSkip, readable = v->nData + v->nEmpty + v->nSlow;
    Log(L"Leitura: %d pontos com dados, %d vazios, %d lentos, %d ilegíveis, %d não lidos.", v->nData, v->nEmpty, v->nSlow, v->nBad, v->nSkip);

    /* 3. teste de gravação (devolve o conteúdo original) */
    if (opened && j->verify && readable > 0 && tl * 1 <= blk && !g_cancel) {
        g_step = 2;
        SetIndet(L"Testando a gravação...");
        if (!LockDiskVolumes(j->src, &vs)) {
            Log(L"Aviso: não foi possível travar as partições do cartão; o teste de gravação foi pulado. Feche programas que usam o cartão.");
        } else {
            locked = TRUE;
            hw = OpenDisk(j->src, TRUE);
            if (hw == INVALID_HANDLE_VALUE) {
                wr = 1;
                wrErr = GetLastError();
            } else {
                uint64_t pos[2] = {size / 2, size / 8 * 7};
                for (int k = 0; k < 2; k++) {
                    uint64_t off = pos[k] - pos[k] % (1024 * 1024);
                    if (!ReadAtRaw(hw, off, buf, tl)) { Log(L"Ponto de teste %d ilegível; pulado.", k + 1); continue; }
                    for (DWORD x = 0; x < tl; x++) pat[x] = (uint8_t)~buf[x];
                    if (!WriteAtRaw(hw, off, pat, tl)) { wr = 1; wrErr = GetLastError(); break; }
                    FlushFileBuffers(hw);
                    int res;
                    if (!ReadAtRaw(hw, off, rb, tl)) res = 3;
                    else if (memcmp(rb, pat, tl) == 0) res = 0;
                    else if (memcmp(rb, buf, tl) == 0) res = 2;
                    else res = 3;
                    if (!WriteAtRaw(hw, off, buf, tl)) {
                        if (res == 0) Log(L"ATENÇÃO: não foi possível devolver o conteúdo original de 4 KB na posição %I64u.", (unsigned long long)off);
                    } else {
                        FlushFileBuffers(hw);
                        if (res == 0 && (!ReadAtRaw(hw, off, rb, tl) || memcmp(rb, buf, tl) != 0))
                            Log(L"ATENÇÃO: o conteúdo original na posição %I64u não voltou igual.", (unsigned long long)off);
                    }
                    if (wr < res) wr = res;
                    if (res != 0) break;
                }
                if (wr < 0) wr = -1; /* nenhum ponto pôde ser testado */
            }
        }
        if (wr == 0) Log(L"Gravação: funciona (testada em 2 pontos; conteúdo original devolvido).");
        else if (wr == 1) { SetLastError(wrErr); LogErr(L"gravação recusada pelo cartão"); }
        else if (wr == 2) Log(L"Gravação: o cartão diz que gravou, mas o conteúdo não mudou.");
        else if (wr == 3) Log(L"Gravação: o conteúdo lido de volta veio diferente do gravado.");
    } else if (opened && !j->verify)
        Log(L"Teste de gravação desmarcado.");

    /* conteúdo do cartão: ocupação, tipos de arquivo e perfil de uso */
    g_step = 3;
    if (opened && readable > 0 && !g_cancel) ScanContent(j, hd, v);
    g_step = 4;
    if (g_cancel) { Log(L"Diagnóstico cancelado."); goto out; }

    /* registro CID (só leitores SD internos expõem) */
    {
        SdId ids[4];
        int nid = ReadSdBusCards(ids, 4);
        for (int k = 0; k < nid; k++) {
            wchar_t oem[16] = L"";
            OidToText(ids[k].oid, oem);
            Log(L"Registro CID (leitor SD interno): fabricante 0x%02X, OEM %ls, produto %ls, revisão %ls.", ids[k].mid, oem, ids[k].pnm, ids[k].rev);
        }
        if (nid == 0) Log(L"Registro CID não disponível: em leitores USB o Windows não mostra o fabricante gravado no cartão.");
    }

    /* capacidade fora do padrão comercial */
    wchar_t capNote[220] = L"";
    {
        static const int gb[] = {1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048};
        for (int k = 0; k < 12; k++) {
            double s = (double)gb[k] * 1e9;
            if ((double)size <= s * 1.02) {
                if ((double)size / s < 0.915 && gb[k] >= 4) {
                    _snwprintf(capNote, 219, L" A capacidade informada (%ls) é menor que a de um cartão comum de %d GB.", sz, gb[k]);
                    capNote[219] = 0;
                    Log(L"Observação: a capacidade informada (%ls) não corresponde a um tamanho comercial comum (o mais próximo é %d GB). Pode indicar controladora com defeito ou cartão falsificado.", sz, gb[k]);
                }
                break;
            }
        }
    }

    /* 4. veredito */
    int pct = readable > 0 ? (v->nData * 100 + readable / 2) / readable : 0;
    if (!opened || (tested > 0 && readable == 0)) {
        v->level = 2; v->next = NX_NONE;
        VS(title, L"O cartão não responde à leitura");
        VS(cause, L"A controladora não entrega dados: pode estar queimada, com o firmware travado ou com contato danificado. Também pode ser o leitor.");
        VS(card, L"Não tem conserto por software.");
        VS(data, L"Só em laboratório de recuperação (extração do chip), com custo alto e sem garantia.");
        VS(todo, L"Teste em outro leitor e outra porta USB. Se os dados forem importantes, procure um laboratório.");
    } else if (wr >= 1) {
        v->level = 2;
        if (wr == 1 && wrErr == ERROR_WRITE_PROTECT) {
            v->level = 1;
            VS(title, L"Gravação bloqueada por proteção");
            VS(cause, L"O cartão, o adaptador ou o leitor está protegido contra gravação: a chave lateral do adaptador ou a proteção interna do cartão.");
            VS(card, L"Talvez. Mova a chave do adaptador e teste outro leitor. Se a proteção for interna do cartão, não há conserto.");
        } else if (wr == 1) {
            VS(title, L"Cartão travado em somente leitura");
            VS(cause, L"A controladora recusa gravações (erro %lu), mas ainda lê. É o modo de proteção que ela ativa quando a memória se desgasta ou falha.", (unsigned long)wrErr);
            VS(card, L"Não. O bloqueio fica no firmware do cartão: formatar, diskpart ou reconstruir partições vão falhar.");
        } else if (wr == 2) {
            VS(title, L"O cartão não guarda o que é gravado");
            VS(cause, L"O cartão aceita a gravação, mas o conteúdo não muda: falha da controladora ou da memória, comum em cartão desgastado ou falsificado.");
            VS(card, L"Não. Descarte o cartão.");
        } else {
            VS(title, L"A gravação corrompe os dados");
            VS(cause, L"O conteúdo lido de volta veio diferente do gravado: a memória flash ou a controladora está com defeito.");
            VS(card, L"Não. Descarte o cartão.");
        }
        if (v->nData > 0) {
            v->next = NX_BACKUP;
            VS(data, L"Sim. A leitura funciona e %d%% dos pontos lidos têm dados. Copie tudo agora, antes que piore.", pct);
            VS(todo, L"Faça um backup (imagem) do cartão e recupere os arquivos a partir da imagem. Depois, descarte o cartão.");
        } else {
            v->next = NX_NONE;
            VS(data, L"Provavelmente não: todos os pontos lidos estão vazios. A controladora perdeu o mapa dos dados ou o cartão foi apagado.");
            VS(todo, L"Programas não recuperam dados que o cartão não entrega. Só um laboratório (extração do chip) pode tentar, sem garantia.");
        }
        if (wr == 1 && wrErr == ERROR_WRITE_PROTECT && v->nData > 0)
            VS(todo, L"Faça o backup antes de qualquer tentativa. Depois confira a chave do adaptador e teste outro leitor.");
    } else if (v->nBad > 0) {
        v->level = 2; v->next = NX_BACKUP;
        VS(title, L"Cartão com áreas ilegíveis");
        VS(cause, L"%d de %d pontos não puderam ser lidos: a memória flash está desgastada ou com defeito.", v->nBad, tested);
        VS(card, L"Não é confiável. Mesmo que volte a funcionar depois de formatar, pode falhar de novo.");
        VS(data, L"Em grande parte, sim. O backup pula as áreas ruins e salva o resto.");
        VS(todo, L"Faça um backup (imagem) agora e recupere os arquivos a partir dele. Evite gravar no cartão.");
    } else if (v->nSlow * 10 >= tested && v->nSlow > 0) {
        v->level = 1; v->next = NX_BACKUP;
        VS(title, L"Leitura muito lenta");
        VS(cause, L"%d pontos demoraram mais de 1 segundo para ler: sinal de desgaste da memória ou de problema no leitor.", v->nSlow);
        VS(card, L"Use com cautela e teste outro leitor.");
        VS(data, L"Sim. Faça um backup enquanto o cartão ainda lê.");
        VS(todo, L"Faça o backup e depois use Verificar setores para examinar o cartão inteiro.");
    } else if (r0 && !mbr && !gpt) {
        v->level = 1;
        if (v->nData == 0) {
            v->next = NX_RESTORE;
            VS(title, L"Cartão vazio, sem partição");
            VS(cause, L"O cartão está zerado: não há tabela de partições nem dados nos pontos lidos.");
            VS(card, wr == 0 ? L"Sim. A gravação funciona; restaure o cartão para voltar a usar." : L"Provavelmente. Restaure o cartão; se falhar, ele está travado em somente leitura.");
            VS(data, L"Não há dados para recuperar.");
            VS(todo, L"Use Restaurar o cartão para criar uma partição e formatar.");
        } else {
            v->next = NX_REBUILD;
            VS(title, L"Tabela de partições perdida");
            VS(cause, L"Há dados, mas a tabela de partições sumiu (cartão RAW). Acontece ao remover o cartão durante a gravação ou com queda de energia.");
            VS(card, wr == 0 ? L"Sim. Depois de salvar os dados, o cartão pode ser restaurado." : L"Provavelmente. O teste de gravação não foi feito.");
            VS(data, L"Sim. Reconstruir partições refaz a tabela sem apagar os dados.");
            VS(todo, L"Use Reconstruir partições. Se não der certo, use Recuperar arquivos apagados.");
        }
    } else {
        v->level = 0; v->next = NX_AUTH;
        VS(title, L"Nenhum defeito encontrado");
        VS(cause, L"A leitura funciona em todo o cartão%ls e a tabela de partições existe.", wr == 0 ? L", a gravação também" : L"");
        VS(card, L"O cartão está funcionando.");
        VS(data, L"Não há sinal de perda de dados.");
        VS(todo, L"Para saber se a capacidade é real, use Autenticidade com o teste completo.%ls%ls", capNote, wr < 0 ? L" O teste de gravação não foi feito." : L"");
    }
    /* saúde resumida para os ícones */
    v->hRead = (!opened || readable == 0 || v->nBad > 0) ? 2 : ((v->nSlow * 10 >= tested && v->nSlow > 0) ? 1 : 0);
    v->hWrite = wr < 0 ? 3 : (wr == 0 ? 0 : ((wr == 1 && wrErr == ERROR_WRITE_PROTECT) ? 1 : 2));
    v->hPart = !r0 ? 2 : ((mbr || gpt) ? 0 : 1);
    if (v->usage) BuildFitText(v, j->src, size);
    Log(L"Resultado: %ls.", v->title);
    v->valid = TRUE;
out:
    if (hw != INVALID_HANDLE_VALUE) CloseHandle(hw);
    if (locked) UnlockVolumes(&vs);
    if (hd != INVALID_HANDLE_VALUE) CloseHandle(hd);
    if (buf) FREE_ALIGNED(buf);
    if (pat) FREE_ALIGNED(pat);
    if (rb) FREE_ALIGNED(rb);
    return v->valid;
}

/* ------------------------------------------------------------ desempenho (benchmark)
 * Leitura: sequencial em 3 regiões (início, meio, fim) e 4 KB aleatório, sem cache do Windows.
 * Gravação (opcional): usa 32 MB no meio do cartão. O conteúdo original é copiado para a memória antes
 * e devolvido (e conferido) no fim, mesmo se o teste for cancelado ou falhar. */
static void FillPat(uint8_t *p, DWORD n, uint32_t seed) {
    uint64_t x = 0x9E3779B97F4A7C15ull ^ ((uint64_t)seed * 0xBF58476D1CE4E5B9ull);
    if (!x) x = 1;
    for (DWORD i = 0; i + 8 <= n; i += 8) {
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        memcpy(p + i, &x, 8);
    }
}
static int CmpDbl(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : (x > y);
}
static uint32_t g_rng = 1;
static uint32_t Rnd32(void) { g_rng = g_rng * 1664525u + 1013904223u; return g_rng >> 8 | (g_rng << 24); }
static uint64_t Rnd64(void) { return ((uint64_t)Rnd32() << 32) ^ Rnd32() ^ ((uint64_t)Rnd32() << 11); }
static const wchar_t *QW(int q) { return q == BQ_GOOD ? L"bom" : (q == BQ_FAIR ? L"médio" : L"fraco"); }

static BOOL DoBench(Job *j) {
    Verdict *v = &g_vd;
    memset(v, 0, sizeof *v);
    v->op = OP_BENCH;
    v->bench = TRUE;
    BenchRes *br = &v->br;
    const DWORD blk = 1024 * 1024, REG = 32u * 1024 * 1024;
    uint32_t sec = j->srcSec ? j->srcSec : 512;
    DWORD tl = sec > 4096 ? sec : 4096;
    uint64_t size = j->srcSize;
    HANDLE hd = INVALID_HANDLE_VALUE, hw = INVALID_HANDLE_VALUE;
    VolSet vs;
    memset(&vs, 0, sizeof vs);
    BOOL locked = FALSE, touched = FALSE, done = FALSE;
    uint8_t *buf = (uint8_t *)ALLOC_ALIGNED(blk), *pat = (uint8_t *)ALLOC_ALIGNED(blk), *rb = (uint8_t *)ALLOC_ALIGNED(blk), *save = NULL;
    uint64_t wOff = 0;
    DWORD wLen = 0;
    wchar_t sz[32];
    FmtSize(size, sz, 32);
    if (!buf || !pat || !rb) { Log(L"ERRO: memória insuficiente."); goto out; }
    if (blk % sec) { Log(L"ERRO: tamanho de setor sem suporte (%u).", sec); goto out; }
    if (size < 64ull * 1024 * 1024) { Log(L"O cartão é pequeno demais (%ls) para medir desempenho.", sz); goto out; }
    Log(L"Desempenho do Disco %d: %ls, setor de %u bytes.", j->src, sz, sec);
    hd = OpenDisk(j->src, FALSE);
    if (hd == INVALID_HANDLE_VALUE) { LogErr(L"abrir o cartão"); goto out; }
    if (!DiskLenMatches(hd, size)) { Log(L"ERRO: o tamanho do disco mudou. Atualize a lista."); goto out; }

    /* 1. leitura sequencial em 3 regiões */
    {
        uint64_t span = 48ull * 1024 * 1024;
        if (span > size / 8) span = size / 8;
        span -= span % blk;
        uint64_t starts[3] = {0, size / 2, size - span - blk};
        double regR[3];
        int nreg = 0;
        double totB = 0, totT = 0;
        SetPhase(L"Medindo a leitura sequencial...", 3 * span);
        for (int r = 0; r < 3; r++) {
            uint64_t off0 = starts[r] - starts[r] % blk;
            double rbytes = 0, rt = 0, winB = 0, winT = 0, tS = NowSec();
            for (uint64_t k = 0; k < span / blk; k++) {
                if (g_cancel) goto out;
                double t0 = NowSec();
                BOOL ok = ReadAtRaw(hd, off0 + k * blk, buf, blk);
                double dt = NowSec() - t0;
                if (!ok) { if (++br->rErr > 8) break; continue; }
                rbytes += blk; rt += dt; winB += blk; winT += dt;
                g_done += blk;
                if ((k & 3) == 3) {
                    if (winT > 0 && v->nCurveR < 64) v->curveR[v->nCurveR++] = (float)(winB / 1048576.0 / winT);
                    winB = winT = 0;
                }
                if (NowSec() - tS > 6.0) break;
            }
            if (rt > 0) regR[nreg++] = rbytes / 1048576.0 / rt;
            totB += rbytes; totT += rt;
        }
        if (totT > 0) br->seqR = totB / 1048576.0 / totT;
        if (nreg > 0) {
            br->rMin = br->rMax = regR[0];
            for (int i = 1; i < nreg; i++) { if (regR[i] < br->rMin) br->rMin = regR[i]; if (regR[i] > br->rMax) br->rMax = regR[i]; }
        }
        Log(L"Leitura sequencial: %.1f MB/s (regiões: %d).", br->seqR, nreg);
    }

    /* 2. leitura aleatória de 4 KB */
    {
        SetPhase(L"Medindo o acesso aleatório (leitura)...", 600);
        g_rng = 2463534242u ^ (uint32_t)size;
        int n = 0;
        double t0 = NowSec(), el = 0;
        while (n < 600) {
            if (g_cancel) goto out;
            uint64_t o = Rnd64() % (size - tl);
            o -= o % tl;
            if (!ReadAtRaw(hd, o, buf, tl)) { if (++br->rErr > 8) break; }
            n++;
            g_done++;
            el = NowSec() - t0;
            if (el > 6.0) break;
        }
        if (el > 0) br->rr = n / el;
        Log(L"Leitura aleatória de 4 KB: %.0f operações/s.", br->rr);
    }

    /* 3. gravação (opcional) */
    if (j->verify && !g_cancel) {
        if (!LockDiskVolumes(j->src, &vs)) {
            Log(L"Aviso: não foi possível travar as partições do cartão; o teste de gravação foi pulado. Feche programas que usam o cartão.");
        } else {
            locked = TRUE;
            hw = OpenDisk(j->src, TRUE);
            if (hw == INVALID_HANDLE_VALUE) LogErr(L"abrir o cartão para gravação");
        }
        if (hw != INVALID_HANDLE_VALUE) {
            wLen = REG;
            wOff = (size - wLen) / 2;
            wOff -= wOff % blk;
            save = (uint8_t *)ALLOC_ALIGNED(wLen);
            BOOL saved = save != NULL;
            if (!saved) Log(L"Aviso: memória insuficiente para o teste de gravação; pulado.");
            SetPhase(L"Guardando o conteúdo original da área de teste...", wLen);
            for (DWORD k = 0; saved && k < wLen / blk; k++) {
                if (g_cancel) goto out;
                if (!ReadAtRaw(hw, wOff + (uint64_t)k * blk, save + (uint64_t)k * blk, blk)) { saved = FALSE; Log(L"Aviso: a área de teste não pôde ser lida; gravação pulada."); }
                g_done += blk;
            }
            if (saved) {
                touched = TRUE;
                uint32_t nblk = wLen / blk, nW = 0;
                double wb = 0, wt = 0, winB = 0, winT = 0, wins[64];
                int nwin = 0;
                double tS = NowSec();
                SetPhase(L"Medindo a gravação sequencial...", wLen);
                g_done = 0;
                for (uint32_t k = 0; k < nblk; k++) {
                    if (g_cancel) goto out;
                    FillPat(pat, blk, k + 1);
                    double t0 = NowSec();
                    BOOL ok = WriteAtRaw(hw, wOff + (uint64_t)k * blk, pat, blk);
                    double dt = NowSec() - t0;
                    if (!ok) { br->wErr++; SetLastError(GetLastError()); LogErr(L"gravar na área de teste"); break; }
                    nW++; wb += blk; wt += dt; winB += blk; winT += dt;
                    g_done += blk;
                    if ((k & 1) == 1) {
                        if (winT > 0) {
                            double sp = winB / 1048576.0 / winT;
                            if (nwin < 64) wins[nwin++] = sp;
                            if (v->nCurveW < 64) v->curveW[v->nCurveW++] = (float)sp;
                        }
                        winB = winT = 0;
                    }
                    if (NowSec() - tS > 10.0) break;
                }
                FlushFileBuffers(hw);
                if (wt > 0 && nW >= 4) {
                    br->hasW = 1;
                    br->seqW = wb / 1048576.0 / wt;
                    if (nwin > 0) {
                        double tmp[64];
                        memcpy(tmp, wins, sizeof(double) * (size_t)nwin);
                        qsort(tmp, (size_t)nwin, sizeof(double), CmpDbl);
                        br->wMin = tmp[0];
                        br->wMed = tmp[nwin / 2];
                    }
                    Log(L"Gravação sequencial: %.1f MB/s (mínima %.1f MB/s).", br->seqW, br->wMin);
                    /* confere o que foi gravado */
                    int bad = 0;
                    for (uint32_t k = 0; k < nW && !g_cancel; k++) {
                        FillPat(pat, blk, k + 1);
                        if (!ReadAtRaw(hw, wOff + (uint64_t)k * blk, rb, blk) || memcmp(rb, pat, blk) != 0) bad++;
                    }
                    if (bad) { br->wErr += bad; Log(L"ERRO: %d bloco(s) de 1 MB voltaram diferentes do que foi gravado.", bad); }
                    /* gravação aleatória de 4 KB dentro da área de teste */
                    SetPhase(L"Medindo o acesso aleatório (gravação)...", 300);
                    g_done = 0;
                    int n = 0;
                    double t1 = NowSec(), el = 0;
                    uint64_t slots = wLen / tl;
                    while (n < 300) {
                        if (g_cancel) goto out;
                        uint64_t o = wOff + (Rnd64() % slots) * tl;
                        FillPat(pat, tl, 0x1000u + (uint32_t)n);
                        if (!WriteAtRaw(hw, o, pat, tl)) { br->wErr++; break; }
                        n++;
                        g_done++;
                        el = NowSec() - t1;
                        if (el > 6.0) break;
                    }
                    if (el > 0) br->rw = n / el;
                    Log(L"Gravação aleatória de 4 KB: %.0f operações/s.", br->rw);
                }
            }
        }
    }
    done = !g_cancel;

out:
    /* devolve o conteúdo original da área de teste, mesmo se cancelado */
    if (touched && hw != INVALID_HANDLE_VALUE && save) {
        SetIndet(L"Devolvendo o conteúdo original do cartão...");
        BOOL ok = TRUE;
        for (DWORD k = 0; k < wLen / blk; k++)
            if (!WriteAtRaw(hw, wOff + (uint64_t)k * blk, save + (uint64_t)k * blk, blk)) ok = FALSE;
        FlushFileBuffers(hw);
        for (DWORD k = 0; ok && k < wLen / blk; k++)
            if (!ReadAtRaw(hw, wOff + (uint64_t)k * blk, rb, blk) || memcmp(rb, save + (uint64_t)k * blk, blk) != 0) ok = FALSE;
        if (ok) Log(L"Conteúdo original da área de teste devolvido e conferido.");
        else {
            Log(L"ATENÇÃO: não foi possível devolver ou conferir o conteúdo original de %u MB na posição %I64u do cartão.", (unsigned)(wLen >> 20), (unsigned long long)wOff);
            SetSummary(MB_ICONWARNING, L"Atenção: o teste de gravação usou 32 MB no meio do cartão e não foi possível confirmar que o conteúdo original voltou igual. Confira os arquivos do cartão.");
        }
    }
    if (hw != INVALID_HANDLE_VALUE) CloseHandle(hw);
    if (locked) UnlockVolumes(&vs);
    if (hd != INVALID_HANDLE_VALUE) CloseHandle(hd);
    if (save) FREE_ALIGNED(save);
    if (buf) FREE_ALIGNED(buf);
    if (pat) FREE_ALIGNED(pat);
    if (rb) FREE_ALIGNED(rb);
    if (g_cancel) Log(L"Teste de desempenho cancelado.");
    if (!done || br->seqR <= 0) return FALSE;

    /* resultado */
    BenchEval(br, &v->bj);
    BenchJudge *bj = &v->bj;
    wchar_t a[32], b[32], c[32], d2[32];
    FmtNum(br->seqR, 1, a, 32);
    if (br->hasW) {
        FmtNum(br->seqW, 1, b, 32);
        FmtNum(br->wMin, 1, c, 32);
        VS(cause, L"Leitura sequencial: %ls MB/s. Gravação sequencial: %ls MB/s (janela mais lenta: %ls MB/s).", a, b, c);
    } else VS(cause, L"Leitura sequencial: %ls MB/s. A gravação não foi testada.", a);
    FmtNum(br->rr, 0, c, 32);
    if (br->hasW) {
        FmtNum(br->rw, 0, d2, 32);
        VS(card, L"Leitura: %ls operações/s (%ls). Gravação: %ls operações/s (%ls).", c, QW(bj->rand), d2, QW(bj->randW));
    } else VS(card, L"Leitura: %ls operações/s (%ls).", c, QW(bj->rand));
    if (br->hasW)
        VS(data, L"Pela gravação sequencial, equivale a %hs. Medido neste leitor: um leitor lento mostra menos do que o cartão entrega.", BENCH_CLASS_NAME[bj->classIdx]);
    else VS(data, L"Marque o teste de gravação para estimar a classe de velocidade (Class 10, U1, U3...).");
    {
        const wchar_t *gm = bj->game == 2 ? L"bom" : (bj->game == 1 ? L"serve para jogos leves" : L"fraco");
        const wchar_t *md = bj->media == 3 ? L"fotos e vídeo até 4K" : (bj->media == 2 ? L"fotos e vídeo Full HD" : (bj->media == 1 ? L"só fotos e vídeo básico" : (bj->media == 0 ? L"não serve (gravação lenta)" : L"não testado")));
        const wchar_t *tr = bj->trust == BT_OK ? L"boa" : (bj->trust == BT_DOUBT ? L"duvidosa" : L"baixa");
        wchar_t er[80] = L"";
        if (br->rErr || br->wErr) _snwprintf(er, 79, L" Erros no teste: %d.", br->rErr + br->wErr);
        VS(todo, L"Game stick: %ls. Foto e vídeo: %ls. Confiança no cartão: %ls.%ls", gm, md, tr, er);
    }
    if (bj->trust == BT_BAD) {
        v->level = 2;
        VS(title, L"Cartão genérico ou instável: não confie nele");
    } else if (bj->trust == BT_DOUBT) {
        v->level = 1;
        VS(title, L"Desempenho irregular: use só para dados que você possa perder");
    } else if (bj->game == 2 && bj->media >= 2) {
        VS(title, bj->media == 3 ? L"Bom para game stick, fotos e vídeo 4K" : L"Bom para game stick, fotos e vídeo Full HD");
    } else if (bj->game == 2) {
        v->level = bj->media == 0 ? 1 : 0;
        VS(title, bj->media == -1 ? L"Bom para game stick (gravação não testada)" : L"Bom para game stick; fraco para vídeo");
    } else if (bj->game == 1) {
        v->level = 1;
        VS(title, bj->media >= 2 ? L"Serve para jogos leves, fotos e vídeo" : L"Serve para jogos leves");
    } else {
        v->level = 1;
        VS(title, bj->media >= 2 ? L"Bom para fotos e vídeo; lento para game stick" : L"Cartão lento: serve só para guardar arquivos");
    }
    g_bm.valid = TRUE; g_bm.disk = j->src; g_bm.size = size; g_bm.br = *br; g_bm.bj = *bj;
    Log(L"Resultado: %ls.", v->title);
    v->valid = TRUE;
    return TRUE;
}

/* ------------------------------------------------------------ proteção do cartão (registros CID/CSD)
 * Dois caminhos separados:
 *  - Leitor interno do computador: comandos SD enviados pelo driver do Windows (IOCTL_SFFDISK_DEVICE_COMMAND).
 *    Só funciona em leitores SD ligados ao barramento SD do Windows; leitores USB não repassam esses comandos.
 *    Neste modo o app só LÊ os registros.
 *  - Arduino/ESP32: o programa arduino/sdtools_arduino fala direto com o cartão por SPI. Lê os registros,
 *    vê se há senha, testa a gravação e pode desligar o bit de proteção temporária. */
#ifndef IOCTL_SFFDISK_DEVICE_COMMAND
#define IOCTL_SFFDISK_DEVICE_COMMAND CTL_CODE(FILE_DEVICE_DISK, 0x7a0, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#endif
typedef struct { USHORT HeaderSize, Flags; int Command; USHORT ProtocolArgumentSize; ULONG DeviceDataBufferSize; ULONG_PTR Information; } SffCmdHdr;
typedef struct { UCHAR Cmd; int CmdClass, TransferDirection, TransferType, ResponseType; } SdCmdDesc;

/* portas seriais (Arduino/ESP32) */
typedef struct { wchar_t port[16], name[200]; int likely; } SerPort;
static SerPort g_ports[32];
static int g_nports = 0;

static void SerEnumPorts(void) {
    static const GUID ports = {0x4D36E978, 0xE325, 0x11CE, {0xBF, 0xC1, 0x08, 0x00, 0x2B, 0xE1, 0x03, 0x18}};
    g_nports = 0;
    HDEVINFO h = SetupDiGetClassDevsW(&ports, NULL, NULL, DIGCF_PRESENT);
    if (h == INVALID_HANDLE_VALUE) return;
    SP_DEVINFO_DATA d;
    d.cbSize = sizeof d;
    for (DWORD i = 0; g_nports < 32 && SetupDiEnumDeviceInfo(h, i, &d); i++) {
        SerPort *p = &g_ports[g_nports];
        memset(p, 0, sizeof *p);
        HKEY k = SetupDiOpenDevRegKey(h, &d, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
        if (k == INVALID_HANDLE_VALUE) continue;
        DWORD sz = sizeof p->port - sizeof(wchar_t), type = 0;
        LONG rr = RegQueryValueExW(k, L"PortName", NULL, &type, (BYTE *)p->port, &sz);
        RegCloseKey(k);
        if (rr != ERROR_SUCCESS || wcsncmp(p->port, L"COM", 3) != 0) continue;
        if (!SetupDiGetDeviceRegistryPropertyW(h, &d, SPDRP_FRIENDLYNAME, NULL, (BYTE *)p->name, sizeof p->name - 4, NULL)) wcscpy(p->name, p->port);
        wchar_t ids[1024] = L"";
        SetupDiGetDeviceRegistryPropertyW(h, &d, SPDRP_HARDWAREID, NULL, (BYTE *)ids, sizeof ids - 4, NULL);
        _wcsupr(ids);
        static const wchar_t *vids[] = {L"VID_2341", L"VID_2A03", L"VID_1A86", L"VID_10C4", L"VID_0403", L"VID_303A", L"VID_239A"};
        for (int v = 0; v < 7; v++)
            if (wcsstr(ids, vids[v])) p->likely = 1;
        g_nports++;
    }
    SetupDiDestroyDeviceInfoList(h);
}

static HANDLE SerOpen(const wchar_t *port) {
    wchar_t p[40];
    _snwprintf(p, 39, L"\\\\.\\%ls", port);
    p[39] = 0;
    HANDLE h = CreateFileW(p, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return h;
    DCB dcb;
    memset(&dcb, 0, sizeof dcb);
    dcb.DCBlength = sizeof dcb;
    GetCommState(h, &dcb);
    dcb.BaudRate = 115200;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE;
    dcb.fParity = FALSE;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    dcb.fRtsControl = RTS_CONTROL_DISABLE;
    dcb.fOutX = FALSE;
    dcb.fInX = FALSE;
    SetCommState(h, &dcb);
    COMMTIMEOUTS to = {50, 0, 100, 0, 1000};
    SetCommTimeouts(h, &to);
    return h;
}

static BOOL SerSend(HANDLE h, const char *s) {
    DWORD w = 0;
    return WriteFile(h, s, (DWORD)strlen(s), &w, NULL) && w == strlen(s);
}

/* lê uma linha (sem \r\n); devolve o tamanho ou -1 se esgotar o tempo */
static int SerLine(HANDLE h, char *out, int max, DWORD ms) {
    int n = 0;
    ULONGLONG t0 = GetTickCount64();
    while (GetTickCount64() - t0 < ms) {
        char c;
        DWORD got = 0;
        if (!ReadFile(h, &c, 1, &got, NULL)) return -1;
        if (!got) continue;
        if (c == '\r') continue;
        if (c == '\n') { out[n] = 0; return n; }
        if (n < max - 1) out[n++] = c;
    }
    out[n] = 0;
    return -1;
}

/* envia um comando e junta as linhas até "END" */
static int SerCmd(HANDLE h, const char *c, char lines[][80], int maxl, DWORD ms) {
    PurgeComm(h, PURGE_RXCLEAR);
    if (!SerSend(h, c)) return -1;
    int n = 0;
    ULONGLONG t0 = GetTickCount64();
    while (GetTickCount64() - t0 < ms) {
        char l[80];
        if (SerLine(h, l, 80, (DWORD)(ms - (GetTickCount64() - t0))) < 0) break;
        if (!strcmp(l, "END")) return n;
        if (n < maxl) strcpy(lines[n++], l);
    }
    return n > 0 ? n : -1;
}

static BOOL ParseHex(const char *s, uint8_t *out, int n) {
    for (int i = 0; i < n; i++) {
        unsigned v;
        if (!s[i * 2] || !s[i * 2 + 1] || sscanf(s + i * 2, "%2x", &v) != 1) return FALSE;
        out[i] = (uint8_t)v;
    }
    return TRUE;
}

static void LogCid(const uint8_t *b) {
    wchar_t pnm[6], oid[3];
    for (int i = 0; i < 5; i++) pnm[i] = (b[3 + i] >= 32 && b[3 + i] < 127) ? (wchar_t)b[3 + i] : L'?';
    pnm[5] = 0;
    for (int i = 0; i < 2; i++) oid[i] = (b[1 + i] >= 32 && b[1 + i] < 127) ? (wchar_t)b[1 + i] : L'?';
    oid[2] = 0;
    unsigned mdt = ((b[13] & 0x0F) << 8) | b[14];
    unsigned long psn = ((unsigned long)b[9] << 24) | ((unsigned long)b[10] << 16) | ((unsigned long)b[11] << 8) | b[12];
    Log(L"CID: fabricante 0x%02X (%ls), OEM \"%ls\", produto \"%ls\", revisão %u.%u, série %08lX, fabricado em %02u/%u.",
        b[0], MidName(b[0]) ? MidName(b[0]) : L"não identificado", oid, pnm, b[8] >> 4, b[8] & 0x0F, psn, mdt & 0x0F, 2000 + (mdt >> 4));
}

static uint64_t CsdBytes(const uint8_t *c) {
    if ((c[0] >> 6) == 1) {
        uint64_t cs = ((uint64_t)(c[7] & 0x3F) << 16) | ((uint64_t)c[8] << 8) | c[9];
        return (cs + 1) * 512ULL * 1024ULL;
    }
    uint64_t rbl = c[5] & 0x0F;
    uint64_t cs = ((uint64_t)(c[6] & 0x03) << 10) | ((uint64_t)c[7] << 2) | (c[8] >> 6);
    uint64_t mult = ((c[9] & 0x03) << 1) | (c[10] >> 7);
    return (cs + 1) << (mult + 2) << rbl;
}

static void LogCsd(const uint8_t *c) {
    wchar_t sz[32];
    FmtSize(CsdBytes(c), sz, 32);
    Log(L"CSD: versão %u, capacidade registrada %ls, proteção temporária %ls, proteção permanente %ls, cópia %ls, proteção por grupos %ls.",
        (c[0] >> 6) + 1, sz, (c[14] & 0x10) ? L"LIGADA" : L"desligada", (c[14] & 0x20) ? L"LIGADA" : L"desligada",
        (c[14] & 0x40) ? L"sim" : L"não", (c[12] & 0x80) ? L"ativa" : L"inativa");
}

static BOOL CsdValid(const uint8_t *c) {
    if ((c[0] >> 6) > 1 || (c[0] & 0x3F) != 0) return FALSE;
    return c[3] == 0x32 || c[3] == 0x5A || c[3] == 0x0B || c[3] == 0x2B;
}
static BOOL CidValid(const uint8_t *b) {
    for (int i = 0; i < 5; i++)
        if (b[3 + i] < 32 || b[3 + i] > 126) return FALSE;
    return b[0] != 0 || b[1] != 0;
}

/* o driver devolve a resposta R2 numa ordem que varia; testa as ordens possíveis e fica com a que faz sentido */
static void NormalizeR2(const uint8_t *raw, uint8_t *out, int t) {
    for (int i = 0; i < 16; i++) {
        if (t == 0) out[i] = raw[i];
        else if (t == 1) out[i] = raw[15 - i];
        else if (t == 2) out[i] = i < 15 ? raw[14 - i] : 0; /* invertida, sem o byte de CRC */
        else out[i] = i < 15 ? raw[i + 1] : 0;
    }
}

static BOOL SffCmd(HANDLE h, UCHAR cmd, uint8_t *out16, DWORD *err) {
    BYTE buf[sizeof(SffCmdHdr) + sizeof(SdCmdDesc) + 16];
    memset(buf, 0, sizeof buf);
    SffCmdHdr *hd = (SffCmdHdr *)buf;
    hd->HeaderSize = sizeof(SffCmdHdr);
    hd->Command = 3; /* SFFDISK_DC_DEVICE_COMMAND */
    hd->ProtocolArgumentSize = sizeof(SdCmdDesc);
    hd->DeviceDataBufferSize = 16;
    SdCmdDesc *dd = (SdCmdDesc *)(buf + sizeof(SffCmdHdr));
    dd->Cmd = cmd;
    dd->CmdClass = 0;          /* SDCC_STANDARD */
    dd->TransferDirection = 1; /* SDTD_READ */
    dd->TransferType = 1;      /* SDTT_CMD_ONLY */
    dd->ResponseType = 4;      /* SDRT_2 */
    DWORD br = 0;
    if (!DeviceIoControl(h, IOCTL_SFFDISK_DEVICE_COMMAND, buf, sizeof buf, buf, sizeof buf, &br, NULL)) {
        *err = GetLastError();
        return FALSE;
    }
    memcpy(out16, buf + sizeof(SffCmdHdr) + sizeof(SdCmdDesc), 16);
    for (int i = 0; i < 16; i++)
        if (out16[i]) return TRUE;
    *err = ERROR_NOT_SUPPORTED;
    return FALSE;
}

typedef struct {
    BOOL haveCsd, haveCid;
    uint8_t csd[16], cid[16];
    int locked;   /* -1 desconhecido */
    int wtest;    /* -1 não testado, 0 ok, 1 recusada, 2 ignorada, 3 corrompida */
    int clr;      /* -1 não tentado, 0 desligada, 1 falhou, 2 não estava ligada, 3 permanente */
    int initErr;  /* Arduino: erro ao iniciar o cartão */
    BOOL arduino, noReader;
} CsdRes;

static void CsdVerdict(const CsdRes *r) {
    Verdict *v = &g_vd;
    memset(v, 0, sizeof *v);
    v->op = OP_CSD;
    v->map = FALSE;
    v->next = NX_NONE;
    BOOL tmp = r->haveCsd && (r->csd[14] & 0x10), perm = r->haveCsd && (r->csd[14] & 0x20);
    if (!r->haveCsd) {
        v->level = 1;
        if (r->arduino && r->initErr) {
            VS(title, L"O cartão não iniciou no Arduino");
            VS(cause, r->initErr == 1 ? L"O cartão não respondeu ao comando inicial. Pode ser ligação errada, módulo sem conversor de nível ou controladora morta."
                                      : L"O cartão respondeu, mas não completou a inicialização (código %d). A controladora pode estar com defeito.", r->initErr);
            VS(card, L"Ainda não dá para saber.");
            VS(data, L"Ainda não dá para saber.");
            VS(todo, L"Confira a ligação (arduino/LEIA.md) com outro cartão que funciona. Se o outro funciona e este não, a controladora deste está morta.");
        } else if (r->arduino) {
            VS(title, L"O Arduino não leu o registro");
            VS(cause, L"O Arduino iniciou o cartão, mas o registro CSD não veio.");
            VS(card, L"Ainda não dá para saber.");
            VS(data, L"Ainda não dá para saber.");
            VS(todo, L"Tire e recoloque o cartão no módulo e tente de novo.");
        } else {
            VS(title, L"Este leitor não permite ler o registro");
            VS(cause, r->noReader ? L"O cartão está num leitor USB. Leitores USB não repassam comandos diretos para o cartão."
                                  : L"O driver deste leitor não aceita comandos diretos. Acontece com leitores USB e com leitores internos que usam driver do fabricante.");
            VS(card, L"Ainda não dá para saber.");
            VS(data, L"Ainda não dá para saber.");
            VS(todo, L"Use o modo Arduino ou ESP32: ele fala direto com o cartão.");
        }
        v->valid = TRUE;
        return;
    }
    if (r->locked == 1) {
        v->level = 1;
        VS(title, L"Cartão bloqueado por senha");
        VS(cause, L"O cartão tem uma senha gravada (comando CMD42), recurso usado por alguns celulares e câmeras antigos.");
        VS(card, L"Sim, com a senha. Sem ela, só apagando o cartão inteiro.");
        VS(data, L"Só com a senha. Tente no aparelho que colocou a senha.");
        VS(todo, L"Coloque o cartão de volta no aparelho onde ele foi usado e remova a senha por lá.");
    } else if (perm) {
        v->level = 2; v->next = NX_BACKUP;
        VS(title, L"Proteção permanente ligada");
        VS(cause, L"O bit de proteção permanente do cartão está ligado. Ele é gravado uma única vez e trava a gravação para sempre.");
        VS(card, L"Não. Esse bit não pode ser desligado.");
        VS(data, L"Sim. A leitura continua funcionando.");
        VS(todo, L"Faça um backup (imagem) do cartão e recupere os arquivos a partir dela.");
    } else if (r->clr == 0) {
        v->level = 0;
        VS(title, L"Proteção temporária desligada");
        VS(cause, L"O bit de proteção temporária estava ligado e foi desligado com sucesso.");
        VS(card, r->wtest == 0 ? L"Sim. A gravação funcionou pelo Arduino." : L"Provavelmente. Coloque o cartão no computador e rode o Diagnóstico para confirmar.");
        VS(data, L"Sim.");
        VS(todo, L"Coloque o cartão no computador e rode o Diagnóstico com o teste de gravação.");
    } else if (tmp && r->clr == 1) {
        v->level = 2; v->next = NX_BACKUP;
        VS(title, L"Não foi possível desligar a proteção");
        VS(cause, L"O bit de proteção temporária está ligado, mas o cartão recusou a alteração do registro.");
        VS(card, L"Provavelmente não. A controladora não aceita mudanças.");
        VS(data, L"Sim, se a leitura funciona.");
        VS(todo, L"Faça um backup (imagem) do cartão e recupere os arquivos a partir dela.");
    } else if (tmp) {
        v->level = 1;
        VS(title, L"Proteção temporária ligada");
        VS(cause, L"O bit de proteção temporária do cartão está ligado. É ele que impede a gravação, e ele pode ser desligado.");
        VS(card, L"Provavelmente sim.");
        VS(data, L"Sim. A leitura funciona.");
        VS(todo, r->arduino ? L"Marque a opção para desligar a proteção temporária e clique em Ler proteção de novo."
                            : L"Para desligar, use o modo Arduino ou ESP32 (pelo Windows o app só lê o registro).");
    } else if (r->wtest == 0) {
        v->level = 1;
        VS(title, L"O cartão grava normalmente pelo Arduino");
        VS(cause, L"Não há proteção ligada e a gravação funcionou direto no cartão. Se no computador ela falha, o problema pode ser o leitor ou o adaptador.");
        VS(card, L"Provavelmente sim. Teste outro leitor ou adaptador no computador.");
        VS(data, L"Sim.");
        VS(todo, L"Troque o leitor ou o adaptador SD e rode o Diagnóstico de novo.");
    } else if (r->wtest >= 1) {
        v->level = 2; v->next = NX_BACKUP;
        VS(title, L"Firmware travado em somente leitura");
        VS(cause, r->wtest == 1 ? L"Não há proteção ligada no registro, mas o cartão recusou a gravação direto no Arduino. Quem bloqueia é o firmware da controladora."
                                : (r->wtest == 2 ? L"Não há proteção ligada, mas o cartão finge que grava e o conteúdo não muda: a controladora travou a memória."
                                                 : L"Não há proteção ligada, mas o que é gravado volta diferente: memória ou controladora com defeito."));
        VS(card, L"Não. Esse bloqueio fica no firmware e não sai por software nem pelo Arduino.");
        VS(data, L"Sim, se a leitura funciona. Copie tudo antes que piore.");
        VS(todo, L"Faça um backup (imagem) do cartão e recupere os arquivos a partir dela. Depois, descarte o cartão.");
    } else {
        v->level = 1; v->next = NX_BACKUP;
        VS(title, L"Nenhuma proteção ligada no registro");
        VS(cause, L"Os bits de proteção do cartão estão desligados. Se ele recusa gravações, quem bloqueia é o firmware da controladora.");
        VS(card, L"Se ele recusa gravações, não: esse modo não sai por software.");
        VS(data, L"Sim, se a leitura funciona.");
        VS(todo, L"Rode o Diagnóstico para confirmar e faça um backup (imagem) se o cartão estiver travado.");
    }
    v->valid = TRUE;
}

static BOOL DoCsdInternal(Job *j) {
    CsdRes r;
    memset(&r, 0, sizeof r);
    r.locked = -1; r.wtest = -1; r.clr = -1;
    SetIndet(L"Lendo os registros do cartão pelo leitor interno...");
    Log(L"Modo leitor interno: Disco %d (%ls).", j->src, j->di.label);
    if (j->di.bus == BusTypeUsb) {
        Log(L"O cartão está num leitor USB; esse tipo de leitor não repassa comandos SD.");
        r.noReader = TRUE;
        CsdVerdict(&r);
        return TRUE;
    }
    wchar_t paths[28][32];
    int np = 0;
    _snwprintf(paths[np++], 31, L"\\\\.\\PhysicalDrive%d", j->src);
    for (const wchar_t *c = j->di.letters; *c && np < 28; c++)
        if (*c >= L'A' && *c <= L'Z' && c[1] == L':') _snwprintf(paths[np++], 31, L"\\\\.\\%c:", *c);
    DWORD err = 0;
    for (int p = 0; p < np && !r.haveCsd; p++) {
        HANDLE h = CreateFileW(paths[p], GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) { err = GetLastError(); continue; }
        uint8_t rc[16], rd[16];
        BOOL okc = SffCmd(h, 9, rc, &err);
        BOOL okd = okc && SffCmd(h, 10, rd, &err);
        CloseHandle(h);
        if (!okc) continue;
        Log(L"Resposta bruta do CSD: %02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X", rc[0], rc[1], rc[2], rc[3], rc[4], rc[5], rc[6], rc[7], rc[8], rc[9], rc[10], rc[11], rc[12], rc[13], rc[14], rc[15]);
        for (int t = 0; t < 4 && !r.haveCsd; t++) {
            uint8_t c[16];
            NormalizeR2(rc, c, t);
            if (!CsdValid(c)) continue;
            memcpy(r.csd, c, 16);
            r.haveCsd = TRUE;
            if (okd) {
                NormalizeR2(rd, r.cid, t);
                r.haveCid = CidValid(r.cid);
            }
        }
        if (!r.haveCsd) Log(L"A resposta do leitor não pôde ser interpretada como um registro CSD.");
    }
    if (!r.haveCsd && err) { SetLastError(err); LogErr(L"enviar comando direto ao cartão"); }
    if (r.haveCid) LogCid(r.cid);
    if (r.haveCsd) {
        LogCsd(r.csd);
        wchar_t a[32], b[32];
        FmtSize(CsdBytes(r.csd), a, 32);
        FmtSize(j->srcSize, b, 32);
        if (CsdBytes(r.csd) != j->srcSize) Log(L"Capacidade no registro: %ls; capacidade que o Windows vê: %ls.", a, b);
    }
    CsdVerdict(&r);
    Log(L"Resultado: %ls.", g_vd.title);
    return TRUE;
}

static BOOL DoCsdArduino(Job *j) {
    CsdRes r;
    memset(&r, 0, sizeof r);
    r.locked = -1; r.wtest = -1; r.clr = -1; r.arduino = TRUE;
    char L[16][80];
    SetIndet(L"Conectando ao Arduino...");
    Log(L"Modo Arduino/ESP32 na porta %ls.", j->file);
    HANDLE h = SerOpen(j->file);
    if (h == INVALID_HANDLE_VALUE) {
        LogErr(L"abrir a porta do Arduino");
        SetSummary(MB_ICONWARNING, L"Não foi possível abrir a porta %ls.\n\nFeche o Monitor Serial da IDE do Arduino ou outro programa que esteja usando a porta.", j->file);
        return FALSE;
    }
    Sleep(2000); /* a maioria das placas reinicia ao abrir a porta */
    BOOL hello = FALSE;
    for (int t = 0; t < 4 && !hello; t++) {
        int n = SerCmd(h, "SDT?\n", L, 16, 1200);
        for (int i = 0; i < n; i++)
            if (!strncmp(L[i], "SDTOOLS ", 8)) { hello = TRUE; Log(L"Placa respondeu: %hs.", L[i]); }
    }
    if (!hello) {
        CloseHandle(h);
        Log(L"O dispositivo em %ls não respondeu como SD Clone Tools.", j->file);
        SetSummary(MB_ICONWARNING, L"A placa em %ls não respondeu.\n\nGrave nela o programa arduino\\sdtools_arduino\\sdtools_arduino.ino (veja arduino\\LEIA.md) e tente de novo.", j->file);
        return FALSE;
    }
    if (g_cancel) { CloseHandle(h); return FALSE; }
    SetIndet(L"Lendo os registros do cartão...");
    int n = SerCmd(h, "INFO\n", L, 16, 8000);
    for (int i = 0; i < n; i++) {
        if (!strncmp(L[i], "INIT ERR ", 9)) r.initErr = atoi(L[i] + 9);
        else if (!strncmp(L[i], "INIT OK", 7)) Log(L"Cartão iniciado: %hs.", L[i] + 8);
        else if (!strncmp(L[i], "CID ", 4) && ParseHex(L[i] + 4, r.cid, 16)) r.haveCid = TRUE;
        else if (!strncmp(L[i], "CSD ", 4) && ParseHex(L[i] + 4, r.csd, 16)) r.haveCsd = TRUE;
        else if (!strncmp(L[i], "STATUS ", 7)) {
            uint8_t s[2];
            if (ParseHex(L[i] + 7, s, 2)) r.locked = (s[1] & 0x01) ? 1 : 0;
        }
    }
    if (n < 0) Log(L"A placa não respondeu ao pedido de leitura.");
    if (r.initErr) Log(L"O cartão não iniciou no Arduino (código %d).", r.initErr);
    if (r.haveCid) LogCid(r.cid);
    if (r.haveCsd) LogCsd(r.csd);
    if (r.locked == 1) Log(L"O cartão está BLOQUEADO POR SENHA.");
    BOOL perm = r.haveCsd && (r.csd[14] & 0x20), tmp = r.haveCsd && (r.csd[14] & 0x10);
    if (r.haveCsd && j->mode == 1 && tmp && !perm && r.locked != 1 && !g_cancel) {
        SetIndet(L"Desligando a proteção temporária...");
        n = SerCmd(h, "CLRTMP\n", L, 16, 8000);
        r.clr = 1;
        for (int i = 0; i < n; i++) {
            if (!strcmp(L[i], "CLRTMP OK")) r.clr = 0;
            else if (!strcmp(L[i], "CLRTMP NOTSET")) r.clr = 2;
            else if (!strcmp(L[i], "CLRTMP PERM")) r.clr = 3;
            else if (!strncmp(L[i], "CSD ", 4)) ParseHex(L[i] + 4, r.csd, 16);
        }
        Log(r.clr == 0 ? L"Proteção temporária desligada." : L"O cartão recusou a alteração do registro.");
    }
    if (r.haveCsd && j->verify && r.locked != 1 && !g_cancel) {
        SetIndet(L"Testando a gravação pelo Arduino...");
        n = SerCmd(h, "WTEST\n", L, 16, 10000);
        for (int i = 0; i < n; i++) {
            if (!strcmp(L[i], "WTEST OK")) r.wtest = 0;
            else if (!strncmp(L[i], "WTEST REJECT", 12)) { r.wtest = 1; Log(L"O cartão recusou a gravação (%hs).", L[i] + 13); }
            else if (!strcmp(L[i], "WTEST IGNORED")) r.wtest = 2;
            else if (!strcmp(L[i], "WTEST CORRUPT")) r.wtest = 3;
            else if (!strcmp(L[i], "RESTORE ERR")) Log(L"ATENÇÃO: não foi possível devolver o conteúdo original do bloco de teste.");
        }
        Log(r.wtest == 0 ? L"Gravação pelo Arduino: funciona (conteúdo original devolvido)."
            : r.wtest == 1 ? L"Gravação pelo Arduino: recusada." : r.wtest == 2 ? L"Gravação pelo Arduino: ignorada (o conteúdo não mudou)."
            : r.wtest == 3 ? L"Gravação pelo Arduino: o conteúdo voltou diferente." : L"Gravação pelo Arduino: não foi possível testar.");
    }
    CloseHandle(h);
    CsdVerdict(&r);
    Log(L"Resultado: %ls.", g_vd.title);
    return TRUE;
}

/* ------------------------------------------------------------ cópia inteligente (smart.h + vhd.h)
 * Backup em .vhd só com os dados usados, clone partição por partição (também para cartão menor)
 * e gravação de imagem .vhd ou de imagem maior que o cartão. */
enum { SS_CARD = 0, SS_RAWFILE, SS_VHD };
typedef struct { int kind; HANDLE h; VhdR vr; } SmSrc;
typedef struct { int kind; HANDLE h; VhdW vw; } SmDst;

static int FileRd(void *c, uint64_t off, void *buf, uint32_t len) { return ReadAtRaw(*(HANDLE *)c, off, buf, len) ? 1 : 0; }
static int FileWr(void *c, uint64_t off, const void *buf, uint32_t len) { return WriteAtRaw(*(HANDLE *)c, off, buf, len) ? 1 : 0; }
static int SmSrcRd(void *c, uint64_t off, void *buf, uint32_t len) {
    SmSrc *s = (SmSrc *)c;
    if (s->kind == SS_VHD) return VhdRRead(&s->vr, off, buf, len);
    return ReadAtRaw(s->h, off, buf, len) ? 1 : 0;
}
static int SmDstWr(void *c, uint64_t off, const void *buf, uint32_t len) {
    SmDst *d = (SmDst *)c;
    if (d->kind == 1) return VhdWWrite(&d->vw, off, buf, len);
    return WriteAtRaw(d->h, off, buf, len) ? 1 : 0;
}
static int SmTickCb(void *c, uint64_t done) { (void)c; g_done = (LONGLONG)done; return g_cancel ? 1 : 0; }
static int SmProgCb(void *c, int part, int n) {
    wchar_t t[120];
    (void)c;
    _snwprintf(t, 119, L"Lendo o mapa de blocos usados — partição %d de %d", part + 1, n);
    t[119] = 0;
    SetPhaseText(t);
    return g_cancel ? 1 : 0;
}
static BOOL EndsWithI(const wchar_t *s, const wchar_t *e) {
    size_t a = wcslen(s), b = wcslen(e);
    return a >= b && _wcsicmp(s + a - b, e) == 0;
}
static const wchar_t *FsShort(int t) {
    switch (t) {
    case FST_FAT12: case FST_FAT16: case FST_FAT32: case FST_EXFAT: case FST_NTFS: case FST_EXT2: case FST_EXT3: case FST_EXT4: return FsNameW(t);
    }
    return L"outro tipo";
}

/* confere o que foi gravado: relê a origem (com os ajustes aplicados) e o destino */
static BOOL SmVerify(const SmPlan *p, SmSrc *src, int (*drd)(void *, uint64_t, void *, uint32_t), void *dctx) {
    uint8_t *a = (uint8_t *)ALLOC_ALIGNED(SM_CHUNK), *b = (uint8_t *)ALLOC_ALIGNED(SM_CHUNK);
    BOOL ok = a && b;
    uint64_t done = 0, diff = 0;
    SetPhase(L"Conferindo o que foi gravado", p->copyBytes);
    for (int i = 0; ok && i < p->copy.n; i++) {
        uint64_t off = p->copy.v[i].off, left = p->copy.v[i].len;
        while (ok && left) {
            if (g_cancel) { ok = FALSE; break; }
            uint32_t n = left > SM_CHUNK ? SM_CHUNK : (uint32_t)left;
            if (!SmSrcRd(src, off, a, n)) { off += n; left -= n; continue; } /* setor ilegível na origem: não dá para comparar */
            for (int k = 0; k < p->npatch; k++) {
                uint64_t ps = p->patch[k].off, pe = ps + p->patch[k].len;
                if (pe <= off || ps >= off + n) continue;
                uint64_t s0 = ps > off ? ps : off, e0 = pe < off + n ? pe : off + n;
                memcpy(a + (s0 - off), p->patch[k].data + (s0 - ps), (size_t)(e0 - s0));
            }
            if (!drd(dctx, off, b, n)) { LogErr(L"ler o destino para conferir"); ok = FALSE; break; }
            if (memcmp(a, b, n) != 0) diff++;
            off += n; left -= n; done += n;
            g_done = (LONGLONG)done;
        }
    }
    if (a) FREE_ALIGNED(a);
    if (b) FREE_ALIGNED(b);
    if (ok && diff) { Log(L"ATENÇÃO: %I64u trecho(s) gravado(s) diferente(s) da origem.", (unsigned long long)diff); ok = FALSE; }
    else if (ok) Log(L"Conferência: tudo o que foi gravado está igual à origem.");
    return ok;
}

static int VhdRd(void *c, uint64_t off, void *buf, uint32_t len) { return VhdRRead((VhdR *)c, off, buf, len); }
static int CardRd(void *c, uint64_t off, void *buf, uint32_t len) { return ReadAtRaw(*(HANDLE *)c, off, buf, len) ? 1 : 0; }

/* srcKind: SS_*; dstVhd: TRUE = arquivo .vhd em j->file (backup); FALSE = cartão j->dst */
static BOOL DoSmart(Job *j, int srcKind, BOOL dstVhd) {
    BOOL ok = FALSE, haveDev = FALSE, havePlan = FALSE, vhdOpen = FALSE;
    SmSrc src;
    SmDst dst;
    Dev2 d;
    SmPlan *p = (SmPlan *)calloc(1, sizeof(SmPlan));
    VolSet vs;
    vs.n = 0;
    uint8_t *buf = NULL;
    uint64_t srcSize = 0, dstSize = 0, bad = 0;
    uint32_t sec = 512;
    wchar_t a[32], b[32], c[32];
    memset(&src, 0, sizeof src);
    memset(&dst, 0, sizeof dst);
    src.h = INVALID_HANDLE_VALUE;
    dst.h = INVALID_HANDLE_VALUE;
    if (!p) { Log(L"ERRO: memória insuficiente."); goto done; }

    /* origem */
    src.kind = srcKind;
    if (srcKind == SS_CARD) {
        src.h = OpenDisk(j->src, FALSE);
        if (src.h == INVALID_HANDLE_VALUE) { LogErr(L"abrir o cartão de origem"); goto done; }
        if (!DiskLenMatches(src.h, j->srcSize)) { Log(L"ERRO: o tamanho do cartão mudou. Atualize a lista."); goto done; }
        srcSize = j->srcSize;
        sec = j->srcSec ? j->srcSec : 512;
    } else {
        LARGE_INTEGER fsz;
        src.h = CreateFileW(j->file, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (src.h == INVALID_HANDLE_VALUE) { LogErr(L"abrir a imagem"); goto done; }
        if (!GetFileSizeEx(src.h, &fsz)) { LogErr(L"ler o tamanho da imagem"); goto done; }
        if (srcKind == SS_VHD) {
            if (!VhdROpen(&src.vr, FileRd, &src.h, (uint64_t)fsz.QuadPart)) { Log(L"ERRO: o arquivo .vhd não é válido ou é de um tipo sem suporte (diferencial)."); goto done; }
            srcSize = src.vr.size;
        } else srcSize = (uint64_t)fsz.QuadPart - (uint64_t)fsz.QuadPart % 512;
        sec = j->dstSec ? j->dstSec : 512;
    }
    if (!Dev2Init(&d, SmSrcRd, &src, srcSize, sec)) { Log(L"ERRO: memória insuficiente."); goto done; }
    haveDev = TRUE;
    dstSize = dstVhd ? srcSize : j->dstSize;

    /* plano */
    SetIndet(L"Lendo as partições e os blocos usados...");
    FmtSize(srcSize, a, 32);
    Log(L"Cópia inteligente: só os dados usados de cada partição. Origem: %ls.", a);
    if (!SmBuildPlan(&d, dstSize, p, SmProgCb, NULL)) {
        wchar_t e[700];
        MultiByteToWideChar(CP_UTF8, 0, p->err, -1, e, 700);
        if (g_cancel) { Log(L"Cancelado. Nada foi gravado."); goto done; }
        Log(L"ERRO: %ls.", e);
        if (p->needBytes) { FmtSize(p->needBytes, b, 32); Log(L"Tamanho mínimo do destino para estes dados: %ls.", b); }
        SetSummary(MB_ICONWARNING, L"Não foi possível fazer a cópia inteligente:\n\n%ls.%ls%ls\n\nNada foi gravado.", e,
                   p->needBytes ? L"\n\nTamanho mínimo do destino: " : L"", p->needBytes ? (FmtSize(p->needBytes, c, 32), c) : L"");
        goto done;
    }
    havePlan = TRUE;
    for (int i = 0; i < p->nparts; i++) {
        SmPart *s = &p->part[i];
        FmtSize(s->size, a, 32);
        FmtSize(s->used, b, 32);
        if (s->newSize != s->size) {
            FmtSize(s->newSize, c, 32);
            Log(L"  Partição %d (%ls, %ls): %ls usados — será reduzida para %ls.", i + 1, FsShort(s->fs), a, b, c);
        } else Log(L"  Partição %d (%ls, %ls): %ls %ls.", i + 1, FsShort(s->fs), a, s->how == SMH_WHOLE ? L"copiada inteira (tipo sem mapa de blocos):" : L"usados,", b);
    }
    FmtSize(p->copyBytes, a, 32);
    FmtSize(srcSize, b, 32);
    FmtSize(p->needBytes, c, 32);
    Log(L"Vai copiar %ls de %ls. Destino mínimo para estes dados: %ls.", a, b, c);

    /* destino */
    if (dstVhd) {
        wchar_t full[MAX_PATH], root[MAX_PATH];
        ULARGE_INTEGER fa;
        if (GetFullPathNameW(j->file, MAX_PATH, full, NULL) && GetVolumePathNameW(full, root, MAX_PATH) &&
            GetDiskFreeSpaceExW(root, &fa, NULL, NULL) && fa.QuadPart < p->copyBytes + p->copyBytes / 50 + 64 * MIB) {
            FmtSize(fa.QuadPart, c, 32);
            Log(L"ERRO: espaço livre insuficiente no destino (%ls livres, a imagem terá cerca de %ls).", c, a);
            goto done;
        }
        dst.kind = 1;
        dst.h = CreateFileW(j->file, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (dst.h == INVALID_HANDLE_VALUE) { LogErr(L"criar o arquivo .vhd"); goto done; }
        if (!VhdWOpen(&dst.vw, dstSize, FileWr, FileRd, &dst.h, (uint32_t)GetTickCount())) { Log(L"ERRO: memória insuficiente."); goto done; }
        vhdOpen = TRUE;
    } else {
        if (j->dst == g_sysDisk) { Log(L"ERRO: disco do sistema — recusado."); goto done; }
        if (!LockDiskVolumes(j->dst, &vs)) goto done;
        dst.kind = 0;
        dst.h = OpenDisk(j->dst, TRUE);
        if (dst.h == INVALID_HANDLE_VALUE) { LogErr(L"abrir o cartão de destino para escrita"); goto done; }
        if (!DiskLenMatches(dst.h, j->dstSize)) { Log(L"ERRO: o tamanho do cartão de destino mudou. Atualize a lista."); goto done; }
    }

    /* cópia */
    buf = (uint8_t *)ALLOC_ALIGNED(SM_CHUNK);
    if (!buf) { Log(L"ERRO: memória insuficiente."); goto done; }
    SetPhase(dstVhd ? L"Criando a imagem só com os dados usados" : L"Copiando só os dados usados", p->copyBytes);
    int rc = SmRun(p, SmSrcRd, &src, SmDstWr, &dst, buf, SmTickCb, NULL, &bad);
    if (rc == 2) { Log(L"Operação cancelada pelo usuário."); goto done; }
    if (rc == -1) { LogErr(L"falha ao gravar no destino"); goto done; }
    if (dstVhd) {
        vhdOpen = FALSE;
        if (!VhdWClose(&dst.vw)) { LogErr(L"finalizar o arquivo .vhd"); goto done; }
        FlushFileBuffers(dst.h);
    } else {
        FlushFileBuffers(dst.h);
        UpdateDiskProps(dst.h);
    }
    if (bad) Log(L"ATENÇÃO: %I64u bytes ilegíveis na origem foram gravados como zeros.", (unsigned long long)bad);

    /* conferência */
    if (j->verify) {
        BOOL vok;
        if (dstVhd) {
            LARGE_INTEGER fsz;
            VhdR vr;
            GetFileSizeEx(dst.h, &fsz);
            if (!VhdROpen(&vr, FileRd, &dst.h, (uint64_t)fsz.QuadPart)) { Log(L"ERRO: o .vhd criado não pôde ser reaberto."); goto done; }
            vok = SmVerify(p, &src, VhdRd, &vr);
            VhdRClose(&vr);
        } else vok = SmVerify(p, &src, CardRd, &dst.h);
        if (!vok) goto done;
    }
    ok = TRUE;
    FmtSize(p->copyBytes, a, 32);
    FmtSize(srcSize, b, 32);
    if (dstVhd) {
        LARGE_INTEGER fsz;
        GetFileSizeEx(dst.h, &fsz);
        FmtSize((uint64_t)fsz.QuadPart, c, 32);
        Log(L"Imagem .vhd criada: %ls no disco (cartão de %ls).", c, b);
        SetSummary(bad ? MB_ICONWARNING : MB_ICONINFORMATION, L"Backup criado só com os dados usados.\n\nArquivo: %ls\nTamanho do arquivo: %ls (o cartão tem %ls)%ls\n\nPara ver os arquivos, dê dois cliques no .vhd (o Windows monta como um disco). Para voltar ao cartão, use Gravar imagem: funciona até em um cartão menor, se os dados couberem.",
                   j->file, c, b, bad ? L"\n\nATENÇÃO: havia setores ilegíveis, gravados como zeros." : L"");
    } else {
        Log(L"Cópia concluída: %ls copiados.", a);
        SetSummary(bad ? MB_ICONWARNING : MB_ICONINFORMATION, L"Cópia concluída: só os dados usados (%ls) foram copiados%ls%ls.%ls\n\nSe o Windows pedir para formatar o disco, clique em Cancelar.", a,
                   j->verify ? L" e conferidos" : L"", p->shrunk ? L"; a última partição foi reduzida para caber no cartão" : L"",
                   bad ? L"\n\nATENÇÃO: havia setores ilegíveis na origem, gravados como zeros." : L"");
    }
done:
    if (vhdOpen) VhdWClose(&dst.vw);
    if (dst.h != INVALID_HANDLE_VALUE) CloseHandle(dst.h);
    UnlockVolumes(&vs);
    if (srcKind == SS_VHD) VhdRClose(&src.vr);
    if (src.h != INVALID_HANDLE_VALUE) CloseHandle(src.h);
    if (haveDev) Dev2Free(&d);
    if (havePlan || p) { if (p) SmPlanFree(p); free(p); }
    if (buf) FREE_ALIGNED(buf);
    if (!ok && dstVhd && dst.kind == 1) DeleteFileW(j->file);
    if (!ok && g_summary[0] == 0)
        SetSummary(MB_ICONERROR, dstVhd ? L"O backup não foi concluído. Veja o registro para detalhes." : L"A cópia não foi concluída. O cartão de destino pode estar incompleto — veja o registro.");
    return ok;
}

static DWORD WINAPI Worker(LPVOID p) {
    Job *j = &g_job;
    BOOL ok = FALSE;
    (void)p;
    g_badLogged = 0;
    g_summary[0] = 0;
    switch (j->op) {
    case OP_C2I: ok = j->mode == 0 ? DoSmart(j, SS_CARD, TRUE) : DoCardToImage(j); break;
    case OP_I2C: ok = j->mode ? DoSmart(j, EndsWithI(j->file, L".vhd") ? SS_VHD : SS_RAWFILE, FALSE) : DoImageToCard(j); break;
    case OP_C2C: ok = j->mode == 0 ? DoSmart(j, SS_CARD, FALSE) : DoCardToCard(j); break;
    case OP_VERIFY: ok = (j->mode == 2) ? DoCapacity(j) : DoVerify(j); break;
    case OP_RECOVER:
        switch (j->mode) {
        case RM_DIAG: ok = DoDiagnose(j, FALSE); break;
        case RM_BROWSE: ok = DoDiagnose(j, TRUE); break;
        case RM_REBUILD: ok = DoRebuild(j); break;
        case RM_CARVE: ok = DoRecover(j); break;
        default: ok = DoRestore(j); break;
        }
        break;
    case OP_AUTH: ok = DoAuth(j); break;
    case OP_DIAG: ok = DoHealth(j); break;
    case OP_CSD: ok = j->cls ? DoCsdArduino(j) : DoCsdInternal(j); break;
    case OP_BENCH: ok = DoBench(j); break;
    }
    Log(ok ? L"=== Fim: concluído ===" : L"=== Fim: não concluído ===");
    PostMessageW(g_hwnd, WM_APP_DONE, ok, 0);
    return 0;
}

/* ---------------------------------------------------------------------- UI */
static void ShowCtl(int id, BOOL s) { ShowWindow(Ctl(id), s ? SW_SHOW : SW_HIDE); }
static void EnableCtl(int id, BOOL e) { EnableWindow(Ctl(id), e); }

static void UpdateEnable(void);
static int RecoverMode(void) {
    int i = (int)SendMessageW(Ctl(ID_COMBOB), CB_GETCURSEL, 0, 0);
    return i < 0 ? 0 : i;
}

static const wchar_t *RECOVER_MODES[] = {
    L"Análise detalhada de partições (não altera nada)",
    L"Ver e copiar arquivos, inclusive de partições Linux",
    L"Reconstruir partições perdidas (não apaga dados)",
    L"Recuperar arquivos apagados (fotos, vídeos, PDF)",
    L"Restaurar o cartão: apaga tudo e formata"};
static const wchar_t *RECOVER_DESC[] = {
    L"Analisa o cartão sem alterar nada: capacidade informada, tabela de partições, sistemas de arquivos e partições perdidas. Explica por que o cartão mostra pouca capacidade ou fica sem acesso.",
    L"Abre as partições do cartão (FAT, exFAT, ext4 de consoles Linux), inclusive as que o Windows não mostra, e copia os arquivos para o PC. Somente leitura.",
    L"Reescreve a tabela de partições (MBR) usando as partições encontradas no cartão. Não apaga dados e guarda uma cópia do início do cartão em Documentos antes de alterar.",
    L"Procura fotos (JPG/PNG), vídeos (MP4/MOV) e PDFs no cartão inteiro ou em um arquivo de imagem e os salva numa pasta do PC. Só lê.",
    L"Apaga tudo, cria uma partição do tamanho total e formata (FAT32 até 32 GB, exFAT acima). Use depois de salvar os dados; não conserta cartão travado em somente leitura."};

/* ---------------------------------------------------------------- layout */
static int g_cardBot = TASK_Y + 100, g_areaTop = 400;
/* "vista de resultado": no Diagnóstico e na Proteção, depois de iniciar, o formulário some e o resultado ocupa a página */
static BOOL g_resView = FALSE;
static const int TASK_CTLS[] = {ID_DESC, ID_LBL2, ID_COMBO2, ID_COMBOB, ID_COMBOC, ID_LBLFILE, ID_EDITFILE, ID_BROWSE,
                                ID_CHK1, ID_CHK2, ID_RADA, ID_RADB, ID_WARN, ID_COMBOP, ID_DETECT};

static BOOL CtlVisible(int id) {
    HWND c = Ctl(id);
    return c && (GetWindowLongW(c, GWL_STYLE) & WS_VISIBLE) != 0;
}
static void Place(int id, int x, int y, int w, int h) { MoveWindow(Ctl(id), S(x), S(y), S(w), S(h), TRUE); }

/* altura (em pixels lógicos) de um texto quebrado em linhas numa largura lógica */
static int TextH(const wchar_t *t, int w, HFONT f) {
    HDC dc = GetDC(g_hwnd);
    HGDIOBJ o = SelectObject(dc, f);
    RECT r = {0, 0, S(w), 0};
    DrawTextW(dc, t, -1, &r, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(dc, o);
    ReleaseDC(g_hwnd, dc);
    return MulDiv(r.bottom, 96, g_dpi) + 1;
}

/* posiciona tudo: cartão do dispositivo, campos visíveis da tarefa, ações e área de resultado/registro */
static void Relayout(void) {
    int refw = g_fontIcon ? 32 : 92;
    Place(ID_LBL1, MX + 56, DEV_Y + 12, 400, 18);
    Place(ID_SHOWALL, MX + MW - 16 - 180, DEV_Y + 10, 180, 22);
    Place(ID_COMBO1, MX + 56, DEV_Y + 32, MW - 56 - 16 - refw - 8, 300);
    Place(ID_REFRESH, MX + MW - 16 - refw, DEV_Y + 32, refw, 32);

    const int X = MX + CARD_PAD, W = MW - 2 * CARD_PAD;
    int y = TASK_Y + CARD_PAD, gap = 0;
    wchar_t d[600];
    GetDlgItemTextW(g_hwnd, ID_DESC, d, 600);
    int dh = TextH(d, W, g_font);
    Place(ID_DESC, X, y, W, dh);
    y += dh + 14; gap = 14;
    if (CtlVisible(ID_RADA)) { Place(ID_RADA, X, y, W, 24); y += 32; Place(ID_RADB, X, y, W, 24); y += 32 + 8; gap = 16; }
    if (CtlVisible(ID_COMBOP)) {
        Place(ID_LBL2, X, y, W, 20);
        y += 24;
        Place(ID_COMBOP, X, y, W - 108, 300);
        Place(ID_DETECT, X + W - 100, y, 100, 32);
        y += 32 + 16; gap = 16;
    } else if (CtlVisible(ID_LBL2)) {
        Place(ID_LBL2, X, y, W, 20);
        y += 24;
        if (CtlVisible(ID_COMBO2)) Place(ID_COMBO2, X, y, W, 300);
        if (CtlVisible(ID_COMBO2) && CtlVisible(ID_COMBOB)) { y += 32 + 10; Place(ID_COMBOB, X, y, W, 300); }
        else if (CtlVisible(ID_COMBOB)) {
            if (CtlVisible(ID_COMBOC)) {
                int bw = W * 55 / 100;
                Place(ID_COMBOB, X, y, bw, 300);
                Place(ID_COMBOC, X + bw + 12, y, W - bw - 12, 300);
            } else
                Place(ID_COMBOB, X, y, W, 300);
        }
        y += 32 + 16; gap = 16;
    }
    if (CtlVisible(ID_LBLFILE)) {
        Place(ID_LBLFILE, X, y, W, 20);
        y += 24;
        Place(ID_EDITFILE, X, y, W - 108, 32);
        Place(ID_BROWSE, X + W - 100, y, 100, 32);
        y += 32 + 16; gap = 16;
    }
    if (CtlVisible(ID_CHK1)) { Place(ID_CHK1, X, y, W, 24); y += 32; gap = 8; }
    if (CtlVisible(ID_CHK2)) { Place(ID_CHK2, X, y, W, 24); y += 32; gap = 8; }
    if (CtlVisible(ID_WARN)) { y += 8 - gap; Place(ID_WARN, X, y, W, 40); y += 40; gap = 0; }
    g_cardBot = y - gap + CARD_PAD;
    int ay = g_cardBot + 16;
    if (g_resView) { g_cardBot = 0; ay = TASK_Y; }
    Place(ID_TITLE, MX, 18, 330, 40);
    Place(ID_STEPS, MX + MW - 400, 24, 400, 32);
    if (IsHero()) {
        /* tela inicial do Diagnóstico: botão grande no meio do painel */
        g_areaTop = TASK_Y;
        int ah0 = WIN_H - 16 - g_areaTop;
        Place(ID_RESULT, MX, g_areaTop, MW, ah0);
        Place(ID_LOG, MX + 10, g_areaTop + 8, MW - 20, ah0 - 16);
        Place(ID_START, MX + (MW - 280) / 2, g_areaTop + 238, 280, 48);
        Place(ID_OPTS, MX + (MW - 150) / 2, g_areaTop + 326, 150, 28);
        InvalidateRect(g_hwnd, NULL, TRUE);
        return;
    }
    Place(ID_START, MX, ay, 180, 32);
    Place(ID_CANCELBTN, MX + 188, ay, 110, 32);
    Place(ID_NEXT, MX + 188, ay, 210, 32); /* no lugar do Cancelar, que some quando nada está rodando */
    Place(ID_LOGBTN, MX + MW - 170, ay, 170, 32);
    Place(ID_OPTS, MX + MW - 170 - 150, ay, 150, 32);
    Place(ID_PROGRESS, MX, ay + 44, MW, 4);
    Place(ID_STATUS, MX, ay + 54, MW, 18);
    g_areaTop = ay + 80;
    int ah = WIN_H - 16 - g_areaTop;
    if (ah < 60) ah = 60;
    Place(ID_RESULT, MX, g_areaTop, MW, ah);
    Place(ID_LOG, MX + 10, g_areaTop + 8, MW - 20, ah - 16);
    InvalidateRect(g_hwnd, NULL, TRUE);
}

/* área inferior: no Diagnóstico alterna entre o veredito e o registro técnico; nas outras tarefas mostra o registro */
static void ShowArea(void) {
    static int lastHero = -1;
    BOOL hero = IsHero();
    if ((int)hero != lastHero) { lastHero = hero; Relayout(); }
    BOOL diag = IS_VIEW(g_op);
    BOOL log = !hero && (!diag || g_showLog);
    BOOL rv = diag && g_resView;
    if (rv)
        for (int i = 0; i < (int)(sizeof TASK_CTLS / sizeof TASK_CTLS[0]); i++) ShowCtl(TASK_CTLS[i], FALSE);
    int nxc = NextFor();
    BOOL nx = nxc != NX_NONE && !hero;
    ShowCtl(ID_OPTS, rv && !g_busy);
    ShowCtl(ID_CANCELBTN, !hero && (g_busy || (!diag && !nx)));
    ShowCtl(ID_RESULT, !log);
    ShowCtl(ID_LOG, log);
    ShowCtl(ID_LOGBTN, diag && !hero);
    ShowCtl(ID_PROGRESS, !hero);
    ShowCtl(ID_STATUS, !hero);
    ShowCtl(ID_STEPS, g_op == OP_DIAG || g_op == OP_AUTH || g_op == OP_BENCH);
    InvalidateRect(Ctl(ID_STEPS), NULL, FALSE);
    SetDlgItemTextW(g_hwnd, ID_LOGBTN, log ? L"Ver resultado" : L"Ver registro técnico");
    if (g_op == OP_DIAG) SetDlgItemTextW(g_hwnd, ID_START, g_busy ? L"Analisando..." : (hero ? L"Iniciar diagnóstico" : L"Analisar de novo"));
    if (nx) SetDlgItemTextW(g_hwnd, ID_NEXT, NX_TEXT[nxc]);
    ShowCtl(ID_NEXT, nx);
    InvalidateRect(Ctl(ID_START), NULL, FALSE);
    InvalidateRect(Ctl(ID_RESULT), NULL, FALSE);
    InvalidateRect(Ctl(ID_LOGBTN), NULL, FALSE);
    InvalidateRect(Ctl(ID_NEXT), NULL, FALSE);
    InvalidateRect(g_hwnd, NULL, TRUE);
}

static void RecoverUi(void) {
    int m = RecoverMode();
    BOOL f = (m == RM_CARVE);
    ShowCtl(ID_LBLFILE, f);
    ShowCtl(ID_EDITFILE, f);
    ShowCtl(ID_BROWSE, f);
    ShowCtl(ID_CHK1, f);
    if (!f) SetChk(ID_CHK1, FALSE);
    if (m >= 0 && m < RM_COUNT) SetDlgItemTextW(g_hwnd, ID_DESC, RECOVER_DESC[m]);
    UpdateEnable();
}

static void FillModeCombo(int op) {
    HWND c = Ctl(ID_COMBOB);
    SendMessageW(c, CB_RESETCONTENT, 0, 0);
    if (op == OP_RECOVER) {
        for (int i = 0; i < RM_COUNT; i++) SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)RECOVER_MODES[i]);
    } else if (op == OP_C2I) {
        SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)L"Inteligente: só os dados usados, imagem .vhd (fica do tamanho dos dados)");
        SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)L"Cópia exata setor a setor, imagem .img (tamanho do cartão inteiro)");
    } else if (op == OP_C2C) {
        SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)L"Inteligente: só os dados usados, partição por partição (aceita cartão menor)");
        SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)L"Cópia exata setor a setor (destino do mesmo tamanho ou maior)");
    } else {
        for (int i = 0; i < NBRANDS; i++) SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)BRANDS[i].name);
    }
    SendMessageW(c, CB_SETCURSEL, 0, 0);
}

static void UpdateEnable(void) {
    if (g_op < 0) return;
    BOOL fileOn = TRUE;
    if (g_op == OP_VERIFY) {
        if (Checked(ID_CHK2)) SetChk(ID_CHK1, FALSE);
        EnableCtl(ID_CHK1, !Checked(ID_CHK2));
        fileOn = Checked(ID_CHK1) && !Checked(ID_CHK2);
    } else if (g_op == OP_RECOVER) fileOn = (RecoverMode() == RM_CARVE);
    if (g_op == OP_CSD) {
        BOOL ard = Checked(ID_RADB);
        ShowCtl(ID_LBL2, ard);
        ShowCtl(ID_COMBOP, ard);
        ShowCtl(ID_DETECT, ard);
        ShowCtl(ID_CHK1, ard);
        ShowCtl(ID_CHK2, ard);
        SetDlgItemTextW(g_hwnd, ID_LBL1, ard ? L"Cartão (no modo Arduino o cartão fica no módulo, esta seleção não é usada)" : L"Cartão (no leitor interno do computador)");
    }
    g_fileOn = fileOn;
    InvalidateRect(Ctl(ID_LBLFILE), NULL, TRUE);
    EnableCtl(ID_EDITFILE, fileOn);
    EnableCtl(ID_BROWSE, fileOn);

    const wchar_t *warn = NULL;
    switch (g_op) {
    case OP_I2C:
    case OP_C2C: warn = L"Todo o conteúdo do cartão de destino será apagado."; break;
    case OP_VERIFY:
        if (Checked(ID_CHK2)) warn = L"O teste de capacidade apaga todo o cartão e pode levar horas.";
        break;
    case OP_AUTH:
        if (Checked(ID_CHK1)) warn = L"O teste completo apaga todo o cartão e pode levar horas.";
        break;
    case OP_RECOVER:
        if (RecoverMode() == RM_RESTORE) warn = L"Esta opção apaga todas as partições e dados do cartão.";
        break;
    case OP_BENCH:
        if (Checked(ID_CHK1)) warn = L"O teste de gravação copia 32 MB do meio do cartão, grava por cima e devolve o original. Não retire o cartão durante o teste.";
        break;
    case OP_CSD:
        if (Checked(ID_RADB) && Checked(ID_CHK1)) warn = L"Vai regravar o registro CSD do cartão (só o bit de proteção temporária).";
        break;
    }
    ShowCtl(ID_WARN, warn != NULL);
    if (warn) {
        SetDlgItemTextW(g_hwnd, ID_WARN, warn);
        InvalidateRect(Ctl(ID_WARN), NULL, TRUE);
    }
    int rm = RecoverMode();
    const wchar_t *st = (g_op == OP_RECOVER) ? ((rm >= 0 && rm < RM_COUNT) ? RECOVER_START[rm] : L"Iniciar") : START_T[g_op];
    SetDlgItemTextW(g_hwnd, ID_START, st);
    InvalidateRect(Ctl(ID_START), NULL, FALSE);
    Relayout();
}

static void FillPorts(void) {
    HWND c = Ctl(ID_COMBOP);
    int sel = 0;
    SerEnumPorts();
    SendMessageW(c, CB_RESETCONTENT, 0, 0);
    for (int i = 0; i < g_nports; i++) {
        wchar_t t[260];
        _snwprintf(t, 259, L"%ls%ls", g_ports[i].name, g_ports[i].likely ? L"  (provável Arduino/ESP32)" : L"");
        t[259] = 0;
        int at = (int)SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)t);
        SendMessageW(c, CB_SETITEMDATA, at, i);
        if (g_ports[i].likely && !g_ports[sel].likely) sel = at;
    }
    if (g_nports == 0) {
        int at = (int)SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)L"Nenhum Arduino encontrado: conecte a placa pelo USB e clique em Detectar");
        SendMessageW(c, CB_SETITEMDATA, at, (LPARAM)-1);
    }
    SendMessageW(c, CB_SETCURSEL, sel, 0);
}

static void RedrawNav(void) {
    for (int i = 0; i < OP_COUNT; i++) InvalidateRect(Ctl(ID_OP0 + i), NULL, FALSE);
}

static void ApplyOp(int op) {
    static const wchar_t *desc[] = {
        L"Guarda o cartão num arquivo. No modo inteligente, a imagem .vhd leva só os dados usados (um cartão de 64 GB com 25 GB usados vira um arquivo de uns 25 GB) e abre no Windows com dois cliques.",
        L"Grava uma imagem (.vhd, .img, .iso, .bin) no cartão e confere. Imagem maior que o cartão também funciona se os dados couberem: a última partição é reduzida.",
        L"Copia um cartão para outro. No modo inteligente copia só os dados usados, partição por partição, e funciona até num cartão menor, se os dados couberem.",
        L"Lê o cartão inteiro procurando setores ilegíveis e, se quiser, compara com uma imagem.",
        L"Repara cartão sem acesso ou com capacidade errada e recupera arquivos.",
        L"Descobre se o cartão é verdadeiro: fabricante gravado no cartão, capacidade real e velocidade.",
        L"Descobre o defeito do cartão, se ele tem conserto e se os dados podem ser salvos.",
        L"Lê os registros internos do cartão (CID e CSD) e mostra se a gravação está travada por um bit de proteção, por senha ou pelo firmware.",
        L"Mede a velocidade de leitura e gravação, sequencial e de 4 KB, e diz se o cartão serve para game stick, fotos e vídeo ou se é um cartão genérico em que não dá para confiar."};
    if (op != g_op) g_diagForm = FALSE;
    if (op == OP_DIAG && !(g_vd.valid && g_vd.op == OP_DIAG)) g_showLog = FALSE;
    g_op = op;
    g_hover = 0;
    g_resView = (op == OP_DIAG && !g_diagForm);
    RedrawNav();
    ShowCtl(ID_DESC, TRUE);
    SetDlgItemTextW(g_hwnd, ID_TITLE, NAV_T[op]);
    SetDlgItemTextW(g_hwnd, ID_DESC, desc[op]);
    ShowCtl(ID_LBL2, op == OP_C2C || op == OP_C2I || op == OP_AUTH || op == OP_RECOVER);
    ShowCtl(ID_COMBO2, op == OP_C2C);
    ShowCtl(ID_COMBOB, op == OP_AUTH || op == OP_RECOVER || op == OP_C2I || op == OP_C2C);
    if (op == OP_AUTH || op == OP_RECOVER || op == OP_C2I || op == OP_C2C) FillModeCombo(op);
    ShowCtl(ID_COMBOC, op == OP_AUTH);
    BOOL file = (op == OP_C2I || op == OP_I2C || op == OP_VERIFY || op == OP_RECOVER);
    ShowCtl(ID_LBLFILE, file);
    ShowCtl(ID_EDITFILE, file);
    ShowCtl(ID_BROWSE, file);
    ShowCtl(ID_CHK1, op <= OP_VERIFY || op == OP_AUTH || op == OP_DIAG || op == OP_BENCH);
    ShowCtl(ID_CHK2, op == OP_VERIFY);
    ShowCtl(ID_RADA, op == OP_CSD);
    ShowCtl(ID_RADB, op == OP_CSD);
    ShowCtl(ID_COMBOP, FALSE);
    ShowCtl(ID_DETECT, FALSE);
    EnableCtl(ID_CHK1, TRUE);
    SetChk(ID_CHK1, FALSE);
    SetChk(ID_CHK2, FALSE);
    switch (op) {
    case OP_C2I:
        SetDlgItemTextW(g_hwnd, ID_LBL1, L"Cartão de origem");
        SetDlgItemTextW(g_hwnd, ID_LBL2, L"Tipo de backup");
        SetDlgItemTextW(g_hwnd, ID_LBLFILE, L"Salvar a imagem em");
        SetDlgItemTextW(g_hwnd, ID_CHK1, L"Conferir a imagem depois de criar (demora mais)");
        break;
    case OP_I2C:
        SetDlgItemTextW(g_hwnd, ID_LBL1, L"Cartão de destino (será apagado)");
        SetDlgItemTextW(g_hwnd, ID_LBLFILE, L"Arquivo de imagem (.vhd, .img, .iso, .bin)");
        SetDlgItemTextW(g_hwnd, ID_CHK1, L"Conferir depois de gravar (recomendado)");
        SetChk(ID_CHK1, TRUE);
        break;
    case OP_C2C:
        SetDlgItemTextW(g_hwnd, ID_LBL1, L"Cartão de origem");
        SetDlgItemTextW(g_hwnd, ID_LBL2, L"Cartão de destino (será apagado)");
        SetDlgItemTextW(g_hwnd, ID_CHK1, L"Conferir depois de copiar (recomendado)");
        SetChk(ID_CHK1, TRUE);
        break;
    case OP_VERIFY:
        SetDlgItemTextW(g_hwnd, ID_LBL1, L"Cartão");
        SetDlgItemTextW(g_hwnd, ID_LBLFILE, L"Imagem para comparar");
        SetDlgItemTextW(g_hwnd, ID_CHK1, L"Comparar com um arquivo de imagem");
        SetDlgItemTextW(g_hwnd, ID_CHK2, L"Teste de capacidade real, estilo H2testw (apaga o cartão; demora horas)");
        break;
    case OP_RECOVER:
        SetDlgItemTextW(g_hwnd, ID_LBL1, L"Cartão");
        SetDlgItemTextW(g_hwnd, ID_LBL2, L"O que fazer");
        SetDlgItemTextW(g_hwnd, ID_LBLFILE, L"Pasta para salvar os arquivos (fora do cartão)");
        SetDlgItemTextW(g_hwnd, ID_CHK1, L"Ler de um arquivo de imagem (.img) em vez do cartão");
        break;
    case OP_AUTH:
        SetDlgItemTextW(g_hwnd, ID_LBL1, L"Cartão");
        SetDlgItemTextW(g_hwnd, ID_LBL2, L"Marca da etiqueta e classe de velocidade anunciada");
        SetDlgItemTextW(g_hwnd, ID_CHK1, L"Teste completo de capacidade e velocidade (apaga o cartão; demora horas)");
        break;
    case OP_CSD:
        SetDlgItemTextW(g_hwnd, ID_RADA, L"Leitor interno do computador (só lê os registros)");
        SetDlgItemTextW(g_hwnd, ID_RADB, L"Arduino ou ESP32 conectado por USB (lê, testa e pode desligar a proteção)");
        SetDlgItemTextW(g_hwnd, ID_LBL2, L"Porta do Arduino");
        SetDlgItemTextW(g_hwnd, ID_CHK1, L"Tentar desligar a proteção temporária, se estiver ligada");
        SetDlgItemTextW(g_hwnd, ID_CHK2, L"Testar gravação pelo Arduino (grava 1 bloco e devolve o original)");
        if (!Checked(ID_RADA) && !Checked(ID_RADB)) SetChk(ID_RADA, TRUE);
        SetChk(ID_CHK2, TRUE);
        FillPorts();
        break;
    case OP_DIAG:
        SetDlgItemTextW(g_hwnd, ID_LBL1, L"Cartão");
        SetDlgItemTextW(g_hwnd, ID_CHK1, L"Testar gravação (grava 4 KB em 2 pontos e devolve o conteúdo original)");
        SetChk(ID_CHK1, TRUE);
        break;
    case OP_BENCH:
        SetDlgItemTextW(g_hwnd, ID_LBL1, L"Cartão");
        SetDlgItemTextW(g_hwnd, ID_CHK1, L"Medir também a gravação (usa 32 MB no meio do cartão e devolve o conteúdo original)");
        SetChk(ID_CHK1, TRUE);
        break;
    }
    SetDlgItemTextW(g_hwnd, ID_EDITFILE, L"");
    if (op == OP_RECOVER) RecoverUi();
    else UpdateEnable();
    ShowArea();
}

static void OnNext(void) {
    switch (NextFor()) {
    case NX_BACKUP: ApplyOp(OP_C2I); break;
    case NX_AUTH: ApplyOp(OP_AUTH); break;
    case NX_BENCH: ApplyOp(OP_BENCH); break;
    case NX_CARVE:
    case NX_REBUILD:
    case NX_RESTORE:
        ApplyOp(OP_RECOVER);
        SendMessageW(Ctl(ID_COMBOB), CB_SETCURSEL, g_vd.next == NX_CARVE ? RM_CARVE : (g_vd.next == NX_REBUILD ? RM_REBUILD : RM_RESTORE), 0);
        RecoverUi();
        break;
    }
}

static void AppendLog(const wchar_t *s) {
    HWND e = Ctl(ID_LOG);
    int n = GetWindowTextLengthW(e);
    SendMessageW(e, EM_SETSEL, n, n);
    SendMessageW(e, EM_REPLACESEL, FALSE, (LPARAM)s);
}

static void SetBusy(BOOL b) {
    g_busy = b;
    for (int i = 0; i < OP_COUNT; i++) EnableCtl(ID_OP0 + i, !b);
    EnableCtl(ID_COMBO1, !b); EnableCtl(ID_COMBO2, !b); EnableCtl(ID_COMBOB, !b); EnableCtl(ID_COMBOC, !b);
    EnableCtl(ID_REFRESH, !b); EnableCtl(ID_SHOWALL, !b); EnableCtl(ID_EJECT, !b);
    EnableCtl(ID_CHK1, !b); EnableCtl(ID_CHK2, !b); EnableCtl(ID_RADA, !b); EnableCtl(ID_RADB, !b);
    EnableCtl(ID_START, !b);
    EnableCtl(ID_CANCELBTN, b);
    if (b) { EnableCtl(ID_EDITFILE, FALSE); EnableCtl(ID_BROWSE, FALSE); g_fileOn = FALSE; InvalidateRect(Ctl(ID_LBLFILE), NULL, TRUE); }
    else UpdateEnable();
    ShowArea();
}

static void UpdateProgress(void) {
    wchar_t ph[200], st[512], a[32], bb[32], sp[40], eta[40];
    HWND pb = Ctl(ID_PROGRESS);
    EnterCriticalSection(&g_cs);
    wcscpy(ph, g_phase);
    LeaveCriticalSection(&g_cs);
    if (g_indet) {
        unsigned t = (unsigned)((GetTickCount64() / 12) % 2000);
        SendMessageW(pb, PBM_SETPOS, t < 1000 ? t : 2000 - t, 0);
        SetDlgItemTextW(g_hwnd, ID_STATUS, ph);
        return;
    }
    LONGLONG done = g_done, total = g_total;
    int pm = total > 0 ? (int)(done * 1000 / total) : 0;
    SendMessageW(pb, PBM_SETPOS, pm, 0);
    ULONGLONG el = GetTickCount64() - g_t0;
    FmtSize((uint64_t)done, a, 32);
    FmtSize((uint64_t)total, bb, 32);
    wcscpy(sp, L"—");
    wcscpy(eta, L"calculando");
    if (el > 2000 && done > 0) {
        double bps = (double)done * 1000.0 / (double)el;
        wchar_t t[32];
        FmtSize((uint64_t)bps, t, 32);
        _snwprintf(sp, 39, L"%ls/s", t);
        if (total > done) FmtDur((double)(total - done) / bps, eta, 40);
        else wcscpy(eta, L"quase pronto");
    }
    _snwprintf(st, 511, L"%ls — %.1f%% (%ls de %ls) — %ls — restam %ls", ph, pm / 10.0, a, bb, sp, eta);
    st[511] = 0;
    for (wchar_t *c = st; *c; c++) if (*c == L'.' && c[1] >= L'0' && c[1] <= L'9' && c > st && c[-1] >= L'0' && c[-1] <= L'9') *c = L',';
    SetDlgItemTextW(g_hwnd, ID_STATUS, st);
}

static void Warn(const wchar_t *t) { MessageBoxW(g_hwnd, t, APP_TITLE, MB_OK | MB_ICONWARNING); }
static BOOL Confirm(const wchar_t *t) {
    return MessageBoxW(g_hwnd, t, L"SDCLONETOOLS — confirmação", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES;
}

static void StartJob(void) {
    if (g_demo[0]) { Warn(L"Modo demonstração: nenhuma operação é feita no disco."); return; }
    g_cancel = 0; g_done = 0; g_total = 0; g_indet = 0;
    g_summary[0] = 0;
    if (IS_VIEW(g_op)) { g_vd.valid = FALSE; g_showLog = FALSE; g_resView = TRUE; g_diagForm = FALSE; g_step = 0; g_mapDone = 0; }
    SetBusy(TRUE);
    if (IS_VIEW(g_op)) Relayout();
    SetDlgItemTextW(g_hwnd, ID_STATUS, L"Iniciando...");
    g_t0 = GetTickCount64();
    g_thread = CreateThread(NULL, 0, Worker, NULL, 0, NULL);
    if (!g_thread) { Warn(L"Não foi possível iniciar a operação."); SetBusy(FALSE); return; }
    SetTimer(g_hwnd, 1, 80, NULL);
}

static void CleanPath(wchar_t *p) {
    size_t l = wcslen(p);
    while (l > 0 && (p[l - 1] == L' ' || p[l - 1] == L'"')) p[--l] = 0;
    wchar_t *s = p;
    while (*s == L' ' || *s == L'"') s++;
    if (s != p) memmove(p, s, (wcslen(s) + 1) * sizeof(wchar_t));
}

static void OnStart(void) {
    if (g_busy) return;
    Job *j = &g_job;
    memset(j, 0, sizeof *j);
    j->op = g_op;
    wchar_t path[MAX_PATH], msg[1800];
    GetDlgItemTextW(g_hwnd, ID_EDITFILE, path, MAX_PATH);
    CleanPath(path);
    DiskInfo *s = FindDisk(SelectedDisk(ID_COMBO1));
    DiskInfo *t = FindDisk(SelectedDisk(ID_COMBO2));
    BOOL c1 = Checked(ID_CHK1), c2 = Checked(ID_CHK2);
    BOOL imgSrc = (g_op == OP_RECOVER && RecoverMode() == RM_CARVE && c1);
    BOOL ard = (g_op == OP_CSD && Checked(ID_RADB));
    static DiskInfo noDisk;
    if (!s && (imgSrc || ard)) { memset(&noDisk, 0, sizeof noDisk); noDisk.index = -1; s = &noDisk; }
    if (!s) { Warn(L"Selecione um cartão. Se ele não aparece, insira-o e clique em Atualizar."); return; }
    if (s->index == g_sysDisk) { Warn(L"Esse é o disco do sistema. Operação recusada."); return; }
    j->src = s->index; j->srcSize = s->size; j->srcSec = s->sec; j->di = *s;
    wcsncpy(j->file, path, MAX_PATH - 1);
    j->verify = c1;
    switch (g_op) {
    case OP_C2I:
        j->mode = RecoverMode(); /* 0 = inteligente (.vhd), 1 = setor a setor (.img) */
        if (!path[0]) { Warn(L"Escolha onde salvar o arquivo de imagem."); return; }
        if (j->mode == 0 && !EndsWithI(path, L".vhd")) {
            wchar_t *dot = wcsrchr(path, L'.'), *sl = wcsrchr(path, L'\\');
            if (dot && (!sl || dot > sl)) *dot = 0;
            if (wcslen(path) + 4 < MAX_PATH) wcscat(path, L".vhd");
            SetDlgItemTextW(g_hwnd, ID_EDITFILE, path);
            wcsncpy(j->file, path, MAX_PATH - 1);
        }
        if (PathOnDisk(path, s->index)) { Warn(L"A imagem não pode ser salva dentro do próprio cartão."); return; }
        break;
    case OP_I2C: {
        if (!path[0]) { Warn(L"Escolha o arquivo de imagem."); return; }
        WIN32_FILE_ATTRIBUTE_DATA fa;
        if (!GetFileAttributesExW(path, GetFileExInfoStandard, &fa) || (fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) { Warn(L"O arquivo de imagem não foi encontrado."); return; }
        uint64_t fs64 = ((uint64_t)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
        if (fs64 == 0) { Warn(L"O arquivo de imagem está vazio."); return; }
        if (PathOnDisk(path, s->index)) { Warn(L"A imagem está dentro do cartão de destino."); return; }
        /* .vhd ou imagem maior que o cartão: grava só os dados usados (e reduz a última partição se precisar) */
        j->mode = (EndsWithI(path, L".vhd") || fs64 > s->size) ? 1 : 0;
        wchar_t a[32], b[32];
        FmtSize(fs64, a, 32); FmtSize(s->size, b, 32);
        _snwprintf(msg, 1799, L"Gravar a imagem:\n%ls (%ls)\n\nno cartão:\n%ls\n\n%lsTODO O CONTEÚDO DO CARTÃO SERÁ APAGADO. Continuar?", path, a, s->label,
                   j->mode && fs64 > s->size && !EndsWithI(path, L".vhd") ? L"A imagem é maior que o cartão: só os dados usados serão gravados e a última partição será reduzida, se os dados couberem.\n\n" : L"");
        if (!Confirm(msg)) return;
        j->dst = s->index; j->dstSize = s->size; j->dstSec = s->sec;
        break;
    }
    case OP_C2C:
        if (!t) { Warn(L"Selecione o cartão de destino."); return; }
        if (t->index == g_sysDisk) { Warn(L"O destino é o disco do sistema. Operação recusada."); return; }
        if (t->index == s->index) { Warn(L"Origem e destino são o mesmo cartão."); return; }
        j->mode = RecoverMode(); /* 0 = inteligente, 1 = setor a setor */
        if (t->size < s->size && j->mode == 1) {
            Warn(L"O cartão de destino é MENOR que o de origem.\n\nNa cópia setor a setor o destino precisa ser do mesmo tamanho ou maior. Escolha o modo inteligente: ele copia só os dados usados e reduz a última partição, se os dados couberem.");
            return;
        }
        _snwprintf(msg, 1799, L"Copiar:\n%ls\n\npara:\n%ls\n\nTODO O CONTEÚDO DO DESTINO SERÁ APAGADO. Continuar?", s->label, t->label);
        if (!Confirm(msg)) return;
        j->dst = t->index; j->dstSize = t->size; j->dstSec = t->sec;
        break;
    case OP_VERIFY:
        j->mode = c2 ? 2 : (c1 ? 1 : 0);
        if (j->mode == 1) {
            if (!path[0] || GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) { Warn(L"Escolha um arquivo de imagem existente para comparar."); return; }
        }
        if (j->mode == 2) {
            _snwprintf(msg, 1799, L"O teste de capacidade grava dados em TODO o cartão:\n%ls\n\nTODO O CONTEÚDO SERÁ APAGADO e o teste pode levar horas. Continuar?", s->label);
            if (!Confirm(msg)) return;
        }
        break;
    case OP_RECOVER:
        j->mode = RecoverMode();
        if (j->mode == RM_CARVE) {
            if (!path[0]) { Warn(L"Escolha a pasta onde salvar os arquivos recuperados."); return; }
            if (PathOnDisk(path, s->index)) { Warn(L"Salve os arquivos em outro disco, nunca no próprio cartão."); return; }
            CreateDirectoryW(path, NULL);
            DWORD at = GetFileAttributesW(path);
            if (at == INVALID_FILE_ATTRIBUTES || !(at & FILE_ATTRIBUTE_DIRECTORY)) { Warn(L"Não foi possível usar essa pasta."); return; }
            if (imgSrc) {
                OPENFILENAMEW ofn;
                memset(&ofn, 0, sizeof ofn);
                ofn.lStructSize = sizeof ofn;
                ofn.hwndOwner = g_hwnd;
                ofn.lpstrFile = j->img;
                ofn.nMaxFile = MAX_PATH;
                ofn.lpstrTitle = L"Escolha o arquivo de imagem do cartão";
                ofn.lpstrFilter = L"Imagens de disco (*.img;*.bin;*.raw;*.dd)\0*.img;*.bin;*.raw;*.dd\0Todos os arquivos (*.*)\0*.*\0";
                ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
                if (!GetOpenFileNameW(&ofn)) return;
            }
        } else if (j->mode == RM_RESTORE) {
            _snwprintf(msg, 1799, L"Restaurar o cartão:\n%ls\n\nTODAS AS PARTIÇÕES E DADOS SERÃO APAGADOS e o cartão será formatado.\n\nCartões de consoles (R36S e similares) perdem o sistema e precisarão de uma nova gravação da imagem (função Imagem → Cartão).\n\nObs.: se o diagnóstico mostrou que o PRÓPRIO CARTÃO informa capacidade pequena, esta função não resolve. Continuar?", s->label);
            if (!Confirm(msg)) return;
        }
        break;
    case OP_CSD:
        j->cls = ard ? 1 : 0;
        j->mode = (ard && c1) ? 1 : 0;
        j->verify = ard && c2;
        if (ard) {
            int i = (int)SendMessageW(Ctl(ID_COMBOP), CB_GETCURSEL, 0, 0);
            int k = i < 0 ? -1 : (int)SendMessageW(Ctl(ID_COMBOP), CB_GETITEMDATA, i, 0);
            if (k < 0 || k >= g_nports) { Warn(L"Nenhuma porta escolhida. Conecte o Arduino e clique em Detectar."); return; }
            wcsncpy(j->file, g_ports[k].port, MAX_PATH - 1);
            if (j->mode && !Confirm(L"Se a proteção temporária estiver ligada, o app vai regravar o registro CSD do cartão para desligá-la.\n\nO bit de proteção permanente nunca é alterado. Continuar?")) return;
        }
        break;
    case OP_AUTH:
        j->mode = c1 ? 2 : 0;
        j->brand = (int)SendMessageW(Ctl(ID_COMBOB), CB_GETCURSEL, 0, 0);
        if (j->brand < 0) j->brand = 0;
        {
            int ci = (int)SendMessageW(Ctl(ID_COMBOC), CB_GETCURSEL, 0, 0);
            j->cls = (ci > 0 && ci < NCLASSES) ? CLASSES[ci].mbs : 0;
        }
        if (j->mode == 2) {
            _snwprintf(msg, 1799, L"O teste completo grava dados em TODO o cartão:\n%ls\n\nTODO O CONTEÚDO SERÁ APAGADO e o teste pode levar horas. Continuar?", s->label);
            if (!Confirm(msg)) return;
        }
        break;
    }
    StartJob();
}

static void OnBrowse(void) {
    wchar_t path[MAX_PATH] = L"";
    GetDlgItemTextW(g_hwnd, ID_EDITFILE, path, MAX_PATH);
    CleanPath(path);
    if (g_op == OP_RECOVER) {
        BROWSEINFOW bi;
        memset(&bi, 0, sizeof bi);
        bi.hwndOwner = g_hwnd;
        bi.lpszTitle = L"Escolha a pasta onde salvar os arquivos recuperados";
        bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
        LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
        if (pidl) {
            wchar_t out[MAX_PATH];
            if (SHGetPathFromIDListW(pidl, out)) SetDlgItemTextW(g_hwnd, ID_EDITFILE, out);
            CoTaskMemFree(pidl);
        }
        return;
    }
    OPENFILENAMEW ofn;
    memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter = L"Imagens de disco (*.vhd;*.img;*.iso;*.bin;*.raw;*.dd)\0*.vhd;*.img;*.iso;*.bin;*.raw;*.dd\0Todos os arquivos (*.*)\0*.*\0";
    if (g_op == OP_C2I) {
        BOOL vhd = RecoverMode() == 0;
        if (!path[0]) wcscpy(path, vhd ? L"cartao.vhd" : L"cartao.img");
        ofn.lpstrFilter = vhd ? L"Imagem VHD (*.vhd)\0*.vhd\0" : L"Imagem de disco (*.img)\0*.img\0Todos os arquivos (*.*)\0*.*\0";
        ofn.lpstrDefExt = vhd ? L"vhd" : L"img";
        ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
        if (GetSaveFileNameW(&ofn)) SetDlgItemTextW(g_hwnd, ID_EDITFILE, path);
    } else {
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        if (GetOpenFileNameW(&ofn)) SetDlgItemTextW(g_hwnd, ID_EDITFILE, path);
    }
}

static void OnDone(BOOL ok) {
    KillTimer(g_hwnd, 1);
    if (g_thread) { WaitForSingleObject(g_thread, 5000); CloseHandle(g_thread); g_thread = NULL; }
    SendMessageW(Ctl(ID_PROGRESS), PBM_SETPOS, ok ? 1000 : (int)(g_total > 0 ? g_done * 1000 / g_total : 0), 0);
    SetDlgItemTextW(g_hwnd, ID_STATUS, ok ? L"Concluído." : L"Não concluído — veja o registro abaixo.");
    SetBusy(FALSE);
    RefreshDisks();
    {
        int jd = g_job.src;
        if (jd != g_jdisk) { g_jdisk = jd; memset(g_jdone, 0, sizeof g_jdone); }
        if (g_job.op == OP_DIAG && g_vd.valid) g_jdone[0] = TRUE;
        if (g_job.op == OP_AUTH && ok) g_jdone[1] = TRUE;
        if (g_job.op == OP_BENCH && g_vd.valid) g_jdone[2] = TRUE;
    }
    ShowArea();
    if (IS_VIEW(g_job.op)) { g_showLog = !g_vd.valid; ShowArea(); }
    FlashWindow(g_hwnd, TRUE);
    if (g_summary[0]) MessageBoxW(g_hwnd, g_summary, APP_TITLE, MB_OK | g_summaryIcon);
    if (g_openBrowse) {
        g_openBrowse = 0;
        if (ok) ShowBrowser();
    }
}

static HWND mk(const wchar_t *cls, const wchar_t *text, DWORD style, int x, int y, int w, int h, int id, DWORD ex) {
    return CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h), g_hwnd, (HMENU)(INT_PTR)id, g_inst, NULL);
}

static BOOL CALLBACK SetFontCb(HWND h, LPARAM f) { SendMessageW(h, WM_SETFONT, (WPARAM)f, TRUE); return TRUE; }

static int CALLBACK FontEnumCb(const LOGFONTW *lf, const TEXTMETRICW *tm, DWORD type, LPARAM l) {
    (void)lf; (void)tm; (void)type;
    *(BOOL *)l = TRUE;
    return 0;
}
static BOOL FontExists(const wchar_t *face) {
    LOGFONTW lf;
    memset(&lf, 0, sizeof lf);
    lf.lfCharSet = DEFAULT_CHARSET;
    wcsncpy(lf.lfFaceName, face, LF_FACESIZE - 1);
    BOOL found = FALSE;
    HDC dc = GetDC(NULL);
    EnumFontFamiliesExW(dc, &lf, FontEnumCb, (LPARAM)&found, 0);
    ReleaseDC(NULL, dc);
    return found;
}
static HFONT MkFont(int px, int weight, const wchar_t *face) {
    return CreateFontW(-S(px), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, face);
}

static void CreateUi(void) {
    const wchar_t *text = FontExists(L"Segoe UI Variable Text") ? L"Segoe UI Variable Text" : L"Segoe UI";
    const wchar_t *disp = FontExists(L"Segoe UI Variable Display") ? L"Segoe UI Variable Display" : L"Segoe UI";
    const wchar_t *icon = FontExists(L"Segoe Fluent Icons") ? L"Segoe Fluent Icons" : (FontExists(L"Segoe MDL2 Assets") ? L"Segoe MDL2 Assets" : NULL);
    g_font = MkFont(14, FW_NORMAL, text);
    g_fontB = MkFont(14, FW_SEMIBOLD, text);
    g_fontVal = MkFont(13, FW_NORMAL, text);
    g_fontValB = MkFont(13, FW_SEMIBOLD, text);
    g_fontSmall = MkFont(12, FW_NORMAL, text);
    g_fontH = MkFont(20, FW_SEMIBOLD, disp);
    g_fontTitle = MkFont(28, FW_SEMIBOLD, disp);
    g_fontMono = MkFont(13, FW_NORMAL, L"Consolas");
    if (icon) {
        g_fontIcon = MkFont(16, FW_NORMAL, icon);
        g_fontIconM = MkFont(20, FW_NORMAL, icon);
        g_fontIconL = MkFont(28, FW_NORMAL, icon);
    }
    g_brBg = CreateSolidBrush(CL_BG);
    g_brCard = CreateSolidBrush(CL_PANEL);
    g_brInput = CreateSolidBrush(CL_INPUT);
    g_brLog = CreateSolidBrush(CL_LOG);

    /* navegação */
    mk(L"STATIC", L"", SS_OWNERDRAW | WS_CLIPSIBLINGS, 0, 0, NAV_W, WIN_H, ID_HEADER, 0);
    for (int i = 0; i < OP_COUNT; i++)
        mk(L"BUTTON", NAV_T[NAV_ORDER[i]], BS_OWNERDRAW | WS_TABSTOP, 8, NavY(i), NAV_W - 16, 36, ID_OP0 + NAV_ORDER[i], 0);
    mk(L"BUTTON", L"Remover com segurança", BS_OWNERDRAW | WS_TABSTOP, 8, WIN_H - 52, NAV_W - 16, 36, ID_EJECT, 0);

    /* título da página */
    mk(L"STATIC", L"", SS_LEFT | SS_NOPREFIX, MX, 18, MW, 40, ID_TITLE, 0);
    mk(L"STATIC", L"", SS_OWNERDRAW, MX + MW - 420, 24, 420, 32, ID_STEPS, 0);

    /* cartão do dispositivo e campos da tarefa (posições finais vêm de Relayout) */
    mk(L"STATIC", L"Cartão", SS_LEFT | SS_NOPREFIX, 0, 0, 100, 18, ID_LBL1, 0);
    mk(L"BUTTON", L"Mostrar todos os discos", BS_OWNERDRAW | WS_TABSTOP, 0, 0, 100, 22, ID_SHOWALL, 0);
    DWORD cb = CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL | WS_TABSTOP;
    mk(L"COMBOBOX", L"", cb, 0, 0, 100, 300, ID_COMBO1, 0);
    mk(L"BUTTON", L"Atualizar", BS_OWNERDRAW | WS_TABSTOP, 0, 0, 32, 32, ID_REFRESH, 0);
    mk(L"STATIC", L"", SS_LEFT | SS_NOPREFIX, 0, 0, 100, 40, ID_DESC, 0);
    mk(L"STATIC", L"", SS_LEFT | SS_NOPREFIX, 0, 0, 100, 20, ID_LBL2, 0);
    mk(L"COMBOBOX", L"", cb, 0, 0, 100, 300, ID_COMBO2, 0);
    mk(L"COMBOBOX", L"", cb, 0, 0, 100, 300, ID_COMBOB, 0);
    mk(L"COMBOBOX", L"", cb, 0, 0, 100, 300, ID_COMBOC, 0);
    mk(L"COMBOBOX", L"", cb, 0, 0, 100, 300, ID_COMBOP, 0);
    mk(L"BUTTON", L"Detectar", BS_OWNERDRAW | WS_TABSTOP, 0, 0, 100, 32, ID_DETECT, 0);
    mk(L"STATIC", L"", SS_LEFT | SS_NOPREFIX, 0, 0, 100, 20, ID_LBLFILE, 0);
    HWND ed = mk(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP | WS_BORDER, 0, 0, 100, 32, ID_EDITFILE, 0);
    SendMessageW(ed, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(S(8), S(8)));
    mk(L"BUTTON", L"Procurar...", BS_OWNERDRAW | WS_TABSTOP, 0, 0, 100, 32, ID_BROWSE, 0);
    mk(L"BUTTON", L"", BS_OWNERDRAW | WS_TABSTOP, 0, 0, 100, 24, ID_CHK1, 0);
    mk(L"BUTTON", L"", BS_OWNERDRAW | WS_TABSTOP, 0, 0, 100, 24, ID_CHK2, 0);
    mk(L"BUTTON", L"", BS_OWNERDRAW | WS_TABSTOP, 0, 0, 100, 24, ID_RADA, 0);
    mk(L"BUTTON", L"", BS_OWNERDRAW | WS_TABSTOP, 0, 0, 100, 24, ID_RADB, 0);
    mk(L"STATIC", L"", SS_OWNERDRAW, 0, 0, 100, 40, ID_WARN, 0);

    /* ações, progresso, resultado e registro */
    mk(L"BUTTON", L"Iniciar", BS_OWNERDRAW | WS_TABSTOP, 0, 0, 180, 32, ID_START, 0);
    mk(L"BUTTON", L"Cancelar", BS_OWNERDRAW | WS_TABSTOP | WS_DISABLED, 0, 0, 110, 32, ID_CANCELBTN, 0);
    mk(L"BUTTON", L"", BS_OWNERDRAW | WS_TABSTOP, 0, 0, 200, 32, ID_NEXT, 0);
    mk(L"BUTTON", L"Ver registro técnico", BS_OWNERDRAW | WS_TABSTOP, 0, 0, 170, 32, ID_LOGBTN, 0);
    mk(L"BUTTON", L"Alterar opções", BS_OWNERDRAW | WS_TABSTOP, 0, 0, 150, 32, ID_OPTS, 0);
    HWND pb = mk(PROGRESS_CLASSW, L"", 0, 0, 0, MW, 4, ID_PROGRESS, 0);
    SendMessageW(pb, PBM_SETRANGE32, 0, 1000);
    mk(L"STATIC", L"Pronto. Confira sempre o cartão selecionado antes de gravar.", SS_LEFT | SS_ENDELLIPSIS | SS_NOPREFIX, 0, 0, MW, 18, ID_STATUS, 0);
    mk(L"STATIC", L"", SS_OWNERDRAW | WS_CLIPSIBLINGS, 0, 0, MW, 200, ID_RESULT, 0);
    HWND lg = mk(L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_TABSTOP, 0, 0, MW, 200, ID_LOG, 0);
    SendMessageW(lg, EM_LIMITTEXT, 0x7FFFFFFE, 0);
    SendMessageW(lg, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(S(4), S(4)));
    for (int i = 0; i < NBRANDS; i++) SendMessageW(Ctl(ID_COMBOB), CB_ADDSTRING, 0, (LPARAM)BRANDS[i].name);
    for (int i = 0; i < NCLASSES; i++) SendMessageW(Ctl(ID_COMBOC), CB_ADDSTRING, 0, (LPARAM)CLASSES[i].name);
    SendMessageW(Ctl(ID_COMBOB), CB_SETCURSEL, 0, 0);
    SendMessageW(Ctl(ID_COMBOC), CB_SETCURSEL, 0, 0);
    EnumChildWindows(g_hwnd, SetFontCb, (LPARAM)g_font);
    SendMessageW(lg, WM_SETFONT, (WPARAM)g_fontMono, TRUE);
    SendMessageW(Ctl(ID_TITLE), WM_SETFONT, (WPARAM)g_fontTitle, TRUE);
    SendMessageW(Ctl(ID_LBL1), WM_SETFONT, (WPARAM)g_fontSmall, TRUE);
    SendMessageW(Ctl(ID_STATUS), WM_SETFONT, (WPARAM)g_fontSmall, TRUE);
    /* controles nativos: tema escuro só quando o Windows está no modo escuro */
    const wchar_t *cfd = g_dark ? L"DarkMode_CFD" : NULL;
    SetTheme(Ctl(ID_COMBO1), cfd, NULL);
    SetTheme(Ctl(ID_COMBO2), cfd, NULL);
    SetTheme(Ctl(ID_COMBOB), cfd, NULL);
    SetTheme(Ctl(ID_COMBOC), cfd, NULL);
    SetTheme(Ctl(ID_COMBOP), cfd, NULL);
    SetTheme(ed, cfd, NULL);
    SetTheme(lg, g_dark ? L"DarkMode_Explorer" : L"Explorer", NULL);
    SetTheme(pb, L"", L"");
    SendMessageW(pb, 0x2001 /* PBM_SETBKCOLOR */, 0, (LPARAM)g_pal.btnEdge);
    SendMessageW(pb, 0x0409 /* PBM_SETBARCOLOR */, 0, (LPARAM)CL_NEON);
    static const int bids[] = {ID_OP0, ID_OP0 + 1, ID_OP0 + 2, ID_OP0 + 3, ID_OP0 + 4, ID_OP0 + 5, ID_OP0 + 6, ID_OP0 + 7, ID_OP0 + 8, ID_DETECT,
                               ID_REFRESH, ID_SHOWALL, ID_BROWSE, ID_CHK1, ID_CHK2, ID_RADA, ID_RADB, ID_START, ID_CANCELBTN, ID_NEXT, ID_LOGBTN, ID_OPTS, ID_EJECT};
    for (int i = 0; i < (int)(sizeof bids / sizeof bids[0]); i++) SetWindowSubclass(Ctl(bids[i]), BtnSub, bids[i], 0);
    SetWindowSubclass(Ctl(ID_STEPS), StepsSub, ID_STEPS, 0);
}

/* ------------------------------------------------ modo demonstração (para prints e testes de tela)
 * SDCloneTools.exe --demo=hero | busy | diag | diagbad | bench | auth
 * Não lê nem grava nenhum disco: preenche a tela com dados de exemplo. */

static void DemoUsage(Verdict *v) {
    UsageStat *u = &v->us;
    memset(u, 0, sizeof *u);
    const uint64_t G = 1024ull * 1024 * 1024, M = 1024ull * 1024;
    u->catBytes[CT_GAME] = 21 * G + 300 * M; u->catFiles[CT_GAME] = 1412;
    u->catBytes[CT_SYS] = 3 * G + 200 * M; u->catFiles[CT_SYS] = 5230;
    u->catBytes[CT_MUSIC] = 900 * M; u->catFiles[CT_MUSIC] = 212;
    u->catBytes[CT_PHOTO] = 420 * M; u->catFiles[CT_PHOTO] = 860;
    u->catBytes[CT_VIDEO] = 610 * M; u->catFiles[CT_VIDEO] = 14;
    u->catBytes[CT_DOC] = 12 * M; u->catFiles[CT_DOC] = 96;
    for (int c = 0; c < CT_N; c++) { u->usedBytes += u->catBytes[c]; u->files += u->catFiles[c]; }
    u->dirs = 418;
    u->hasRoms = u->hasEmu = u->hasLinux = 1;
    u->scannedParts = 2;
    u->scannedBytes = 59 * G;
    static const struct { const char *e; uint32_t f; uint64_t b; int c; } ex[] = {
        {"zip", 610, 9 * G, CT_GAME}, {"chd", 42, 6 * G, CT_GAME}, {"gba", 380, 3 * G, CT_GAME}, {"so", 812, G, CT_SYS},
        {"mp3", 212, 900 * M, CT_MUSIC}, {"jpg", 860, 420 * M, CT_PHOTO}, {"sfc", 380, 400 * M, CT_GAME}, {"mp4", 14, 610 * M, CT_VIDEO}};
    for (int i = 0; i < 8; i++) {
        strcpy(u->ext[i].ext, ex[i].e);
        u->ext[i].files = ex[i].f; u->ext[i].bytes = ex[i].b; u->ext[i].cat = ex[i].c;
    }
    u->next = 8;
    v->usage = TRUE;
    v->uCard = 63864569856ull;
    v->uProfile = UP_CONSOLE;
    VS(uTitle, L"%ls", ProfileTitle(UP_CONSOLE));
    v->uFitLevel = 3;
}

static void SetupDemo(void) {
    if (!g_demo[0]) return;
    g_ndisks = 1;
    DiskInfo *d = &g_disks[0];
    memset(d, 0, sizeof *d);
    d->index = 1; d->size = 63864569856ull; d->sec = 512; d->bus = 7; d->removable = TRUE; d->likelyCard = TRUE;
    wcscpy(d->vendor, L"SanDisk"); wcscpy(d->product, L"Ultra microSDXC"); wcscpy(d->letters, L"E:");
    wcscpy(d->label, L"Disco 1 — SanDisk Ultra microSDXC — 59,5 GB [USB] (E:)");
    FillDiskCombo(ID_COMBO1, 1);
    Verdict *v = &g_vd;
    memset(v, 0, sizeof *v);
    srand(7);
    for (int i = 0; i < MAP_N; i++) v->cell[i] = (i % 23 == 5 || i > 110) ? MC_EMPTY : MC_DATA;
    if (strcmp(g_demo, "hero") == 0) { ApplyOp(OP_DIAG); return; }
    if (strcmp(g_demo, "busy") == 0) {
        ApplyOp(OP_DIAG);
        g_step = 3; g_mapDone = MAP_N;
        SetPhaseText(L"Lendo o conteúdo — 4.812 arquivos, 301 pastas");
        g_resView = TRUE;
        SetBusy(TRUE);
        Relayout();
        SetTimer(g_hwnd, 1, 80, NULL);
        return;
    }
    if (strcmp(g_demo, "diag") == 0 || strcmp(g_demo, "diagbad") == 0) {
        BOOL bad = strcmp(g_demo, "diagbad") == 0;
        v->op = OP_DIAG; v->map = TRUE;
        for (int i = 0; i < MAP_N; i++) {
            switch (v->cell[i]) { case MC_DATA: v->nData++; break; default: v->nEmpty++; }
        }
        DemoUsage(v);
        if (bad) {
            v->level = 2; v->next = NX_BACKUP; v->hRead = 0; v->hWrite = 2; v->hPart = 0;
            VS(title, L"Cartão travado em somente leitura");
            VS(todo, L"Faça um backup (imagem) do cartão e recupere os arquivos a partir da imagem.");
            v->uFitLevel = 2;
        } else {
            v->level = 0; v->next = NX_AUTH;
            VS(title, L"Nenhum defeito encontrado");
            g_jdisk = 1; g_jdone[0] = TRUE;
        }
        v->valid = TRUE;
        ApplyOp(OP_DIAG);
        return;
    }
    if (strcmp(g_demo, "bench") == 0) {
        g_jdisk = 1; g_jdone[0] = g_jdone[1] = g_jdone[2] = TRUE;
        v->op = OP_BENCH; v->bench = TRUE;
        BenchRes *br = &v->br;
        br->seqR = 87.4; br->seqW = 41.2; br->rMin = 84; br->rMax = 90; br->wMed = 42; br->wMin = 30; br->rr = 1850; br->rw = 610; br->hasW = 1;
        BenchEval(br, &v->bj);
        for (int i = 0; i < 36; i++) v->curveR[i] = (float)(85 + (rand() % 60) / 10.0);
        v->nCurveR = 36;
        for (int i = 0; i < 16; i++) v->curveW[i] = (float)(38 + (rand() % 80) / 10.0);
        v->nCurveW = 16;
        VS(title, L"Bom para game stick, fotos e vídeo 4K");
        VS(cause, L"Leitura sequencial: 87,4 MB/s. Gravação sequencial: 41,2 MB/s (janela mais lenta: 30,0 MB/s).");
        VS(card, L"Leitura: 1850 operações/s (bom). Gravação: 610 operações/s (bom).");
        VS(data, L"Pela gravação sequencial, equivale a U3 / V30. Medido neste leitor: um leitor lento mostra menos do que o cartão entrega.");
        VS(todo, L"Game stick: bom. Foto e vídeo: fotos e vídeo até 4K. Confiança no cartão: boa.");
        v->valid = TRUE;
        ApplyOp(OP_BENCH);
        g_resView = TRUE;
        Relayout();
        ShowArea();
        return;
    }
    if (strcmp(g_demo, "auth") == 0) { g_jdisk = 1; g_jdone[0] = TRUE; ApplyOp(OP_AUTH); return; }
    if (strcmp(g_demo, "backup") == 0) { ApplyOp(OP_C2I); SetDlgItemTextW(g_hwnd, ID_EDITFILE, L"D:\\Backups\\cartao_r36s.vhd"); return; }
    if (strcmp(g_demo, "clone") == 0) {
        g_ndisks = 2;
        DiskInfo *e = &g_disks[1];
        *e = *d;
        e->index = 2; e->size = 31914983424ull; wcscpy(e->vendor, L"Samsung"); wcscpy(e->product, L"EVO Plus"); wcscpy(e->letters, L"F:");
        wcscpy(e->label, L"Disco 2 — Samsung EVO Plus — 29,7 GB [USB] (F:)");
        FillDiskCombo(ID_COMBO1, 1);
        FillDiskCombo(ID_COMBO2, 2);
        ApplyOp(OP_C2C);
        return;
    }
}

/* cartão novo conectado: seleciona e volta para a tela inicial do Diagnóstico */
static void OnDisksChanged(void) {
    static int known[32], nknown = -1;
    int fresh = -1;
    if (nknown >= 0)
        for (int i = 0; i < g_ndisks; i++) {
            BOOL seen = FALSE;
            for (int k = 0; k < nknown; k++) if (known[k] == g_disks[i].index) seen = TRUE;
            if (!seen) fresh = g_disks[i].index;
        }
    nknown = 0;
    for (int i = 0; i < g_ndisks && i < 32; i++) known[nknown++] = g_disks[i].index;
    if (fresh < 0 || g_busy) { ShowArea(); return; }
    HWND c = Ctl(ID_COMBO1);
    int n = (int)SendMessageW(c, CB_GETCOUNT, 0, 0);
    for (int i = 0; i < n; i++)
        if ((int)SendMessageW(c, CB_GETITEMDATA, i, 0) == fresh) SendMessageW(c, CB_SETCURSEL, i, 0);
    g_vd.valid = FALSE;
    g_showLog = FALSE;
    g_diagForm = FALSE;
    if (g_op != OP_DIAG) ApplyOp(OP_DIAG);
    else ShowArea();
    FlashWindow(g_hwnd, TRUE);
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE:
        g_hwnd = h;
        CreateUi();
        RefreshDisks();
        ApplyOp(OP_DIAG);
        OnDisksChanged();
        SetupDemo();
        Log(L"Pronto. Escolha o cartão e a tarefa no menu à esquerda.");
        return 0;
    case WM_COMMAND: {
        int id = LOWORD(w), code = HIWORD(w);
        if (id >= ID_OP0 && id < ID_OP0 + OP_COUNT && code == BN_CLICKED) { if (!g_busy) ApplyOp(id - ID_OP0); }
        else if (id == ID_REFRESH && code == BN_CLICKED) { if (!g_busy) RefreshDisks(); }
        else if (id == ID_EJECT && code == BN_CLICKED) { if (!g_busy) DoEject(); }
        else if (id == ID_COMBO1 && code == CBN_SELCHANGE) {
            if (!g_busy && IS_VIEW(g_op) && g_op != OP_CSD) { g_vd.valid = FALSE; g_showLog = FALSE; }
            ShowArea();
        }
        else if (id == ID_SHOWALL && code == BN_CLICKED) { if (!g_busy) { SetChk(ID_SHOWALL, !Checked(ID_SHOWALL)); RefreshDisks(); } }
        else if (id == ID_BROWSE && code == BN_CLICKED) OnBrowse();
        else if (id == ID_COMBOB && code == CBN_SELCHANGE) {
            if (g_op == OP_RECOVER) RecoverUi();
            else if (g_op == OP_C2I) { /* troca a extensão do arquivo conforme o tipo de backup */
                wchar_t pth[MAX_PATH];
                GetDlgItemTextW(g_hwnd, ID_EDITFILE, pth, MAX_PATH);
                wchar_t *dot = wcsrchr(pth, L'.'), *sl = wcsrchr(pth, L'\\');
                if (pth[0] && dot && (!sl || dot > sl) && wcslen(pth) < MAX_PATH - 5) { wcscpy(dot, RecoverMode() == 0 ? L".vhd" : L".img"); SetDlgItemTextW(g_hwnd, ID_EDITFILE, pth); }
                UpdateEnable();
            } else UpdateEnable();
        }
        else if ((id == ID_CHK1 || id == ID_CHK2 || id == ID_RADA || id == ID_RADB) && code == BN_CLICKED) {
            if (id == ID_RADA || id == ID_RADB) { SetChk(ID_RADA, id == ID_RADA); SetChk(ID_RADB, id == ID_RADB); }
            else SetChk(id, !Checked(id));
            UpdateEnable();
        }
        else if (id == ID_START && code == BN_CLICKED) OnStart();
        else if (id == ID_NEXT && code == BN_CLICKED) { if (!g_busy) OnNext(); }
        else if (id == ID_LOGBTN && code == BN_CLICKED) { g_showLog = !g_showLog; ShowArea(); }
        else if (id == ID_OPTS && code == BN_CLICKED) { if (!g_busy) { if (g_op == OP_DIAG) g_diagForm = TRUE; ApplyOp(g_op); } }
        else if (id == ID_DETECT && code == BN_CLICKED) {
            if (!g_busy) {
                FillPorts();
                int lk = 0;
                for (int i = 0; i < g_nports; i++) lk += g_ports[i].likely;
                if (g_nports == 0) Warn(L"Nenhuma porta serial encontrada.\n\nConecte o Arduino ou ESP32 pelo cabo USB (cabo de dados, não só de carga) e instale o driver da placa (CH340 ou CP210x, se for um clone).");
                else SetDlgItemTextW(h, ID_STATUS, lk ? L"Arduino/ESP32 detectado. Confira a porta e clique em Ler proteção." : L"Portas encontradas, mas nenhuma parece ser um Arduino/ESP32. Escolha a porta manualmente.");
            }
        }
        else if (id == ID_CANCELBTN && code == BN_CLICKED) {
            if (g_busy) { g_cancel = 1; SetDlgItemTextW(h, ID_STATUS, L"Cancelando..."); }
        }
        return 0;
    }
    case WM_ERASEBKGND: {
        HDC dc = (HDC)w;
        RECT rc;
        GetClientRect(h, &rc);
        Fill(dc, rc, CL_BG);
        /* cartão do dispositivo */
        RECT dv = {S(MX), S(DEV_Y), S(MX + MW), S(DEV_Y + DEV_H)};
        RRect(dc, dv, S(6), CL_PANEL, CL_EDGE);
        RECT gi = {S(MX + 12), S(DEV_Y), S(MX + 48), S(DEV_Y + DEV_H)};
        if (g_fontIconL) Glyph(dc, 0xE7F1, &gi, CL_NEON, g_fontIconL);
        /* cartão da tarefa */
        if (g_cardBot > TASK_Y) {
            RECT tk = {S(MX), S(TASK_Y), S(MX + MW), S(g_cardBot)};
            RRect(dc, tk, S(6), CL_PANEL, CL_EDGE);
        }
        /* moldura do registro */
        if (CtlVisible(ID_LOG)) {
            RECT lr = {S(MX), S(g_areaTop), S(MX + MW), rc.bottom - S(16)};
            RRect(dc, lr, S(6), CL_LOG, CL_EDGE);
        }
        return 1;
    }
    case WM_DRAWITEM: {
        DRAWITEMSTRUCT *d = (DRAWITEMSTRUCT *)l;
        if (d->CtlType == ODT_BUTTON) DrawButton(d);
        else if (d->CtlType == ODT_COMBOBOX) DrawCombo(d);
        else if (d->CtlType == ODT_STATIC) {
            if (d->CtlID == ID_HEADER) DrawSidebar(d);
            else if (d->CtlID == ID_WARN) DrawWarn(d);
            else if (d->CtlID == ID_RESULT) DrawResult(d);
            else if (d->CtlID == ID_STEPS) DrawSteps(d);
        }
        return TRUE;
    }
    case WM_MEASUREITEM: {
        MEASUREITEMSTRUCT *mi = (MEASUREITEMSTRUCT *)l;
        if (mi->CtlType == ODT_COMBOBOX) { mi->itemHeight = S(26); return TRUE; }
        break;
    }
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)w;
        HWND c = (HWND)l;
        int id = GetDlgCtrlID(c);
        if (m == WM_CTLCOLORLISTBOX) { SetTextColor(dc, CL_TXT); SetBkColor(dc, CL_INPUT); return (LRESULT)g_brInput; }
        if (id == ID_LOG) { SetTextColor(dc, CL_LOGTXT); SetBkColor(dc, CL_LOG); return (LRESULT)g_brLog; }
        if (id == ID_EDITFILE) { SetTextColor(dc, IsWindowEnabled(c) ? CL_TXT : CL_DIM); SetBkColor(dc, CL_INPUT); return (LRESULT)g_brInput; }
        COLORREF tc = CL_TXT, bk = CL_BG;
        HBRUSH br = g_brBg;
        if (id == ID_LBL1 || id == ID_LBL2 || id == ID_LBLFILE || id == ID_DESC) {
            bk = CL_PANEL;
            br = g_brCard;
            if (id == ID_LBL1 || id == ID_DESC) tc = CL_DIM;
            else if (id == ID_LBLFILE && !g_fileOn) tc = CL_DIM;
        } else if (id == ID_STATUS) tc = CL_DIM;
        SetTextColor(dc, tc);
        SetBkColor(dc, bk);
        return (LRESULT)br;
    }
    case WM_TIMER:
        if (w == 1) { UpdateProgress(); if (IS_VIEW(g_op)) InvalidateRect(Ctl(ID_RESULT), NULL, FALSE); }
        else if (w == 2) { KillTimer(h, 2); if (!g_busy && !g_demo[0]) { RefreshDisks(); OnDisksChanged(); if (g_op == OP_CSD) FillPorts(); } }
        return 0;
    case WM_DEVICECHANGE:
        if (!g_busy && (w == DBT_DEVNODES_CHANGED)) SetTimer(h, 2, 1200, NULL);
        return TRUE;
    case WM_APP_LOG: {
        wchar_t *s = (wchar_t *)l;
        if (s) { AppendLog(s); free(s); }
        return 0;
    }
    case WM_APP_DONE:
        OnDone((BOOL)w);
        return 0;
    case WM_APP_ASK:
        return Confirm((const wchar_t *)l) ? IDYES : IDNO;
    case WM_CLOSE:
        if (g_busy) {
            if (MessageBoxW(h, L"Há uma operação em andamento. Cancelar e sair?", APP_TITLE, MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) return 0;
            g_cancel = 1;
            if (g_thread) WaitForSingleObject(g_thread, 15000);
        }
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

int WINAPI WinMain(HINSTANCE hi, HINSTANCE hp, LPSTR cmd, int show) {
    (void)hp;
    g_inst = hi;
    if (cmd) {
        const char *dm = strstr(cmd, "--demo=");
        if (dm) { strncpy(g_demo, dm + 7, sizeof g_demo - 1); for (char *c = g_demo; *c; c++) if (*c == ' ') *c = 0; }
    }
    InitializeCriticalSection(&g_cs);
    INITCOMMONCONTROLSEX icc = {sizeof icc, ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS | ICC_LISTVIEW_CLASSES};
    InitCommonControlsEx(&icc);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    LoadPalette();
    GpInit();
    HDC dc = GetDC(NULL);
    g_dpi = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(NULL, dc);
    if (g_dpi < 96) g_dpi = 96;
    /* em telas pequenas com zoom alto, reduz a escala para a janela caber inteira */
    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    while (g_dpi > 96 && (MulDiv(WIN_H + 40, g_dpi, 96) > wa.bottom - wa.top || MulDiv(WIN_W + 16, g_dpi, 96) > wa.right - wa.left)) g_dpi -= 4;
    if (g_dpi < 96) g_dpi = 96;
    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(CL_BG);
    wc.lpszClassName = L"SDCloneWnd";
    wc.hIcon = LoadIconW(hi, MAKEINTRESOURCEW(1));
    wc.hIconSm = wc.hIcon;
    RegisterClassExW(&wc);
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    RECT rc = {0, 0, S(WIN_W), S(WIN_H)};
    AdjustWindowRect(&rc, style, FALSE);
    int ww = rc.right - rc.left, wh = rc.bottom - rc.top;
    int sx = wa.left + (wa.right - wa.left - ww) / 2, sy = wa.top + (wa.bottom - wa.top - wh) / 2;
    if (sy < wa.top) sy = wa.top;
    if (sx < wa.left) sx = wa.left;
    HWND hw = CreateWindowExW(0, L"SDCloneWnd", L"SD Clone Tools", style, sx, sy, ww, wh, NULL, NULL, hi, NULL);
    if (!hw) return 1;
    DarkTitlebar(hw);
    ShowWindow(hw, show);
    UpdateWindow(hw);
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(hw, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    GpDone();
    CoUninitialize();
    return (int)msg.wParam;
}
