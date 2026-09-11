/* search.h — case-insensitive full-document search over rendered text. */

#ifndef NEMDOWN_SEARCH_H
#define NEMDOWN_SEARCH_H

#include <cairo.h>

#include "doc/node.h"

typedef struct {
  nd_block *block;
  uint32_t  start, end; /* byte range within the block's text */
  double    y, h;       /* document-space position of the match's own LINE */
  int       line;       /* line index within the block's layout */
} nd_match;

typedef struct {
  nd_match *items;
  uint32_t  count, cap;
  int       current;    /* index into items, or -1 */
} nd_matches;

/* Matching folds case via GLib, so it is UTF-8 aware rather than ASCII-only. */
void nd_search_run(nd_block *root, const char *needle, nd_matches *out);
void nd_search_free(nd_matches *m);
/* y0/y1 bound the visible band in document space. Without them this walks
 * every match in the document, and every line of each match's block, on every
 * frame — which is quadratic in match count and freezes the app outright. */
void nd_search_paint(cairo_t *cr, const nd_matches *m, double y0, double y1);

#endif /* NEMDOWN_SEARCH_H */
