/* The keyboard map and the controller button swaps, shared by the game's F1
 * menu (rbx_settings.c) and Config.exe. Stored in settings.ini beside the
 * executable:
 *
 *   [controls]  swap_ab=0|1  swap_xy=0|1        (controllers only)
 *   [keyboard]  <action>=<vk>,<vk>              (Windows virtual-key codes, 0 = none)
 *
 * Each action is one control of the Xbox pad; the keyboard drives pad 1. */
#ifndef RBX_KEYMAP_H
#define RBX_KEYMAP_H

#include <windows.h>
#include <stddef.h>

#define RBX_KEY_SLOTS 2

enum { RBX_ACT_BUTTON, RBX_ACT_ANALOG, RBX_ACT_AXIS };
enum { RBX_AXIS_LX, RBX_AXIS_LY, RBX_AXIS_RX, RBX_AXIS_RY };

typedef struct {
    const char *label, *key;
    unsigned char kind;              /* RBX_ACT_* */
    unsigned short target;           /* wButtons bit, bAnalogButtons index or RBX_AXIS_* */
    signed char sign;                /* RBX_ACT_AXIS: -1 or +1 */
    unsigned char def[RBX_KEY_SLOTS];
} RbxAction;

enum { RBX_ACTION_COUNT = 24 };
extern const RbxAction rbx_actions[RBX_ACTION_COUNT];

typedef struct {
    unsigned char vk[RBX_ACTION_COUNT][RBX_KEY_SLOTS];
    unsigned char swap_ab, swap_xy;
} RbxKeymap;

void rbx_keymap_defaults(RbxKeymap *map);
/* Missing keys keep their defaults. */
void rbx_keymap_load(RbxKeymap *map, const WCHAR *ini);
int  rbx_keymap_save(const RbxKeymap *map, const WCHAR *ini);
/* Bind vk to (action, slot); the key is taken off any other action first,
 * so one key never drives two controls. vk 0 clears the slot. */
void rbx_keymap_bind(RbxKeymap *map, int action, int slot, int vk);
/* Keys the menus and the window keep for themselves: Esc, F1, F9, F11, Delete. */
int  rbx_key_reserved(int vk);
/* "Enter", "Num 8", "Left Shift"... or "-" for 0. */
void rbx_key_name(int vk, char *out, size_t n);

#endif
