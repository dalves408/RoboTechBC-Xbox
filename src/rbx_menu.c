/* rbx_menu.c -- see rbx_menu.h. Layout units are a 960 x 640 window; every
 * size is scaled to fit the real one. */
#include <stdio.h>

#include "rbx_menu.h"
#include "rbx_ui.h"

#define BASE_W 960
#define BASE_H 640
#define ROW_H 32
#define VIDEO_ROW_H 30   /* ten video rows, two section headers and the buttons above the note */
#define KEY_H 22

const char *const rbx_page_names[RBX_PAGE_COUNT] = { "VIDEO", "CONTROLS" };
const char *const rbx_swap_values[2] = { "Normal", "Swapped" };
const char *const rbx_swap_labels[2] = { "Swap A and B", "Swap X and Y" };
const char *const rbx_swap_notes[2] = {
    "For controllers with A and B the other way round (Nintendo layout). Keyboard keys are not swapped.",
    "For controllers with X and Y the other way round (Nintendo layout). Keyboard keys are not swapped.",
};

#define UP_DOWN "\xE2\x86\x91 \xE2\x86\x93"
#define LEFT_RIGHT "\xE2\x86\x90 \xE2\x86\x92"

int rbx_grid_action(int grid_row, int col)
{
    return (col < 2 ? 0 : RBX_GRID_ROWS) + grid_row;
}

static void hints(HDC dc, const RbxFonts *f, const RECT *r, const RbxMenuState *s)
{
    static const char *const kb[] = { UP_DOWN, "Select", LEFT_RIGHT, "Change", "ENTER", "Choose",
                                      "TAB", "Page", "F1 / ESC", "Back to game" };
    static const char *const kb_keys[] = { "ENTER", "Change key", "DEL", "Clear", "TAB", "Page",
                                           "F1 / ESC", "Back to game" };
    static const char *const kb_listen[] = { "ANY KEY", "Bind it", "ESC", "Cancel", "DEL", "Clear" };
    static const char *const pad[] = { "D-PAD", "Select", LEFT_RIGHT, "Change", "A", "Choose",
                                       "WHITE / BLACK", "Page", "B / START", "Back" };
    if (s->listening)
        rbx_keyhints(dc, f, r, kb_listen, 3);
    else if (s->pad)
        rbx_keyhints(dc, f, r, pad, 5);
    else if (s->page == RBX_PAGE_CONTROLS && s->row >= RBX_CROW_GRID && s->row < RBX_CROW_BUTTONS)
        rbx_keyhints(dc, f, r, kb_keys, 4);
    else
        rbx_keyhints(dc, f, r, kb, 5);
}

static void video_page(HDC dc, const RbxFonts *f, const RbxMenuState *s, int x0, int x1, int y,
                       const char **note)
{
    RECT sec, row, b0, b1;
    int i;
    for (i = 0; i < RBX_OPT_COUNT; i++) {
        if (i == 0 || i == RBX_OPT_DISPLAY) {
            SetRect(&sec, x0, y, x1, y + rbx_px(f, 30));
            rbx_section(dc, f, &sec, i ? 2 : 1, i ? "GRAPHICS" : "DISPLAY");
            y += rbx_px(f, 32);
        }
        SetRect(&row, x0, y, x1, y + rbx_px(f, VIDEO_ROW_H - 2));
        rbx_option(dc, f, &row, rbx_options[i].label, rbx_options[i].values[s->choice[i]],
                   s->choice[i], rbx_options[i].count, i == s->row, 0, NULL, NULL);
        y += rbx_px(f, VIDEO_ROW_H);
    }
    y += rbx_px(f, 10);
    SetRect(&b0, x0, y, (x0 + x1) / 2 - rbx_px(f, 8), y + rbx_px(f, 38));
    SetRect(&b1, (x0 + x1) / 2 + rbx_px(f, 8), y, x1, y + rbx_px(f, 38));
    rbx_button(dc, f, &b0, "RESUME", RBX_BTN_PRIMARY, s->row == RBX_ROW_RESUME, 0);
    rbx_button(dc, f, &b1, "QUIT GAME", RBX_BTN_DANGER, s->row == RBX_ROW_QUIT, 0);
    *note = s->row < 0 ? "Left and right switch pages. Changes apply at once and are saved."
          : s->row < RBX_OPT_COUNT ? rbx_options[s->row].note
          : s->row == RBX_ROW_RESUME ? "Close the menu and return to the battle. Settings are already saved."
                                     : "Leave the game. Settings are already saved.";
}

