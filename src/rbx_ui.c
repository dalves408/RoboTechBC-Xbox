/* rbx_ui.c -- see rbx_ui.h. */
#include <stdio.h>
#include <string.h>

#include "rbx_ui.h"

/* The palette. */
#define C_BG_TOP    RGB(16, 30, 54)
#define C_BG_BOT    RGB(4, 9, 20)
#define C_GRID      RGB(22, 38, 64)
#define C_GRID_HI   RGB(30, 52, 86)
#define C_PANEL_TOP RGB(14, 27, 49)
#define C_PANEL_BOT RGB(7, 14, 28)
#define C_EDGE      RGB(60, 98, 146)
#define C_FRAME     RGB(150, 196, 240)   /* the game's own pale-blue panel rim */
#define C_ROW_LIT   RGB(30, 47, 74)
#define C_ROW_HOVER RGB(20, 34, 56)
#define C_BOX       RGB(9, 18, 34)
#define C_AMBER     RGB(255, 168, 38)
#define C_AMBER_DK  RGB(122, 76, 14)
#define C_SKY       RGB(110, 190, 255)
#define C_WHITE     RGB(238, 243, 250)
#define C_TEXT      RGB(196, 208, 226)
#define C_DIM       RGB(116, 138, 168)
#define C_RED       RGB(222, 62, 50)
#define C_RED_DK    RGB(92, 22, 20)

int rbx_px(const RbxFonts *f, int v) { return (int)(v * f->s + 0.5f); }

float rbx_fit(int w, int h, int base_w, int base_h)
{
    float sx = (float)w / (float)base_w, sy = (float)h / (float)base_h;
    return sx < sy ? sx : sy;
}

static int CALLBACK has_face(const LOGFONTA *lf, const TEXTMETRICA *tm, DWORD type, LPARAM found)
{
    (void)lf; (void)tm; (void)type;
    *(int *)found = 1;
    return 0;
}

static const char *face(void)
{
    static const char *pick;
    if (!pick) {
        LOGFONTA lf;
        HDC dc = GetDC(NULL);
        int found = 0;
        memset(&lf, 0, sizeof lf);
        lf.lfCharSet = DEFAULT_CHARSET;
        strcpy(lf.lfFaceName, "Bahnschrift");  /* Windows 10 and later: a condensed, technical face */
        EnumFontFamiliesExA(dc, &lf, has_face, (LPARAM)&found, 0);
        ReleaseDC(NULL, dc);
        pick = found ? "Bahnschrift" : "Segoe UI";
    }
    return pick;
}

static HFONT make_font(int px, int weight)
{
    return CreateFontA(-px, 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, 0, face());
}

void rbx_fonts_make(RbxFonts *f, float scale)
{
    if (scale < 0.5f) scale = 0.5f;
    f->s = scale;
    f->value_pct = 46;
    f->title = make_font(rbx_px(f, 30), FW_BOLD);
    f->head = make_font(rbx_px(f, 16), FW_BOLD);
    f->body = make_font(rbx_px(f, 16), FW_NORMAL);
    f->fine = make_font(rbx_px(f, 13), FW_NORMAL);
    f->key = make_font(rbx_px(f, 14), FW_SEMIBOLD);
    f->tag = make_font(rbx_px(f, 11), FW_BOLD);
}

void rbx_fonts_free(RbxFonts *f)
{
    DeleteObject(f->title); DeleteObject(f->head); DeleteObject(f->body);
    DeleteObject(f->fine); DeleteObject(f->key); DeleteObject(f->tag);
    memset(f, 0, sizeof *f);
}

/* Text is UTF-8 (the key hints use arrows). */
static int widen(const char *s, WCHAR *out, int n)
{
    int len = MultiByteToWideChar(CP_UTF8, 0, s, -1, out, n);
    if (len <= 0) { out[0] = 0; return 0; }
    return len - 1;
}

