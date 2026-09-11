/* sidebar.c — properties pane over contents pane. */

#include "ui/sidebar.h"

#include <stdio.h>
#include <string.h>

#include "ui/theme.h"
#include "ui/typography.h"

#define SB_PAD_X        16.0
#define SB_PAD_Y        14.0
#define SB_ROW_GAP       6.0
#define SB_LABEL_SIZE   11.0
#define SB_VALUE_SIZE   13.0
#define SB_TOC_SIZE     13.0
#define SB_TOC_ROW      24.0
#define SB_TOC_INDENT   12.0
#define SB_CHIP_PAD_X    7.0
#define SB_CHIP_H       19.0
/* Properties are capped so the contents pane always has room; most
 * frontmatter is a handful of keys and fits well inside this. */
#define SB_PROPS_MAX_FRAC 0.42

void nd_sidebar_init(struct nd_sidebar *sb, PangoContext *pctx) {
  memset(sb, 0, sizeof *sb);
  sb->pctx = pctx;
  sb->hover_toc = -1;
  sb->active_toc = -1;
}

static PangoLayout *mk(struct nd_sidebar *sb, const char *text, double size,
                       bool bold) {
  PangoLayout *pl = pango_layout_new(sb->pctx);
  PangoFontDescription *fd = pango_font_description_new();
  pango_font_description_set_family(fd, ND_SANS);
  pango_font_description_set_absolute_size(fd, size * PANGO_SCALE);
  pango_font_description_set_weight(
      fd, bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL);
  pango_layout_set_font_description(pl, fd);
  pango_font_description_free(fd);
  pango_layout_set_text(pl, text ? text : "", -1);
  return pl;
}

static void rounded(cairo_t *cr, double x, double y, double w, double h, double r) {
  if (r > h / 2) r = h / 2;
  cairo_new_sub_path(cr);
  cairo_arc(cr, x + w - r, y + r,     r, -G_PI / 2, 0);
  cairo_arc(cr, x + w - r, y + h - r, r, 0,          G_PI / 2);
  cairo_arc(cr, x + r,     y + h - r, r, G_PI / 2,   G_PI);
  cairo_arc(cr, x + r,     y + r,     r, G_PI,       3 * G_PI / 2);
  cairo_close_path(cr);
}

static void section_label(struct nd_sidebar *sb, cairo_t *cr, const char *text,
                          double x, double y) {
  PangoLayout *pl = mk(sb, text, SB_LABEL_SIZE, true);
  PangoAttrList *al = pango_attr_list_new();
  pango_attr_list_insert(al, pango_attr_letter_spacing_new(PANGO_SCALE));
  pango_layout_set_attributes(pl, al);
  pango_attr_list_unref(al);
  nd_src(cr, CTP_OVERLAY0);
  cairo_move_to(cr, x, y);
  pango_cairo_show_layout(cr, pl);
  g_object_unref(pl);
}

/* Values render by type: lists as chips, booleans as a drawn checkbox, the
 * rest as text. */
static double paint_value(struct nd_sidebar *sb, cairo_t *cr,
                          const nd_property *p, double x, double y, double w) {
  if (p->type == ND_PROP_LIST) {
    double cx = x, cy = y, line_h = SB_CHIP_H + 4.0;
    for (size_t i = 0; i < p->nitems; i++) {
      PangoLayout *pl = mk(sb, p->items[i], SB_VALUE_SIZE - 1, false);
      int tw, th;
      pango_layout_get_pixel_size(pl, &tw, &th);
      double cw = tw + 2 * SB_CHIP_PAD_X;
      if (cx + cw > x + w && cx > x) { cx = x; cy += line_h; }

      nd_src_a(cr, CTP_TEAL, 0.14);
      rounded(cr, cx, cy, cw, SB_CHIP_H, 5.0);
      cairo_fill(cr);
      nd_src(cr, CTP_TEAL);
      cairo_move_to(cr, cx + SB_CHIP_PAD_X, cy + (SB_CHIP_H - th) / 2);
      pango_cairo_show_layout(cr, pl);
      g_object_unref(pl);

      cx += cw + 5.0;
    }
    return cy + line_h;
  }

  if (p->type == ND_PROP_CHECKBOX) {
    double s = 14.0;
    rounded(cr, x, y + 2.0, s, s, 4.0);
    if (p->checkbox) {
      nd_src(cr, CTP_TEAL);
      cairo_fill(cr);
      nd_src(cr, CTP_BASE);
      cairo_set_line_width(cr, 2.0);
      cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
      cairo_move_to(cr, x + 0.26 * s, y + 2.0 + 0.53 * s);
      cairo_line_to(cr, x + 0.44 * s, y + 2.0 + 0.72 * s);
      cairo_line_to(cr, x + 0.76 * s, y + 2.0 + 0.30 * s);
      cairo_stroke(cr);
    } else {
      nd_src(cr, CTP_OVERLAY1);
      cairo_set_line_width(cr, 1.5);
      cairo_stroke(cr);
    }
    return y + s + 6.0;
  }

  char buf[64];
  const char *text = p->text;
  if (p->type == ND_PROP_NUMBER) {
    snprintf(buf, sizeof buf, "%g", p->number);
    text = buf;
  } else if (p->type == ND_PROP_EMPTY) {
    text = p->unsupported ? "(unsupported)" : "—";
  }

  PangoLayout *pl = mk(sb, text, SB_VALUE_SIZE, false);
  pango_layout_set_width(pl, (int)(w * PANGO_SCALE));
  pango_layout_set_wrap(pl, PANGO_WRAP_WORD_CHAR);
  nd_src(cr, (p->type == ND_PROP_EMPTY) ? CTP_OVERLAY0 : CTP_TEXT);
  cairo_move_to(cr, x, y);
  pango_cairo_show_layout(cr, pl);
  int tw, th;
  pango_layout_get_pixel_size(pl, &tw, &th);
  g_object_unref(pl);
  return y + th + SB_ROW_GAP;
}

