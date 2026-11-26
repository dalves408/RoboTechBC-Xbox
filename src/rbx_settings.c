/* Robotech's host settings. The Esc/F1 workflow follows the NHLHitzPro player;
 * the look (rbx_menu.c, rbx_ui.c) is shared with Config.exe. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rbx_settings.h"
#include "rbx_menu.h"
#include "rbx_ui.h"
#include "video/gpu_present.h"
#include "input/keyboard.h"
#include "input/xinput_xbox.h"

extern HWND xbox_fb_window(void);
extern int xbox_fb_is_fullscreen(void);
extern void xbox_fb_toggle_fullscreen(void);
extern void (*xbox_fb_fullscreen_changed)(int);
extern int (*xbox_fb_key_hook)(unsigned, long, int);
extern void (*xbox_fb_tick_hook)(void);
extern void rbx_fb_focus_tick(void);
extern void xa2_set_muted(int);
extern int rbx_pad_peek(DWORD port, XBOX_INPUT_STATE *state);
extern int (*rbx_input_block_hook)(void);
extern int rbx_sdl_window_tick(HWND, int (*)(unsigned, long, int), int (*)(void));
extern int (*rbx_keyboard_poll_hook)(XBOX_INPUT_STATE *out);
extern int (*rbx_pad_swaps_hook)(void);

/* The options themselves are in rbx_options.h (shared with Config.exe). */
enum { SCALE = RBX_OPT_SCALE, FULLSCREEN = RBX_OPT_FULLSCREEN, STRETCH = RBX_OPT_STRETCH,
       WIDE = RBX_OPT_WIDE, VSYNC = RBX_OPT_VSYNC, RATE = RBX_OPT_RATE, AA = RBX_OPT_AA,
       ANISO = RBX_OPT_ANISO, HIRES = RBX_OPT_HIRES, INTERP = RBX_OPT_INTERP, CLOSE = RBX_ROW_RESUME, QUIT = RBX_ROW_QUIT,
       ROWS = RBX_ROW_COUNT };
enum { CLOSED, SETTINGS, CONFIRM };
#define options rbx_options
static unsigned choice[ROWS];
static WCHAR ini[MAX_PATH];
static HWND overlay;
static int mode, selected, yes, initialized, sdl_window_input, scenario_mode, pad_input, from_pad;
static int page, col, listening;
static const char *status;
static RbxKeymap keymap;       /* CONTROLS: read by the game thread, changed by the menu */
static volatile LONG block_input, hold_frame, video, available_msaa;
static DWORD release_at;
static WORD last_pad;

unsigned rbx_settings_video(void) { return (unsigned)InterlockedCompareExchange(&video, 0, 0); }
void rbx_settings_msaa_supported(unsigned mask) { InterlockedExchange(&available_msaa, (LONG)mask); }
int rbx_settings_input_blocked(void) { return InterlockedCompareExchange(&block_input, 0, 0) != 0; }
void rbx_settings_wait(void)
{
    while (InterlockedCompareExchange(&hold_frame, 0, 0))
        Sleep(8);
}

static void paint_overlay(void);

static void store_choice(int row)
{
    WCHAR value[12], key[32];
    if (!ini[0]) return;
    swprintf_s(value, 12, L"%u", choice[row]);
    MultiByteToWideChar(CP_UTF8, 0, options[row].key, -1, key, 32);
    WritePrivateProfileStringW(L"video", key, value, ini);
}

static void apply_choice(int row)
{
    /* One word gives the render thread a consistent settings snapshot. */
    LONG render = (LONG)(choice[SCALE] | choice[AA] << 3 | choice[ANISO] << 5 |
                         choice[HIRES] << 8 | choice[RATE] << 9 | choice[WIDE] << 12 |
                         choice[INTERP] << 15);
    InterlockedExchange(&video, render);
    if (row == FULLSCREEN && xbox_fb_window() &&
        (int)choice[FULLSCREEN] != xbox_fb_is_fullscreen())
        xbox_fb_toggle_fullscreen();
    gpu_present_set_stretch(choice[STRETCH]);
    gpu_present_set_vsync(choice[VSYNC]);
}

