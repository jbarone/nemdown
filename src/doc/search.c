/* search.c — find and highlight matches. */

#include "doc/search.h"

#include <glib.h>
#include <pango/pangocairo.h>
#include <stdlib.h>
#include <string.h>

#include "ui/theme.h"

static void push(nd_matches *m, nd_block *b, uint32_t s, uint32_t e) {
  if (m->count == m->cap) {
    m->cap = m->cap ? m->cap * 2 : 64;
    m->items = realloc(m->items, m->cap * sizeof *m->items);
  }
  m->items[m->count].block = b;
  m->items[m->count].start = s;
  m->items[m->count].end = e;
  m->items[m->count].y = b->lay.y;
  m->count++;
}

/* Case folding is done with g_utf8_casefold rather than strcasestr so that
 * non-ASCII text matches the way a reader expects. Folding can change byte
 * length, so matches are found on the folded copy and mapped back through a
 * per-character offset table. */
static void search_block(nd_block *b, const char *folded_needle,
                         size_t needle_len, nd_matches *out) {
  if (!b->inl.text || b->inl.len == 0 || !b->lay.pl) return;

  char *folded = g_utf8_casefold(b->inl.text, (gssize)b->inl.len);
  if (!folded) return;

  /* Map each folded byte position back to an original byte position. */
  size_t flen = strlen(folded);
  uint32_t *map = g_malloc0((flen + 1) * sizeof *map);
  {
    const char *o = b->inl.text;
    const char *f = folded;
    const char *oend = b->inl.text + b->inl.len;
    while (*f && o < oend) {
      gunichar oc = g_utf8_get_char(o);
      char *ofold = g_utf8_casefold(o, g_utf8_next_char(o) - o);
      size_t n = ofold ? strlen(ofold) : 1;
      for (size_t k = 0; k < n && (size_t)(f - folded) + k <= flen; k++)
        map[(f - folded) + k] = (uint32_t)(o - b->inl.text);
      g_free(ofold);
      f += n;
      o = g_utf8_next_char(o);
      (void)oc;
    }
    map[flen] = b->inl.len;
  }

  const char *p = folded;
  while ((p = strstr(p, folded_needle)) != NULL) {
    size_t fs = (size_t)(p - folded);
    size_t fe = fs + needle_len;
    if (fe > flen) break;
    push(out, b, map[fs], map[fe]);
    p += needle_len;
  }

  g_free(map);
  g_free(folded);
}

static void walk(nd_block *b, const char *needle, size_t nlen, nd_matches *out) {
  search_block(b, needle, nlen, out);
  for (uint32_t i = 0; i < b->nkids; i++) walk(b->kids[i], needle, nlen, out);
}

void nd_search_run(nd_block *root, const char *needle, nd_matches *out) {
  out->count = 0;
  out->current = -1;
  if (!needle || !*needle) return;

  char *folded = g_utf8_casefold(needle, -1);
  if (!folded) return;

  walk(root, folded, strlen(folded), out);
  if (out->count) out->current = 0;
  g_free(folded);
}

void nd_search_free(nd_matches *m) {
  free(m->items);
  m->items = NULL;
  m->count = m->cap = 0;
  m->current = -1;
}

void nd_search_paint(cairo_t *cr, const nd_matches *m) {
  for (uint32_t i = 0; i < m->count; i++) {
    const nd_match *mt = &m->items[i];
    nd_block *b = mt->block;
    if (!b->lay.pl) continue;

    bool current = ((int)i == m->current);

    PangoLayoutIter *iter = pango_layout_get_iter(b->lay.pl);
    do {
      PangoLayoutLine *line = pango_layout_iter_get_line_readonly(iter);
      int *ranges = NULL, nranges = 0;
      pango_layout_line_get_x_ranges(line, (int)mt->start, (int)mt->end,
                                     &ranges, &nranges);
      PangoRectangle lr;
      pango_layout_iter_get_line_extents(iter, NULL, &lr);

      for (int r = 0; r < nranges; r++) {
        double x0 = (double)ranges[r * 2] / PANGO_SCALE;
        double x1 = (double)ranges[r * 2 + 1] / PANGO_SCALE;
        /* The active match is the accent; the rest are dimmer, so "which one
         * am I on" is answerable at a glance. */
        if (current) nd_src_a(cr, CTP_TEAL, 0.55);
        else         nd_src_a(cr, CTP_YELLOW, 0.28);
        cairo_rectangle(cr, b->lay.x + x0,
                        b->lay.y + (double)lr.y / PANGO_SCALE,
                        x1 - x0, (double)lr.height / PANGO_SCALE);
        cairo_fill(cr);
      }
      g_free(ranges);
    } while (pango_layout_iter_next_line(iter));
    pango_layout_iter_free(iter);
  }
}
