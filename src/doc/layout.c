/* layout.c — measure the block tree into per-block geometry. */

#include "doc/layout.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "doc/callout.h"
#include "doc/highlight.h"
#include "doc/images.h"
#include "ui/theme.h"

#define ND_LIST_INDENT   28.0
#define ND_MARKER_COL    20.0
#define ND_QUOTE_INDENT  20.0
#define ND_CODE_PAD_X    16.0
#define ND_CODE_PAD_Y    14.0
#define ND_CELL_PAD_X    12.0
#define ND_CELL_PAD_Y     8.0
#define ND_HR_SPACE      24.0
#define ND_LIST_SPACE    12.0
#define ND_CALLOUT_PAD_X 16.0
#define ND_CALLOUT_PAD_Y 12.0
#define ND_CALLOUT_ICON  26.0
#define ND_IMG_MAX_H    600.0
#define ND_IMG_FAIL_H    96.0
#define ND_MATH_PAD_X    16.0
#define ND_MATH_PAD_Y    12.0

PangoContext *nd_pango_context_new(void) {
  PangoFontMap *fm = pango_cairo_font_map_get_default();
  PangoContext *pctx = pango_font_map_create_context(fm);

  /* Pango pixels == CSS pixels, so the numbers in typography.c mean what they
   * say regardless of any system DPI setting. */
  pango_cairo_context_set_resolution(pctx, 96.0);

  cairo_font_options_t *fo = cairo_font_options_create();
  /* THE load-bearing line. With hinted metrics, glyph advances snap to integer
   * DEVICE pixels, which differ between scale 1.0 and 1.5 — so layout would
   * change with display scale, forcing a relayout whenever the window moves
   * between monitors and letting hit-test geometry drift. With hinting off,
   * metrics are real-valued and layout is exactly scale-invariant. */
  cairo_font_options_set_hint_metrics(fo, CAIRO_HINT_METRICS_OFF);
  cairo_font_options_set_hint_style(fo, CAIRO_HINT_STYLE_SLIGHT);
  /* Subpixel AA assumes a known physical RGB layout, but the compositor may
   * rescale our buffer. Grayscale is the honest choice. */
  cairo_font_options_set_antialias(fo, CAIRO_ANTIALIAS_GRAY);
  pango_cairo_context_set_font_options(pctx, fo);
  cairo_font_options_destroy(fo);

  return pctx;
}

static PangoFontDescription *desc_for(const nd_style *st, double font_scale) {
  PangoFontDescription *fd = pango_font_description_new();
  pango_font_description_set_family(fd, st->family);
  pango_font_description_set_weight(
      fd, st->bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL);
  pango_font_description_set_style(
      fd, st->italic ? PANGO_STYLE_ITALIC : PANGO_STYLE_NORMAL);
  /* Absolute size: we want pixels, not points. */
  pango_font_description_set_absolute_size(
      fd, st->size * font_scale * PANGO_SCALE);
  return fd;
}

static void attr_add(PangoAttrList *al, PangoAttribute *a,
                     uint32_t start, uint32_t end) {
  a->start_index = start;
  a->end_index = end;
  pango_attr_list_insert(al, a); /* the list takes ownership */
}

static PangoAttribute *fg(uint32_t hex) {
  return pango_attr_foreground_new(ND_P16R(hex), ND_P16G(hex), ND_P16B(hex));
}

static PangoAttribute *bg(uint32_t hex) {
  return pango_attr_background_new(ND_P16R(hex), ND_P16G(hex), ND_P16B(hex));
}

/* Runs arrive sorted by nesting depth, outermost first, so inner styles are
 * inserted later and win same-type conflicts. */
