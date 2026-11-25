/* Config.exe: Robotech: Battlecry recomp settings, outside the game. Reads and
 * writes the same settings.ini (beside this program) as the in-game F1 menu,
 * with the same look (src/rbx_ui.c), the same video options (rbx_options.h)
 * and the same key map (rbx_keymap.h). Two pages, VIDEO and CONTROLS.
 * Mouse: point and click; right click or the wheel steps a setting back,
 * right click clears a key. Keyboard: arrows move, Enter changes, Page Up /
 * Page Down switch pages, Esc closes. */
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <stdio.h>
#include <string.h>

#include "rbx_options.h"
#include "rbx_keymap.h"
#include "rbx_menu.h"
#include "rbx_ui.h"

#define BASE_W 960
#define BASE_H 700

enum { B_RESET, B_SAVE, B_PLAY, B_CLOSE, B_COUNT };
static const char *const B_TEXT[B_COUNT] = { "DEFAULTS", "SAVE", "SAVE & PLAY", "CLOSE" };

/* What is on screen, rebuilt by every paint: the selection is an index into it. */
enum { IT_TAB, IT_OPT, IT_SWAP, IT_KEY, IT_BTN };
typedef struct { int kind, a, b; RECT r, prev, next; } Item;
static Item s_item[80];
static int s_items;

static unsigned s_value[RBX_OPT_COUNT], s_saved[RBX_OPT_COUNT];
static RbxKeymap s_map, s_map_saved;
static WCHAR s_dir[MAX_PATH], s_ini[MAX_PATH];
static int s_page, s_sel = 1, s_hover = -1, s_listen, s_have_game;
static char s_status[160];
static COLORREF s_status_color;

static void locate(void)
{
    WCHAR path[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, s_dir, MAX_PATH);
    WCHAR *slash;
    if (!n || n >= MAX_PATH || !(slash = wcsrchr(s_dir, L'\\')))
        GetCurrentDirectoryW(MAX_PATH, s_dir);
    else
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
    rbx_keymap_load(&s_map, s_ini);
    s_map_saved = s_map;
}

static int dirty(void)
{
    return memcmp(s_value, s_saved, sizeof s_value) != 0 || memcmp(&s_map, &s_map_saved, sizeof s_map) != 0;
}

static void say(const char *text, int error)
{
    snprintf(s_status, sizeof s_status, "%s", text);
    s_status_color = error ? RGB(222, 62, 50) : RGB(110, 190, 255);
}

static int save(void)
{
    int i, ok = 1;
    for (i = 0; i < RBX_OPT_COUNT; i++) {
        WCHAR key[32], value[12];
        MultiByteToWideChar(CP_UTF8, 0, rbx_options[i].key, -1, key, 32);
        swprintf(value, 12, L"%u", s_value[i]);
        ok &= WritePrivateProfileStringW(L"video", key, value, s_ini) != 0;
    }
    ok &= rbx_keymap_save(&s_map, s_ini);
    WritePrivateProfileStringW(NULL, NULL, NULL, s_ini);    /* flush */
    if (ok) {
        memcpy(s_saved, s_value, sizeof s_saved);
        s_map_saved = s_map;
        say("Saved. The game uses these the next time it starts.", 0);
    } else
        say("Could not write settings.ini. Is the folder read-only?", 1);
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
        say("Could not start the game. Put Config.exe in the game's folder.", 1);
}

static void press(HWND h, int button)
{
    switch (button) {
    case B_RESET:
        if (s_page == RBX_PAGE_VIDEO) {
            int i;
            for (i = 0; i < RBX_OPT_COUNT; i++) s_value[i] = rbx_options[i].initial;
            say("Video settings back to their defaults. Select SAVE to keep them.", 0);
        } else {
            rbx_keymap_defaults(&s_map);
            say("Keys and buttons back to their defaults. Select SAVE to keep them.", 0);
        }
        break;
    case B_SAVE: save(); break;
    case B_PLAY: if (save()) play(h); break;
    case B_CLOSE: DestroyWindow(h); break;
    }
}

