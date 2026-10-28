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
#include "core.h"
#include "diag.h"
#define HDR 40

#define APP_TITLE L"SDCLONETOOLS"
#define CHUNK (4u * 1024 * 1024)
#define MIN64(a, b) ((a) < (b) ? (a) : (b))

#define WM_APP_LOG (WM_APP + 1)
#define WM_APP_DONE (WM_APP + 2)

enum { OP_C2I = 0, OP_I2C, OP_C2C, OP_VERIFY, OP_RECOVER, OP_AUTH };

enum {
    ID_OP0 = 100, /* 100..105 */
    ID_DESC = 120, ID_LBL1, ID_COMBO1, ID_REFRESH, ID_SHOWALL, ID_LBL2, ID_COMBO2, ID_COMBOB, ID_COMBOC,
    ID_LBLFILE, ID_EDITFILE, ID_BROWSE, ID_CHK1, ID_CHK2, ID_RADA, ID_RADB,
    ID_START, ID_CANCELBTN, ID_PROGRESS, ID_STATUS, ID_LOG, ID_HEADER,
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
    wchar_t file[MAX_PATH];
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

/* ============================ TEMA DARK / NEON ============================ */
#define CL_BG     RGB(8, 12, 24)
#define CL_PANEL  RGB(14, 22, 42)
#define CL_PANEL2 RGB(16, 44, 82)
#define CL_EDGE   RGB(34, 54, 96)
#define CL_NEON   RGB(0, 212, 255)
#define CL_NEON2  RGB(30, 144, 255)
#define CL_TXT    RGB(214, 230, 255)
#define CL_DIM    RGB(96, 116, 150)
#define CL_DANGER RGB(255, 84, 112)
#define CL_INPUT  RGB(10, 17, 34)
#define CL_LOG    RGB(6, 10, 20)
#define CL_LOGTXT RGB(120, 220, 255)

static HBRUSH g_brBg, g_brInput, g_brLog;
static HFONT g_fontB, g_fontMono, g_fontTitle, g_fontSmall;
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
    BOOL on = TRUE;
    if (FAILED(f(h, 20, &on, sizeof on))) f(h, 19, &on, sizeof on);
    COLORREF c = CL_BG;
    f(h, 35, &c, sizeof c);
    c = CL_NEON2;
    f(h, 34, &c, sizeof c);
    c = CL_TXT;
    f(h, 36, &c, sizeof c);
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
    Fill(dc, r, CL_BG);
    if (id >= ID_OP0 && id <= ID_OP0 + 5) {
        BOOL on = (id - ID_OP0 == g_op);
        Fill(dc, r, on ? CL_PANEL2 : (hot ? RGB(20, 34, 64) : CL_PANEL));
        Frame(dc, r, on ? (dis ? CL_NEON2 : CL_NEON) : (hot ? CL_NEON2 : CL_EDGE));
        if (on) {
            RECT b = {0, h - S(3), w, h};
            Fill(dc, b, dis ? CL_NEON2 : CL_NEON);
        }
        RECT tr = {S(4), 0, w - S(4), h - S(3)}, cr = tr;
        SelectObject(dc, g_fontB);
        DrawTextW(dc, t, -1, &cr, DT_CALCRECT | DT_WORDBREAK | DT_CENTER);
        int th = cr.bottom - cr.top;
        tr.top = (tr.bottom - th) / 2;
        tr.bottom = tr.top + th;
        Txt(dc, t, &tr, DT_WORDBREAK | DT_CENTER, on ? RGB(235, 248, 255) : (dis ? CL_DIM : CL_TXT), g_fontB);
    } else if (id == ID_START || id == ID_CANCELBTN || id == ID_REFRESH || id == ID_BROWSE || (id >= ID_BR_UP && id <= ID_BR_CLOSE)) {
        BOOL isStart = (id == ID_START), isCancel = (id == ID_CANCELBTN || id == ID_BR_CANCEL);
        COLORREF fill, border, tc;
        if (isStart) {
            fill = dis ? CL_EDGE : (sel ? RGB(0, 140, 190) : (hot ? RGB(80, 230, 255) : CL_NEON));
            border = fill;
            tc = dis ? CL_DIM : CL_BG;
        } else {
            fill = sel ? CL_PANEL2 : (hot ? RGB(18, 32, 60) : CL_PANEL);
            border = dis ? CL_EDGE : (isCancel ? CL_DANGER : (hot ? CL_NEON : CL_NEON2));
            tc = dis ? CL_DIM : (isCancel ? CL_DANGER : CL_TXT);
        }
        Fill(dc, r, fill);
        Frame(dc, r, border);
        if (hot && !isStart) {
            RECT in = r;
            InflateRect(&in, -1, -1);
            Frame(dc, in, CL_EDGE);
        }
        Txt(dc, t, &r, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX, tc, (isStart || isCancel) ? g_fontB : g_font);
    } else {
        BOOL radio = (id == ID_RADA || id == ID_RADB), on = Checked(id);
        int bs = S(16), y0 = (h - bs) / 2;
        RECT bx = {S(2), y0, S(2) + bs, y0 + bs};
        COLORREF bc = dis ? CL_EDGE : ((hot || foc) ? CL_NEON : CL_NEON2);
        if (radio) {
            HBRUSH nb = CreateSolidBrush(CL_INPUT);
            HPEN pn = CreatePen(PS_SOLID, 1, bc);
            HGDIOBJ ob1 = SelectObject(dc, nb), ob2 = SelectObject(dc, pn);
            Ellipse(dc, bx.left, bx.top, bx.right, bx.bottom);
            SelectObject(dc, ob1);
            SelectObject(dc, ob2);
            DeleteObject(nb);
            DeleteObject(pn);
            if (on) {
                HBRUSH ib = CreateSolidBrush(dis ? CL_DIM : CL_NEON);
                HPEN ip = CreatePen(PS_SOLID, 1, dis ? CL_DIM : CL_NEON);
                ob1 = SelectObject(dc, ib);
                ob2 = SelectObject(dc, ip);
                Ellipse(dc, bx.left + S(4), bx.top + S(4), bx.right - S(4), bx.bottom - S(4));
                SelectObject(dc, ob1);
                SelectObject(dc, ob2);
                DeleteObject(ib);
                DeleteObject(ip);
            }
        } else {
            Fill(dc, bx, CL_INPUT);
            Frame(dc, bx, bc);
            if (on) {
                RECT in = bx;
                InflateRect(&in, -S(3), -S(3));
                Fill(dc, in, dis ? CL_DIM : CL_NEON);
            }
        }
        RECT tr = {bx.right + S(8), 0, w, h};
        Txt(dc, t, &tr, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS | DT_NOPREFIX, dis ? CL_DIM : CL_TXT, g_font);
    }
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
    Fill(d->hDC, rc, hi ? RGB(16, 52, 96) : CL_INPUT);
    if ((int)d->itemID < 0) return;
    wchar_t t[400];
    t[0] = 0;
    if (SendMessageW(d->hwndItem, CB_GETLBTEXT, d->itemID, (LPARAM)t) == CB_ERR) return;
    if (hi) {
        RECT b = {rc.left, rc.top, rc.left + S(3), rc.bottom};
        Fill(d->hDC, b, CL_NEON);
    }
    RECT tr = rc;
    tr.left += S(8);
    tr.right -= S(4);
    Txt(d->hDC, t, &tr, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX, dis ? CL_DIM : (hi ? RGB(235, 248, 255) : CL_TXT), g_font);
}

