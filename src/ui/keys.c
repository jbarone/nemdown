/* keys.c — the keybinding list, drawn by `?` and printed by --help. */

#include "ui/keys.h"

#include <math.h>
#include <stdbool.h>
#include <stddef.h>

#include "ui/theme.h"

struct key_row {
  const char *keys;
  const char *what;
};

struct key_group {
  const char *title;
  const struct key_row *rows;
  size_t n;
};

static const struct key_row reading[] = {
  {"j / k",           "next / previous block"},
  {"} / {",           "next / previous block, guide on or off"},
  {"g / G",           "first / last block"},
  {"Home / End",      "first / last block"},
  {"Up / Down",       "scroll a line"},
  {"PgUp / PgDn",     "scroll a page"},
  {"Ctrl-u / Ctrl-d", "half a page up / down"},
  {"h / l",           "pan a code fence"},
  {"f",               "reading guide on / off"},
};

static const struct key_row document[] = {
  {"/",               "search"},
  {"n / N",           "next / previous match"},
  {"o",               "open a file"},
  {"r",               "reload from disk"},
  {"Backspace",       "back to the last note"},
  {"b",               "toggle the sidebar"},
  {"+ / - / 0",       "zoom in / out / reset"},
  {"Ctrl-C",          "copy the selection, or the whole note"},
  {"?",               "show these keys"},
  {"q",               "quit"},
  {"Esc",             "close a panel or search, clear a selection, quit"},
};

#define N(a) (sizeof (a) / sizeof (a)[0])

static const struct key_group groups[] = {
  {"Reading",  reading,  N(reading)},
  {"Document", document, N(document)},
};

void nd_keys_print(FILE *out) {
  for (size_t g = 0; g < N(groups); g++) {
    fprintf(out, "%s%s\n", g ? "\n" : "", groups[g].title);
    for (size_t i = 0; i < groups[g].n; i++)
      fprintf(out, "  %-17s %s\n", groups[g].rows[i].keys,
              groups[g].rows[i].what);
  }
}

/* ---- the panel ---------------------------------------------------------- */

#define PAD      24.0   /* inside the panel's border */
#define COL_GAP  28.0   /* between the key column and the description */
#define ROW_H    22.0
#define TITLE_H  34.0   /* the "Keys" heading, baseline to first group */
#define HEAD_H   28.0   /* a group heading */
#define GROUP_GAP 10.0
#define FOOT_H   34.0   /* the dismiss hint */
#define MARGIN   16.0   /* least space kept between panel and window edge */

static void set_font(cairo_t *cr, double size, bool bold) {
  cairo_select_font_face(cr, ND_MONO, CAIRO_FONT_SLANT_NORMAL,
                         bold ? CAIRO_FONT_WEIGHT_BOLD
                              : CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, size);
}

static double advance(cairo_t *cr, const char *s) {
  cairo_text_extents_t e;
  cairo_text_extents(cr, s, &e);
  return e.x_advance;
}

static void text_at(cairo_t *cr, double x, double y, const char *s) {
  cairo_move_to(cr, x, y);
  cairo_show_text(cr, s);
}

void nd_keys_paint(cairo_t *cr, double w, double h, double scale) {
  cairo_save(cr);

  nd_src_a(cr, CTP_CRUST, 0.72);
  cairo_rectangle(cr, 0, 0, w, h);
  cairo_fill(cr);

  /* Measured rather than fixed, so a longer row widens the panel instead of
   * running out through its border. */
  set_font(cr, 13.0, false);
  double key_w = 0.0, what_w = 0.0;
  size_t rows = 0;
  for (size_t g = 0; g < N(groups); g++) {
    rows += groups[g].n;
    for (size_t i = 0; i < groups[g].n; i++) {
      double k = advance(cr, groups[g].rows[i].keys);
      double d = advance(cr, groups[g].rows[i].what);
      if (k > key_w) key_w = k;
      if (d > what_w) what_w = d;
    }
  }

  double pw = PAD + key_w + COL_GAP + what_w + PAD;
  double ph = PAD + TITLE_H + (double)N(groups) * HEAD_H +
              (double)rows * ROW_H + (double)(N(groups) - 1) * GROUP_GAP +
              FOOT_H + PAD;

  /* Shrink, never grow: a short window still gets the whole list. */
  double s = 1.0;
  if (w - 2 * MARGIN < pw * s) s = (w - 2 * MARGIN) / pw;
  if (h - 2 * MARGIN < ph * s) s = (h - 2 * MARGIN) / ph;
  if (s <= 0.0) { cairo_restore(cr); return; }

  double px = (w - pw * s) / 2.0, py = (h - ph * s) / 2.0;
  if (s == 1.0) { px = nd_snap(px, scale); py = nd_snap(py, scale); }
  cairo_translate(cr, px, py);
  cairo_scale(cr, s, s);

  /* Panel: rounded, Mantle on Crust, a Surface0 hairline round it. */
  double r = 10.0;
  cairo_new_sub_path(cr);
  cairo_arc(cr, pw - r, r,      r, -M_PI / 2, 0);
  cairo_arc(cr, pw - r, ph - r, r, 0,          M_PI / 2);
  cairo_arc(cr, r,      ph - r, r, M_PI / 2,   M_PI);
  cairo_arc(cr, r,      r,      r, M_PI,       3 * M_PI / 2);
  cairo_close_path(cr);
  nd_src(cr, CTP_MANTLE);
  cairo_fill_preserve(cr);
  nd_src(cr, CTP_SURFACE0);
  cairo_set_line_width(cr, 1.0);
  cairo_stroke(cr);

  double y = PAD + 16.0;
  set_font(cr, 16.0, true);
  nd_src(cr, ND_ACCENT);
  text_at(cr, PAD, y, "Keys");
  y += TITLE_H - 16.0;

  for (size_t g = 0; g < N(groups); g++) {
    if (g) y += GROUP_GAP;
    set_font(cr, 12.0, true);
    nd_src(cr, CTP_LAVENDER);
    text_at(cr, PAD, y + 18.0, groups[g].title);
    y += HEAD_H;

    set_font(cr, 13.0, false);
    for (size_t i = 0; i < groups[g].n; i++) {
      nd_src(cr, ND_ACCENT);
      text_at(cr, PAD, y + 15.0, groups[g].rows[i].keys);
      nd_src(cr, CTP_TEXT);
      text_at(cr, PAD + key_w + COL_GAP, y + 15.0, groups[g].rows[i].what);
      y += ROW_H;
    }
  }

  set_font(cr, 12.0, false);
  nd_src(cr, ND_MUTED);
  text_at(cr, PAD, y + 24.0, "? or Esc to close");

  cairo_restore(cr);
}
