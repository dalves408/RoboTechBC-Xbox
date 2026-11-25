/* Config.exe: Robotech: Battlecry recomp settings, outside the game. Reads and
 * writes the same [video] keys of settings.ini (beside this program) as the
 * in-game F1 menu, with the same look (src/rbx_ui.c) and the same option list
 * (src/rbx_options.h). Mouse: point and click, right click or the wheel steps
 * back. Keyboard: arrows, Tab, Enter; Esc closes. */
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <stdio.h>
#include <string.h>

#include "rbx_options.h"
#include "rbx_ui.h"

#define BASE_W 960
#define BASE_H 640

enum { B_RESET, B_SAVE, B_PLAY, B_CLOSE, B_COUNT };
static const char *const B_TEXT[B_COUNT] = { "DEFAULTS", "SAVE", "SAVE & PLAY", "CLOSE" };
/* Focus order: the options (left column, then right), then the buttons. */
#define ROWS (RBX_OPT_COUNT + B_COUNT)

static unsigned s_value[RBX_OPT_COUNT], s_saved[RBX_OPT_COUNT];
static WCHAR s_dir[MAX_PATH], s_ini[MAX_PATH];
static int s_sel, s_hover = -1, s_have_game;
static char s_status[160];
static RECT s_row[RBX_OPT_COUNT], s_prev[RBX_OPT_COUNT], s_next[RBX_OPT_COUNT], s_btn[B_COUNT];

static void locate(void)
{
    WCHAR path[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, s_dir, MAX_PATH);
    WCHAR *slash;
    if (!n || n >= MAX_PATH || !(slash = wcsrchr(s_dir, L'\\'))) {
        GetCurrentDirectoryW(MAX_PATH, s_dir);
    } else
        *slash = 0;
    swprintf(s_ini, MAX_PATH, L"%ls\\settings.ini", s_dir);
    swprintf(path, MAX_PATH, L"%ls\\game_files\\default.xbe", s_dir);
    s_have_game = GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

static void load(void)
{
    int i;
    for (i = 0; i < RBX_OPT_COUNT; i++) {
        WCHAR key[32];
        unsigned v;
        MultiByteToWideChar(CP_UTF8, 0, rbx_options[i].key, -1, key, 32);
        v = GetPrivateProfileIntW(L"video", key, (INT)rbx_options[i].initial, s_ini);
        s_value[i] = s_saved[i] = v < rbx_options[i].count ? v : rbx_options[i].initial;
    }
}

static int dirty(void) { return memcmp(s_value, s_saved, sizeof s_value) != 0; }

static int save(void)
{
    int i, ok = 1;
    for (i = 0; i < RBX_OPT_COUNT; i++) {
        WCHAR key[32], value[12];
        MultiByteToWideChar(CP_UTF8, 0, rbx_options[i].key, -1, key, 32);
        swprintf(value, 12, L"%u", s_value[i]);
        ok &= WritePrivateProfileStringW(L"video", key, value, s_ini) != 0;
    }
    WritePrivateProfileStringW(NULL, NULL, NULL, s_ini);    /* flush */
    if (ok) {
        memcpy(s_saved, s_value, sizeof s_saved);
        snprintf(s_status, sizeof s_status, "Saved. The game reads these the next time it starts.");
    } else
        snprintf(s_status, sizeof s_status, "Could not write settings.ini. Is the folder read-only?");
    return ok;
}

static void play(HWND h)
{
    WCHAR path[MAX_PATH];
    HINSTANCE r;
    /* Play.bat (a packaged build) sets up logs and screenshots; else the game itself. */
    swprintf(path, MAX_PATH, L"%ls\\Play.bat", s_dir);
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES)
        swprintf(path, MAX_PATH, L"%ls\\robotech_bc_recomp.exe", s_dir);
    r = ShellExecuteW(h, L"open", path, NULL, s_dir, SW_SHOWNORMAL);
    if ((INT_PTR)r > 32)
        DestroyWindow(h);
    else
        snprintf(s_status, sizeof s_status, "Could not start the game. Put Config.exe in the game's folder.");
}

static void step(int row, int dir)
{
    int n;
    if (row < 0 || row >= RBX_OPT_COUNT) return;
    n = (int)rbx_options[row].count;
    s_value[row] = (unsigned)(((int)s_value[row] + n + dir) % n);
    s_status[0] = 0;
}