void rbx_text(HDC dc, HFONT font, COLORREF color, const RECT *r, const char *s, UINT fmt)
{
    RECT t = *r;
    WCHAR w[256];
    int n = widen(s, w, 256);
    HGDIOBJ old = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    DrawTextW(dc, w, n, &t, fmt | DT_NOPREFIX);
    SelectObject(dc, old);
}

int rbx_text_w(HDC dc, HFONT font, const char *s)
{
    SIZE sz = {0, 0};
    WCHAR w[256];
    int n = widen(s, w, 256);
    HGDIOBJ old = SelectObject(dc, font);
    GetTextExtentPoint32W(dc, w, n, &sz);
    SelectObject(dc, old);
    return sz.cx;
}

/* Letter-spaced capitals, like the manual's stencilled headings. */
static void spaced(HDC dc, HFONT font, COLORREF color, int x, int y, const char *s, int extra)
{
    WCHAR w[128];
    int n = widen(s, w, 128);
    HGDIOBJ old = SelectObject(dc, font);
    int prev = SetTextCharacterExtra(dc, extra);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    TextOutW(dc, x, y, w, n);
    SetTextCharacterExtra(dc, prev);
    SelectObject(dc, old);
}

static int spaced_w(HDC dc, HFONT font, const char *s, int extra)
{
    WCHAR w[128];
    return rbx_text_w(dc, font, s) + extra * widen(s, w, 128);
}

static COLORREF mix(COLORREF a, COLORREF b, int t, int n)
{
    if (n <= 0) return a;
    return RGB(GetRValue(a) + (GetRValue(b) - GetRValue(a)) * t / n,
               GetGValue(a) + (GetGValue(b) - GetGValue(a)) * t / n,
               GetBValue(a) + (GetBValue(b) - GetBValue(a)) * t / n);
}

static void fill(HDC dc, int l, int t, int r, int b, COLORREF c)
{
    RECT rc;
    HBRUSH br = CreateSolidBrush(c);
    SetRect(&rc, l, t, r, b);
    FillRect(dc, &rc, br);
    DeleteObject(br);
}

static void line(HDC dc, int x0, int y0, int x1, int y1, COLORREF c, int width)
{
    HPEN pen = CreatePen(PS_SOLID, width, c);
    HGDIOBJ old = SelectObject(dc, pen);
    MoveToEx(dc, x0, y0, NULL);
    LineTo(dc, x1, y1);
    SelectObject(dc, old);
    DeleteObject(pen);
}

static void gradient(HDC dc, const RECT *r, COLORREF top, COLORREF bottom)
{
    int h = r->bottom - r->top, y;
    for (y = 0; y < h; y += 2)
        fill(dc, r->left, r->top + y, r->right, r->top + y + 2, mix(top, bottom, y, h));
}

/* The plotting grid: thin lines every `step`, a brighter one every fourth. */
static void grid(HDC dc, const RECT *r, int step)
{
    int x, y, i;
    if (step < 6) step = 6;
    for (i = 0, x = r->left + step; x < r->right; x += step, i++)
        line(dc, x, r->top, x, r->bottom, (i % 4 == 3) ? C_GRID_HI : C_GRID, 1);
    for (i = 0, y = r->top + step; y < r->bottom; y += step, i++)
        line(dc, r->left, y, r->right, y, (i % 4 == 3) ? C_GRID_HI : C_GRID, 1);
}

void rbx_backdrop(HDC dc, const RECT *r)
{
    gradient(dc, r, C_BG_TOP, C_BG_BOT);
    grid(dc, r, (r->bottom - r->top) / 20);
}

static void chamfer_points(const RECT *r, int c, POINT p[6])
{
    p[0].x = r->left + c;      p[0].y = r->top;
    p[1].x = r->right - 1;     p[1].y = r->top;
    p[2].x = r->right - 1;     p[2].y = r->bottom - 1 - c;
    p[3].x = r->right - 1 - c; p[3].y = r->bottom - 1;
    p[4].x = r->left;          p[4].y = r->bottom - 1;
    p[5].x = r->left;          p[5].y = r->top + c;
}

