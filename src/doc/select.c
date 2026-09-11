/* select.c — selection across blocks. */

#include "doc/select.h"

#include <glib.h>
#include <pango/pangocairo.h>
#include <stdlib.h>
#include <string.h>

#include "ui/theme.h"

/* Selection needs a stable linear order over the blocks that hold text. The
 * tree is walked in document order and leaves are numbered; indices stay valid
 * as long as the tree does, which is all selection needs. */
#define MAX_LEAVES 8192

struct leaves {
  nd_block *b[MAX_LEAVES];
  uint32_t  n;
};

static void collect(nd_block *b, struct leaves *out) {
  if (out->n >= MAX_LEAVES) return;
  uint32_t len = 0;
  nd_block_text(b, &len);
  if (b->lay.pl && len > 0) out->b[out->n++] = b;
  for (uint32_t i = 0; i < b->nkids; i++) collect(b->kids[i], out);
}

static void leaves_of(nd_block *root, struct leaves *out) {
  out->n = 0;
  collect(root, out);
}

static int point_cmp(nd_point a, nd_point b) {
  if (a.block != b.block) return a.block < b.block ? -1 : 1;
  if (a.byte  != b.byte)  return a.byte  < b.byte  ? -1 : 1;
  return 0;
}

void nd_select_range(const nd_selection *sel, nd_point *lo, nd_point *hi) {
  if (point_cmp(sel->anchor, sel->focus) <= 0) {
    *lo = sel->anchor; *hi = sel->focus;
  } else {
    *lo = sel->focus;  *hi = sel->anchor;
  }
}

bool nd_select_point_at(nd_block *root, double x, double doc_y, nd_point *out) {
  struct leaves lv;
  leaves_of(root, &lv);
  out->valid = false;

  nd_block *best = NULL;
  uint32_t best_i = 0;

  for (uint32_t i = 0; i < lv.n; i++) {
    nd_block *b = lv.b[i];
    if (doc_y >= b->lay.y && doc_y <= b->lay.y + b->lay.h) { best = b; best_i = i; break; }
    /* Past the end: remember the last block above the point so a drag below
     * the document still selects to the end. */
    if (b->lay.y + b->lay.h < doc_y) { best = b; best_i = i; }
  }
  if (!best) return false;

  double lx = x - best->lay.x, ly = doc_y - best->lay.y;
  int index = 0, trailing = 0;
  pango_layout_xy_to_index(best->lay.pl, (int)(lx * PANGO_SCALE),
                           (int)(ly * PANGO_SCALE), &index, &trailing);

  /* `trailing` counts CHARACTERS past index, not bytes. Adding it directly
   * would land mid-sequence on any non-ASCII text and split a codepoint. */
  uint32_t tlen = 0;
  const char *text = nd_block_text(best, &tlen);
  uint32_t byte = (uint32_t)index;
  if (byte > tlen) byte = tlen;
  for (int t = 0; t < trailing && byte < tlen; t++) {
    const char *next = g_utf8_next_char(text + byte);
    byte = (uint32_t)(next - text);
  }
  if (byte > tlen) byte = tlen;

  out->block = best_i;
  out->byte = byte;
  out->valid = true;
  return true;
}