static void fullscreen_changed(int fullscreen)
{
    choice[FULLSCREEN] = (unsigned)fullscreen;
    store_choice(FULLSCREEN);
    if (mode) paint_overlay();
}

static LRESULT CALLBACK overlay_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_NCHITTEST) return HTTRANSPARENT;
    return DefWindowProcA(window, message, wparam, lparam);
}

/* Draw the menu (rbx_menu.c) into a 32-bit bitmap the size of the game
 * window: the panel solid, the game around it dimmed, then hand it to the
 * layered window. Painted only when something changes. */
static void paint_overlay(void)
{
    RECT client, panel, window;
    int cut = 0;
    if (!overlay || !mode || !GetClientRect(overlay, &client)) return;
    int w = client.right, h = client.bottom;
    if (w <= 0 || h <= 0) return;
    BITMAPINFO bi;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    HDC screen = GetDC(overlay), dc = CreateCompatibleDC(screen);
    unsigned *pixels = NULL;
    HBITMAP bitmap = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, (void **)&pixels, NULL, 0);
    if (bitmap && pixels) {
        HGDIOBJ old = SelectObject(dc, bitmap);
        memset(pixels, 0, (size_t)w * h * 4);
        if (mode == SETTINGS) {
            RbxMenuState state = {page, selected, col, listening, pad_input, choice, &keymap, status};
            rbx_menu_draw_settings(dc, w, h, &state, &panel, &cut);
        }
        else
            rbx_menu_draw_quit(dc, w, h, yes, pad_input, &panel, &cut);
        GdiFlush();
        /* GDI leaves alpha at 0: make the panel opaque and the rest black at 66%. */
        for (int y = 0; y < h; y++) {
            unsigned *row = pixels + (size_t)y * w;
            int inside_row = y >= panel.top && y < panel.bottom;
            for (int x = 0; x < w; x++)
                row[x] = inside_row && rbx_in_panel(&panel, cut, x, y) ? row[x] | 0xFF000000u
                                                                       : 0xA8000000u;
        }
        POINT source = {0, 0}, target;
        SIZE size = {w, h};
        BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        GetWindowRect(overlay, &window);
        target.x = window.left;
        target.y = window.top;
        UpdateLayeredWindow(overlay, screen, &target, &size, dc, &source, 0, &blend, ULW_ALPHA);
        SelectObject(dc, old);
    }
    if (bitmap) DeleteObject(bitmap);
    DeleteDC(dc);
    ReleaseDC(overlay, screen);
}

/* The overlay covers the game window's client area. */
static void position_overlay(void)
{
    HWND parent = xbox_fb_window();
    RECT client, current;
    POINT origin = {0, 0};
    if (!overlay || !parent || !GetClientRect(parent, &client)) return;
    ClientToScreen(parent, &origin);
    int width = client.right > 1 ? client.right : 1, height = client.bottom > 1 ? client.bottom : 1;
    GetWindowRect(overlay, &current);
    if (current.left != origin.x || current.top != origin.y ||
        current.right - current.left != width || current.bottom - current.top != height) {
        SetWindowPos(overlay, HWND_TOP, origin.x, origin.y, width, height, SWP_NOACTIVATE);
        paint_overlay();
    }
}

