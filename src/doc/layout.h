/* layout.h — internal layout pass over the block tree. */

#ifndef NEMDOWN_LAYOUT_H
#define NEMDOWN_LAYOUT_H

#include <pango/pangocairo.h>

#include "doc/arena.h"
#include "doc/node.h"
#include "ui/typography.h"

struct nd_image_cache;

struct nd_layout_ctx {
  PangoContext    *pctx;
  struct nd_arena *arena; /* for highlighter token spans */
  struct nd_image_cache *images;
  double           font_scale;
  double           column_x, column_w;
};

/* Builds the shared Pango context. Metrics hinting is OFF, which is what makes
 * layout scale-invariant; see the comment in the implementation. */
PangoContext *nd_pango_context_new(void);

PangoLayout *nd_layout_for(struct nd_layout_ctx *ctx, const nd_inline *inl,
                           const nd_style *st, double width);

/* Lays the tree out and returns total content height. */
double nd_layout_tree(struct nd_layout_ctx *ctx, nd_block *root);

void nd_layout_free_tree(nd_block *root);

/* A task cell's text is pushed right by this much to make room for its box. */
double nd_cell_task_indent(void);

/* Where a task item's or task cell's checkbox is drawn: one function for the
 * painter and the hit test, so the box and its target cannot drift apart. */
void nd_task_box(const nd_block *b, double *bx, double *by, double *s);

#endif /* NEMDOWN_LAYOUT_H */