static PangoAttrList *attrs_for(const nd_inline *inl, const nd_style *st,
                                double font_scale) {
  PangoAttrList *al = pango_attr_list_new();

  /* An exact line box, rather than set_spacing() which adds to the font's
   * natural height and so varies by family. */
  attr_add(al, pango_attr_line_height_new_absolute(
                   (int)(st->line_height * font_scale * PANGO_SCALE)),
           0, G_MAXUINT);

  for (uint32_t i = 0; i < inl->nruns; i++) {
    const nd_run *r = &inl->runs[i];
    if (r->start >= r->end) continue;
    uint32_t s = r->start, e = r->end, f = r->flags;

    if (f & ND_RUN_STRONG)
      attr_add(al, pango_attr_weight_new(PANGO_WEIGHT_BOLD), s, e);
    if (f & ND_RUN_EM)
      attr_add(al, pango_attr_style_new(PANGO_STYLE_ITALIC), s, e);
    if (f & ND_RUN_U)
      attr_add(al, pango_attr_underline_new(PANGO_UNDERLINE_SINGLE), s, e);

    if (f & ND_RUN_DEL) {
      attr_add(al, pango_attr_strikethrough_new(TRUE), s, e);
      attr_add(al, fg(CTP_OVERLAY1), s, e);
    }
    if (f & ND_RUN_HIGHLIGHT) {
      attr_add(al, bg(CTP_YELLOW), s, e);
      attr_add(al, fg(CTP_BASE), s, e);
    }
    if (f & (ND_RUN_LINK | ND_RUN_WIKILINK)) {
      if (f & ND_RUN_WIKILINK_DEAD) {
        /* Present, but visibly going nowhere — the same courtesy Obsidian
         * extends to an unresolved link. */
        attr_add(al, fg(CTP_OVERLAY1), s, e);
        attr_add(al, pango_attr_underline_new(PANGO_UNDERLINE_DOUBLE), s, e);
        attr_add(al, pango_attr_underline_color_new(ND_P16R(CTP_SURFACE2),
                                                    ND_P16G(CTP_SURFACE2),
                                                    ND_P16B(CTP_SURFACE2)), s, e);
      } else {
        uint32_t c = (f & ND_RUN_WIKILINK) ? CTP_LAVENDER : CTP_BLUE;
        attr_add(al, fg(c), s, e);
        attr_add(al, pango_attr_underline_new(PANGO_UNDERLINE_SINGLE), s, e);
      }
    }
    /* Innermost-looking styles last so they win. */
    if (f & ND_RUN_MATH) {
      attr_add(al, pango_attr_family_new(ND_MONO), s, e);
      attr_add(al, pango_attr_style_new(PANGO_STYLE_ITALIC), s, e);
      attr_add(al, fg(CTP_LAVENDER), s, e);
      attr_add(al, bg(CTP_SURFACE0), s, e);
    }
    if (f & ND_RUN_CODE) {
      attr_add(al, pango_attr_family_new(ND_MONO), s, e);
      attr_add(al, pango_attr_scale_new(0.90), s, e);
      attr_add(al, fg(CTP_PEACH), s, e);
      attr_add(al, bg(CTP_SURFACE0), s, e);
      attr_add(al, pango_attr_insert_hyphens_new(FALSE), s, e);
    }
  }
  return al;
}

PangoLayout *nd_layout_for(struct nd_layout_ctx *ctx, const nd_inline *inl,
                           const nd_style *st, double width) {
  PangoLayout *pl = pango_layout_new(ctx->pctx);

  PangoFontDescription *fd = desc_for(st, ctx->font_scale);
  pango_layout_set_font_description(pl, fd);
  pango_font_description_free(fd);

  pango_layout_set_text(pl, inl->text ? inl->text : "", (int)inl->len);

  PangoAttrList *al = attrs_for(inl, st, ctx->font_scale);
  pango_layout_set_attributes(pl, al);
  pango_attr_list_unref(al);

  if (width > 0) {
    pango_layout_set_width(pl, (int)(width * PANGO_SCALE));
    pango_layout_set_wrap(pl, PANGO_WRAP_WORD_CHAR);
  } else {
    pango_layout_set_width(pl, -1);
  }
  return pl;
}

static double layout_block(struct nd_layout_ctx *ctx, nd_block *b,
                           double x, double y, double w);