void nd_select_paint(cairo_t *cr, nd_block *root, const nd_selection *sel) {
  if (!sel->active || !sel->anchor.valid || !sel->focus.valid) return;

  nd_point lo, hi;
  nd_select_range(sel, &lo, &hi);
  if (point_cmp(lo, hi) == 0) return;

  struct leaves lv;
  leaves_of(root, &lv);

  nd_src_a(cr, CTP_SURFACE2, 0.55);

  for (uint32_t i = lo.block; i <= hi.block && i < lv.n; i++) {
    nd_block *b = lv.b[i];
    uint32_t blen = 0;
    nd_block_text(b, &blen);
    uint32_t s = (i == lo.block) ? lo.byte : 0;
    uint32_t e = (i == hi.block) ? hi.byte : blen;
    if (s >= e) continue;

    /* x-ranges per line handle a selection that wraps mid-paragraph. */
    PangoLayoutIter *iter = pango_layout_get_iter(b->lay.pl);
    do {
      PangoLayoutLine *line = pango_layout_iter_get_line_readonly(iter);
      int *ranges = NULL, nranges = 0;
      pango_layout_line_get_x_ranges(line, (int)s, (int)e, &ranges, &nranges);

      PangoRectangle lr;
      pango_layout_iter_get_line_extents(iter, NULL, &lr);

      for (int r = 0; r < nranges; r++) {
        double x0 = (double)ranges[r * 2] / PANGO_SCALE;
        double x1 = (double)ranges[r * 2 + 1] / PANGO_SCALE;
        cairo_rectangle(cr, b->lay.x + x0,
                        b->lay.y + (double)lr.y / PANGO_SCALE,
                        x1 - x0, (double)lr.height / PANGO_SCALE);
      }
      g_free(ranges);
    } while (pango_layout_iter_next_line(iter));
    pango_layout_iter_free(iter);
  }
  cairo_fill(cr);
}

char *nd_select_copy(nd_block *root, const nd_selection *sel) {
  if (!sel->anchor.valid || !sel->focus.valid) return NULL;

  nd_point lo, hi;
  nd_select_range(sel, &lo, &hi);
  if (point_cmp(lo, hi) == 0) return NULL;

  struct leaves lv;
  leaves_of(root, &lv);

  GString *out = g_string_new(NULL);
  for (uint32_t i = lo.block; i <= hi.block && i < lv.n; i++) {
    nd_block *b = lv.b[i];
    uint32_t blen = 0;
    const char *btext = nd_block_text(b, &blen);
    uint32_t s = (i == lo.block) ? lo.byte : 0;
    uint32_t e = (i == hi.block) ? hi.byte : blen;
    if (s > blen) s = blen;
    if (e > blen) e = blen;
    if (s >= e) continue;

    if (out->len) g_string_append_c(out, '\n');
    g_string_append_len(out, btext + s, (gssize)(e - s));
  }

  if (!out->len) { g_string_free(out, TRUE); return NULL; }
  return g_string_free(out, FALSE); /* caller frees with g_free */
}

void nd_select_word_at(nd_block *root, nd_point p, nd_point *lo, nd_point *hi) {
  struct leaves lv;
  leaves_of(root, &lv);
  *lo = *hi = p;
  if (p.block >= lv.n) return;

  nd_block *b = lv.b[p.block];
  uint32_t n = 0;
  const char *t = nd_block_text(b, &n);
  if (!t || n == 0) return;

  uint32_t i = p.byte;
  if (i > n) i = n;

  /* Pango's own word boundaries rather than splitting on whitespace, so
   * double-clicking `main(int` in a code fence takes `main`, not both. */
  glong nchars = g_utf8_strlen(t, (gssize)n);
  PangoLogAttr *attrs = g_malloc0((size_t)(nchars + 1) * sizeof *attrs);
  pango_get_log_attrs(t, (int)n, -1, NULL, attrs, (int)(nchars + 1));

  /* Byte offset -> character index. */
  glong ci = g_utf8_pointer_to_offset(t, t + i);
  if (ci > nchars) ci = nchars;

  glong start = ci;
  while (start > 0 && !attrs[start].is_word_start) start--;
  glong end = ci;
  while (end < nchars && !attrs[end].is_word_end) end++;

  /* Clicking in the gap between words yields an empty range; fall back to the
   * character under the pointer so a double-click always selects something. */
  if (end <= start) end = (start < nchars) ? start + 1 : start;

  lo->byte = (uint32_t)(g_utf8_offset_to_pointer(t, start) - t);
  hi->byte = (uint32_t)(g_utf8_offset_to_pointer(t, end) - t);

  g_free(attrs);
}

void nd_select_block_at(nd_block *root, nd_point p, nd_point *lo, nd_point *hi) {
  struct leaves lv;
  leaves_of(root, &lv);
  *lo = *hi = p;
  lo->byte = 0;
  uint32_t blen = 0;
  if (p.block < lv.n) nd_block_text(lv.b[p.block], &blen);
  hi->byte = blen;
}