/* Change the item: dir +1 / -1 steps a setting, opens a key slot for a new key. */
static void change(HWND h, int i, int dir)
{
    Item *it;
    if (i < 0 || i >= s_items) return;
    it = &s_item[i];
    s_status[0] = 0;
    switch (it->kind) {
    case IT_TAB: s_page = it->a; s_sel = it->a; s_listen = 0; break;
    case IT_OPT: {
        int n = (int)rbx_options[it->a].count;
        s_value[it->a] = (unsigned)(((int)s_value[it->a] + n + dir) % n);
        break;
    }
    case IT_SWAP:
        if (it->a) s_map.swap_xy = !s_map.swap_xy;
        else s_map.swap_ab = !s_map.swap_ab;
        break;
    case IT_KEY: s_listen = 1; break;
    case IT_BTN: press(h, it->a); break;
    }
}

static void bind(int vk)
{
    Item *it = &s_item[s_sel];
    if (vk == VK_ESCAPE) { s_listen = 0; return; }
    if (vk == VK_DELETE) vk = 0;
    else if (rbx_key_reserved(vk) || vk > 255) {
        say("That key is kept for the game's menus and window (Esc, F1, F9, F11, Delete).", 1);
        return;
    }
    rbx_keymap_bind(&s_map, it->a, it->b, vk);
    s_listen = 0;
    s_status[0] = 0;
}

/* ---- drawing -------------------------------------------------------------- */

static Item *add(int kind, int a, int b, const RECT *r)
{
    Item *it = &s_item[s_items < (int)(sizeof s_item / sizeof s_item[0]) - 1 ? s_items++ : s_items];
    it->kind = kind;
    it->a = a;
    it->b = b;
    it->r = *r;
    SetRectEmpty(&it->prev);
    SetRectEmpty(&it->next);
    return it;
}

static int is(int i, int kind) { return i >= 0 && i < s_items && s_item[i].kind == kind; }

static const char *note_text(void)
{
    int i = s_hover >= 0 ? s_hover : s_sel;
    if (i < 0 || i >= s_items) return "";
    switch (s_item[i].kind) {
    case IT_TAB: return "Video and controls. Page Up and Page Down switch pages.";
    case IT_OPT: return rbx_options[s_item[i].a].note;
    case IT_SWAP: return rbx_swap_notes[s_item[i].a];
    case IT_KEY: return s_listen ? "Press the key for this control. Esc cancels, Delete clears the slot."
                                 : "Click a slot and press a key. Right click or Delete clears it. "
                                   "A key does one thing: binding it moves it here.";
    default:
        switch (s_item[i].a) {
        case B_RESET: return s_page == RBX_PAGE_VIDEO ? "Put the video settings back to their defaults."
                                                       : "Put the keys and the button swaps back to their defaults.";
        case B_SAVE: return "Write settings.ini. F1 in the game edits the same file.";
        case B_PLAY: return "Save, then start the game.";
        default: return "Close without saving.";
        }
    }
}

static void draw_video(HDC dc, RbxFonts *f, int x0, int x1, int y0)
{
    RECT sec, row;
    int gap = rbx_px(f, 32), colw = (x1 - x0 - gap) / 2, i;
    f->value_pct = 58;              /* two narrower columns: give the readouts more room */
    for (i = 0; i < RBX_OPT_COUNT; i++) {
        int left = i < RBX_OPT_DISPLAY, k = left ? i : i - RBX_OPT_DISPLAY, cx = left ? x0 : x0 + colw + gap, y;
        Item *it;
        if (k == 0) {
            SetRect(&sec, cx, y0, cx + colw, y0 + rbx_px(f, 30));
            rbx_section(dc, f, &sec, left ? 1 : 2, left ? "DISPLAY" : "GRAPHICS");
        }
        y = y0 + rbx_px(f, 40) + k * rbx_px(f, 54);
        SetRect(&row, cx, y, cx + colw, y + rbx_px(f, 42));
        it = add(IT_OPT, i, 0, &row);
        rbx_option(dc, f, &row, rbx_options[i].label, rbx_options[i].values[s_value[i]], s_value[i],
                   rbx_options[i].count, s_sel == s_items - 1, s_hover == s_items - 1 && s_sel != s_items - 1,
                   &it->prev, &it->next);
    }
}