/* Gap between siblings is max(before, after), not the sum: CSS-style margin
 * collapsing. Summing gives a heading after a paragraph 16+32=48px and the
 * vertical rhythm falls apart. */
static double layout_children(struct nd_layout_ctx *ctx, nd_block *b,
                              double x, double y, double w) {
  double prev_after = 0.0;
  bool first = true;

  for (uint32_t i = 0; i < b->nkids; i++) {
    nd_block *k = b->kids[i];
    const nd_style *st = &nd_styles[ND_ST_BODY];
    double before = 0.0, after = 0.0;

    switch (k->kind) {
      case ND_HEADING: st = nd_heading_style(k->level); break;
      case ND_CODE:    st = &nd_styles[ND_ST_CODE];     break;
      case ND_QUOTE:   st = &nd_styles[ND_ST_QUOTE];    break;
      default: break;
    }
    before = st->space_before;
    after  = st->space_after;

    if (k->kind == ND_LIST)  { before = ND_LIST_SPACE; after = ND_LIST_SPACE; }
    if (k->kind == ND_HR)    { before = ND_HR_SPACE;   after = ND_HR_SPACE;   }
    if (k->kind == ND_ITEM)  { before = 0; after = b->list_tight ? 2.0 : 8.0; }
    if (k->kind == ND_TABLE) { before = 16.0; after = 16.0; }

    if (!first) y += (prev_after > before) ? prev_after : before;
    first = false;

    y = layout_block(ctx, k, x, y, w);
    prev_after = after;
  }
  return y;
}

