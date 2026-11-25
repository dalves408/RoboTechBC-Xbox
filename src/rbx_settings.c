/* Robotech's host settings. The Esc/F1 workflow follows the NHLHitzPro player. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rbx_settings.h"
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

enum { SCALE, FULLSCREEN, STRETCH, WIDE, VSYNC, RATE, AA, ANISO, HIRES, CLOSE, QUIT, ROWS };
enum { CLOSED, SETTINGS, CONFIRM };
static const struct {
    const char *label, *key;
    unsigned count, initial;
    const char *values[7];
} options[ROWS] = {
    {"Render scale", "scale", 5, 0, {"1x / 640 x 480", "2x / 1280 x 960", "3x / 1920 x 1440", "4x / 2560 x 1920", "5x / 3200 x 2400"}},
    {"Fullscreen", "fullscreen", 2, 0, {"Off", "On"}},
    {"Stretch image", "stretch", 2, 0, {"Off / Keep shape", "On / Fill window"}},
    /* Widescreen renders more of the scene left and right (Hor+) and keeps the HUD and menus at 4:3 in the middle. */
    {"Widescreen", "widescreen", 5, 0, {"Off / 4:3", "Auto / Match window", "16:9", "16:10", "21:9"}},
    {"VSync", "vsync", 2, 0, {"Off", "On"}},
    {"FPS limit", "rate", 7, 6, {"30 FPS", "60 FPS", "120 FPS", "144 FPS", "240 FPS", "360 FPS", "Uncapped"}},
    {"Anti-aliasing", "msaa", 4, 0, {"Off", "2x MSAA", "4x MSAA", "8x MSAA"}},
    {"Anisotropic filtering", "anisotropy", 5, 0, {"Off", "2x", "4x", "8x", "16x"}},
    {"Hi-res textures", "hires", 2, 1, {"Off", "On"}},
    {"Close settings"}, {"Quit game"}
};
static unsigned choice[ROWS];
static WCHAR ini[MAX_PATH];
static HWND overlay;
static HFONT body, heading;
static int mode, selected, yes, initialized, sdl_window_input, scenario_mode;
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
                         choice[HIRES] << 8 | choice[RATE] << 9 | choice[WIDE] << 12);
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
    if (overlay) InvalidateRect(overlay, NULL, FALSE);
}

static void fill(HDC dc, RECT rect, COLORREF color)
{
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &rect, brush);
    DeleteObject(brush);
}

static void label(HDC dc, const char *text, RECT rect, COLORREF color, UINT alignment)
{
    SetTextColor(dc, color);
    DrawTextA(dc, text, -1, &rect, alignment | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
}

static LRESULT CALLBACK overlay_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_NCHITTEST) return HTTRANSPARENT;
    if (message == WM_PAINT) {
        PAINTSTRUCT paint;
        HDC dc = BeginPaint(window, &paint);
        RECT r;
        GetClientRect(window, &r);
        fill(dc, r, RGB(16, 24, 36));
        fill(dc, (RECT){0, 0, r.right, 3}, RGB(65, 202, 196));
        SetBkMode(dc, TRANSPARENT);
        SelectObject(dc, heading);
        label(dc, mode == SETTINGS ? "Video settings" : "Quit Robotech: Battlecry?",
              (RECT){20, 12, r.right - 20, 49}, RGB(242, 249, 255), DT_LEFT);
        SelectObject(dc, body);
        if (mode == SETTINGS) {
            for (int row = 0; row < ROWS; row++) {
                int y = 55 + row * 24;
                if (row == selected)
                    fill(dc, (RECT){11, y, r.right - 11, y + 24}, RGB(31, 69, 82));
                label(dc, options[row].label, (RECT){20, y, r.right - 20, y + 24},
                      RGB(242, 249, 255), DT_LEFT);
                if (options[row].count)
                    label(dc, options[row].values[choice[row]],
                          (RECT){20, y, r.right - 20, y + 24},
                          RGB(126, 226, 218), DT_RIGHT);
            }
            label(dc, "Arrows change settings   Enter selects   Esc or F1 closes",
                  (RECT){20, r.bottom - 32, r.right - 20, r.bottom - 8},
                  RGB(171, 192, 209), DT_LEFT);
        } else {
            fill(dc, yes ? (RECT){r.right / 2, 59, r.right - 15, 89}
                         : (RECT){15, 59, r.right / 2, 89}, RGB(31, 69, 82));
            label(dc, "Keep playing", (RECT){15, 59, r.right / 2, 89}, RGB(242, 249, 255), DT_CENTER);
            label(dc, "Quit game", (RECT){r.right / 2, 59, r.right - 15, 89}, RGB(242, 249, 255), DT_CENTER);
        }
        EndPaint(window, &paint);
        return 0;
    }
    return DefWindowProcA(window, message, wparam, lparam);
}