static void chamfer(HDC dc, const RECT *r, int c, COLORREF fill_color, COLORREF border)
{
    POINT p[6];
    HBRUSH br = CreateSolidBrush(fill_color);
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ ob = SelectObject(dc, br), op = SelectObject(dc, pen);
    chamfer_points(r, c, p);
    Polygon(dc, p, 6);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(br);
    DeleteObject(pen);
}

int rbx_in_panel(const RECT *r, int c, int x, int y)
{
    if (x < r->left || x >= r->right || y < r->top || y >= r->bottom) return 0;
    if ((x - r->left) + (y - r->top) < c) return 0;                 /* top-left cut */
    if ((r->right - 1 - x) + (r->bottom - 1 - y) < c) return 0;     /* bottom-right cut */
    return 1;
}

/* An L-shaped targeting bracket at a corner; dx/dy point into the panel. */
static void bracket(HDC dc, int x, int y, int dx, int dy, int len, int width, COLORREF c)
{
    line(dc, x, y, x + dx * len, y, c, width);
    line(dc, x, y, x, y + dy * len, c, width);
}

void rbx_panel(HDC dc, const RbxFonts *f, const RECT *r, int c)
{
    POINT p[6];
    HRGN rgn;
    int inset = rbx_px(f, 5), len = rbx_px(f, 18), w = rbx_px(f, 2) > 1 ? rbx_px(f, 2) : 1;
    chamfer_points(r, c, p);
    rgn = CreatePolygonRgn(p, 6, WINDING);
    SelectClipRgn(dc, rgn);
    gradient(dc, r, C_PANEL_TOP, C_PANEL_BOT);
    grid(dc, r, rbx_px(f, 24));
    SelectClipRgn(dc, NULL);
    DeleteObject(rgn);
    {
        HPEN pen = CreatePen(PS_SOLID, w, C_FRAME);
        HGDIOBJ op = SelectObject(dc, pen), ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
        Polygon(dc, p, 6);
        SelectObject(dc, op);
        SelectObject(dc, ob);
        DeleteObject(pen);
    }
    /* Amber edge along each cut corner, brackets on the square ones. */
    line(dc, r->left, r->top + c, r->left + c, r->top, C_AMBER, w);
    line(dc, r->right - 1 - c, r->bottom - 1, r->right - 1, r->bottom - 1 - c, C_AMBER, w);
    bracket(dc, r->right - 1 - inset, r->top + inset, -1, 1, len, w, C_AMBER);
    bracket(dc, r->left + inset, r->bottom - 1 - inset, 1, -1, len, w, C_AMBER);
}

static void hazard(HDC dc, const RECT *r, int step)
{
    HRGN rgn = CreateRectRgnIndirect(r);
    int x, h = r->bottom - r->top;
    SelectClipRgn(dc, rgn);
    fill(dc, r->left, r->top, r->right, r->bottom, C_AMBER_DK);
    for (x = r->left - h; x < r->right; x += step * 2) {
        POINT q[4];
        HBRUSH br = CreateSolidBrush(C_AMBER);
        HPEN pen = CreatePen(PS_SOLID, 1, C_AMBER);
        HGDIOBJ ob = SelectObject(dc, br), op = SelectObject(dc, pen);
        q[0].x = x;            q[0].y = r->bottom;
        q[1].x = x + h;        q[1].y = r->top;
        q[2].x = x + h + step; q[2].y = r->top;
        q[3].x = x + step;     q[3].y = r->bottom;
        Polygon(dc, q, 4);
        SelectObject(dc, ob);
        SelectObject(dc, op);
        DeleteObject(br);
        DeleteObject(pen);
    }
    SelectClipRgn(dc, NULL);
    DeleteObject(rgn);
}