static double layout_block(struct nd_layout_ctx *ctx, nd_block *b,
                           double x, double y, double w) {
  b->lay.x = x;
  b->lay.y = y;
  b->lay.w = w;

  switch (b->kind) {
    case ND_PARA:
    case ND_HEADING:
    case ND_CELL: {
      const nd_style *st = (b->kind == ND_HEADING) ? nd_heading_style(b->level)
                         : (b->kind == ND_CELL)    ? &nd_styles[ND_ST_TABLE_CELL]
                                                   : &nd_styles[ND_ST_BODY];
      b->lay.pl = nd_layout_for(ctx, &b->inl, st, w);
      int pw, ph;
      pango_layout_get_pixel_size(b->lay.pl, &pw, &ph);
      b->lay.h = ph;
      y += ph;

      /* A heading may also contain promoted image children. */
      if (b->nkids) y = layout_children(ctx, b, x, y, w);
      break;
    }

    case ND_CODE: {
      const nd_style *st = &nd_styles[ND_ST_CODE];
      nd_inline tmp = {.text = b->code_text, .len = b->code_len,
                       .runs = NULL, .nruns = 0};
      /* No wrapping: code overflows horizontally and is clipped, like Obsidian. */
      b->lay.pl = nd_layout_for(ctx, &tmp, st, -1);

      /* Highlighting runs once, here, and is cached in the retained layout. */
      nd_tokspan *toks = NULL;
      uint32_t ntok = nd_highlight(ctx->arena, b->code_lang, b->code_text,
                                   b->code_len, &toks);
      if (ntok) {
        PangoAttrList *al = pango_layout_get_attributes(b->lay.pl);
        for (uint32_t t = 0; t < ntok; t++) {
          uint32_t kind = toks[t].kind;
          attr_add(al, fg(nd_tok_colors[kind]), toks[t].start, toks[t].end);
          if (nd_tok_italic[kind])
            attr_add(al, pango_attr_style_new(PANGO_STYLE_ITALIC),
                     toks[t].start, toks[t].end);
        }
      }
      int pw, ph;
      pango_layout_get_pixel_size(b->lay.pl, &pw, &ph);
      b->lay.natural_w = pw;
      double pad_top = b->code_lang ? ND_CODE_PAD_Y + 16.0 : ND_CODE_PAD_Y;
      b->lay.h = ph + pad_top + ND_CODE_PAD_Y;

      /* A narrower window means less overflow to pan through; without this the
       * block stays scrolled past its own end. */
      double overflow = b->lay.natural_w - (w - 2 * ND_CODE_PAD_X);
      if (overflow < 0) overflow = 0;
      if (b->lay.hscroll > overflow) b->lay.hscroll = overflow;
      y += b->lay.h;
      break;
    }

    case ND_MATH_BLOCK: {
      const nd_style *st = &nd_styles[ND_ST_MATH];
      b->lay.pl = nd_layout_for(ctx, &b->inl, st, w - 2 * ND_MATH_PAD_X);
      pango_layout_set_alignment(b->lay.pl, PANGO_ALIGN_CENTER);
      int pw, ph;
      pango_layout_get_pixel_size(b->lay.pl, &pw, &ph);
      b->lay.h = ph + 2 * ND_MATH_PAD_Y;
      y += b->lay.h;
      break;
    }

    case ND_HR:
      b->lay.h = 1.0;
      y += 1.0;
      break;

    case ND_QUOTE: {
      double inner_x = x + ND_QUOTE_INDENT;
      double inner_w = w - ND_QUOTE_INDENT;
      double end = layout_children(ctx, b, inner_x, y + 2.0, inner_w);
      b->lay.h = (end + 2.0) - y;
      y = end + 2.0;
      break;
    }

    case ND_CALLOUT: {
      double inner_x = x + ND_CALLOUT_PAD_X;
      double inner_w = w - 2 * ND_CALLOUT_PAD_X;
      double cy = y + ND_CALLOUT_PAD_Y;

      const nd_style *ts = &nd_styles[ND_ST_CALLOUT_TITLE];
      b->lay.marker = nd_layout_for(ctx, &b->title, ts,
                                    inner_w - ND_CALLOUT_ICON);
      int tw, th;
      pango_layout_get_pixel_size(b->lay.marker, &tw, &th);
      cy += th;

      if (!b->callout_folded && b->nkids) {
        cy += 6.0;
        cy = layout_children(ctx, b, inner_x, cy, inner_w);
      }
      b->lay.h = (cy + ND_CALLOUT_PAD_Y) - y;
      y = cy + ND_CALLOUT_PAD_Y;
      break;
    }

    case ND_LIST: {
      double end = layout_children(ctx, b, x, y, w);
      b->lay.h = end - y;
      y = end;
      break;
    }

    case ND_ITEM: {
      nd_block *list = b->parent;
      if (list && list->kind == ND_LIST && list->list_ordered && !b->item_is_task) {
        uint32_t index = 0;
        for (uint32_t i = 0; i < list->nkids; i++)
          if (list->kids[i] == b) { index = i; break; }

        char buf[24];
        snprintf(buf, sizeof buf, "%u.", list->list_start + index);
        nd_inline mark = {.text = buf, .len = (uint32_t)strlen(buf)};
        b->lay.marker = nd_layout_for(ctx, &mark, &nd_styles[ND_ST_BODY], -1);
        /* Tabular figures so `9.` and `10.` share a right edge. */
        PangoAttrList *al = pango_layout_get_attributes(b->lay.marker);
        if (al) {
          PangoAttribute *tn = pango_attr_font_features_new("tnum 1");
          tn->start_index = 0;
          tn->end_index = G_MAXUINT;
          pango_attr_list_insert(al, tn);
        }
        pango_layout_set_alignment(b->lay.marker, PANGO_ALIGN_RIGHT);
      }

      double inner_x = x + ND_LIST_INDENT;
      double inner_w = w - ND_LIST_INDENT;
      double end = layout_children(ctx, b, inner_x, y, inner_w);
      b->lay.h = end - y;
      y = end;
      break;
    }

    case ND_TABLE: {
      uint32_t ncols = 0;
      for (uint32_t r = 0; r < b->nkids; r++)
        if (b->kids[r]->nkids > ncols) ncols = b->kids[r]->nkids;
      if (ncols == 0) { b->lay.h = 0; break; }

      double *natural = calloc(ncols, sizeof *natural);
      double *minimum = calloc(ncols, sizeof *minimum);

      for (uint32_t r = 0; r < b->nkids; r++) {
        nd_block *row = b->kids[r];
        const nd_style *cs = row->row_is_header ? &nd_styles[ND_ST_TABLE_HEAD]
                                                : &nd_styles[ND_ST_TABLE_CELL];
        for (uint32_t c = 0; c < row->nkids && c < ncols; c++) {
          nd_block *cell = row->kids[c];
          PangoLayout *pl = nd_layout_for(ctx, &cell->inl, cs, -1);
          int pw, ph;
          pango_layout_get_pixel_size(pl, &pw, &ph);
          if (pw > natural[c]) natural[c] = pw;
          g_object_unref(pl);

          /* Width 1 forces Pango to wrap at every opportunity, so the reported
           * width is the widest unbreakable unit: the column's true minimum. */
          pl = nd_layout_for(ctx, &cell->inl, cs, 1.0 / PANGO_SCALE);
          pango_layout_set_width(pl, 1);
          pango_layout_get_pixel_size(pl, &pw, &ph);
          if (pw > minimum[c]) minimum[c] = pw;
          g_object_unref(pl);
        }
      }

      double pad = 2 * ND_CELL_PAD_X;
      double total = 0;
      for (uint32_t c = 0; c < ncols; c++) total += natural[c] + pad;

      double *final_w = calloc(ncols, sizeof *final_w);
      if (total <= w) {
        for (uint32_t c = 0; c < ncols; c++) final_w[c] = natural[c];
      } else {
        double over = total - w, headroom = 0;
        for (uint32_t c = 0; c < ncols; c++) headroom += natural[c] - minimum[c];
        for (uint32_t c = 0; c < ncols; c++) {
          double give = headroom > 0 ? (natural[c] - minimum[c]) / headroom * over
                                     : over / ncols;
          final_w[c] = natural[c] - give;
          if (final_w[c] < minimum[c]) final_w[c] = minimum[c];
          if (final_w[c] < 24.0) final_w[c] = 24.0;
        }
      }

      /* Layout results, not document data: they are rebuilt on every reflow,
       * so they are malloc'd and released by nd_layout_free_tree rather than
       * put in the arena, which would grow on every resize. */
      free(b->lay.colw);
      free(b->lay.rowy);
      b->lay.colw = final_w;
      b->lay.rowy = calloc(b->nkids ? b->nkids : 1, sizeof(double));

      double ty = y;
      for (uint32_t r = 0; r < b->nkids; r++) {
        nd_block *row = b->kids[r];
        const nd_style *cs = row->row_is_header ? &nd_styles[ND_ST_TABLE_HEAD]
                                                : &nd_styles[ND_ST_TABLE_CELL];
        double cx = x, rowh = 0;
        b->lay.rowy[r] = ty;
        row->lay.x = x; row->lay.y = ty; row->lay.w = w;

        for (uint32_t c = 0; c < row->nkids && c < ncols; c++) {
          nd_block *cell = row->kids[c];
          cell->lay.pl = nd_layout_for(ctx, &cell->inl, cs, final_w[c]);
          static const PangoAlignment amap[] = {
            PANGO_ALIGN_LEFT, PANGO_ALIGN_LEFT, PANGO_ALIGN_CENTER,
            PANGO_ALIGN_RIGHT
          };
          pango_layout_set_alignment(
              cell->lay.pl, amap[cell->cell_align < 4 ? cell->cell_align : 0]);
          int pw, ph;
          pango_layout_get_pixel_size(cell->lay.pl, &pw, &ph);
          cell->lay.x = cx + ND_CELL_PAD_X;
          cell->lay.y = ty + ND_CELL_PAD_Y;
          cell->lay.w = final_w[c];
          cell->lay.h = ph;
          if (ph > rowh) rowh = ph;
          cx += final_w[c] + pad;
        }
        double full = rowh + 2 * ND_CELL_PAD_Y;
        row->lay.h = full;
        ty += full;
      }

      free(natural); free(minimum);
      b->lay.h = ty - y;
      y = ty;
      break;
    }

    case ND_IMAGE: {
      double nw = 0, nh = 0;
      bool have = ctx->images &&
                  nd_images_probe(ctx->images, b->img_src, &nw, &nh);

      if (!have || nw < 1 || nh < 1) {
        b->lay.img_w = 0;
        b->lay.h = ND_IMG_FAIL_H;
        b->lay.x = x;
        y += b->lay.h;
        break;
      }

      /* An explicit `|400` or `|400x300` hint wins, still clamped to the
       * column; raster is never upscaled past its natural size. */
      double want_w = nw, want_h = nh;
      if (b->img_size && *b->img_size) {
        double hw = 0, hh = 0;
        /* sscanf accepts "inf", "nan" and "1e400". A non-finite hint then
         * propagates through the ratio arithmetic below into NaN, and NaN
         * silently defeats every `>` clamp that follows — so screen it here
         * rather than trying to catch it downstream. */
        if (sscanf(b->img_size, "%lfx%lf", &hw, &hh) == 2 &&
            isfinite(hw) && isfinite(hh) && hw > 0 && hh > 0) {
          want_w = hw; want_h = hh;
        } else if (sscanf(b->img_size, "%lf", &hw) == 1 &&
                   isfinite(hw) && hw > 0) {
          want_w = hw;
          want_h = nh * (hw / nw);
        }
      } else if (want_w > w) {
        want_w = w;
        want_h = nh * (w / nw);
      }

      if (want_w > w) { want_h *= w / want_w; want_w = w; }
      if (want_h > ND_IMG_MAX_H) { want_w *= ND_IMG_MAX_H / want_h;
                                   want_h = ND_IMG_MAX_H; }

      /* Belt and braces: nw/nh come from an image decoder, so even with a sane
       * hint the ratios above could in principle go non-finite. */
      if (!isfinite(want_w) || !isfinite(want_h) || want_w < 1 || want_h < 1) {
        b->lay.img_w = 0;
        b->lay.h = ND_IMG_FAIL_H;
        b->lay.x = x;
        y += b->lay.h;
        break;
      }

      b->lay.img_w = want_w;
      b->lay.img_h = want_h;
      b->lay.x = x + (w - want_w) / 2.0; /* centred in the column */
      b->lay.h = want_h;
      y += want_h;

      if (b->img_caption.len) {
        const nd_style *cs = &nd_styles[ND_ST_CAPTION];
        b->lay.marker = nd_layout_for(ctx, &b->img_caption, cs, w);
        pango_layout_set_alignment(b->lay.marker, PANGO_ALIGN_CENTER);
        int cw, chh;
        pango_layout_get_pixel_size(b->lay.marker, &cw, &chh);
        b->lay.h += cs->space_before + chh;
        y += cs->space_before + chh;
      }
      break;
    }

    default: {
      double end = layout_children(ctx, b, x, y, w);
      b->lay.h = end - y;
      y = end;
      break;
    }
  }

  return y;
}

double nd_layout_tree(struct nd_layout_ctx *ctx, nd_block *root) {
  double y = ND_PAD_TOP;
  y = layout_children(ctx, root, ctx->column_x, y, ctx->column_w);
  root->lay.h = y + ND_PAD_BOTTOM;
  return root->lay.h;
}

void nd_layout_free_tree(nd_block *root) {
  if (!root) return;
  if (root->lay.pl)     { g_object_unref(root->lay.pl);     root->lay.pl = NULL; }
  if (root->lay.marker) { g_object_unref(root->lay.marker); root->lay.marker = NULL; }
  free(root->lay.colw); root->lay.colw = NULL;
  free(root->lay.rowy); root->lay.rowy = NULL;
  for (uint32_t i = 0; i < root->nkids; i++) nd_layout_free_tree(root->kids[i]);
}