static void position_overlay(void)
{
    HWND parent = xbox_fb_window();
    RECT client, current;
    POINT origin = {0, 0};
    int width = 444, height = mode == SETTINGS ? 358 : 105;
    if (!overlay || !parent || !GetClientRect(parent, &client)) return;
    ClientToScreen(parent, &origin);
    int x = origin.x + (client.right - width) / 2;
    int y = origin.y + (client.bottom - height) / 2;
    GetWindowRect(overlay, &current);
    if (current.left != x || current.top != y ||
        current.right - current.left != width || current.bottom - current.top != height)
        SetWindowPos(overlay, HWND_TOP, x, y, width, height, SWP_NOACTIVATE);
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
        SetLayeredWindowAttributes(overlay, 0, 248, LWA_ALPHA);
        body = CreateFontA(-15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                           OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, 0, "Segoe UI");
        heading = CreateFontA(-23, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, 0, "Segoe UI");
    }
    if (!mode) {
        InterlockedExchange(&block_input, 1);
        xbox_InputKeysClear();
        InterlockedExchange(&hold_frame, 1);
        xa2_set_muted(1);
    }
    mode = next;
    selected = yes = 0;
    position_overlay();
    ShowWindow(overlay, SW_SHOWNOACTIVATE);
    InvalidateRect(overlay, NULL, FALSE);
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

int rbx_settings_key(unsigned key, long flags, int down)
{
    if (scenario_mode) return 1;
    int repeat = (flags & (1L << 30)) != 0;
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
        switch (key) {
        case VK_UP: selected = (selected + ROWS - 1) % ROWS; break;
        case VK_DOWN: selected = (selected + 1) % ROWS; break;
        case VK_LEFT: change_value(-1); break;
        case VK_RIGHT: change_value(1); break;
        case VK_RETURN:
            if (repeat) break;
            if (selected == CLOSE) close_overlay();
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
    if (mode) InvalidateRect(overlay, NULL, FALSE);
    return 1;
}

static int window_key(unsigned key, long flags, int down)
{
    if (scenario_mode) return 1;
    if (!sdl_window_input) return rbx_settings_key(key, flags, down);
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
        if (mode) InvalidateRect(overlay, NULL, FALSE);
    }
    if (mode) position_overlay();
    else rbx_fb_focus_tick();

    XBOX_INPUT_STATE state;
    WORD pad = 0;
    if (rbx_pad_peek(0, &state)) {
        pad = state.Gamepad.wButtons;
        if (state.Gamepad.bAnalogButtons[XBOX_BUTTON_A]) pad |= 0x1000;
        if (state.Gamepad.bAnalogButtons[XBOX_BUTTON_B]) pad |= 0x2000;
        if (state.Gamepad.sThumbLY > 18000) pad |= 1;
        if (state.Gamepad.sThumbLY < -18000) pad |= 2;
        if (state.Gamepad.sThumbLX < -18000) pad |= 4;
        if (state.Gamepad.sThumbLX > 18000) pad |= 8;
    }
    WORD pressed = pad & ~last_pad;
    last_pad = pad;
    if (GetForegroundWindow() == parent) {
        if (!mode && (pad & 0x30) == 0x30 && (pressed & 0x10)) open_overlay(SETTINGS);
        else if (mode) {
            if (pressed & (0x2000 | 0x10)) close_overlay();
            else if (pressed & 1) rbx_settings_key(VK_UP, 0, 1);
            else if (pressed & 2) rbx_settings_key(VK_DOWN, 0, 1);
            else if (pressed & 4) rbx_settings_key(VK_LEFT, 0, 1);
            else if (pressed & 8) rbx_settings_key(VK_RIGHT, 0, 1);
            else if (pressed & 0x1000) rbx_settings_key(VK_RETURN, 0, 1);
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
    xbox_fb_key_hook = window_key;
    xbox_fb_tick_hook = rbx_settings_tick;
    xbox_fb_fullscreen_changed = fullscreen_changed;
    rbx_input_block_hook = rbx_settings_input_blocked;
}