/* Measured by laying out against a throwaway surface: cheaper to write than a
 * second measure-only path that could drift out of sync with the painter. */
static double props_content_height(struct nd_sidebar *sb, const nd_props *props,
                                   double w) {
  cairo_surface_t *cs = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
  cairo_t *cr = cairo_create(cs);
  double y = SB_PAD_Y + 18.0;
  if (props->error) y += 34.0;
  for (size_t i = 0; i < props->count; i++) {
    y += 15.0;
    y = paint_value(sb, cr, &props->items[i], SB_PAD_X, y, w - 2 * SB_PAD_X);
  }
  cairo_destroy(cr);
  cairo_surface_destroy(cs);
  return y + SB_PAD_Y;
}

/* A 4px overlay bar, drawn only when the content actually overflows. */
static void pane_scrollbar(cairo_t *cr, double pane_x, double pane_y,
                           double pane_w, double pane_h, double content_h,
                           double scroll, double alpha) {
  if (alpha <= 0.01 || content_h <= pane_h + 1.0) return;

  double track = pane_h - 8.0;
  double frac = pane_h / content_h;
  double thumb = track * frac;
  if (thumb < 24.0) thumb = 24.0;

  double max_scroll = content_h - pane_h;
  double t = max_scroll > 0 ? scroll / max_scroll : 0;
  if (t < 0) t = 0;
  if (t > 1) t = 1;

  double x = pane_x + pane_w - 4.0 - 3.0;
  double y = pane_y + 4.0 + t * (track - thumb);

  nd_src_a(cr, CTP_OVERLAY0, 0.45 * alpha);
  rounded(cr, x, y, 4.0, thumb, 2.0);
  cairo_fill(cr);
}

