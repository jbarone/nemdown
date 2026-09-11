/* paint.c — draw the visible band of the block tree. */

#include "doc/paint.h"

#include <pango/pangocairo.h>

#include "doc/callout.h"
#include "doc/images.h"
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

/* Painting is a tree walk, so the cache rides along in a file-scope pointer set
 * once per frame rather than threading an extra parameter through every case. */
static struct nd_image_cache *paint_images;

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
        const nd_style *ls = &nd_styles[ND_ST_CODE_LABEL];
        cairo_select_font_face(cr, ND_MONO, CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, ls->size);
        cairo_move_to(cr, b->lay.x + ND_CODE_PAD_X,
                      b->lay.y + 6.0 + ls->size);
        cairo_show_text(cr, b->code_lang);
      }

      if (b->lay.pl) {
        /* Code does not wrap: clip to the panel and pan within it. */
        cairo_save(cr);
        cairo_rectangle(cr, b->lay.x + 1, b->lay.y + 1,
                        b->lay.w - 2, b->lay.h - 2);
        cairo_clip(cr);
        nd_src(cr, CTP_TEXT);
        cairo_move_to(cr, b->lay.x + ND_CODE_PAD_X - b->lay.hscroll,
                      b->lay.y + pad_top);
        pango_cairo_show_layout(cr, b->lay.pl);
        cairo_restore(cr);

        /* A fade at whichever edge has content beyond it: the only cue that
         * the block scrolls at all. */
        double visible = b->lay.w - 2 * ND_CODE_PAD_X;
        double overflow = b->lay.natural_w - visible;
        double fade = 28.0;

        if (overflow > 0 && b->lay.hscroll < overflow - 0.5) {
          cairo_pattern_t *p = cairo_pattern_create_linear(
              b->lay.x + b->lay.w - fade, 0, b->lay.x + b->lay.w - 1, 0);
          cairo_pattern_add_color_stop_rgba(p, 0, ND_R(CTP_MANTLE),
                                            ND_G(CTP_MANTLE), ND_B(CTP_MANTLE), 0.0);
          cairo_pattern_add_color_stop_rgba(p, 1, ND_R(CTP_MANTLE),
                                            ND_G(CTP_MANTLE), ND_B(CTP_MANTLE), 1.0);
          cairo_set_source(cr, p);
          cairo_rectangle(cr, b->lay.x + b->lay.w - fade, b->lay.y + 1,
                          fade - 1, b->lay.h - 2);
          cairo_fill(cr);
          cairo_pattern_destroy(p);
        }
        if (b->lay.hscroll > 0.5) {
          cairo_pattern_t *p = cairo_pattern_create_linear(
              b->lay.x + 1, 0, b->lay.x + fade, 0);
          cairo_pattern_add_color_stop_rgba(p, 0, ND_R(CTP_MANTLE),
                                            ND_G(CTP_MANTLE), ND_B(CTP_MANTLE), 1.0);
          cairo_pattern_add_color_stop_rgba(p, 1, ND_R(CTP_MANTLE),
                                            ND_G(CTP_MANTLE), ND_B(CTP_MANTLE), 0.0);
          cairo_set_source(cr, p);
          cairo_rectangle(cr, b->lay.x + 1, b->lay.y + 1, fade, b->lay.h - 2);
          cairo_fill(cr);
          cairo_pattern_destroy(p);
        }
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

      /* A chevron, drawn rather than a glyph so its rotation is exact. */
      {
        double cx = b->lay.x + b->lay.w - 16.0 - 10.0;
        double cy = b->lay.y + 20.0;
        nd_src_a(cr, ct->color, 0.75);
        cairo_set_line_width(cr, 1.6);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
        if (b->callout_folded) {         /* pointing right: collapsed */
          cairo_move_to(cr, cx - 2.0, cy - 4.0);
          cairo_line_to(cr, cx + 2.5, cy);
          cairo_line_to(cr, cx - 2.0, cy + 4.0);
        } else {                          /* pointing down: expanded */
          cairo_move_to(cr, cx - 4.0, cy - 2.0);
          cairo_line_to(cr, cx, cy + 2.5);
          cairo_line_to(cr, cx + 4.0, cy - 2.0);
        }
        cairo_stroke(cr);
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

    case ND_MATH_BLOCK: {
      nd_src(cr, CTP_SURFACE0);
      rounded_rect(cr, b->lay.x, b->lay.y, b->lay.w, b->lay.h, 6.0);
      cairo_fill(cr);

      if (b->lay.pl) {
        nd_src(cr, CTP_LAVENDER);
        cairo_move_to(cr, b->lay.x + 16.0, b->lay.y + 12.0);
        pango_cairo_show_layout(cr, b->lay.pl);
      }

      /* Says plainly that this is maths we are showing, not typesetting. */
      nd_src(cr, CTP_OVERLAY0);
      cairo_select_font_face(cr, ND_MONO, CAIRO_FONT_SLANT_NORMAL,
                             CAIRO_FONT_WEIGHT_NORMAL);
      cairo_set_font_size(cr, 10.0);
      cairo_text_extents_t ext;
      cairo_text_extents(cr, "TeX", &ext);
      cairo_move_to(cr, b->lay.x + b->lay.w - ext.width - 10.0, b->lay.y + 15.0);
      cairo_show_text(cr, "TeX");
      break;
    }

    case ND_HR:
      nd_src(cr, CTP_SURFACE1);
      cairo_rectangle(cr, b->lay.x, b->lay.y, b->lay.w, 1.0);
      cairo_fill(cr);
      break;

    case ND_IMAGE: {
      if (b->lay.img_w > 0 && paint_images) {
        cairo_surface_t *img =
            nd_images_get(paint_images, b->img_src, b->lay.img_w, b->lay.img_h);
        if (img) {
          cairo_save(cr);
          cairo_translate(cr, b->lay.x, b->lay.y);
          cairo_set_source_surface(cr, img, 0, 0);
          cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
          cairo_rectangle(cr, 0, 0, b->lay.img_w, b->lay.img_h);
          cairo_fill(cr);
          cairo_restore(cr);

          if (b->lay.marker) {
            const nd_style *cs = &nd_styles[ND_ST_CAPTION];
            nd_src(cr, cs->color);
            /* The caption is centred over the COLUMN, not the image, so it
             * stays centred when the image is narrower than the text. */
            cairo_move_to(cr, b->lay.x - (b->lay.w - b->lay.img_w) / 2.0,
                          b->lay.y + b->lay.img_h + cs->space_before);
            pango_cairo_show_layout(cr, b->lay.marker);
          }
          break;
        }
      }

      /* Missing, unreadable, or remote: say so rather than drawing nothing. */
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

      /* Marker geometry follows the body style rather than fixed numbers, so
       * changing the type scale (or zooming) keeps bullets and checkboxes
       * aligned with the text they belong to. */
      const nd_style *bs = &nd_styles[ND_ST_BODY];
      double baseline = b->lay.y + bs->line_height * 0.72;

      nd_src(cr, CTP_OVERLAY1);
      cairo_select_font_face(cr, ND_SANS, CAIRO_FONT_SLANT_NORMAL,
                             CAIRO_FONT_WEIGHT_NORMAL);
      cairo_set_font_size(cr, bs->size);
      cairo_move_to(cr, b->lay.x + 8.0, baseline);

      if (b->item_is_task) {
        /* Drawn, not a glyph: a font checkbox gives no control over stroke
         * weight and its centring depends on metrics we would have to fight. */
        double s = bs->size;
        double bx = b->lay.x + 6.0;
        double by = b->lay.y + (bs->line_height - s) / 2.0;
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
                   double viewport_h, struct nd_image_cache *images) {
  double y0 = scroll_y, y1 = scroll_y + viewport_h;
  paint_images = images;

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
