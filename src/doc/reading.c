/* reading.c — the document's reading stops.
 *
 * A flat, document-ordered list of the things a person reads one at a time, so
 * the shell can show which one they are on. It is deliberately coarser than
 * blocks and finer than top-level blocks: a list is not one stop, because a
 * ten-item list would put the marker beside half a screen and say nothing, and
 * a list item is not split further, because a stop you cannot finish is worse
 * than no stop at all.
 *
 * Rebuilt on every layout, because it is pure geometry and the geometry moves
 * with the column width.
 */

#include "doc/reading.h"

#include <stdlib.h>

static void stops_push(struct nd_reading *r, const nd_block *b) {
  if (r->n == r->cap) {
    uint32_t cap = r->cap ? r->cap * 2 : 64;
    nd_para *p = realloc(r->stops, cap * sizeof *p);
    if (!p) return;   /* out of memory: stop growing, keep what we have */
    r->stops = p;
    r->cap = cap;
  }
  r->stops[r->n].y = b->lay.y;
  r->stops[r->n].h = b->lay.h;
  r->stops[r->n].x = b->lay.x;
  r->stops[r->n].w = b->lay.w;
  r->n++;
}

static void walk(struct nd_reading *r, const nd_block *b) {
  for (uint32_t i = 0; i < b->nkids; i++) {
    const nd_block *k = b->kids[i];
    switch (k->kind) {
      case ND_HR:
        /* Nothing to read. Skipping it also stops the marker flickering
         * through a zero-height block on the way past. */
        break;

      case ND_LIST:
        /* Descend to the items, but no further: a nested list's items are
         * reached by this same recursion, and an item's own paragraphs are
         * one thought. */
        walk(r, k);
        break;

      case ND_ITEM: {
        /* An item that only contains a nested list is a header for it, not a
         * stop of its own -- otherwise the marker pauses on nothing. */
        bool only_list = k->nkids > 0;
        for (uint32_t j = 0; j < k->nkids; j++)
          if (k->kids[j]->kind != ND_LIST) { only_list = false; break; }
        if (!only_list && k->lay.h > 0) stops_push(r, k);
        for (uint32_t j = 0; j < k->nkids; j++)
          if (k->kids[j]->kind == ND_LIST) walk(r, k->kids[j]);
        break;
      }

      default:
        /* Everything else is read as a unit: paragraphs, headings, code
         * fences, tables, images, quotes and callouts. A callout is one
         * thought even though it contains paragraphs. */
        if (k->lay.h > 0) stops_push(r, k);
        break;
    }
  }
}

void nd_reading_build(struct nd_reading *r, const nd_block *root) {
  r->n = 0;
  if (root) walk(r, root);
}

void nd_reading_free(struct nd_reading *r) {
  free(r->stops);
  r->stops = NULL;
  r->n = r->cap = 0;
}

/* How much of a stop has to remain on screen for it to still be the one being
 * read. About a line: below that there is nothing left to read, and keeping
 * the marker there points at text that has already gone. */
#define ND_READ_MIN_VISIBLE 24.0

int nd_reading_at(const struct nd_reading *r, double doc_y) {
  if (r->n == 0) return -1;

  /* Find the first stop not entirely above the fold... */
  uint32_t lo = 0, hi = r->n;
  while (lo < hi) {
    uint32_t m = (lo + hi) / 2;
    if (r->stops[m].y + r->stops[m].h <= doc_y) lo = m + 1;
    else hi = m;
  }

  /* ...then hand on while only a sliver of it is left. Without this a
   * paragraph stays current until its last pixel leaves, so the marker ends up
   * beside text that is no longer on screen -- which is worse than useless,
   * because it points confidently at nothing. Each step strictly increases the
   * bottom edge, so this advances at most a couple of times. */
  while (lo < r->n) {
    double h = r->stops[lo].h;
    double want = h < ND_READ_MIN_VISIBLE ? h : ND_READ_MIN_VISIBLE;
    if (r->stops[lo].y + h - doc_y >= want) break;
    lo++;
  }

  return (int)(lo < r->n ? lo : r->n - 1);
}