void rbx_header(HDC dc, const RbxFonts *f, const RECT *band, const char *title,
                const char *subtitle, const char *tag)
{
    int x = band->left, h = band->bottom - band->top;
    int bar = rbx_px(f, 6), gap = rbx_px(f, 14);
    RECT t;
    /* the amber bar, with a notch cut out at the top like the manual's tabs */
    fill(dc, x, band->top + rbx_px(f, 6), x + bar, band->bottom - rbx_px(f, 4), C_AMBER);
    x += bar + gap;
    SetRect(&t, x, band->top, band->right, band->top + h * 62 / 100);
    {
        HGDIOBJ old = SelectObject(dc, f->title);
        int prev = SetTextCharacterExtra(dc, rbx_px(f, 2));
        rbx_text(dc, f->title, C_WHITE, &t, title, DT_LEFT | DT_BOTTOM | DT_SINGLELINE);
        SetTextCharacterExtra(dc, prev);
        SelectObject(dc, old);
    }
    if (subtitle)
        spaced(dc, f->tag, C_AMBER, x + rbx_px(f, 1), t.bottom + rbx_px(f, 4), subtitle, rbx_px(f, 3));
    if (tag) {
        int tw = spaced_w(dc, f->tag, tag, rbx_px(f, 2)) + rbx_px(f, 20);
        int th = rbx_px(f, 22), stripes = rbx_px(f, 46);
        int ty = band->top + (h - th) / 2 - rbx_px(f, 4);
        RECT hz, box;
        SetRect(&hz, band->right - stripes, ty, band->right, ty + th);
        hazard(dc, &hz, rbx_px(f, 6));
        SetRect(&box, hz.left - rbx_px(f, 4) - tw, ty, hz.left - rbx_px(f, 4), ty + th);
        fill(dc, box.left, box.top, box.right, box.bottom, C_BOX);
        {
            HPEN pen = CreatePen(PS_SOLID, 1, C_AMBER);
            HGDIOBJ op = SelectObject(dc, pen), ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
            Rectangle(dc, box.left, box.top, box.right, box.bottom);
            SelectObject(dc, op);
            SelectObject(dc, ob);
            DeleteObject(pen);
        }
        {
            HGDIOBJ old = SelectObject(dc, f->tag);
            TEXTMETRICA tm;
            GetTextMetricsA(dc, &tm);
            SelectObject(dc, old);
            spaced(dc, f->tag, C_AMBER, box.left + rbx_px(f, 10), box.top + (th - tm.tmHeight) / 2, tag,
                   rbx_px(f, 2));
        }
    }
    /* the rule under the band, with tick marks */
    {
        int y = band->bottom + rbx_px(f, 4), i, step = rbx_px(f, 12);
        line(dc, band->left, y, band->right, y, C_EDGE, 1);
        for (i = 0; i < 6; i++)
            line(dc, band->right - i * step, y, band->right - i * step, y + rbx_px(f, 4), C_EDGE, 1);
    }
}

