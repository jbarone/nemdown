/* select.h — cross-block text selection.
 *
 * Copies RENDERED TEXT, not markdown source. md4c's text callback carries no
 * source offset (task_mark_offset is the sole exception in its API), so
 * mapping rendered characters back to source bytes is not reachable without
 * reimplementing source tracking across the parser.
 */

#ifndef NEMDOWN_SELECT_H
#define NEMDOWN_SELECT_H

#include "doc/doc.h"
#include "doc/node.h"

typedef struct {
  uint32_t block; /* index into the flattened leaf order */
  uint32_t byte;  /* byte offset within that block's text */
  bool     valid;
} nd_point;

typedef struct {
  nd_point anchor, focus;
  bool     active;
} nd_selection;

/* Resolves a document-space point to a selectable position. */
bool nd_select_point_at(nd_block *root, double x, double doc_y, nd_point *out);

/* Normalised range, anchor-before-focus. */
void nd_select_range(const nd_selection *sel, nd_point *lo, nd_point *hi);

/* Paints selection rectangles behind the glyphs. */
void nd_select_paint(cairo_t *cr, nd_block *root, const nd_selection *sel);

/* Concatenates the selected text. Caller frees. NULL when empty. */
char *nd_select_copy(nd_block *root, const nd_selection *sel);

/* Word and block extents, for double- and triple-click. */
void nd_select_word_at(nd_block *root, nd_point p, nd_point *lo, nd_point *hi);
void nd_select_block_at(nd_block *root, nd_point p, nd_point *lo, nd_point *hi);

#endif /* NEMDOWN_SELECT_H */
