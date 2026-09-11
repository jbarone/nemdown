/* paint.c — draw the visible band of the block tree. */

#include "doc/paint.h"

#include <pango/pangocairo.h>

#include "doc/callout.h"
#include "ui/theme.h"
#include "ui/typography.h"

#define ND_CODE_PAD_X   16.0
#define ND_CODE_PAD_Y   14.0
#define ND_QUOTE_BAR     3.0
#define ND_RADIUS        8.0

static void rounded_rect(cairo_t *cr, double x, double y, double w, double h,
                         double r) {
  if (r > w / 2) r = w / 2;
  if (r > h / 2) r = h / 2;
  cairo_new_sub_path(cr);
  cairo_arc(cr, x + w - r, y + r,     r, -G_PI / 2, 0);
  cairo_arc(cr, x + w - r, y + h - r, r, 0,          G_PI / 2);
  cairo_arc(cr, x + r,     y + h - r, r, G_PI / 2,   G_PI);
  cairo_arc(cr, x + r,     y + r,     r, G_PI,       3 * G_PI / 2);
  cairo_close_path(cr);
}

static void paint_block(cairo_t *cr, nd_block *b, double y0, double y1);

static void paint_children(cairo_t *cr, nd_block *b, double y0, double y1) {
  for (uint32_t i = 0; i < b->nkids; i++) {
    nd_block *k = b->kids[i];
    if (k->lay.y > y1) break;                       /* rest is below the fold */
    if (k->lay.y + k->lay.h < y0) continue;         /* above it */
    paint_block(cr, k, y0, y1);
  }
}

