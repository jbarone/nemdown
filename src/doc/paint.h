/* paint.h — internal paint pass. */

#ifndef NEMDOWN_PAINT_H
#define NEMDOWN_PAINT_H

#include <cairo.h>

#include "doc/node.h"

struct nd_image_cache;

void nd_paint_tree(cairo_t *cr, nd_block *root, double scroll_y,
                   double viewport_h, struct nd_image_cache *images);

#endif /* NEMDOWN_PAINT_H */
