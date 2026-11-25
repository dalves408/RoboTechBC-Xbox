/* rbx_keymap.c -- see rbx_keymap.h. */
#include <stdio.h>
#include <string.h>

#include "rbx_keymap.h"

/* Pad targets (xinput_xbox.h), kept here so Config.exe needs no toolkit header. */
#define B_UP 0x0001
#define B_DOWN 0x0002
#define B_LEFT 0x0004
#define B_RIGHT 0x0008
#define B_START 0x0010
#define B_BACK 0x0020
#define B_LS 0x0040
#define B_RS 0x0080
enum { A_A, A_B, A_X, A_Y, A_BLACK, A_WHITE, A_LT, A_RT };

/* The defaults are the README's table. */
const RbxAction rbx_actions[RBX_ACTION_COUNT] = {
    {"Move forward",     "move_up",     RBX_ACT_AXIS,   RBX_AXIS_LY, +1, {'W', VK_NUMPAD8}},
    {"Move back",        "move_down",   RBX_ACT_AXIS,   RBX_AXIS_LY, -1, {'S', VK_NUMPAD2}},
    {"Move left",        "move_left",   RBX_ACT_AXIS,   RBX_AXIS_LX, -1, {'A', VK_NUMPAD4}},
    {"Move right",       "move_right",  RBX_ACT_AXIS,   RBX_AXIS_LX, +1, {'D', VK_NUMPAD6}},
    {"Look up",          "look_up",     RBX_ACT_AXIS,   RBX_AXIS_RY, +1, {'I', 0}},
    {"Look down",        "look_down",   RBX_ACT_AXIS,   RBX_AXIS_RY, -1, {'K', 0}},
    {"Look left",        "look_left",   RBX_ACT_AXIS,   RBX_AXIS_RX, -1, {'J', 0}},
    {"Look right",       "look_right",  RBX_ACT_AXIS,   RBX_AXIS_RX, +1, {'L', 0}},
    {"D-pad up",         "dpad_up",     RBX_ACT_BUTTON, B_UP,         0, {VK_UP, 0}},
    {"D-pad down",       "dpad_down",   RBX_ACT_BUTTON, B_DOWN,       0, {VK_DOWN, 0}},
    {"D-pad left",       "dpad_left",   RBX_ACT_BUTTON, B_LEFT,       0, {VK_LEFT, 0}},
    {"D-pad right",      "dpad_right",  RBX_ACT_BUTTON, B_RIGHT,      0, {VK_RIGHT, 0}},
    {"A",                "a",           RBX_ACT_ANALOG, A_A,          0, {VK_RETURN, 'Z'}},
    {"B",                "b",           RBX_ACT_ANALOG, A_B,          0, {'X', 0}},
    {"X",                "x",           RBX_ACT_ANALOG, A_X,          0, {'C', 0}},
    {"Y",                "y",           RBX_ACT_ANALOG, A_Y,          0, {'V', 0}},
    {"White",            "white",       RBX_ACT_ANALOG, A_WHITE,      0, {'Q', 0}},
    {"Black",            "black",       RBX_ACT_ANALOG, A_BLACK,      0, {'E', 0}},
    {"Left trigger",     "lt",          RBX_ACT_ANALOG, A_LT,         0, {'1', 0}},
    {"Right trigger",    "rt",          RBX_ACT_ANALOG, A_RT,         0, {'3', 0}},
    {"Left stick click", "ls",          RBX_ACT_BUTTON, B_LS,         0, {VK_SHIFT, 0}},
    {"Right stick click","rs",          RBX_ACT_BUTTON, B_RS,         0, {VK_CONTROL, 0}},
    {"Back",             "back",        RBX_ACT_BUTTON, B_BACK,       0, {VK_BACK, 0}},
    {"Start (pause)",    "start",       RBX_ACT_BUTTON, B_START,      0, {VK_SPACE, 0}},
};

void rbx_keymap_defaults(RbxKeymap *map)
{
    int i, s;
    for (i = 0; i < RBX_ACTION_COUNT; i++)
        for (s = 0; s < RBX_KEY_SLOTS; s++)
            map->vk[i][s] = rbx_actions[i].def[s];
    map->swap_ab = map->swap_xy = 0;
}

static void wide_key(const char *key, WCHAR *out, int n)
{
    MultiByteToWideChar(CP_UTF8, 0, key, -1, out, n);
}

