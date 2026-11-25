/* rbx_menu.h -- the F1 settings page and the Esc quit prompt, drawn with
 * rbx_ui into a DC the size of the game window. rbx_settings.c owns the
 * window and the input; these only draw. *panel and *cut return the panel's
 * shape so the caller can keep it solid and dim the game around it. */
#ifndef RBX_MENU_H
#define RBX_MENU_H

#include <windows.h>
#include "rbx_options.h"

/* Rows of the settings page: the options, then the two buttons. */
enum { RBX_ROW_RESUME = RBX_OPT_COUNT, RBX_ROW_QUIT, RBX_ROW_COUNT };

void rbx_menu_draw_settings(HDC dc, int w, int h, const unsigned *choice, int selected,
                            int pad, RECT *panel, int *cut);
void rbx_menu_draw_quit(HDC dc, int w, int h, int yes, int pad, RECT *panel, int *cut);

#endif