static void open_overlay(int next)
{
    HWND parent = xbox_fb_window();
    if (!parent) return;
    if (!overlay) {
        HINSTANCE instance = GetModuleHandleA(NULL);
        WNDCLASSA wc = {0};
        wc.hInstance = instance;
        wc.lpfnWndProc = overlay_proc;
        wc.lpszClassName = "RobotechSettingsOverlay";
        if (!RegisterClassA(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return;
        overlay = CreateWindowExA(WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW |
                                  WS_EX_TRANSPARENT, wc.lpszClassName, "", WS_POPUP,
                                  0, 0, 1, 1, parent, NULL, instance, NULL);
        if (!overlay) return;
    }
    if (!mode) {
        InterlockedExchange(&block_input, 1);
        xbox_InputKeysClear();
        InterlockedExchange(&hold_frame, 1);
        xa2_set_muted(1);
    }
    mode = next;
    selected = yes = col = listening = 0;
    status = NULL;
    position_overlay();
    paint_overlay();
    ShowWindow(overlay, SW_SHOWNOACTIVATE);
}

static void close_overlay(void)
{
    ShowWindow(overlay, SW_HIDE);
    mode = CLOSED;
    release_at = GetTickCount() + 180;
    xbox_InputKeysClear();
    xa2_set_muted(0);
    InterlockedExchange(&hold_frame, 0);
}

static void change_value(int direction)
{
    if (selected >= RBX_OPT_COUNT) {
        /* The button row: left and right move between RESUME and QUIT GAME. */
        selected = selected == CLOSE ? QUIT : CLOSE;
        return;
    }
    int count = (int)options[selected].count;
    if (!count) return;
    int value = (int)choice[selected];
    unsigned mask = (unsigned)InterlockedCompareExchange(&available_msaa, 0, 0) | 1u;
    do {
        value = (value + count + direction) % count;
    } while (selected == AA && !(mask & (1u << value)));
    if ((unsigned)value == choice[selected]) return;
    choice[selected] = (unsigned)value;
    apply_choice(selected);
    store_choice(selected);
}

/* ---- the CONTROLS page ---- */

static void save_keys(void)
{
    if (ini[0] && !rbx_keymap_save(&keymap, ini))
        status = "Could not save to settings.ini. The change lasts until the game closes.";
}

static void switch_page(int direction)
{
    page = (page + RBX_PAGE_COUNT + direction) % RBX_PAGE_COUNT;
    col = listening = 0;
    status = NULL;
    if (selected >= 0) selected = 0;
}

/* Up and down on either page; -1 is the tab row above the first row. */
static void move_row(int direction)
{
    int last = page == RBX_PAGE_VIDEO ? QUIT : RBX_CROW_BUTTONS;
    status = NULL;
    if (page == RBX_PAGE_VIDEO) {
        if (direction < 0) selected = selected == -1 ? QUIT : selected >= CLOSE ? CLOSE - 1 : selected - 1;
        else selected = selected >= CLOSE ? -1 : selected + 1;
        return;
    }
    int next = selected + direction;
    if (next < -1) next = last;
    if (next > last) next = -1;
    /* Between the key grid and the button row, keep to the same side. */
    if (selected == RBX_CROW_BUTTONS && next == RBX_CROW_BUTTONS - 1) col = col == RBX_CBTN_DEFAULTS ? 0 : 2;
    else if (next == RBX_CROW_BUTTONS && selected == RBX_CROW_BUTTONS - 1) col = col < 2 ? RBX_CBTN_DEFAULTS : RBX_CBTN_RESUME;
    else if (next == RBX_CROW_BUTTONS) col = RBX_CBTN_RESUME;
    else if (next >= RBX_CROW_GRID && (selected < RBX_CROW_GRID || selected == RBX_CROW_BUTTONS) &&
             next != RBX_CROW_BUTTONS - 1)
        col = 0;
    selected = next;
}

static void controls_side(int direction)
{
    if (selected == RBX_CROW_SWAP_AB) keymap.swap_ab = !keymap.swap_ab, save_keys();
    else if (selected == RBX_CROW_SWAP_XY) keymap.swap_xy = !keymap.swap_xy, save_keys();
    else if (selected == RBX_CROW_BUTTONS) col = (col + RBX_CBTN_COUNT + direction) % RBX_CBTN_COUNT;
    else col = (col + 4 + direction) % 4;
}

static void controls_enter(void)
{
    if (selected <= RBX_CROW_SWAP_XY) { controls_side(1); return; }
    if (selected < RBX_CROW_BUTTONS) { listening = 1; status = NULL; return; }
    if (col == RBX_CBTN_DEFAULTS) {
        unsigned char ab = keymap.swap_ab, xy = keymap.swap_xy;
        rbx_keymap_defaults(&keymap);
        keymap.swap_ab = ab;
        keymap.swap_xy = xy;
        save_keys();
        if (!status) status = "Keys reset to the default layout.";
    } else if (col == RBX_CBTN_RESUME) close_overlay();
    else open_overlay(CONFIRM);
}

/* The selected key slot gets the next key. */
static void listen_key(unsigned key)
{
    int action = rbx_grid_action(selected - RBX_CROW_GRID, col), slot = col % 2;
    if (key == VK_ESCAPE || key == VK_F1) { listening = 0; return; }
    if (key == VK_LSHIFT || key == VK_RSHIFT) key = VK_SHIFT;
    if (key == VK_LCONTROL || key == VK_RCONTROL) key = VK_CONTROL;
    if (key == VK_LMENU || key == VK_RMENU) key = VK_MENU;
    if (key == VK_DELETE) key = 0;
    else if (rbx_key_reserved((int)key) || key > 255) {
        status = "That key is kept for the menus and the window (Esc, F1, F9, F11, Delete).";
        return;
    }
    rbx_keymap_bind(&keymap, action, slot, (int)key);
    listening = 0;
    status = NULL;
    save_keys();
}

int rbx_settings_key(unsigned key, long flags, int down)
{
    if (scenario_mode) return 1;
    int repeat = (flags & (1L << 30)) != 0;
    if (down && pad_input != from_pad) {
        /* The key hints follow the last thing used: keyboard or controller. */
        pad_input = from_pad;
        if (mode) paint_overlay();
    }
    if (mode == SETTINGS && listening) {
        if (down && !repeat) listen_key(key);
        if (mode) paint_overlay();
        return 1;
    }
    if (key == VK_ESCAPE || key == VK_F1) {
        if (down && !repeat) {
            int next = key == VK_F1 ? SETTINGS : CONFIRM;
            if (mode == next || (mode == SETTINGS && key == VK_ESCAPE)) close_overlay();
            else open_overlay(next);
        }
        return 1;
    }
    if (!mode) return 0;
    if (!down) return 1;
    if (mode == SETTINGS) {
        int controls = page == RBX_PAGE_CONTROLS;
        switch (key) {
        case VK_TAB: case VK_NEXT: if (!repeat) switch_page(1); break;
        case VK_PRIOR: if (!repeat) switch_page(-1); break;
        case VK_UP: move_row(-1); break;
        case VK_DOWN: move_row(1); break;
        case VK_LEFT: case VK_RIGHT: {
            int d = key == VK_LEFT ? -1 : 1;
            if (selected < 0) { if (!repeat) switch_page(d); }
            else if (controls) controls_side(d);
            else change_value(d);
            break;
        }
        case VK_DELETE:
            if (controls && selected >= RBX_CROW_GRID && selected < RBX_CROW_BUTTONS) {
                rbx_keymap_bind(&keymap, rbx_grid_action(selected - RBX_CROW_GRID, col), col % 2, 0);
                save_keys();
            }
            break;
        case VK_RETURN:
            if (repeat) break;
            if (selected < 0) switch_page(1);
            else if (controls) controls_enter();
            else if (selected == CLOSE) close_overlay();
            else if (selected == QUIT) open_overlay(CONFIRM);
            else change_value(1);
            break;
        }
    } else {
        if (key == VK_LEFT || key == VK_RIGHT || key == VK_TAB) { if (!repeat) yes = !yes; }
        if (key == VK_RETURN && !repeat) {
            if (yes) PostMessageA(xbox_fb_window(), WM_CLOSE, 0, 0);
            close_overlay();
        }
    }
    if (mode) paint_overlay();
    return 1;
}

/* ---- the keyboard as pad 1, through the key map ---- */

static int keyboard_focus(void)
{
    HWND fg = GetForegroundWindow(), fb = xbox_fb_window();
    if (!fg) return 0;
    /* The console counts too: the game was always playable with it in front. */
    return fg == fb || fg == GetConsoleWindow() || (fb && GetAncestor(fg, GA_ROOTOWNER) == fb);
}

static int keyboard_poll(XBOX_INPUT_STATE *out)
{
    static DWORD packet;
    static int enabled = -1;
    if (enabled < 0) {
        const char *e = getenv("RECOMP_KEYBOARD");
        enabled = !(e && *e == '0');
    }
    if (!enabled) return 0;
    memset(out, 0, sizeof *out);
    out->dwPacketNumber = ++packet;
    if (!keyboard_focus()) return 1;
    int pos[4] = {0}, neg[4] = {0};
    for (int a = 0; a < RBX_ACTION_COUNT; a++) {
        const RbxAction *act = &rbx_actions[a];
        int held = 0;
        for (int k = 0; k < RBX_KEY_SLOTS; k++) {
            int vk = keymap.vk[a][k];
            if (vk && (GetAsyncKeyState(vk) & 0x8000)) held = 1;
        }
        if (!held) continue;
        if (act->kind == RBX_ACT_BUTTON) out->Gamepad.wButtons |= act->target;
        else if (act->kind == RBX_ACT_ANALOG) out->Gamepad.bAnalogButtons[act->target] = 255;
        else if (act->sign > 0) pos[act->target] = 1;
        else neg[act->target] = 1;
    }
    SHORT *axis[4] = {&out->Gamepad.sThumbLX, &out->Gamepad.sThumbLY,
                      &out->Gamepad.sThumbRX, &out->Gamepad.sThumbRY};
    for (int i = 0; i < 4; i++)
        *axis[i] = (SHORT)(pos[i] == neg[i] ? 0 : pos[i] ? 32767 : -32768);
    return 1;
}

static int pad_swaps(void) { return (keymap.swap_ab ? 1 : 0) | (keymap.swap_xy ? 2 : 0); }

static int window_key(unsigned key, long flags, int down)
{
    if (scenario_mode) return 1;
    if (!sdl_window_input) return rbx_settings_key(key, flags, down);
    /* While the menu is open, Delete clears a key slot: the framebuffer window
     * keeps it (a screenshot) from SDL's wrapper, so hand it over here. */
    if (key == VK_DELETE && mode != CLOSED) return rbx_settings_key(key, flags, down);
    /* Leave window and diagnostic shortcuts to the framebuffer window. */
    if (key == VK_F11 || key == VK_F9 || key == VK_DELETE ||
        (key == VK_RETURN && (flags & (1L << 29))))
        return mode != CLOSED;
    return 1; /* SDL's window wrapper has already queued the key. */
}

void rbx_settings_tick(void)
{
    HWND parent = xbox_fb_window();
    if (!parent) return;
    if (!initialized) {
        initialized = 1;
        apply_choice(FULLSCREEN);
    }
    sdl_window_input = rbx_sdl_window_tick(parent, rbx_settings_key, rbx_settings_input_blocked);
    if (scenario_mode) return;
    unsigned mask = (unsigned)InterlockedCompareExchange(&available_msaa, 0, 0);
    if (mask && !(mask & (1u << choice[AA]))) {
        do { choice[AA]--; } while (!(mask & (1u << choice[AA])));
        apply_choice(AA);
        if (mode) paint_overlay();
    }
    if (mode) position_overlay();
    else rbx_fb_focus_tick();

    XBOX_INPUT_STATE state;
    WORD pad = 0;
    if (rbx_pad_peek(0, &state)) {
        pad = state.Gamepad.wButtons;
        if (state.Gamepad.bAnalogButtons[XBOX_BUTTON_A]) pad |= 0x1000;
        if (state.Gamepad.bAnalogButtons[XBOX_BUTTON_B]) pad |= 0x2000;
        if (state.Gamepad.bAnalogButtons[XBOX_BUTTON_WHITE]) pad |= 0x4000;
        if (state.Gamepad.bAnalogButtons[XBOX_BUTTON_BLACK]) pad |= 0x8000;
        if (state.Gamepad.sThumbLY > 18000) pad |= 1;
        if (state.Gamepad.sThumbLY < -18000) pad |= 2;
        if (state.Gamepad.sThumbLX < -18000) pad |= 4;
        if (state.Gamepad.sThumbLX > 18000) pad |= 8;
    }
    WORD pressed = pad & ~last_pad;
    last_pad = pad;
    if (GetForegroundWindow() == parent) {
        if (!mode && (pad & 0x30) == 0x30 && (pressed & 0x10)) {
            pad_input = 1;
            open_overlay(SETTINGS);
        }
        else if (mode) {
            from_pad = 1;
            if (pressed && !pad_input) { pad_input = 1; paint_overlay(); }
            if (listening) {
                /* Waiting for a keyboard key: the pad can only cancel. */
                if (pressed & (0x2000 | 0x10)) { listening = 0; paint_overlay(); }
            } else if (pressed & (0x2000 | 0x10)) close_overlay();
            else if (mode == SETTINGS && (pressed & 0x4000)) { switch_page(-1); paint_overlay(); }
            else if (mode == SETTINGS && (pressed & 0x8000)) { switch_page(1); paint_overlay(); }
            else if (pressed & 1) rbx_settings_key(VK_UP, 0, 1);
            else if (pressed & 2) rbx_settings_key(VK_DOWN, 0, 1);
            else if (pressed & 4) rbx_settings_key(VK_LEFT, 0, 1);
            else if (pressed & 8) rbx_settings_key(VK_RIGHT, 0, 1);
            else if (pressed & 0x1000) rbx_settings_key(VK_RETURN, 0, 1);
            from_pad = 0;
        }
    }
    if (!mode && rbx_settings_input_blocked() && !pad &&
        !(GetAsyncKeyState(VK_ESCAPE) & 0x8000) && !(GetAsyncKeyState(VK_RETURN) & 0x8000) &&
        (LONG)(GetTickCount() - release_at) >= 0) {
        xbox_InputKeysClear();
        InterlockedExchange(&block_input, 0);
    }
}

void rbx_settings_install(void)
{
    const char *override = getenv("RECOMP_RBX_SETTINGS");
    const char *frame_pad = getenv("RECOMP_RBX_FRAME_PAD");
    scenario_mode = frame_pad && *frame_pad;
    WCHAR name[MAX_PATH] = L"settings.ini";
    if (!override || !*override ||
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, override, -1, name, MAX_PATH)) {
        DWORD n = GetFullPathNameW(name, MAX_PATH, ini, NULL);
        if (!n || n >= MAX_PATH) ini[0] = 0;
    }

    for (int row = 0; row < CLOSE; row++) {
        unsigned value = options[row].initial;
        if (ini[0]) {
            WCHAR key[32];
            MultiByteToWideChar(CP_UTF8, 0, options[row].key, -1, key, 32);
            value = GetPrivateProfileIntW(L"video", key, value, ini);
        }
        choice[row] = value < options[row].count ? value : options[row].initial;
    }
    const char *fullscreen = getenv("RECOMP_FULLSCREEN");
    const char *vsync = getenv("RECOMP_PRESENT_VSYNC");
    const char *stretch = getenv("RECOMP_PRESENT_STRETCH");
    if (fullscreen) choice[FULLSCREEN] = *fullscreen && *fullscreen != '0';
    if (vsync) choice[VSYNC] = *vsync && *vsync != '0';
    if (stretch) choice[STRETCH] = *stretch && *stretch != '0';
    const char *wide = getenv("RECOMP_RBX_WIDESCREEN");
    if (wide && *wide) {
        /* off (0), auto (1), 16:9, 16:10, 21:9; any other w:h is a custom shape the renderer reads itself */
        static const char *const names[] = {"off", "auto", "16:9", "16:10", "21:9"};
        for (unsigned i = 0; i < 5; i++)
            if (!_stricmp(wide, names[i])) choice[WIDE] = i;
        if (!strcmp(wide, "0")) choice[WIDE] = 0;
        if (!strcmp(wide, "1")) choice[WIDE] = 1;
    }
    apply_choice(-1);
    rbx_keymap_load(&keymap, ini[0] ? ini : NULL);
    /* RECOMP_RBX_TOOLKIT_KEYS=1 goes back to the toolkit's fixed key map. */
    const char *toolkit_keys = getenv("RECOMP_RBX_TOOLKIT_KEYS");
    if (!(toolkit_keys && *toolkit_keys == '1')) rbx_keyboard_poll_hook = keyboard_poll;
    rbx_pad_swaps_hook = pad_swaps;
    xbox_fb_key_hook = window_key;
    xbox_fb_tick_hook = rbx_settings_tick;
    xbox_fb_fullscreen_changed = fullscreen_changed;
    rbx_input_block_hook = rbx_settings_input_blocked;
}