static void press(HWND h, int button)
{
    switch (button) {
    case B_RESET: {
        int i;
        for (i = 0; i < RBX_OPT_COUNT; i++) s_value[i] = rbx_options[i].initial;
        snprintf(s_status, sizeof s_status, "Defaults restored. Select SAVE to keep them.");
        break;
    }
    case B_SAVE: save(); break;
    case B_PLAY: if (save()) play(h); break;
    case B_CLOSE: DestroyWindow(h); break;
    }
}

/* ---- drawing -------------------------------------------------------------- */

static void draw(HDC dc, int w, int h)
{
    RbxFonts f;
    RECT all, p, band, sec, row, note, bar;
    int i, x0, x1, mid, gap, colw, y0, y, focus;
    char line[200];

    SetRect(&all, 0, 0, w, h);
    rbx_backdrop(dc, &all);
    rbx_fonts_make(&f, rbx_fit(w, h, BASE_W, BASE_H));
    f.value_pct = 58;              /* two narrower columns: give the readouts more room */
    {
        int pw = rbx_px(&f, 912), ph = rbx_px(&f, 600);
        SetRect(&p, (w - pw) / 2, (h - ph) / 2, (w - pw) / 2 + pw, (h - ph) / 2 + ph);
    }
    rbx_panel(dc, &f, &p, rbx_px(&f, 28));
    x0 = p.left + rbx_px(&f, 36);
    x1 = p.right - rbx_px(&f, 36);
    SetRect(&band, x0, p.top + rbx_px(&f, 18), x1, p.top + rbx_px(&f, 82));
    rbx_header(dc, &f, &band, "ROBOTECH: BATTLECRY", "RECOMP  //  SETTINGS", "RDF FIELD MANUAL");

    gap = rbx_px(&f, 32);
    colw = (x1 - x0 - gap) / 2;
    mid = x0 + colw + gap;
    y0 = p.top + rbx_px(&f, 100);
    for (i = 0; i < RBX_OPT_COUNT; i++) {
        int left = i < RBX_OPT_DISPLAY, k = left ? i : i - RBX_OPT_DISPLAY;
        int cx = left ? x0 : mid;
        if (k == 0) {
            SetRect(&sec, cx, y0, cx + colw, y0 + rbx_px(&f, 30));
            rbx_section(dc, &f, &sec, left ? 1 : 2, left ? "DISPLAY" : "GRAPHICS");
        }
        y = y0 + rbx_px(&f, 36) + k * rbx_px(&f, 48);
        SetRect(&row, cx, y, cx + colw, y + rbx_px(&f, 40));
        s_row[i] = row;
        rbx_option(dc, &f, &row, rbx_options[i].label, rbx_options[i].values[s_value[i]], s_value[i],
                   rbx_options[i].count, s_sel == i, s_hover == i && s_sel != i, &s_prev[i], &s_next[i]);
    }
    y = y0 + rbx_px(&f, 36) + 5 * rbx_px(&f, 48) + rbx_px(&f, 4);

    /* The selected (or pointed at) setting's note. */
    focus = s_hover >= 0 && s_hover < RBX_OPT_COUNT ? s_hover : s_sel;
    SetRect(&note, x0, y, x1, y + rbx_px(&f, 34));
    if (focus < RBX_OPT_COUNT)
        rbx_note(dc, &f, &note, "NOTE", rbx_options[focus].note);
    else
        rbx_note(dc, &f, &note, "NOTE", focus - RBX_OPT_COUNT == B_RESET ? "Put every setting back to its default."
                                       : focus - RBX_OPT_COUNT == B_SAVE ? "Write settings.ini. F1 in the game edits the same file."
                                       : focus - RBX_OPT_COUNT == B_PLAY ? "Save, then start the game."
                                                                         : "Close without saving.");

    /* Status: unsaved changes, a message, or where the files are. */
    y = note.bottom + rbx_px(&f, 10);
    SetRect(&bar, x0, y, x1, y + rbx_px(&f, 24));
    if (s_status[0])
        snprintf(line, sizeof line, "%s", s_status);
    else if (dirty())
        snprintf(line, sizeof line, "Unsaved changes.");
    else
        snprintf(line, sizeof line, s_have_game ? "Game files found. Settings: settings.ini in this folder."
                                                : "No game_files folder here. Put Config.exe beside robotech_bc_recomp.exe.");
    rbx_text(dc, f.fine, dirty() && !s_status[0] ? RGB(255, 168, 38)
                          : s_have_game ? RGB(116, 138, 168) : RGB(222, 62, 50),
             &bar, line, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    /* Buttons at the bottom right; DEFAULTS alone at the left. */
    {
        int bh = rbx_px(&f, 42), by = p.bottom - rbx_px(&f, 30) - bh, bw = rbx_px(&f, 150), bx = x1;
        for (i = B_COUNT - 1; i >= 1; i--) {
            int wdt = i == B_PLAY ? rbx_px(&f, 190) : bw;
            SetRect(&s_btn[i], bx - wdt, by, bx, by + bh);
            bx -= wdt + rbx_px(&f, 12);
        }
        SetRect(&s_btn[B_RESET], x0, by, x0 + bw, by + bh);
        for (i = 0; i < B_COUNT; i++)
            rbx_button(dc, &f, &s_btn[i], B_TEXT[i], i == B_PLAY ? RBX_BTN_PRIMARY : RBX_BTN_NORMAL,
                       s_sel == RBX_OPT_COUNT + i, s_hover == RBX_OPT_COUNT + i);
    }
    rbx_fonts_free(&f);
}

static void paint(HWND h)
{
    PAINTSTRUCT ps;
    RECT r;
    HDC dc = BeginPaint(h, &ps), mem;
    HBITMAP bm;
    HGDIOBJ old;
    GetClientRect(h, &r);
    mem = CreateCompatibleDC(dc);
    bm = CreateCompatibleBitmap(dc, r.right, r.bottom);
    old = SelectObject(mem, bm);
    draw(mem, r.right, r.bottom);
    BitBlt(dc, 0, 0, r.right, r.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bm);
    DeleteDC(mem);
    EndPaint(h, &ps);
}

static int render_to(const char *path, int w, int h)
{
    BITMAPINFO bi;
    BITMAPFILEHEADER fh;
    void *bits = NULL;
    HDC dc = CreateCompatibleDC(NULL);
    HBITMAP bm;
    FILE *f;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bm = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!bm || !(f = fopen(path, "wb"))) return 1;
    SelectObject(dc, bm);
    draw(dc, w, h);
    GdiFlush();
    memset(&fh, 0, sizeof fh);
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof fh + sizeof bi.bmiHeader;
    fh.bfSize = fh.bfOffBits + (DWORD)w * h * 4;
    fwrite(&fh, sizeof fh, 1, f);
    fwrite(&bi.bmiHeader, sizeof bi.bmiHeader, 1, f);
    fwrite(bits, 4, (size_t)w * h, f);
    fclose(f);
    DeleteObject(bm);
    DeleteDC(dc);
    return 0;
}