static void draw_controls(HDC dc, RbxFonts *f, int x0, int x1, int y)
{
    RECT sec, row, slots[RBX_KEY_SLOTS];
    int gap = rbx_px(f, 32), colw = (x1 - x0 - gap) / 2, i, c, k;
    char name[24];
    SetRect(&sec, x0, y, x1, y + rbx_px(f, 30));
    rbx_section(dc, f, &sec, 1, "CONTROLLER");
    y += rbx_px(f, 34);
    f->value_pct = 50;
    for (i = 0; i < 2; i++) {
        unsigned v = i ? s_map.swap_xy : s_map.swap_ab;
        int cx = x0 + i * (colw + gap);
        Item *it;
        SetRect(&row, cx, y, cx + colw, y + rbx_px(f, 38));
        it = add(IT_SWAP, i, 0, &row);
        rbx_option(dc, f, &row, rbx_swap_labels[i], rbx_swap_values[v], v, 2, s_sel == s_items - 1,
                   s_hover == s_items - 1 && s_sel != s_items - 1, &it->prev, &it->next);
    }
    y += rbx_px(f, 46);
    SetRect(&sec, x0, y, x1, y + rbx_px(f, 30));
    rbx_section(dc, f, &sec, 2, "KEYBOARD  /  PLAYER 1");
    y += rbx_px(f, 34);
    for (c = 0; c < 2; c++)
        for (i = 0; i < RBX_GRID_ROWS; i++) {
            int a = rbx_grid_action(i, c * 2), cx = x0 + c * (colw + gap), ry = y + i * rbx_px(f, 24);
            int first = s_items, sel_row = s_sel >= first && s_sel < first + RBX_KEY_SLOTS;
            int hov_row = s_hover >= first && s_hover < first + RBX_KEY_SLOTS;
            SetRect(&row, cx, ry, cx + colw, ry + rbx_px(f, 23));
            rbx_keyrow(dc, f, &row, rbx_actions[a].label, sel_row, hov_row && !sel_row, RBX_KEY_SLOTS, slots);
            for (k = 0; k < RBX_KEY_SLOTS; k++) {
                int id = s_items;
                add(IT_KEY, a, k, &slots[k]);
                rbx_key_name(s_map.vk[a][k], name, sizeof name);
                rbx_keyslot(dc, f, &slots[k], name, s_sel == id, s_hover == id, s_sel == id && s_listen);
            }
        }
}