static void DrawHeader(DRAWITEMSTRUCT *d) {
    int w = d->rcItem.right - d->rcItem.left, h = d->rcItem.bottom - d->rcItem.top;
    HDC dc = CreateCompatibleDC(d->hDC);
    HBITMAP bm = CreateCompatibleBitmap(d->hDC, w, h);
    HGDIOBJ ob = SelectObject(dc, bm);
    for (int x = 0; x < w; x += 4) {
        int t = x * 255 / (w > 0 ? w : 1);
        RECT s = {x, 0, x + 4, h};
        Fill(dc, s, RGB(8 + t * 4 / 255, 12 + t * 34 / 255, 24 + t * 66 / 255));
    }
    RECT l2 = {0, h - 4, w, h - 2}, l1 = {0, h - 2, w, h};
    Fill(dc, l2, RGB(0, 70, 110));
    Fill(dc, l1, CL_NEON);
    SelectObject(dc, g_fontTitle);
    SetBkMode(dc, TRANSPARENT);
    const wchar_t *title = L"SDCLONETOOLS";
    SIZE sz;
    GetTextExtentPoint32W(dc, L"M", 1, &sz);
    int ty = (h - 4 - sz.cy) / 2, x = S(14), sp = S(4);
    for (int pass = 0; pass < 2; pass++) {
        int cx = x;
        for (const wchar_t *c = title; *c; c++) {
            GetTextExtentPoint32W(dc, c, 1, &sz);
            if (pass == 0) {
                SetTextColor(dc, RGB(0, 80, 125));
                TextOutW(dc, cx - 1, ty, c, 1);
                TextOutW(dc, cx + 1, ty, c, 1);
                TextOutW(dc, cx, ty - 1, c, 1);
                TextOutW(dc, cx, ty + 1, c, 1);
            } else {
                SetTextColor(dc, CL_NEON);
                TextOutW(dc, cx, ty, c, 1);
            }
            cx += sz.cx + sp;
        }
    }
    RECT sr = {0, 0, w - S(14), h - 4};
    Txt(dc, L"CARTÕES SD · GAMESTICK & PORTÁTEIS", &sr, DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_NOPREFIX, RGB(80, 170, 210), g_fontSmall);
    BitBlt(d->hDC, d->rcItem.left, d->rcItem.top, w, h, dc, 0, 0, SRCCOPY);
    SelectObject(dc, ob);
    DeleteObject(bm);
    DeleteDC(dc);
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
    for (DWORD o = 0; o < len; o += sec) {
        BOOL ok = FALSE;
        for (int t = 0; t < 2 && !ok; t++) ok = ReadAtRaw(h, off + o, buf + o, sec);
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
    BOOL ok = FALSE;
    HANDLE hd = OpenDisk(j->src, FALSE);
    RecCtx r;
    Dev d;
    memset(&r, 0, sizeof r);
    if (hd == INVALID_HANDLE_VALUE) { LogErr(L"abrir o cartão"); goto done; }
    if (!DiskLenMatches(hd, j->srcSize)) { Log(L"ERRO: o tamanho do disco mudou. Atualize a lista."); goto done; }
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
    Log(L"Procurando JPG, PNG, PDF e vídeos MP4/MOV no Disco %d. Os arquivos vão para: %ls", j->src, r.folder);
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

/* testa se o cartão aceita gravação de verdade (chave de proteção, cartão falso/defeituoso). Restaura o que escreveu. */
static BOOL ProbeWrite(int disk) {
    DiskInfo *di = FindDisk(disk);
    VolSet vs;
    BOOL good = TRUE;
    vs.n = 0;
    if (!di) return TRUE;
    uint32_t sec = di->sec, len = sec > 4096 ? sec : 4096;
    uint64_t sz = di->size;
    Log(L"Testando se o cartão aceita gravação (grava e lê de volta em 3 pontos; o conteúdo original é restaurado)...");
    LockDiskVolumes(disk, &vs);
    HANDLE h = OpenDisk(disk, TRUE);
    if (h == INVALID_HANDLE_VALUE) {
        LogErr(L"abrir o cartão para escrita");
        UnlockVolumes(&vs);
        return FALSE;
    }
    DWORD br;
    if (!DeviceIoControl(h, IOCTL_DISK_IS_WRITABLE, NULL, 0, NULL, 0, &br, NULL) && GetLastError() == ERROR_WRITE_PROTECT) {
        Log(L"RESULTADO: o cartão está PROTEGIDO CONTRA GRAVAÇÃO (chave de trava do cartão SD/adaptador microSD, ou do leitor).");
        SetSummary(MB_ICONERROR, L"O cartão está PROTEGIDO CONTRA GRAVAÇÃO.\n\nCorrija: desloque a chave lateral do cartão SD (ou do adaptador do microSD) para a posição destravada e tente de novo. Se não houver chave, tente outro leitor.");
        CloseHandle(h);
        UnlockVolumes(&vs);
        return FALSE;
    }
    uint8_t *o = (uint8_t *)ALLOC_ALIGNED(len), *w = (uint8_t *)ALLOC_ALIGNED(len), *r = (uint8_t *)ALLOC_ALIGNED(len);
    if (o && w && r) {
        uint64_t offs[3] = {1024 * 1024, (sz / 2 / sec) * sec, ((sz - 1024 * 1024) / sec) * sec};
        for (int i = 0; i < 3; i++) {
            if (offs[i] + len > sz) continue;
            FillPattern(w, 0, len, 0x5DC1A55ull + (uint64_t)i);
            if (!ReadAtRaw(h, offs[i], o, len)) { Log(L"  posição %I64u: leitura falhou", (unsigned long long)offs[i]); good = FALSE; continue; }
            BOOL wr = WriteAtRaw(h, offs[i], w, len);
            FlushFileBuffers(h);
            BOOL rd = wr && ReadAtRaw(h, offs[i], r, len);
            BOOL same = rd && memcmp(w, r, len) == 0;
            WriteAtRaw(h, offs[i], o, len);
            Log(L"  posição %I64u: gravação %ls, leitura de volta %ls", (unsigned long long)offs[i], wr ? L"ok" : L"FALHOU", same ? L"idêntica" : L"DIFERENTE/FALHOU");
            if (!same) good = FALSE;
        }
    }
    if (o) FREE_ALIGNED(o);
    if (w) FREE_ALIGNED(w);
    if (r) FREE_ALIGNED(r);
    CloseHandle(h);
    UnlockVolumes(&vs);
    if (!good) {
        Log(L"RESULTADO: o cartão NÃO grava corretamente. Verifique a chave de trava do cartão/adaptador, troque de leitor; se continuar, o cartão está com defeito ou é falso (rode Autenticidade) e deve ser trocado.");
        SetSummary(MB_ICONERROR, L"O cartão não aceitou gravação corretamente, por isso não foi possível formatar.\n\n1) Confira a chave de trava do cartão SD ou do adaptador microSD (posição destravada).\n2) Teste outro leitor/porta USB.\n3) Se continuar, o cartão está com defeito ou é falso: rode 'Autenticidade' e troque o cartão.");
    } else Log(L"O cartão aceitou a gravação nos 3 pontos testados.");
    return good;
}

static BOOL FormatDisk(int disk, const char *fs, const char *label) {
    wchar_t tmpdir[MAX_PATH], tmp[MAX_PATH], cmd[MAX_PATH + 64];
    char script[700];
    HANDLE hf;
    DWORD w, code = 1;
    SetIndet(L"Formatando o cartão (limpar, particionar e formatar)...");
    _snprintf(script, sizeof script - 1,
              "select disk %d\r\nattributes disk clear readonly noerr\r\nonline disk noerr\r\nclean\r\nconvert mbr noerr\r\ncreate partition primary\r\nformat fs=%s quick label=%s\r\nassign\r\nexit\r\n",
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
    if (code != 0) {
        Log(L"ERRO: o diskpart terminou com código %lu. Veja as mensagens acima.", (unsigned long)code);
        ProbeWrite(disk);
    }
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

static DWORD WINAPI Worker(LPVOID p) {
    Job *j = &g_job;
    BOOL ok = FALSE;
    (void)p;
    g_badLogged = 0;
    g_summary[0] = 0;
    switch (j->op) {
    case OP_C2I: ok = DoCardToImage(j); break;
    case OP_I2C: ok = DoImageToCard(j); break;
    case OP_C2C: ok = j->mode ? DoSmartCopy(j) : DoCardToCard(j); break;
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
    L"1. Diagnosticar o cartão: descobrir o que há de errado (não altera nada)",
    L"2. Ver / copiar arquivos, inclusive de partições que o Windows não vê",
    L"3. Reconstruir partições perdidas (cartão sem acesso/RAW; preserva os dados)",
    L"4. Recuperar arquivos apagados (fotos, vídeos MP4/MOV, PDF)",
    L"5. Restaurar o cartão: apaga TUDO e formata de novo (devolve a capacidade total)"};
static const wchar_t *RECOVER_DESC[] = {
    L"Analisa o cartão sem alterar nada: capacidade informada, tabela de partições, sistemas de arquivos e partições perdidas. Explica por que o cartão mostra pouca capacidade ou fica sem acesso, e o que fazer.",
    L"Abre as partições do cartão (FAT, exFAT, ext4 de consoles Linux), inclusive as que o Windows não mostra, e permite copiar os arquivos para o PC. Somente leitura.",
    L"Reescreve a tabela de partições (MBR) usando as partições encontradas no cartão. NÃO apaga dados. Guarda uma cópia do início do cartão em Documentos antes de alterar.",
    L"Procura no cartão inteiro fotos (JPG/PNG), vídeos (MP4/MOV) e PDFs apagados e os salva numa pasta do PC. Só lê o cartão.",
    L"Apaga tudo, cria uma partição do tamanho total e formata (FAT32 até 32 GB, exFAT acima). Use depois de salvar os dados; não conserta cartão que informa capacidade errada."};

static void RecoverUi(void) {
    int m = RecoverMode();
    BOOL f = (m == RM_CARVE);
    ShowCtl(ID_LBLFILE, f);
    ShowCtl(ID_EDITFILE, f);
    ShowCtl(ID_BROWSE, f);
    if (m >= 0 && m < RM_COUNT) SetDlgItemTextW(g_hwnd, ID_DESC, RECOVER_DESC[m]);
    UpdateEnable();
}

static void FillModeCombo(int op) {
    HWND c = Ctl(ID_COMBOB);
    SendMessageW(c, CB_RESETCONTENT, 0, 0);
    if (op == OP_RECOVER) {
        for (int i = 0; i < RM_COUNT; i++) SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)RECOVER_MODES[i]);
        MoveWindow(c, S(10), S(168 + HDR), S(620), S(240), TRUE);
    } else {
        for (int i = 0; i < NBRANDS; i++) SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)BRANDS[i].name);
        MoveWindow(c, S(10), S(168 + HDR), S(300), S(240), TRUE);
    }
    SendMessageW(c, CB_SETCURSEL, 0, 0);
}

static void UpdateEnable(void) {
    BOOL fileOn = TRUE;
    if (g_op == OP_VERIFY) {
        if (Checked(ID_CHK2)) SetChk(ID_CHK1, FALSE);
        EnableCtl(ID_CHK1, !Checked(ID_CHK2));
        fileOn = Checked(ID_CHK1) && !Checked(ID_CHK2);
    } else if (g_op == OP_RECOVER) fileOn = (RecoverMode() == RM_CARVE);
    g_fileOn = fileOn;
    InvalidateRect(Ctl(ID_LBLFILE), NULL, TRUE);
    EnableCtl(ID_EDITFILE, fileOn);
    EnableCtl(ID_BROWSE, fileOn);
}

static void ApplyOp(int op) {
    static const wchar_t *desc[] = {
        L"Cria um arquivo de imagem (.img) com uma cópia exata, setor por setor, de todo o cartão. Serve para backup ou para clonar depois.",
        L"Grava um arquivo de imagem (.img, .iso, .bin) no cartão. TODO o conteúdo atual do cartão será apagado.",
        L"Copia diretamente um cartão para outro (o destino deve ser igual ou maior). TODO o conteúdo do destino será apagado.",
        L"Lê o cartão inteiro procurando setores ilegíveis. Opcionalmente compara com uma imagem. Para saber se a capacidade é verdadeira, use Autenticidade.",
        L"Repara cartão sem acesso ou com capacidade errada: diagnostica e explica o problema, abre partições que o Windows não vê, reconstrói partições perdidas, recupera arquivos apagados ou restaura o cartão.",
        L"Descobre se o cartão é verdadeiro ou falso (SanDisk, Samsung, Kingston, Lexar...): fabricante gravado no cartão, capacidade real e velocidade."};
    g_op = op;
    for (int i = 0; i < 6; i++) InvalidateRect(Ctl(ID_OP0 + i), NULL, FALSE);
    SetDlgItemTextW(g_hwnd, ID_DESC, desc[op]);
    ShowCtl(ID_LBL2, op == OP_C2C || op == OP_AUTH || op == OP_RECOVER);
    ShowCtl(ID_COMBO2, op == OP_C2C);
    ShowCtl(ID_COMBOB, op == OP_AUTH || op == OP_RECOVER);
    if (op == OP_AUTH || op == OP_RECOVER) FillModeCombo(op);
    ShowCtl(ID_COMBOC, op == OP_AUTH);
    ShowCtl(ID_LBLFILE, op != OP_C2C && op != OP_AUTH);
    ShowCtl(ID_EDITFILE, op != OP_C2C && op != OP_AUTH);
    ShowCtl(ID_BROWSE, op != OP_C2C && op != OP_AUTH);
    ShowCtl(ID_CHK1, op <= OP_VERIFY || op == OP_AUTH);
    ShowCtl(ID_CHK2, op == OP_VERIFY || op == OP_C2C);
    ShowCtl(ID_RADA, FALSE);
    ShowCtl(ID_RADB, FALSE);
    EnableCtl(ID_CHK1, TRUE);
    SetChk(ID_CHK1, FALSE);
    SetChk(ID_CHK2, FALSE);
    switch (op) {
    case OP_C2I:
        SetDlgItemTextW(g_hwnd, ID_LBL1, L"Cartão de origem:");
        SetDlgItemTextW(g_hwnd, ID_LBLFILE, L"Salvar a imagem em (arquivo .img):");
        SetDlgItemTextW(g_hwnd, ID_CHK1, L"Verificar a imagem depois de criar (compara com o cartão; demora mais)");
        break;
    case OP_I2C:
        SetDlgItemTextW(g_hwnd, ID_LBL1, L"Cartão de destino (SERÁ APAGADO):");
        SetDlgItemTextW(g_hwnd, ID_LBLFILE, L"Arquivo de imagem (.img, .iso, .bin):");
        SetDlgItemTextW(g_hwnd, ID_CHK1, L"Verificar depois de gravar (recomendado)");
        SetChk(ID_CHK1, TRUE);
        break;
    case OP_C2C:
        SetDlgItemTextW(g_hwnd, ID_LBL1, L"Cartão de origem:");
        SetDlgItemTextW(g_hwnd, ID_LBL2, L"Cartão de destino (SERÁ APAGADO):");
        SetDlgItemTextW(g_hwnd, ID_CHK1, L"Verificar depois de copiar (recomendado)");
        SetDlgItemTextW(g_hwnd, ID_CHK2, L"Copiar só os dados (o destino pode ser MENOR que a origem)");
        SetChk(ID_CHK1, TRUE);
        break;
    case OP_VERIFY:
        SetDlgItemTextW(g_hwnd, ID_LBL1, L"Cartão a verificar:");
        SetDlgItemTextW(g_hwnd, ID_LBLFILE, L"Arquivo de imagem para comparar:");
        SetDlgItemTextW(g_hwnd, ID_CHK1, L"Comparar também com um arquivo de imagem");
        SetDlgItemTextW(g_hwnd, ID_CHK2, L"Teste de capacidade real, estilo H2testw (APAGA todo o cartão; muito demorado)");
        break;
    case OP_RECOVER:
        SetDlgItemTextW(g_hwnd, ID_LBL1, L"Cartão:");
        SetDlgItemTextW(g_hwnd, ID_LBLFILE, L"Pasta onde salvar os arquivos recuperados (NÃO use o próprio cartão):");
        SetDlgItemTextW(g_hwnd, ID_LBL2, L"O que fazer com o cartão:");
        break;
    case OP_AUTH:
        SetDlgItemTextW(g_hwnd, ID_LBL1, L"Cartão a analisar:");
        SetDlgItemTextW(g_hwnd, ID_LBL2, L"Marca escrita na etiqueta  /  classe de velocidade anunciada:");
        SetDlgItemTextW(g_hwnd, ID_CHK1, L"Teste completo de capacidade e velocidade (APAGA todo o cartão; é o único que prova a capacidade)");
        break;
    }
    SetDlgItemTextW(g_hwnd, ID_EDITFILE, L"");
    if (op == OP_RECOVER) RecoverUi();
    else UpdateEnable();
    InvalidateRect(g_hwnd, NULL, TRUE);
}

static void AppendLog(const wchar_t *s) {
    HWND e = Ctl(ID_LOG);
    int n = GetWindowTextLengthW(e);
    SendMessageW(e, EM_SETSEL, n, n);
    SendMessageW(e, EM_REPLACESEL, FALSE, (LPARAM)s);
}

static void SetBusy(BOOL b) {
    g_busy = b;
    for (int i = 0; i < 6; i++) EnableCtl(ID_OP0 + i, !b);
    EnableCtl(ID_COMBO1, !b); EnableCtl(ID_COMBO2, !b); EnableCtl(ID_COMBOB, !b); EnableCtl(ID_COMBOC, !b);
    EnableCtl(ID_REFRESH, !b); EnableCtl(ID_SHOWALL, !b);
    EnableCtl(ID_CHK1, !b); EnableCtl(ID_CHK2, !b); EnableCtl(ID_RADA, !b); EnableCtl(ID_RADB, !b);
    EnableCtl(ID_START, !b);
    EnableCtl(ID_CANCELBTN, b);
    if (b) { EnableCtl(ID_EDITFILE, FALSE); EnableCtl(ID_BROWSE, FALSE); g_fileOn = FALSE; InvalidateRect(Ctl(ID_LBLFILE), NULL, TRUE); }
    else UpdateEnable();
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
    g_cancel = 0; g_done = 0; g_total = 0; g_indet = 0;
    g_summary[0] = 0;
    SetBusy(TRUE);
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
    if (!s) { Warn(L"Selecione um cartão. Se ele não aparece, insira-o e clique em Atualizar."); return; }
    if (s->index == g_sysDisk) { Warn(L"Esse é o disco do sistema. Operação recusada."); return; }
    j->src = s->index; j->srcSize = s->size; j->srcSec = s->sec; j->di = *s;
    wcsncpy(j->file, path, MAX_PATH - 1);
    j->verify = c1;
    switch (g_op) {
    case OP_C2I:
        if (!path[0]) { Warn(L"Escolha onde salvar o arquivo de imagem."); return; }
        if (PathOnDisk(path, s->index)) { Warn(L"A imagem não pode ser salva dentro do próprio cartão."); return; }
        break;
    case OP_I2C: {
        if (!path[0]) { Warn(L"Escolha o arquivo de imagem."); return; }
        WIN32_FILE_ATTRIBUTE_DATA fa;
        if (!GetFileAttributesExW(path, GetFileExInfoStandard, &fa) || (fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) { Warn(L"O arquivo de imagem não foi encontrado."); return; }
        uint64_t fs64 = ((uint64_t)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
        if (fs64 == 0) { Warn(L"O arquivo de imagem está vazio."); return; }
        if (fs64 > s->size) { Warn(L"A imagem é maior que o cartão de destino."); return; }
        if (PathOnDisk(path, s->index)) { Warn(L"A imagem está dentro do cartão de destino."); return; }
        wchar_t a[32], b[32];
        FmtSize(fs64, a, 32); FmtSize(s->size, b, 32);
        _snwprintf(msg, 1799, L"Gravar a imagem:\n%ls (%ls)\n\nno cartão:\n%ls\n\nTODO O CONTEÚDO DO CARTÃO SERÁ APAGADO. Continuar?", path, a, s->label);
        if (!Confirm(msg)) return;
        j->dst = s->index; j->dstSize = s->size; j->dstSec = s->sec;
        break;
    }
    case OP_C2C:
        if (!t) { Warn(L"Selecione o cartão de destino."); return; }
        if (t->index == g_sysDisk) { Warn(L"O destino é o disco do sistema. Operação recusada."); return; }
        if (t->index == s->index) { Warn(L"Origem e destino são o mesmo cartão."); return; }
        j->mode = c2 ? 1 : 0;
        if (t->size < s->size && !j->mode) {
            Warn(L"O cartão de destino é MENOR que o de origem.\n\nSe os DADOS cabem no destino, marque a opção \"Copiar só os dados (o destino pode ser MENOR que a origem)\" e tente de novo.");
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
        } else if (j->mode == RM_RESTORE) {
            _snwprintf(msg, 1799, L"Restaurar o cartão:\n%ls\n\nTODAS AS PARTIÇÕES E DADOS SERÃO APAGADOS e o cartão será formatado.\n\nCartões de consoles (R36S e similares) perdem o sistema e precisarão de uma nova gravação da imagem (função Imagem → Cartão).\n\nObs.: se o diagnóstico mostrou que o PRÓPRIO CARTÃO informa capacidade pequena, esta função não resolve. Continuar?", s->label);
            if (!Confirm(msg)) return;
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
    ofn.lpstrFilter = L"Imagens de disco (*.img;*.iso;*.bin;*.raw;*.dd)\0*.img;*.iso;*.bin;*.raw;*.dd\0Todos os arquivos (*.*)\0*.*\0";
    if (g_op == OP_C2I) {
        if (!path[0]) wcscpy(path, L"cartao.img");
        ofn.lpstrDefExt = L"img";
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
    FlashWindow(g_hwnd, TRUE);
    if (g_summary[0]) MessageBoxW(g_hwnd, g_summary, APP_TITLE, MB_OK | g_summaryIcon);
    if (g_openBrowse) {
        g_openBrowse = 0;
        if (ok) ShowBrowser();
    }
}

static HWND mk(const wchar_t *cls, const wchar_t *text, DWORD style, int x, int y, int w, int h, int id, DWORD ex) {
    return CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y + HDR), S(w), S(h), g_hwnd, (HMENU)(INT_PTR)id, g_inst, NULL);
}

static BOOL CALLBACK SetFontCb(HWND h, LPARAM f) { SendMessageW(h, WM_SETFONT, (WPARAM)f, TRUE); return TRUE; }

static void CreateUi(void) {
    static const wchar_t *ops[] = {L"Cartão → Imagem", L"Imagem → Cartão", L"Cartão → Cartão", L"Verificar cartão", L"Recuperar cartão", L"Autenticidade"};
    NONCLIENTMETRICSW ncm;
    memset(&ncm, 0, sizeof ncm);
    ncm.cbSize = sizeof ncm;
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0);
    g_font = CreateFontIndirectW(&ncm.lfMessageFont);
    LOGFONTW lf = ncm.lfMessageFont;
    lf.lfWeight = FW_SEMIBOLD;
    g_fontB = CreateFontIndirectW(&lf);
    lf = ncm.lfMessageFont;
    wcscpy(lf.lfFaceName, L"Consolas");
    g_fontMono = CreateFontIndirectW(&lf);
    g_fontTitle = CreateFontW(-S(24), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_fontSmall = CreateFontW(-S(12), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_brBg = CreateSolidBrush(CL_BG);
    g_brInput = CreateSolidBrush(CL_INPUT);
    g_brLog = CreateSolidBrush(CL_LOG);

    CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_OWNERDRAW, 0, 0, S(640), S(HDR), g_hwnd, (HMENU)(INT_PTR)ID_HEADER, g_inst, NULL);
    for (int i = 0; i < 6; i++)
        mk(L"BUTTON", ops[i], BS_OWNERDRAW | WS_TABSTOP, 10 + i * 103, 10, 101, 40, ID_OP0 + i, 0);
    mk(L"STATIC", L"", SS_LEFT, 10, 56, 620, 34, ID_DESC, 0);
    mk(L"STATIC", L"Cartão:", SS_LEFT, 10, 94, 360, 18, ID_LBL1, 0);
    mk(L"BUTTON", L"Mostrar todos os discos (cuidado)", BS_OWNERDRAW | WS_TABSTOP, 380, 92, 250, 20, ID_SHOWALL, 0);
    DWORD cb = CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL | WS_TABSTOP;
    mk(L"COMBOBOX", L"", cb, 10, 114, 520, 240, ID_COMBO1, 0);
    mk(L"BUTTON", L"Atualizar", BS_OWNERDRAW | WS_TABSTOP, 540, 113, 90, 26, ID_REFRESH, 0);
    mk(L"STATIC", L"", SS_LEFT, 10, 148, 620, 18, ID_LBL2, 0);
    mk(L"COMBOBOX", L"", cb, 10, 168, 520, 240, ID_COMBO2, 0);
    mk(L"COMBOBOX", L"", cb, 10, 168, 300, 240, ID_COMBOB, 0);
    mk(L"COMBOBOX", L"", cb, 320, 168, 210, 240, ID_COMBOC, 0);
    mk(L"STATIC", L"", SS_LEFT, 10, 202, 620, 18, ID_LBLFILE, 0);
    HWND ed = mk(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, 10, 222, 520, 24, ID_EDITFILE, 0);
    SendMessageW(ed, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(S(6), S(6)));
    mk(L"BUTTON", L"Procurar...", BS_OWNERDRAW | WS_TABSTOP, 540, 221, 90, 26, ID_BROWSE, 0);
    mk(L"BUTTON", L"", BS_OWNERDRAW | WS_TABSTOP, 10, 256, 620, 20, ID_CHK1, 0);
    mk(L"BUTTON", L"", BS_OWNERDRAW | WS_TABSTOP, 10, 280, 620, 20, ID_CHK2, 0);
    mk(L"BUTTON", L"", BS_OWNERDRAW | WS_TABSTOP, 10, 256, 620, 20, ID_RADA, 0);
    mk(L"BUTTON", L"", BS_OWNERDRAW | WS_TABSTOP, 10, 280, 620, 20, ID_RADB, 0);
    mk(L"BUTTON", L"Iniciar", BS_OWNERDRAW | WS_TABSTOP, 10, 314, 200, 38, ID_START, 0);
    mk(L"BUTTON", L"Cancelar", BS_OWNERDRAW | WS_TABSTOP | WS_DISABLED, 220, 314, 120, 38, ID_CANCELBTN, 0);
    HWND pb = mk(PROGRESS_CLASSW, L"", 0, 10, 364, 620, 22, ID_PROGRESS, 0);
    SendMessageW(pb, PBM_SETRANGE32, 0, 1000);
    mk(L"STATIC", L"Pronto. Confira sempre o disco selecionado antes de gravar.", SS_LEFT | SS_ENDELLIPSIS, 10, 390, 620, 20, ID_STATUS, 0);
    HWND lg = mk(L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_TABSTOP, 10, 416, 620, 150, ID_LOG, 0);
    SendMessageW(lg, EM_LIMITTEXT, 0x7FFFFFFE, 0);
    SendMessageW(lg, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(S(6), S(6)));
    for (int i = 0; i < NBRANDS; i++) SendMessageW(Ctl(ID_COMBOB), CB_ADDSTRING, 0, (LPARAM)BRANDS[i].name);
    for (int i = 0; i < NCLASSES; i++) SendMessageW(Ctl(ID_COMBOC), CB_ADDSTRING, 0, (LPARAM)CLASSES[i].name);
    SendMessageW(Ctl(ID_COMBOB), CB_SETCURSEL, 0, 0);
    SendMessageW(Ctl(ID_COMBOC), CB_SETCURSEL, 0, 0);
    EnumChildWindows(g_hwnd, SetFontCb, (LPARAM)g_font);
    SendMessageW(lg, WM_SETFONT, (WPARAM)g_fontMono, TRUE);
    /* tema escuro nos controles que continuam nativos (combos, edit, barra de rolagem, progresso) */
    SetTheme(Ctl(ID_COMBO1), L"DarkMode_CFD", NULL);
    SetTheme(Ctl(ID_COMBO2), L"DarkMode_CFD", NULL);
    SetTheme(Ctl(ID_COMBOB), L"DarkMode_CFD", NULL);
    SetTheme(Ctl(ID_COMBOC), L"DarkMode_CFD", NULL);
    SetTheme(ed, L"DarkMode_CFD", NULL);
    SetTheme(lg, L"DarkMode_Explorer", NULL);
    SetTheme(pb, L"", L"");
    SendMessageW(pb, 0x2001 /* PBM_SETBKCOLOR */, 0, (LPARAM)CL_INPUT);
    SendMessageW(pb, 0x0409 /* PBM_SETBARCOLOR */, 0, (LPARAM)CL_NEON);
    static const int bids[] = {ID_OP0, ID_OP0 + 1, ID_OP0 + 2, ID_OP0 + 3, ID_OP0 + 4, ID_OP0 + 5, ID_REFRESH, ID_SHOWALL, ID_BROWSE, ID_CHK1, ID_CHK2, ID_RADA, ID_RADB, ID_START, ID_CANCELBTN};
    for (int i = 0; i < (int)(sizeof bids / sizeof bids[0]); i++) SetWindowSubclass(Ctl(bids[i]), BtnSub, bids[i], 0);
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE:
        g_hwnd = h;
        CreateUi();
        RefreshDisks();
        ApplyOp(0);
        Log(L"Pronto. Insira o cartão e escolha a função acima. Execute sempre como administrador.");
        return 0;
    case WM_COMMAND: {
        int id = LOWORD(w), code = HIWORD(w);
        if (id >= ID_OP0 && id <= ID_OP0 + 5 && code == BN_CLICKED) { if (!g_busy) ApplyOp(id - ID_OP0); }
        else if (id == ID_REFRESH && code == BN_CLICKED) { if (!g_busy) RefreshDisks(); }
        else if (id == ID_SHOWALL && code == BN_CLICKED) { if (!g_busy) { SetChk(ID_SHOWALL, !Checked(ID_SHOWALL)); RefreshDisks(); } }
        else if (id == ID_BROWSE && code == BN_CLICKED) OnBrowse();
        else if (id == ID_COMBOB && code == CBN_SELCHANGE) { if (g_op == OP_RECOVER) RecoverUi(); }
        else if ((id == ID_CHK1 || id == ID_CHK2 || id == ID_RADA || id == ID_RADB) && code == BN_CLICKED) {
            if (id == ID_RADA || id == ID_RADB) { SetChk(ID_RADA, id == ID_RADA); SetChk(ID_RADB, id == ID_RADB); }
            else SetChk(id, !Checked(id));
            UpdateEnable();
        }
        else if (id == ID_START && code == BN_CLICKED) OnStart();
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
        static const int fr[] = {ID_COMBO1, ID_COMBO2, ID_COMBOB, ID_COMBOC, ID_EDITFILE, ID_LOG};
        for (int i = 0; i < 6; i++) {
            HWND c = Ctl(fr[i]);
            if (!c || !(GetWindowLongW(c, GWL_STYLE) & WS_VISIBLE)) continue;
            RECT r;
            GetWindowRect(c, &r);
            MapWindowPoints(NULL, h, (POINT *)&r, 2);
            InflateRect(&r, 1, 1);
            Frame(dc, r, fr[i] == ID_LOG ? CL_NEON2 : CL_EDGE);
        }
        return 1;
    }
    case WM_DRAWITEM: {
        DRAWITEMSTRUCT *d = (DRAWITEMSTRUCT *)l;
        if (d->CtlType == ODT_BUTTON) DrawButton(d);
        else if (d->CtlType == ODT_COMBOBOX) DrawCombo(d);
        else if (d->CtlType == ODT_STATIC) DrawHeader(d);
        return TRUE;
    }
    case WM_MEASUREITEM: {
        MEASUREITEMSTRUCT *mi = (MEASUREITEMSTRUCT *)l;
        if (mi->CtlType == ODT_COMBOBOX) { mi->itemHeight = S(20); return TRUE; }
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
        COLORREF tc = CL_TXT;
        if (id == ID_STATUS) tc = RGB(110, 225, 255);
        else if (id == ID_DESC) tc = RGB(150, 170, 205);
        else if (id == ID_LBLFILE && !g_fileOn) tc = CL_DIM;
        SetTextColor(dc, tc);
        SetBkColor(dc, CL_BG);
        return (LRESULT)g_brBg;
    }
    case WM_TIMER:
        if (w == 1) UpdateProgress();
        else if (w == 2) { KillTimer(h, 2); if (!g_busy) RefreshDisks(); }
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
    (void)hp; (void)cmd;
    g_inst = hi;
    InitializeCriticalSection(&g_cs);
    INITCOMMONCONTROLSEX icc = {sizeof icc, ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS | ICC_LISTVIEW_CLASSES};
    InitCommonControlsEx(&icc);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    HDC dc = GetDC(NULL);
    g_dpi = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(NULL, dc);
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
    RECT rc = {0, 0, S(640), S(616)};
    AdjustWindowRect(&rc, style, FALSE);
    int ww = rc.right - rc.left, wh = rc.bottom - rc.top;
    int sx = (GetSystemMetrics(SM_CXSCREEN) - ww) / 2, sy = (GetSystemMetrics(SM_CYSCREEN) - wh) / 2;
    if (sy < 0) sy = 0;
    HWND hw = CreateWindowExW(0, L"SDCloneWnd", L"SDCLONETOOLS — copiar, gravar, verificar, autenticar e reparar cartões SD", style, sx, sy, ww, wh, NULL, NULL, hi, NULL);
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
    CoUninitialize();
    return (int)msg.wParam;
}