void rbx_keymap_load(RbxKeymap *map, const WCHAR *ini)
{
    int i;
    rbx_keymap_defaults(map);
    if (!ini || !ini[0]) return;
    map->swap_ab = GetPrivateProfileIntW(L"controls", L"swap_ab", 0, ini) != 0;
    map->swap_xy = GetPrivateProfileIntW(L"controls", L"swap_xy", 0, ini) != 0;
    for (i = 0; i < RBX_ACTION_COUNT; i++) {
        WCHAR key[32], value[32];
        unsigned a = 0, b = 0;
        wide_key(rbx_actions[i].key, key, 32);
        if (!GetPrivateProfileStringW(L"keyboard", key, L"", value, 32, ini) || !value[0])
            continue;
        if (swscanf(value, L"%u,%u", &a, &b) >= 1) {
            map->vk[i][0] = (unsigned char)(a < 256 && !rbx_key_reserved((int)a) ? a : 0);
            map->vk[i][1] = (unsigned char)(b < 256 && !rbx_key_reserved((int)b) ? b : 0);
        }
    }
}

int rbx_keymap_save(const RbxKeymap *map, const WCHAR *ini)
{
    int i, ok = 1;
    if (!ini || !ini[0]) return 0;
    ok &= WritePrivateProfileStringW(L"controls", L"swap_ab", map->swap_ab ? L"1" : L"0", ini) != 0;
    ok &= WritePrivateProfileStringW(L"controls", L"swap_xy", map->swap_xy ? L"1" : L"0", ini) != 0;
    for (i = 0; i < RBX_ACTION_COUNT; i++) {
        WCHAR key[32], value[32];
        wide_key(rbx_actions[i].key, key, 32);
        swprintf(value, 32, L"%u,%u", (unsigned)map->vk[i][0], (unsigned)map->vk[i][1]);
        ok &= WritePrivateProfileStringW(L"keyboard", key, value, ini) != 0;
    }
    return ok;
}

void rbx_keymap_bind(RbxKeymap *map, int action, int slot, int vk)
{
    int i, s;
    if (action < 0 || action >= RBX_ACTION_COUNT || slot < 0 || slot >= RBX_KEY_SLOTS) return;
    if (vk < 0 || vk > 255 || rbx_key_reserved(vk)) return;
    if (vk)
        for (i = 0; i < RBX_ACTION_COUNT; i++)
            for (s = 0; s < RBX_KEY_SLOTS; s++)
                if (map->vk[i][s] == vk) map->vk[i][s] = 0;
    map->vk[action][slot] = (unsigned char)vk;
}

int rbx_key_reserved(int vk)
{
    return vk == VK_ESCAPE || vk == VK_F1 || vk == VK_F9 || vk == VK_F11 || vk == VK_DELETE ||
           vk == VK_LWIN || vk == VK_RWIN;
}

void rbx_key_name(int vk, char *out, size_t n)
{
    static const struct { int vk; const char *name; } names[] = {
        {VK_RETURN, "Enter"}, {VK_SPACE, "Space"}, {VK_BACK, "Backspace"}, {VK_TAB, "Tab"},
        {VK_SHIFT, "Shift"}, {VK_CONTROL, "Ctrl"}, {VK_MENU, "Alt"},
        {VK_LSHIFT, "L Shift"}, {VK_RSHIFT, "R Shift"}, {VK_LCONTROL, "L Ctrl"}, {VK_RCONTROL, "R Ctrl"},
        {VK_LMENU, "L Alt"}, {VK_RMENU, "R Alt"}, {VK_CAPITAL, "Caps Lock"}, {VK_PAUSE, "Pause"},
        {VK_UP, "Up"}, {VK_DOWN, "Down"}, {VK_LEFT, "Left"}, {VK_RIGHT, "Right"},
        {VK_PRIOR, "Page Up"}, {VK_NEXT, "Page Down"}, {VK_HOME, "Home"}, {VK_END, "End"},
        {VK_INSERT, "Insert"}, {VK_MULTIPLY, "Num *"}, {VK_ADD, "Num +"}, {VK_SUBTRACT, "Num -"},
        {VK_DECIMAL, "Num ."}, {VK_DIVIDE, "Num /"}, {VK_OEM_1, ";"}, {VK_OEM_PLUS, "="},
        {VK_OEM_COMMA, ","}, {VK_OEM_MINUS, "-"}, {VK_OEM_PERIOD, "."}, {VK_OEM_2, "/"},
        {VK_OEM_3, "`"}, {VK_OEM_4, "["}, {VK_OEM_5, "\\"}, {VK_OEM_6, "]"}, {VK_OEM_7, "'"},
    };
    size_t i;
    if (!n) return;
    if (!vk) { snprintf(out, n, "-"); return; }
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) { snprintf(out, n, "%c", vk); return; }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) { snprintf(out, n, "Num %d", vk - VK_NUMPAD0); return; }
    if (vk >= VK_F1 && vk <= VK_F24) { snprintf(out, n, "F%d", vk - VK_F1 + 1); return; }
    for (i = 0; i < sizeof names / sizeof names[0]; i++)
        if (names[i].vk == vk) { snprintf(out, n, "%s", names[i].name); return; }
    snprintf(out, n, "Key %02X", vk);
}
