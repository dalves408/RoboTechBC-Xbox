/* rbx_ui.h -- GDI drawing in the look of the Robotech: Battlecry field manual
 * (RDF tactical display: deep navy panels on a faint plotting grid, chamfered
 * corners with targeting brackets, amber section tabs and highlights, sky
 * blue readouts, red for the way out). Shared by Config.exe and the in-game
 * F1/Esc overlay, so both look the same. Plain GDI: no extra libraries. */
#ifndef RBX_UI_H
#define RBX_UI_H

#include <windows.h>

typedef struct {
    float s;                        /* scale: 1.0 at the 960 x 640 base layout */
    HFONT title, head, body, fine, key, tag;
    int value_pct;                  /* rbx_option: the readout's share of the row (46) */
} RbxFonts;

void rbx_fonts_make(RbxFonts *f, float scale);
void rbx_fonts_free(RbxFonts *f);
int  rbx_px(const RbxFonts *f, int v);

/* The scale that fits a base_w x base_h layout into w x h. */
float rbx_fit(int w, int h, int base_w, int base_h);

void rbx_text(HDC dc, HFONT font, COLORREF color, const RECT *r, const char *s, UINT fmt);
int  rbx_text_w(HDC dc, HFONT font, const char *s);

/* The whole-window background (Config.exe): navy gradient and plotting grid. */
void rbx_backdrop(HDC dc, const RECT *r);
/* A chamfered panel: top-left and bottom-right corners cut by c pixels, the
 * other two corners marked with amber targeting brackets. */
void rbx_panel(HDC dc, const RbxFonts *f, const RECT *r, int c);
/* 1 when (x, y) is inside rbx_panel(r, c)'s shape. */
int  rbx_in_panel(const RECT *r, int c, int x, int y);

/* The title band: an amber bar, the title, a spaced subtitle under it, and a
 * small boxed tag with hazard stripes at the right (tag may be NULL). */
void rbx_header(HDC dc, const RbxFonts *f, const RECT *band, const char *title,
                const char *subtitle, const char *tag);
/* A numbered section heading: "01  DISPLAY ------". */
void rbx_section(HDC dc, const RbxFonts *f, const RECT *row, int number, const char *text);
/* A setting: label at the left, "<  value  >" at the right with one pip per
 * value. left/right get the arrows' rectangles (for the mouse). */
void rbx_option(HDC dc, const RbxFonts *f, const RECT *row, const char *label, const char *value,
                unsigned index, unsigned count, int selected, int hover, RECT *left, RECT *right);
enum { RBX_BTN_NORMAL, RBX_BTN_PRIMARY, RBX_BTN_DANGER };
void rbx_button(HDC dc, const RbxFonts *f, const RECT *r, const char *text, int style,
                int selected, int hover);
/* A "NOTE" strip: the field manual's call-out box. */
void rbx_note(HDC dc, const RbxFonts *f, const RECT *r, const char *tag, const char *text);
/* Page tabs in a row: the active one lit amber (outlined when focused).
 * rects (may be NULL) gets each tab's rectangle. */
void rbx_tabs(HDC dc, const RbxFonts *f, const RECT *r, const char *const *names, int n, int active,
              int focused, RECT *rects);
/* A key slot: the key's name in a keycap, "Press a key" while listening. */
void rbx_keyslot(HDC dc, const RbxFonts *f, const RECT *r, const char *name, int selected, int hover,
                 int listening);
/* A key binding row: label, then the slots' rectangles in slots[] (the
 * caller draws them with rbx_keyslot). */
void rbx_keyrow(HDC dc, const RbxFonts *f, const RECT *row, const char *label, int selected, int hover,
                int nslots, RECT *slots);
/* Key hints: pairs of {keycap, meaning}, drawn left to right. */
void rbx_keyhints(HDC dc, const RbxFonts *f, const RECT *r, const char *const *pairs, int npairs);

#endif