/* ---- input ---------------------------------------------------------------- */

static int hit(int x, int y, int *arrow)
{
    POINT pt = {x, y};
    int i;
    *arrow = 0;
    for (i = 0; i < RBX_OPT_COUNT; i++)
        if (PtInRect(&s_row[i], pt)) {
            if (PtInRect(&s_prev[i], pt)) *arrow = -1;
            else if (PtInRect(&s_next[i], pt)) *arrow = 1;
            return i;
        }
    for (i = 0; i < B_COUNT; i++)
        if (PtInRect(&s_btn[i], pt)) return RBX_OPT_COUNT + i;
    return -1;
}

/* Arrow keys move in the two-column layout. */
static void move(int dx, int dy)
{
    int s = s_sel;
    if (s < RBX_OPT_COUNT) {
        int left = s < RBX_OPT_DISPLAY, k = left ? s : s - RBX_OPT_DISPLAY;
        int n = left ? RBX_OPT_DISPLAY : RBX_OPT_COUNT - RBX_OPT_DISPLAY;
        if (dx) {
            int other = left ? RBX_OPT_COUNT - RBX_OPT_DISPLAY : RBX_OPT_DISPLAY;
            if (k >= other) k = other - 1;
            s = left ? RBX_OPT_DISPLAY + k : k;
        } else if (k + dy < 0) {
            s = RBX_OPT_COUNT + B_PLAY;
        } else if (k + dy >= n) {
            s = RBX_OPT_COUNT + (left ? B_RESET : B_PLAY);
        } else
            s += dy;
    } else {
        int b = s - RBX_OPT_COUNT;
        if (dx) b = (b + B_COUNT + dx) % B_COUNT;
        else s = dy > 0 ? 0 : (b == B_RESET ? RBX_OPT_DISPLAY - 1 : RBX_OPT_COUNT - 1);
        if (dx) s = RBX_OPT_COUNT + b;
    }
    s_sel = s;
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_PAINT: paint(h); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: InvalidateRect(h, NULL, FALSE); return 0;
    case WM_GETMINMAXINFO: {
        MINMAXINFO *mm = (MINMAXINFO *)l;
        RECT r = {0, 0, 720, 480};
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        mm->ptMinTrackSize.x = r.right - r.left;
        mm->ptMinTrackSize.y = r.bottom - r.top;
        return 0;
    }
    case WM_MOUSEMOVE: {
        int a, now = hit(GET_X_LPARAM(l), GET_Y_LPARAM(l), &a);
        TRACKMOUSEEVENT t = {sizeof t, TME_LEAVE, h, 0};
        TrackMouseEvent(&t);
        if (now != s_hover) { s_hover = now; InvalidateRect(h, NULL, FALSE); }
        return 0;
    }
    case WM_MOUSELEAVE:
        if (s_hover != -1) { s_hover = -1; InvalidateRect(h, NULL, FALSE); }
        return 0;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN: {
        int a, row = hit(GET_X_LPARAM(l), GET_Y_LPARAM(l), &a);
        if (row < 0) return 0;
        s_sel = row;
        if (row < RBX_OPT_COUNT) step(row, m == WM_RBUTTONDOWN || a < 0 ? -1 : 1);
        else if (m == WM_LBUTTONDOWN) press(h, row - RBX_OPT_COUNT);
        InvalidateRect(h, NULL, FALSE);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        int row = s_hover >= 0 ? s_hover : s_sel;
        if (row < RBX_OPT_COUNT) {
            s_sel = row;
            step(row, GET_WHEEL_DELTA_WPARAM(w) > 0 ? 1 : -1);
            InvalidateRect(h, NULL, FALSE);
        }
        return 0;
    }
    case WM_KEYDOWN:
        switch (w) {
        case VK_UP: move(0, -1); break;
        case VK_DOWN: move(0, 1); break;
        case VK_LEFT: if (s_sel < RBX_OPT_COUNT) step(s_sel, -1); else move(-1, 0); break;
        case VK_RIGHT: if (s_sel < RBX_OPT_COUNT) step(s_sel, 1); else move(1, 0); break;
        case VK_TAB: s_sel = (s_sel + (GetKeyState(VK_SHIFT) < 0 ? ROWS - 1 : 1)) % ROWS; break;
        case VK_RETURN:
        case VK_SPACE:
            if (s_sel < RBX_OPT_COUNT) step(s_sel, 1);
            else press(h, s_sel - RBX_OPT_COUNT);
            break;
        case VK_ESCAPE: DestroyWindow(h); return 0;
        default: return 0;
        }
        s_hover = -1;
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    WNDCLASSEXW wc;
    RECT r;
    HWND h;
    MSG msg;
    float dpi = 1.0f;
    (void)prev;
    SetProcessDPIAware();
    {
        HDC dc = GetDC(NULL);
        dpi = GetDeviceCaps(dc, LOGPIXELSY) / 96.0f;
        ReleaseDC(NULL, dc);
    }
    locate();
    load();
    /* Config.exe --render <file.bmp> [w h]: draw the window into a bitmap and
     * exit (screenshots for the docs). */
    if (cmd && !strncmp(cmd, "--render ", 9)) {
        char out[MAX_PATH];
        int w = BASE_W, hh = BASE_H;
        if (sscanf(cmd + 9, "%259s %d %d", out, &w, &hh) >= 1)
            return render_to(out, w, hh);
        return 1;
    }
    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.lpszClassName = L"RobotechRecompConfig";
    RegisterClassExW(&wc);
    SetRect(&r, 0, 0, (int)(BASE_W * dpi), (int)(BASE_H * dpi));
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    h = CreateWindowExW(0, wc.lpszClassName, L"Robotech: Battlecry Recomp Settings", WS_OVERLAPPEDWINDOW,
                        CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, NULL, NULL, inst, NULL);
    if (!h) return 1;
    ShowWindow(h, show);
    UpdateWindow(h);
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