void nd_sidebar_paint(struct nd_sidebar *sb, cairo_t *cr, double w, double h,
                      const nd_props *props, const nd_toc *toc,
                      const char *title, double scale) {
  (void)title;

  double avail = w - 2 * SB_PAD_X;

  sb->view_h = h;
  sb->props_content_h = (props->count || props->error)
      ? props_content_height(sb, props, w) : 0.0;

  /* The pane is capped so the contents pane always has room; the content can
   * be taller, which is what the pane's own scroll offset is for. */
  double cap = h * SB_PROPS_MAX_FRAC;
  double props_h = sb->props_content_h > cap ? cap : sb->props_content_h;
  sb->props_h = props_h;

  sb->toc_content_h = toc->count
      ? (double)toc->count * SB_TOC_ROW + SB_PAD_Y + 20.0 + SB_PAD_Y : 0.0;

  /* Clamp after a reload or resize may have shrunk the content. */
  double pmax = sb->props_content_h - props_h;
  if (pmax < 0) pmax = 0;
  if (sb->props_scroll > pmax) sb->props_scroll = pmax;

  double tmax = sb->toc_content_h - (h - props_h);
  if (tmax < 0) tmax = 0;
  if (sb->toc_scroll > tmax) sb->toc_scroll = tmax;

  /* ---- properties ---- */
  if (props_h > 0) {
    cairo_save(cr);
    cairo_rectangle(cr, 0, 0, w, props_h);
    cairo_clip(cr);
    cairo_translate(cr, 0, -sb->props_scroll);

    double y = SB_PAD_Y;
    section_label(sb, cr, "PROPERTIES", SB_PAD_X, y);
    y += 18.0;

    if (props->error) {
      char buf[192];
      snprintf(buf, sizeof buf, "Invalid frontmatter (line %d)",
               props->error_line);
      PangoLayout *pl = mk(sb, buf, SB_VALUE_SIZE - 1, false);
      pango_layout_set_width(pl, (int)(avail * PANGO_SCALE));
      pango_layout_set_wrap(pl, PANGO_WRAP_WORD_CHAR);
      nd_src(cr, CTP_RED);
      cairo_move_to(cr, SB_PAD_X, y + 8.0);
      pango_cairo_show_layout(cr, pl);
      g_object_unref(pl);
      y += 34.0;
    }

    for (size_t i = 0; i < props->count; i++) {
      const nd_property *p = &props->items[i];
      PangoLayout *kl = mk(sb, p->key, SB_LABEL_SIZE, false);
      nd_src(cr, CTP_OVERLAY1);
      cairo_move_to(cr, SB_PAD_X, y);
      pango_cairo_show_layout(cr, kl);
      g_object_unref(kl);
      y += 15.0;
      y = paint_value(sb, cr, p, SB_PAD_X, y, avail);
    }
    cairo_restore(cr);

    pane_scrollbar(cr, 0, 0, w, props_h, sb->props_content_h,
                   sb->props_scroll, sb->props_alpha);

    nd_src(cr, CTP_SURFACE0);
    cairo_rectangle(cr, 0, nd_snap(props_h, scale), w, 1.0 / scale);
    cairo_fill(cr);
  }

  /* ---- contents ---- */
  double ty = props_h + SB_PAD_Y;
  cairo_save(cr);
  cairo_rectangle(cr, 0, props_h, w, h - props_h);
  cairo_clip(cr);

  /* The heading stays put; only the entries scroll under it. */
  section_label(sb, cr, "CONTENTS", SB_PAD_X, ty);
  ty += 20.0;

  cairo_save(cr);
  cairo_rectangle(cr, 0, ty, w, h - ty);
  cairo_clip(cr);
  cairo_translate(cr, 0, -sb->toc_scroll);

  for (size_t i = 0; i < toc->count; i++) {
    const nd_toc_entry *e = &toc->items[i];
    double rx = SB_PAD_X + e->depth * SB_TOC_INDENT;
    double row_y = ty + (double)i * SB_TOC_ROW;

    bool active = ((int)i == sb->active_toc);
    if ((int)i == sb->hover_toc && !active) {
      nd_src(cr, CTP_SURFACE0);
      rounded(cr, 6.0, row_y - 2.0, w - 12.0, SB_TOC_ROW - 2.0, 5.0);
      cairo_fill(cr);
    }
    if (active) {
      nd_src(cr, CTP_TEAL);
      cairo_rectangle(cr, 6.0, row_y - 2.0, 2.0, SB_TOC_ROW - 4.0);
      cairo_fill(cr);
    }

    PangoLayout *pl = mk(sb, e->text, SB_TOC_SIZE, e->depth == 0);
    pango_layout_set_width(pl, (int)((w - rx - SB_PAD_X) * PANGO_SCALE));
    /* Truncate rather than wrap: a wrapped TOC loses its scannability. */
    pango_layout_set_ellipsize(pl, PANGO_ELLIPSIZE_END);
    nd_src(cr, active ? CTP_TEAL : CTP_SUBTEXT0);
    cairo_move_to(cr, rx, row_y);
    pango_cairo_show_layout(cr, pl);
    g_object_unref(pl);
  }
  cairo_restore(cr);
  cairo_restore(cr);

  pane_scrollbar(cr, 0, props_h, w, h - props_h, sb->toc_content_h,
                 sb->toc_scroll, sb->toc_alpha);
}

nd_pane nd_sidebar_pane_at(const struct nd_sidebar *sb, double y) {
  if (sb->props_h > 0 && y < sb->props_h) return ND_PANE_PROPS;
  return ND_PANE_TOC;
}

bool nd_sidebar_scroll(struct nd_sidebar *sb, nd_pane pane, double dy) {
  double *scroll, max;

  if (pane == ND_PANE_PROPS) {
    scroll = &sb->props_scroll;
    max = sb->props_content_h - sb->props_h;
  } else {
    scroll = &sb->toc_scroll;
    max = sb->toc_content_h - (sb->view_h - sb->props_h);
  }
  if (max < 0) max = 0;

  double before = *scroll;
  *scroll += dy;
  if (*scroll < 0) *scroll = 0;
  if (*scroll > max) *scroll = max;
  return *scroll != before;
}

int nd_sidebar_toc_at(const struct nd_sidebar *sb, double w, double h,
                      const nd_props *props, const nd_toc *toc,
                      double x, double y) {
  (void)props; (void)h;
  if (x < 0 || x > w) return -1;
  double ty = sb->props_h + SB_PAD_Y + 20.0 - sb->toc_scroll;
  if (y < ty) return -1;
  int idx = (int)((y - ty) / SB_TOC_ROW);
  return (idx >= 0 && (size_t)idx < toc->count) ? idx : -1;
}
