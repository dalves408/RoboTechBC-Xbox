/* rbx_menu.h -- the F1 settings pages (VIDEO, CONTROLS) and the Esc quit
 * prompt, drawn with rbx_ui into a DC the size of the game window.
 * rbx_settings.c owns the window and the input; these only draw. *panel and
 * *cut return the panel's shape so the caller can keep it solid and dim the
 * game around it. */
#ifndef RBX_MENU_H
#define RBX_MENU_H

#include <windows.h>
#include "rbx_options.h"
#include "rbx_keymap.h"

enum { RBX_PAGE_VIDEO, RBX_PAGE_CONTROLS, RBX_PAGE_COUNT };

/* VIDEO rows: the options, then the two buttons. */
enum { RBX_ROW_RESUME = RBX_OPT_COUNT, RBX_ROW_QUIT, RBX_ROW_COUNT };

/* CONTROLS rows: the two swaps, the key grid (two columns of
 * RBX_GRID_ROWS actions; col 0-1 are the left column's key slots, 2-3 the
 * right's), then the button row (col 0 DEFAULT KEYS, 1 RESUME, 2 QUIT). */
#define RBX_GRID_ROWS (RBX_ACTION_COUNT / 2)
enum { RBX_CROW_SWAP_AB, RBX_CROW_SWAP_XY, RBX_CROW_GRID,
       RBX_CROW_BUTTONS = RBX_CROW_GRID + RBX_GRID_ROWS, RBX_CROW_COUNT };
enum { RBX_CBTN_DEFAULTS, RBX_CBTN_RESUME, RBX_CBTN_QUIT, RBX_CBTN_COUNT };

typedef struct {
    int page;                   /* RBX_PAGE_* */
    int row;                    /* a row of the page, or -1 for the tabs */
    int col;                    /* CONTROLS: the grid slot (0-3) or the button (0-2) */
    int listening;              /* the selected key slot waits for a key */
    int pad;                    /* key hints for a controller */
    const unsigned *choice;     /* VIDEO values */
    const RbxKeymap *map;
    const char *status;         /* overrides the note when set */
} RbxMenuState;

/* The action and slot behind a grid row and column. */
int rbx_grid_action(int grid_row, int col);

void rbx_menu_draw_settings(HDC dc, int w, int h, const RbxMenuState *s, RECT *panel, int *cut);
void rbx_menu_draw_quit(HDC dc, int w, int h, int yes, int pad, RECT *panel, int *cut);

/* Labels shared with Config.exe. */
extern const char *const rbx_page_names[RBX_PAGE_COUNT];
extern const char *const rbx_swap_values[2];
extern const char *const rbx_swap_labels[2];
extern const char *const rbx_swap_notes[2];

#endif