static void controls_page(HDC dc, const RbxFonts *f, const RbxMenuState *s, int x0, int x1, int y,
                          const char **note)
{
    RECT sec, row, slots[RBX_KEY_SLOTS], b[RBX_CBTN_COUNT];
    int i, c, gap = rbx_px(f, 24), colw = (x1 - x0 - gap) / 2;
    static const char *const btn[RBX_CBTN_COUNT] = { "DEFAULT KEYS", "RESUME", "QUIT GAME" };
    char name[24];
    RbxFonts narrow = *f;
    narrow.value_pct = 50;
    f = &narrow;

    SetRect(&sec, x0, y, x1, y + rbx_px(f, 30));
    rbx_section(dc, f, &sec, 1, "CONTROLLER");
    y += rbx_px(f, 32);
    /* The two swaps side by side, read left to right. */
    for (i = 0; i < 2; i++) {
        unsigned v = i ? s->map->swap_xy : s->map->swap_ab;
        int cx = x0 + i * (colw + gap);
        SetRect(&row, cx, y, cx + colw, y + rbx_px(f, ROW_H - 2));
        rbx_option(dc, f, &row, rbx_swap_labels[i], rbx_swap_values[v], v, 2,
                   s->row == RBX_CROW_SWAP_AB + i, 0, NULL, NULL);
    }
    y += rbx_px(f, ROW_H + 4);
    SetRect(&sec, x0, y, x1, y + rbx_px(f, 30));
    rbx_section(dc, f, &sec, 2, "KEYBOARD");
    y += rbx_px(f, 32);
    for (i = 0; i < RBX_GRID_ROWS; i++) {
        for (c = 0; c < 2; c++) {
            int a = rbx_grid_action(i, c * 2), sel = s->row == RBX_CROW_GRID + i && s->col / 2 == c, k;
            int cx = x0 + c * (colw + gap);
            SetRect(&row, cx, y, cx + colw, y + rbx_px(f, KEY_H - 1));
            rbx_keyrow(dc, f, &row, rbx_actions[a].label, sel, 0, RBX_KEY_SLOTS, slots);
            for (k = 0; k < RBX_KEY_SLOTS; k++) {
                int here = sel && s->col % 2 == k;
                rbx_key_name(s->map->vk[a][k], name, sizeof name);
                rbx_keyslot(dc, f, &slots[k], name, here, 0, here && s->listening);
            }
        }
        y += rbx_px(f, KEY_H);
    }
    y += rbx_px(f, 10);
    {
        int bw = (x1 - x0 - 2 * rbx_px(f, 12)) / 3;
        for (i = 0; i < RBX_CBTN_COUNT; i++) {
            int bx = x0 + i * (bw + rbx_px(f, 12));
            SetRect(&b[i], bx, y, bx + bw, y + rbx_px(f, 38));
            rbx_button(dc, f, &b[i], btn[i], i == RBX_CBTN_RESUME ? RBX_BTN_PRIMARY
                                             : i == RBX_CBTN_QUIT ? RBX_BTN_DANGER : RBX_BTN_NORMAL,
                       s->row == RBX_CROW_BUTTONS && s->col == i, 0);
        }
    }
    if (s->row < 0)
        *note = "Left and right switch pages. Changes apply at once and are saved.";
    else if (s->row <= RBX_CROW_SWAP_XY)
        *note = rbx_swap_notes[s->row];
    else if (s->row < RBX_CROW_BUTTONS)
        *note = s->listening ? "Press the key for this control. Esc cancels, Delete clears the slot."
                             : "Keys drive controller 1. A key can only do one thing: binding it moves it here.";
    else
        *note = s->col == RBX_CBTN_DEFAULTS ? "Put every key back to the default layout (the swaps stay)."
              : s->col == RBX_CBTN_RESUME ? "Close the menu and return to the battle. Settings are already saved."
                                          : "Leave the game. Settings are already saved.";
}