static void draw(HDC dc, int w, int h)
{
    RbxFonts f;
    RECT all, p, band, tabs, note, bar, tab_r[RBX_PAGE_COUNT];
    int i, x0, x1, content;
    char line[200];

    s_items = 0;
    SetRect(&all, 0, 0, w, h);
    rbx_backdrop(dc, &all);
    rbx_fonts_make(&f, rbx_fit(w, h, BASE_W, BASE_H));
    {
        int pw = rbx_px(&f, 912), ph = rbx_px(&f, 664);
        SetRect(&p, (w - pw) / 2, (h - ph) / 2, (w - pw) / 2 + pw, (h - ph) / 2 + ph);
    }
    rbx_panel(dc, &f, &p, rbx_px(&f, 28));
    x0 = p.left + rbx_px(&f, 36);
    x1 = p.right - rbx_px(&f, 36);
    SetRect(&band, x0, p.top + rbx_px(&f, 18), x1, p.top + rbx_px(&f, 80));
    rbx_header(dc, &f, &band, "ROBOTECH: BATTLECRY",
               s_page == RBX_PAGE_VIDEO ? "RECOMP  //  VIDEO SETTINGS" : "RECOMP  //  CONTROLS",
               "RDF FIELD MANUAL");
    SetRect(&tabs, x0, band.bottom + rbx_px(&f, 12), x1, band.bottom + rbx_px(&f, 40));
    rbx_tabs(dc, &f, &tabs, rbx_page_names, RBX_PAGE_COUNT, s_page, is(s_sel, IT_TAB), tab_r);
    for (i = 0; i < RBX_PAGE_COUNT; i++) add(IT_TAB, i, 0, &tab_r[i]);
    content = tabs.bottom + rbx_px(&f, 12);
    if (s_page == RBX_PAGE_VIDEO) draw_video(dc, &f, x0, x1, content);
    else draw_controls(dc, &f, x0, x1, content);

    SetRect(&note, x0, p.bottom - rbx_px(&f, 122), x1, p.bottom - rbx_px(&f, 90));
    rbx_note(dc, &f, &note, "NOTE", note_text());

    /* Status: a message, unsaved changes, or where the files are. */
    SetRect(&bar, x0, note.bottom + rbx_px(&f, 4), x1, note.bottom + rbx_px(&f, 24));
    if (s_status[0])
        rbx_text(dc, f.fine, s_status_color, &bar, s_status, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    else {
        snprintf(line, sizeof line, "%s", dirty() ? "Unsaved changes."
                 : s_have_game ? "Game files found. Settings: settings.ini in this folder."
                               : "No game_files folder here. Put Config.exe beside robotech_bc_recomp.exe.");
        rbx_text(dc, f.fine, dirty() ? RGB(255, 168, 38) : s_have_game ? RGB(116, 138, 168) : RGB(222, 62, 50),
                 &bar, line, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    /* Buttons at the bottom right; DEFAULTS alone at the left. */
    {
        int bh = rbx_px(&f, 42), by = p.bottom - rbx_px(&f, 20) - bh, bw = rbx_px(&f, 150), bx = x1;
        RECT br[B_COUNT];
        for (i = B_COUNT - 1; i >= 1; i--) {
            int wdt = i == B_PLAY ? rbx_px(&f, 190) : bw;
            SetRect(&br[i], bx - wdt, by, bx, by + bh);
            bx -= wdt + rbx_px(&f, 12);
        }
        SetRect(&br[B_RESET], x0, by, x0 + bw, by + bh);
        for (i = 0; i < B_COUNT; i++) {
            int id = s_items;
            add(IT_BTN, i, 0, &br[i]);
            rbx_button(dc, &f, &br[i], B_TEXT[i], i == B_PLAY ? RBX_BTN_PRIMARY : RBX_BTN_NORMAL,
                       s_sel == id, s_hover == id);
        }
    }
    if (s_sel >= s_items) s_sel = s_items - 1;
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
    draw(dc, w, h);             /* lays out the items */
    draw(dc, w, h);             /* and draws with the selection in place */
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
    for (i = 0; i < s_items; i++)
        if (PtInRect(&s_item[i].r, pt)) {
            if (PtInRect(&s_item[i].prev, pt)) *arrow = -1;
            else if (PtInRect(&s_item[i].next, pt)) *arrow = 1;
            return i;
        }
    return -1;
}

/* Arrow keys: the nearest item in that direction (rows first, then columns). */
static void move(int dx, int dy)
{
    const RECT *c = &s_item[s_sel].r;
    int cx = (c->left + c->right) / 2, cy = (c->top + c->bottom) / 2, best = -1, i;
    long best_d = 0;
    for (i = 0; i < s_items; i++) {
        const RECT *r = &s_item[i].r;
        int x = (r->left + r->right) / 2, y = (r->top + r->bottom) / 2;
        long along = dx ? (long)(x - cx) * dx : (long)(y - cy) * dy, across = dx ? labs(y - cy) : labs(x - cx);
        long d;
        if (i == s_sel || along <= 0) continue;
        d = along + across * 3;
        if (best < 0 || d < best_d) { best = i; best_d = d; }
    }
    if (best >= 0) s_sel = best;
}

static void switch_page(int page)
{
    s_page = (page + RBX_PAGE_COUNT) % RBX_PAGE_COUNT;
    s_sel = s_page;             /* its tab */
    s_listen = 0;
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_PAINT: paint(h); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: InvalidateRect(h, NULL, FALSE); return 0;
    case WM_GETMINMAXINFO: {
        MINMAXINFO *mm = (MINMAXINFO *)l;
        RECT r = {0, 0, 760, 554};
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
        int a, i = hit(GET_X_LPARAM(l), GET_Y_LPARAM(l), &a);
        if (s_listen) s_listen = 0;
        if (i < 0) { InvalidateRect(h, NULL, FALSE); return 0; }
        s_sel = i;
        if (m == WM_RBUTTONDOWN) {
            if (s_item[i].kind == IT_KEY) { rbx_keymap_bind(&s_map, s_item[i].a, s_item[i].b, 0); s_status[0] = 0; }
            else if (s_item[i].kind == IT_OPT || s_item[i].kind == IT_SWAP) change(h, i, -1);
        } else
            change(h, i, a < 0 ? -1 : 1);
        if (IsWindow(h)) InvalidateRect(h, NULL, FALSE);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        int i = s_hover >= 0 ? s_hover : s_sel;
        if (is(i, IT_OPT) || is(i, IT_SWAP)) {
            s_sel = i;
            change(h, i, GET_WHEEL_DELTA_WPARAM(w) > 0 ? 1 : -1);
            InvalidateRect(h, NULL, FALSE);
        }
        return 0;
    }
    case WM_SYSKEYDOWN:             /* Alt and F10: only to bind them */
    case WM_KEYDOWN:
        if (m == WM_SYSKEYDOWN && !s_listen) break;
        if (s_listen) {
            if (!(l & (1L << 30))) bind((int)w);
            InvalidateRect(h, NULL, FALSE);
            return 0;
        }
        switch (w) {
        case VK_UP: move(0, -1); break;
        case VK_DOWN: move(0, 1); break;
        case VK_LEFT:
        case VK_RIGHT: {
            int d = w == VK_LEFT ? -1 : 1;
            if (is(s_sel, IT_OPT) || is(s_sel, IT_SWAP)) change(h, s_sel, d);
            else if (is(s_sel, IT_TAB)) switch_page(s_page + d);
            else move(d, 0);
            break;
        }
        case VK_PRIOR: switch_page(s_page - 1); break;
        case VK_NEXT: switch_page(s_page + 1); break;
        case VK_TAB: s_sel = (s_sel + (GetKeyState(VK_SHIFT) < 0 ? s_items - 1 : 1)) % s_items; break;
        case VK_DELETE:
            if (is(s_sel, IT_KEY)) rbx_keymap_bind(&s_map, s_item[s_sel].a, s_item[s_sel].b, 0);
            break;
        case VK_RETURN:
        case VK_SPACE:
            change(h, s_sel, 1);
            break;
        case VK_ESCAPE: DestroyWindow(h); return 0;
        default: return 0;
        }
        s_hover = -1;
        if (IsWindow(h)) InvalidateRect(h, NULL, FALSE);
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
    /* Config.exe --render <file.bmp> [w h [page [selected]]]: draw the window
     * into a bitmap and exit (screenshots for the docs). */
    if (cmd && !strncmp(cmd, "--render ", 9)) {
        char out[MAX_PATH];
        int w = BASE_W, hh = BASE_H, page = 0, sel = 2;
        if (sscanf(cmd + 9, "%259s %d %d %d %d", out, &w, &hh, &page, &sel) < 1) return 1;
        s_page = page;
        s_sel = sel;
        return render_to(out, w, hh);
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
    s_sel = 2;                  /* the first setting */
    ShowWindow(h, show);
    UpdateWindow(h);
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
