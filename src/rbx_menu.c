/* rbx_menu.c -- see rbx_menu.h. Layout units are a 960 x 640 window; every
 * size is scaled to fit the real one. */
#include "rbx_menu.h"
#include "rbx_ui.h"

#define BASE_W 960
#define BASE_H 640
#define ROW_H 34

static const char *const KEYS_KB[] = {
    "\xE2\x86\x91 \xE2\x86\x93", "Select", "\xE2\x86\x90 \xE2\x86\x92", "Change", "ENTER", "Choose", "F1 / ESC", "Back to game",
};
static const char *const KEYS_PAD[] = {
    "D-PAD", "Select", "\xE2\x86\x90 \xE2\x86\x92", "Change", "A", "Choose", "B / START", "Back to game",
};

void rbx_menu_draw_settings(HDC dc, int w, int h, const unsigned *choice, int selected,
                            int pad, RECT *panel, int *cut)
{
    RbxFonts f;
    RECT p, band, sec, row, b0, b1, note, keys;
    int i, x0, x1, y, pw, ph;

    rbx_fonts_make(&f, rbx_fit(w, h, BASE_W, BASE_H));
    pw = rbx_px(&f, 720);
    ph = rbx_px(&f, 604);
    SetRect(&p, (w - pw) / 2, (h - ph) / 2, (w - pw) / 2 + pw, (h - ph) / 2 + ph);
    *panel = p;
    *cut = rbx_px(&f, 26);
    rbx_panel(dc, &f, &p, *cut);

    x0 = p.left + rbx_px(&f, 34);
    x1 = p.right - rbx_px(&f, 34);
    SetRect(&band, x0, p.top + rbx_px(&f, 18), x1, p.top + rbx_px(&f, 80));
    rbx_header(dc, &f, &band, "SETTINGS", "VF-1 SYSTEMS  //  VIDEO", "RDF FIELD MANUAL");

    y = p.top + rbx_px(&f, 94);
    for (i = 0; i < RBX_OPT_COUNT; i++) {
        if (i == 0 || i == RBX_OPT_DISPLAY) {
            SetRect(&sec, x0, y, x1, y + rbx_px(&f, 30));
            rbx_section(dc, &f, &sec, i ? 2 : 1, i ? "GRAPHICS" : "DISPLAY");
            y += rbx_px(&f, 32);
        }
        SetRect(&row, x0, y, x1, y + rbx_px(&f, ROW_H - 2));
        rbx_option(dc, &f, &row, rbx_options[i].label, rbx_options[i].values[choice[i]],
                   choice[i], rbx_options[i].count, i == selected, 0, NULL, NULL);
        y += rbx_px(&f, ROW_H);
    }

    y += rbx_px(&f, 14);
    SetRect(&b0, x0, y, (x0 + x1) / 2 - rbx_px(&f, 8), y + rbx_px(&f, 40));
    SetRect(&b1, (x0 + x1) / 2 + rbx_px(&f, 8), y, x1, y + rbx_px(&f, 40));
    rbx_button(dc, &f, &b0, "RESUME", RBX_BTN_PRIMARY, selected == RBX_ROW_RESUME, 0);
    rbx_button(dc, &f, &b1, "QUIT GAME", RBX_BTN_DANGER, selected == RBX_ROW_QUIT, 0);

    y = b0.bottom + rbx_px(&f, 12);
    SetRect(&note, x0, y, x1, y + rbx_px(&f, 30));
    rbx_note(dc, &f, &note, "NOTE",
             selected < RBX_OPT_COUNT ? rbx_options[selected].note
             : selected == RBX_ROW_RESUME ? "Close the menu and return to the battle. Settings are already saved."
                                          : "Leave the game. Settings are already saved.");

    SetRect(&keys, x0, note.bottom + rbx_px(&f, 6), x1, p.bottom - rbx_px(&f, 8));
    rbx_keyhints(dc, &f, &keys, pad ? KEYS_PAD : KEYS_KB, 4);
    rbx_fonts_free(&f);
}

void rbx_menu_draw_quit(HDC dc, int w, int h, int yes, int pad, RECT *panel, int *cut)
{
    static const char *const kb[] = { "\xE2\x86\x90 \xE2\x86\x92", "Select", "ENTER", "Choose", "ESC", "Keep playing" };
    static const char *const gp[] = { "\xE2\x86\x90 \xE2\x86\x92", "Select", "A", "Choose", "B", "Keep playing" };
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