static void paint_block(cairo_t *cr, nd_block *b, double y0, double y1) {
  switch (b->kind) {
    case ND_PARA:
    case ND_CELL:
      if (b->lay.pl) {
        nd_src(cr, CTP_TEXT);
        cairo_move_to(cr, b->lay.x, b->lay.y);
        pango_cairo_show_layout(cr, b->lay.pl);
      }
      paint_children(cr, b, y0, y1);
      break;

    case ND_HEADING: {
      const nd_style *st = nd_heading_style(b->level);
      if (b->lay.pl) {
        nd_src(cr, st->color);
        cairo_move_to(cr, b->lay.x, b->lay.y);
        pango_cairo_show_layout(cr, b->lay.pl);
      }
      /* A rule under H1 only. In a single-file viewer the one H1 is the title,
       * where a rule reads as a separator; H2 rules would turn a long technical
       * document into a ladder. */
      if (b->level == 1) {
        nd_src(cr, CTP_SURFACE1);
        cairo_rectangle(cr, b->lay.x, b->lay.y + b->lay.h + 8.0, b->lay.w, 1.0);
        cairo_fill(cr);
      }
      paint_children(cr, b, y0, y1);
      break;
    }

    case ND_CODE: {
      double pad_top = b->code_lang ? ND_CODE_PAD_Y + 16.0 : ND_CODE_PAD_Y;

      nd_src(cr, CTP_MANTLE);
      rounded_rect(cr, b->lay.x, b->lay.y, b->lay.w, b->lay.h, ND_RADIUS);
      cairo_fill_preserve(cr);
      nd_src(cr, CTP_SURFACE0);
      cairo_set_line_width(cr, 1.0);
      cairo_stroke(cr);

      if (b->code_lang && *b->code_lang) {
        nd_src(cr, CTP_OVERLAY0);
        cairo_move_to(cr, b->lay.x + ND_CODE_PAD_X, b->lay.y + 6.0);
        /* Drawn with the toy API only for this tiny label; everything else
         * goes through Pango. */
        cairo_select_font_face(cr, ND_MONO, CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 11.0);
        cairo_move_to(cr, b->lay.x + ND_CODE_PAD_X, b->lay.y + 17.0);
        cairo_show_text(cr, b->code_lang);
      }

      if (b->lay.pl) {
        /* Code does not wrap, so clip it to the panel and let it overflow. */
        cairo_save(cr);
        cairo_rectangle(cr, b->lay.x + 1, b->lay.y + 1,
                        b->lay.w - 2, b->lay.h - 2);
        cairo_clip(cr);
        nd_src(cr, CTP_TEXT);
        cairo_move_to(cr, b->lay.x + ND_CODE_PAD_X, b->lay.y + pad_top);
        pango_cairo_show_layout(cr, b->lay.pl);
        cairo_restore(cr);
      }
      break;
    }

    case ND_CALLOUT: {
      const nd_callout_type *ct = &nd_callout_types[b->callout_type];

      /* Tinted panel: the type colour at 10% over the page, which is what
       * Obsidian does. */
      rounded_rect(cr, b->lay.x, b->lay.y, b->lay.w, b->lay.h, ND_RADIUS);
      nd_src_a(cr, ct->color, 0.10);
      cairo_fill_preserve(cr);

      /* Clip to the rounded path before filling the bar, so its left corners
       * pick up the radius for free. */
      cairo_save(cr);
      cairo_clip(cr);
      nd_src(cr, ct->color);
      cairo_rectangle(cr, b->lay.x, b->lay.y, 3.0, b->lay.h);
      cairo_fill(cr);
      cairo_restore(cr);

      nd_src(cr, ct->color);
      cairo_select_font_face(cr, ND_SANS, CAIRO_FONT_SLANT_NORMAL,
                             CAIRO_FONT_WEIGHT_NORMAL);
      cairo_set_font_size(cr, 15.0);
      cairo_move_to(cr, b->lay.x + 16.0, b->lay.y + 12.0 + 15.0);
      cairo_show_text(cr, ct->icon);

      if (b->lay.marker) {
        nd_src(cr, ct->color);
        cairo_move_to(cr, b->lay.x + 16.0 + 26.0, b->lay.y + 12.0);
        pango_cairo_show_layout(cr, b->lay.marker);
      }
      if (!b->callout_folded) paint_children(cr, b, y0, y1);
      break;
    }

    case ND_TABLE: {
      nd_src(cr, CTP_SURFACE1);
      rounded_rect(cr, b->lay.x, b->lay.y, b->lay.w, b->lay.h, 6.0);
      cairo_set_line_width(cr, 1.0);
      cairo_stroke(cr);

      for (uint32_t r = 0; r < b->nkids; r++) {
        nd_block *row = b->kids[r];
        if (row->row_is_header) {
          cairo_save(cr);
          rounded_rect(cr, b->lay.x, b->lay.y, b->lay.w, b->lay.h, 6.0);
          cairo_clip(cr);
          nd_src(cr, CTP_SURFACE0);
          cairo_rectangle(cr, b->lay.x, row->lay.y, b->lay.w, row->lay.h);
          cairo_fill(cr);
          cairo_restore(cr);

          nd_src(cr, CTP_SURFACE2);
          cairo_rectangle(cr, b->lay.x, row->lay.y + row->lay.h, b->lay.w, 1.0);
          cairo_fill(cr);
        } else if (r > 0) {
          nd_src(cr, CTP_SURFACE1);
          cairo_rectangle(cr, b->lay.x, row->lay.y, b->lay.w, 1.0);
          cairo_fill(cr);
        }

        /* No vertical rules: Obsidian's tables are horizontal-only. */
        for (uint32_t c = 0; c < row->nkids; c++) {
          nd_block *cell = row->kids[c];
          if (!cell->lay.pl) continue;
          nd_src(cr, row->row_is_header ? CTP_SUBTEXT1 : CTP_TEXT);
          cairo_move_to(cr, cell->lay.x, cell->lay.y);
          pango_cairo_show_layout(cr, cell->lay.pl);
        }
      }
      break;
    }

    case ND_QUOTE:
      nd_src(cr, CTP_SURFACE2);
      cairo_rectangle(cr, b->lay.x - 20.0, b->lay.y, ND_QUOTE_BAR, b->lay.h);
      cairo_fill(cr);
      paint_children(cr, b, y0, y1);
      break;

    case ND_HR:
      nd_src(cr, CTP_SURFACE1);
      cairo_rectangle(cr, b->lay.x, b->lay.y, b->lay.w, 1.0);
      cairo_fill(cr);
      break;

    case ND_IMAGE: {
      /* Placeholder until the decode cache lands. */
      nd_src(cr, CTP_SURFACE0);
      rounded_rect(cr, b->lay.x, b->lay.y, b->lay.w, b->lay.h, ND_RADIUS);
      cairo_fill_preserve(cr);
      nd_src(cr, CTP_SURFACE2);
      double dashes[] = {4.0, 4.0};
      cairo_set_dash(cr, dashes, 2, 0);
      cairo_set_line_width(cr, 1.0);
      cairo_stroke(cr);
      cairo_set_dash(cr, NULL, 0, 0);

      nd_src(cr, CTP_OVERLAY1);
      cairo_select_font_face(cr, ND_SANS, CAIRO_FONT_SLANT_NORMAL,
                             CAIRO_FONT_WEIGHT_NORMAL);
      cairo_set_font_size(cr, 13.0);
      cairo_move_to(cr, b->lay.x + 16.0, b->lay.y + b->lay.h / 2 + 4.0);
      cairo_show_text(cr, b->img_src ? b->img_src : "image");
      break;
    }

    case ND_ITEM: {
      /* Bullet cycles by depth so nesting reads at a glance. */
      int depth = 0;
      for (nd_block *p = b->parent; p; p = p->parent)
        if (p->kind == ND_LIST) depth++;

      nd_src(cr, CTP_OVERLAY1);
      cairo_select_font_face(cr, ND_SANS, CAIRO_FONT_SLANT_NORMAL,
                             CAIRO_FONT_WEIGHT_NORMAL);
      cairo_set_font_size(cr, 16.0);
      cairo_move_to(cr, b->lay.x + 8.0, b->lay.y + 18.0);

      if (b->item_is_task) {
        /* Drawn, not a glyph: a font checkbox gives no control over stroke
         * weight and its centring depends on metrics we would have to fight. */
        double bx = b->lay.x + 6.0, by = b->lay.y + 5.0, s = 16.0;
        rounded_rect(cr, bx, by, s, s, 4.0);
        if (b->item_checked) {
          nd_src(cr, CTP_TEAL);
          cairo_fill(cr);
          nd_src(cr, CTP_BASE);
          cairo_set_line_width(cr, 2.0);
          cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
          cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
          cairo_move_to(cr, bx + 0.26 * s, by + 0.53 * s);
          cairo_line_to(cr, bx + 0.44 * s, by + 0.72 * s);
          cairo_line_to(cr, bx + 0.76 * s, by + 0.30 * s);
          cairo_stroke(cr);
        } else {
          nd_src(cr, CTP_OVERLAY1);
          cairo_set_line_width(cr, 1.5);
          cairo_stroke(cr);
        }
      } else if (b->lay.marker) {
        /* Ordered list: the number was laid out with tabular figures so the
         * markers share a right edge. */
        int mw, mh;
        pango_layout_get_pixel_size(b->lay.marker, &mw, &mh);
        nd_src(cr, CTP_OVERLAY1);
        cairo_move_to(cr, b->lay.x + 20.0 - mw, b->lay.y);
        pango_cairo_show_layout(cr, b->lay.marker);
      } else {
        const char *bullet = (depth % 3 == 1) ? "\xE2\x80\xA2"      /* • */
                           : (depth % 3 == 2) ? "\xE2\x97\xA6"      /* ◦ */
                                              : "\xE2\x96\xAA";     /* ▪ */
        cairo_show_text(cr, bullet);
      }
      paint_children(cr, b, y0, y1);
      break;
    }

    default:
      paint_children(cr, b, y0, y1);
      break;
  }
}

void nd_paint_tree(cairo_t *cr, nd_block *root, double scroll_y,
                   double viewport_h) {
  double y0 = scroll_y, y1 = scroll_y + viewport_h;

  cairo_save(cr);
  cairo_translate(cr, 0, -scroll_y);

  /* Binary search the TOP-LEVEL blocks only. Containers span their children in
   * y, which breaks the sorted/non-overlapping precondition a full-tree search
   * needs; top-level blocks are the numerous ones anyway, so this is the win. */
  uint32_t lo = 0, hi = root->nkids;
  while (lo < hi) {
    uint32_t mid = (lo + hi) / 2;
    nd_block *k = root->kids[mid];
    if (k->lay.y + k->lay.h <= y0) lo = mid + 1;
    else                           hi = mid;
  }

  for (uint32_t i = lo; i < root->nkids; i++) {
    nd_block *k = root->kids[i];
    if (k->lay.y > y1) break;
    paint_block(cr, k, y0, y1);
  }

  cairo_restore(cr);
}