void rbx_section(HDC dc, const RbxFonts *f, const RECT *row, int number, const char *text)
{
    char num[8];
    RECT box;
    int bh = rbx_px(f, 18), by = row->bottom - bh - rbx_px(f, 4), x;
    TEXTMETRICA tm;
    HGDIOBJ old;
    snprintf(num, sizeof num, "%02d", number);
    SetRect(&box, row->left, by, row->left + rbx_px(f, 28), by + bh);
    fill(dc, box.left, box.top, box.right, box.bottom, C_AMBER);
    rbx_text(dc, f->tag, RGB(20, 12, 0), &box, num, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    old = SelectObject(dc, f->head);
    GetTextMetricsA(dc, &tm);
    SelectObject(dc, old);
    x = box.right + rbx_px(f, 10);
    spaced(dc, f->head, C_WHITE, x, by + (bh - tm.tmHeight) / 2, text, rbx_px(f, 3));
    x += spaced_w(dc, f->head, text, rbx_px(f, 3)) + rbx_px(f, 10);
    line(dc, x, by + bh / 2, row->right, by + bh / 2, C_EDGE, 1);
    line(dc, row->right - rbx_px(f, 1), by + bh / 2 - rbx_px(f, 4), row->right - rbx_px(f, 1),
         by + bh / 2 + rbx_px(f, 4), C_AMBER, rbx_px(f, 2));
}

static void arrow(HDC dc, int cx, int cy, int s, int dir, COLORREF c)
{
    POINT p[3];
    HBRUSH br = CreateSolidBrush(c);
    HPEN pen = CreatePen(PS_SOLID, 1, c);
    HGDIOBJ ob = SelectObject(dc, br), op = SelectObject(dc, pen);
    p[0].x = cx + dir * s; p[0].y = cy;
    p[1].x = cx - dir * s; p[1].y = cy - s - s / 3;
    p[2].x = cx - dir * s; p[2].y = cy + s + s / 3;
    Polygon(dc, p, 3);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(br);
    DeleteObject(pen);
}

void rbx_option(HDC dc, const RbxFonts *f, const RECT *row, const char *label, const char *value,
                unsigned index, unsigned count, int selected, int hover, RECT *left, RECT *right)
{
    RECT lr = *row, box, l, rr, v;
    int bw = (row->right - row->left) * f->value_pct / 100, a, i;
    if (selected || hover) {
        fill(dc, row->left, row->top, row->right, row->bottom, selected ? C_ROW_LIT : C_ROW_HOVER);
        if (selected)
            fill(dc, row->left, row->top, row->left + rbx_px(f, 4), row->bottom, C_AMBER);
    }
    lr.left += rbx_px(f, 16);
    lr.right = row->right - bw - rbx_px(f, 8);
    rbx_text(dc, f->body, selected ? C_WHITE : C_TEXT, &lr, label,
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    SetRect(&box, row->right - bw, row->top + rbx_px(f, 3), row->right - rbx_px(f, 6), row->bottom - rbx_px(f, 3));
    chamfer(dc, &box, rbx_px(f, 6), C_BOX, selected ? C_AMBER : C_EDGE);
    a = box.bottom - box.top;
    SetRect(&l, box.left, box.top, box.left + a, box.bottom);
    SetRect(&rr, box.right - a, box.top, box.right, box.bottom);
    if (count > 1) {
        COLORREF ac = selected ? C_AMBER : (hover ? C_TEXT : C_DIM);
        arrow(dc, (l.left + l.right) / 2, (l.top + l.bottom) / 2, rbx_px(f, 4), -1, ac);
        arrow(dc, (rr.left + rr.right) / 2, (rr.top + rr.bottom) / 2, rbx_px(f, 4), 1, ac);
    }
    SetRect(&v, l.right, box.top, rr.left, box.bottom - (count > 1 ? rbx_px(f, 5) : 0));
    rbx_text(dc, f->key, selected ? C_WHITE : C_SKY, &v, value,
             DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    /* One pip per value under the readout, the current one lit. */
    if (count > 1) {
        int pw = rbx_px(f, 6), ph = rbx_px(f, 2) > 1 ? rbx_px(f, 2) : 2, gap = rbx_px(f, 3);
        int total = (int)count * pw + ((int)count - 1) * gap;
        int px0 = (box.left + box.right) / 2 - total / 2, py = box.bottom - rbx_px(f, 3) - ph;
        for (i = 0; i < (int)count; i++)
            fill(dc, px0 + i * (pw + gap), py, px0 + i * (pw + gap) + pw, py + ph,
                 (unsigned)i == index ? (selected ? C_AMBER : C_SKY) : C_EDGE);
    }
    if (left) *left = l;
    if (right) *right = rr;
}

void rbx_button(HDC dc, const RbxFonts *f, const RECT *r, const char *text, int style,
                int selected, int hover)
{
    COLORREF accent = style == RBX_BTN_DANGER ? C_RED : C_AMBER;
    COLORREF body = style == RBX_BTN_DANGER ? C_RED_DK : C_AMBER_DK;
    COLORREF bg = selected ? accent : (hover ? body : C_BOX);
    COLORREF edge = selected || hover || style != RBX_BTN_NORMAL ? accent : C_EDGE;
    COLORREF ink = selected ? (style == RBX_BTN_DANGER ? C_WHITE : RGB(24, 14, 0))
                            : (style == RBX_BTN_NORMAL && !hover ? C_TEXT : C_WHITE);
    int extra = rbx_px(f, 3), x, y;
    TEXTMETRICA tm;
    HGDIOBJ old;
    chamfer(dc, r, rbx_px(f, 9), bg, edge);
    old = SelectObject(dc, f->head);
    GetTextMetricsA(dc, &tm);
    SelectObject(dc, old);
    x = (r->left + r->right) / 2 - spaced_w(dc, f->head, text, extra) / 2 + extra / 2;
    y = (r->top + r->bottom) / 2 - tm.tmHeight / 2;
    spaced(dc, f->head, ink, x, y, text, extra);
}

void rbx_note(HDC dc, const RbxFonts *f, const RECT *r, const char *tag, const char *text)
{
    RECT t, tr;
    int tw;
    fill(dc, r->left, r->top, r->right, r->bottom, RGB(10, 20, 38));
    fill(dc, r->left, r->top, r->left + rbx_px(f, 3), r->bottom, C_SKY);
    tw = spaced_w(dc, f->tag, tag, rbx_px(f, 2)) + rbx_px(f, 14);
    SetRect(&tr, r->left + rbx_px(f, 12), r->top + (r->bottom - r->top) / 2 - rbx_px(f, 9),
            r->left + rbx_px(f, 12) + tw, r->top + (r->bottom - r->top) / 2 + rbx_px(f, 9));
    {
        HPEN pen = CreatePen(PS_SOLID, 1, C_SKY);
        HGDIOBJ op = SelectObject(dc, pen), ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
        Rectangle(dc, tr.left, tr.top, tr.right, tr.bottom);
        SelectObject(dc, op);
        SelectObject(dc, ob);
        DeleteObject(pen);
    }
    {
        TEXTMETRICA tm;
        HGDIOBJ old = SelectObject(dc, f->tag);
        GetTextMetricsA(dc, &tm);
        SelectObject(dc, old);
        spaced(dc, f->tag, C_SKY, tr.left + rbx_px(f, 7), (tr.top + tr.bottom) / 2 - tm.tmHeight / 2, tag,
               rbx_px(f, 2));
    }
    SetRect(&t, tr.right + rbx_px(f, 12), r->top, r->right - rbx_px(f, 10), r->bottom);
    rbx_text(dc, f->fine, C_TEXT, &t, text, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

void rbx_keyhints(HDC dc, const RbxFonts *f, const RECT *r, const char *const *pairs, int npairs)
{
    int x = r->left, i, h = rbx_px(f, 20), y = (r->top + r->bottom) / 2 - h / 2;
    for (i = 0; i < npairs; i++) {
        const char *cap = pairs[i * 2], *what = pairs[i * 2 + 1];
        int cw = rbx_text_w(dc, f->tag, cap) + rbx_px(f, 14), ww;
        RECT c, t;
        if (cw < h) cw = h;
        SetRect(&c, x, y, x + cw, y + h);
        chamfer(dc, &c, rbx_px(f, 4), C_BOX, C_DIM);
        rbx_text(dc, f->tag, C_WHITE, &c, cap, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        x = c.right + rbx_px(f, 6);
        ww = rbx_text_w(dc, f->fine, what);
        if (x + ww > r->right) break;
        SetRect(&t, x, r->top, x + ww + 1, r->bottom);
        rbx_text(dc, f->fine, C_DIM, &t, what, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        x += ww + rbx_px(f, 18);
    }
}