void rbx_menu_draw_settings(HDC dc, int w, int h, const RbxMenuState *s, RECT *panel, int *cut)
{
    RbxFonts f;
    RECT p, band, tabs, note, keys;
    int x0, x1, pw, ph;
    const char *text = "";

    rbx_fonts_make(&f, rbx_fit(w, h, BASE_W, BASE_H));
    pw = rbx_px(&f, 740);
    ph = rbx_px(&f, 624);
    SetRect(&p, (w - pw) / 2, (h - ph) / 2, (w - pw) / 2 + pw, (h - ph) / 2 + ph);
    *panel = p;
    *cut = rbx_px(&f, 26);
    rbx_panel(dc, &f, &p, *cut);

    x0 = p.left + rbx_px(&f, 34);
    x1 = p.right - rbx_px(&f, 34);
    SetRect(&band, x0, p.top + rbx_px(&f, 16), x1, p.top + rbx_px(&f, 74));
    rbx_header(dc, &f, &band, "SETTINGS", s->page == RBX_PAGE_VIDEO ? "VF-1 SYSTEMS  //  VIDEO"
                                                                     : "VF-1 SYSTEMS  //  CONTROLS",
               "RDF FIELD MANUAL");
    SetRect(&tabs, x0, band.bottom + rbx_px(&f, 12), x1, band.bottom + rbx_px(&f, 40));
    rbx_tabs(dc, &f, &tabs, rbx_page_names, RBX_PAGE_COUNT, s->page, s->row < 0, NULL);

    if (s->page == RBX_PAGE_VIDEO)
        video_page(dc, &f, s, x0, x1, tabs.bottom + rbx_px(&f, 6), &text);
    else
        controls_page(dc, &f, s, x0, x1, tabs.bottom + rbx_px(&f, 6), &text);

    SetRect(&note, x0, p.bottom - rbx_px(&f, 72), x1, p.bottom - rbx_px(&f, 42));
    rbx_note(dc, &f, &note, s->status ? "INFO" : "NOTE", s->status ? s->status : text);
    SetRect(&keys, x0, note.bottom + rbx_px(&f, 4), x1, p.bottom - rbx_px(&f, 8));
    hints(dc, &f, &keys, s);
    rbx_fonts_free(&f);
}

void rbx_menu_draw_quit(HDC dc, int w, int h, int yes, int pad, RECT *panel, int *cut)
{
    static const char *const kb[] = { LEFT_RIGHT, "Select", "ENTER", "Choose", "ESC", "Keep playing" };
    static const char *const gp[] = { LEFT_RIGHT, "Select", "A", "Choose", "B", "Keep playing" };
    RbxFonts f;
    RECT p, band, msg, b0, b1, keys;
    int pw, ph, x0, x1;

    rbx_fonts_make(&f, rbx_fit(w, h, BASE_W, BASE_H));
    pw = rbx_px(&f, 500);
    ph = rbx_px(&f, 250);
    SetRect(&p, (w - pw) / 2, (h - ph) / 2, (w - pw) / 2 + pw, (h - ph) / 2 + ph);
    *panel = p;
    *cut = rbx_px(&f, 22);
    rbx_panel(dc, &f, &p, *cut);
    x0 = p.left + rbx_px(&f, 30);
    x1 = p.right - rbx_px(&f, 30);
    SetRect(&band, x0, p.top + rbx_px(&f, 16), x1, p.top + rbx_px(&f, 76));
    rbx_header(dc, &f, &band, "QUIT GAME", "RETURN TO WINDOWS", NULL);
    SetRect(&msg, x0, band.bottom + rbx_px(&f, 12), x1, band.bottom + rbx_px(&f, 52));
    rbx_text(dc, f.body, RGB(238, 243, 250), &msg, "Do you want to quit Robotech: Battlecry?",
             DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SetRect(&b0, x0, msg.bottom + rbx_px(&f, 8), (x0 + x1) / 2 - rbx_px(&f, 8), msg.bottom + rbx_px(&f, 48));
    SetRect(&b1, (x0 + x1) / 2 + rbx_px(&f, 8), b0.top, x1, b0.bottom);
    rbx_button(dc, &f, &b0, "KEEP PLAYING", RBX_BTN_PRIMARY, !yes, 0);
    rbx_button(dc, &f, &b1, "QUIT GAME", RBX_BTN_DANGER, yes, 0);
    SetRect(&keys, x0, b0.bottom + rbx_px(&f, 8), x1, p.bottom - rbx_px(&f, 8));
    rbx_keyhints(dc, &f, &keys, pad ? gp : kb, 3);
    rbx_fonts_free(&f);
}
