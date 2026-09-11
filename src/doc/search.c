/* search.c — find and highlight matches. */

#include "doc/search.h"

#include <glib.h>
#include <pango/pangocairo.h>
#include <stdlib.h>
#include <string.h>

#include "ui/theme.h"

/* Highlighting every hit in a very large document is neither useful nor
 * affordable; editors cap this for the same reason. */
#define ND_MAX_MATCHES 20000

static void push(nd_matches *m, nd_block *b, uint32_t s, uint32_t e) {
  if (m->count >= ND_MAX_MATCHES) return;
  if (m->count == m->cap) {
    uint32_t cap = m->cap ? m->cap * 2 : 64;
    nd_match *grown = realloc(m->items, (size_t)cap * sizeof *grown);
    if (!grown) return;
    m->items = grown;
    m->cap = cap;
  }
  nd_match *mt = &m->items[m->count];
  mt->block = b;
  mt->start = s;
  mt->end = e;
  mt->line = 0;
  mt->y = b->lay.y;
  mt->h = 0;
  m->count++;
}

/* Case folding is done with g_utf8_casefold rather than strcasestr so that
 * non-ASCII text matches the way a reader expects. Folding can change byte
 * length, so matches are found on the folded copy and mapped back through a
 * per-character offset table. */
static void search_block(nd_block *b, const char *folded_needle,
                         size_t needle_len, nd_matches *out) {
  uint32_t blen = 0;
  const char *btext = nd_block_text(b, &blen);
  if (!btext || blen == 0 || !b->lay.pl) return;

  char *folded = g_utf8_casefold(btext, (gssize)blen);
  if (!folded) return;

  /* Map each folded byte position back to an original byte position. */
  size_t flen = strlen(folded);
  uint32_t *map = g_malloc0((flen + 1) * sizeof *map);
  {
    const char *o = btext;
    const char *f = folded;
    const char *oend = btext + blen;
    while (*f && o < oend) {
      gunichar oc = g_utf8_get_char(o);
      char *ofold = g_utf8_casefold(o, g_utf8_next_char(o) - o);
      size_t n = ofold ? strlen(ofold) : 1;
      for (size_t k = 0; k < n && (size_t)(f - folded) + k <= flen; k++)
        map[(f - folded) + k] = (uint32_t)(o - btext);
      g_free(ofold);
      f += n;
      o = g_utf8_next_char(o);
      (void)oc;
    }
    map[flen] = blen;
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

/* Give every match in [from, out->count) its line index and document-space y.
 *
 * One pass over the layout, not one lookup per match: both
 * pango_layout_index_to_line_x and pango_layout_get_line_readonly walk the
 * line list from the start, so per-match lookup is quadratic in a document
 * that is one long paragraph — which is exactly the shape that made this
 * 79 seconds a frame. Matches and lines are both in ascending order, so a
 * single merge assigns all of them.
 *
 * get_line_yrange is the right call here: line extents are relative to the
 * LINE, so using them as a document offset silently defeats viewport culling. */
static void assign_positions(nd_block *b, nd_matches *out, uint32_t from) {
  if (!b->lay.pl || from >= out->count) return;

  PangoLayoutIter *iter = pango_layout_get_iter(b->lay.pl);
  uint32_t k = from;
  int line_no = 0;

  do {
    PangoLayoutLine *line = pango_layout_iter_get_line_readonly(iter);
    int ly0 = 0, ly1 = 0;
    pango_layout_iter_get_line_yrange(iter, &ly0, &ly1);

    uint32_t le = (uint32_t)(line->start_index + line->length);
    while (k < out->count && out->items[k].start < le) {
      out->items[k].line = line_no;
      out->items[k].y = b->lay.y + (double)ly0 / PANGO_SCALE;
      out->items[k].h = (double)(ly1 - ly0) / PANGO_SCALE;
      k++;
    }
    line_no++;
  } while (k < out->count && pango_layout_iter_next_line(iter));

  pango_layout_iter_free(iter);
}

static void walk(nd_block *b, const char *needle, size_t nlen, nd_matches *out) {
  uint32_t before = out->count;
  search_block(b, needle, nlen, out);
  assign_positions(b, out, before);
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

/* Matches arrive grouped by block (the walk searches a block fully before
 * descending) and sorted by offset within it, which is what lets this paint in
 * one pass.
 *
 * The obvious shape — for each match, iterate the block's lines — is quadratic:
 * a single paragraph can be the whole document, so every one of N matches walks
 * all L lines. On a 312KB file with 86k matches that was 79 SECONDS per frame,
 * which is a permanent freeze, not a slowdown. Iterating lines on the outside
 * and binary-searching the matches for each line makes it linear in what is
 * actually on screen. */
/* Each match already knows its own line and y, resolved at search time, so
 * this is O(visible matches) with no layout walking at all. */
void nd_search_paint(cairo_t *cr, const nd_matches *m, double y0, double y1) {
  for (uint32_t i = 0; i < m->count; i++) {
    const nd_match *mt = &m->items[i];
    nd_block *b = mt->block;
    if (!b->lay.pl) continue;
    if (mt->y + mt->h < y0 || mt->y > y1) continue;

    PangoLayoutLine *line = pango_layout_get_line_readonly(b->lay.pl, mt->line);
    if (!line) continue;

    int *ranges = NULL, nranges = 0;
    /* A match that wraps across lines highlights only its first line. Search
     * terms that wrap are rare enough not to warrant walking for the rest. */
    pango_layout_line_get_x_ranges(line, (int)mt->start, (int)mt->end,
                                   &ranges, &nranges);

    /* The active match takes the accent; the rest stay dim, so "which one am
     * I on" is answerable at a glance. */
    if ((int)i == m->current) nd_src_a(cr, CTP_TEAL, 0.55);
    else                      nd_src_a(cr, CTP_YELLOW, 0.28);

    for (int r = 0; r < nranges; r++) {
      double x0 = (double)ranges[r * 2] / PANGO_SCALE;
      double x1 = (double)ranges[r * 2 + 1] / PANGO_SCALE;
      cairo_rectangle(cr, b->lay.x + x0, mt->y, x1 - x0, mt->h);
    }
    cairo_fill(cr);
    g_free(ranges);
  }
}
