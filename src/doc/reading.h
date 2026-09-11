/* reading.h — reading stops, for the focus marker in the shell.
 *
 * The engine owns the geometry and nothing else: which stop is current, how
 * the marker animates and whether it is shown at all are the shell's, in
 * keeping with the rule that the engine never sees an input event.
 */

#ifndef NEMDOWN_READING_H
#define NEMDOWN_READING_H

#include <stdint.h>

#include "doc/node.h"

/* One readable unit, in document space. */
typedef struct {
  double y, h;   /* vertical extent */
  double x, w;   /* the block's own box, so a marker can hug its column */
} nd_para;

struct nd_reading {
  nd_para *stops;
  uint32_t n, cap;
};

/* Rebuilt after every layout: these are pure geometry. */
void nd_reading_build(struct nd_reading *r, const nd_block *root);
void nd_reading_free(struct nd_reading *r);

/* The stop being read in the viewport [doc_y, doc_y + view_h): the first one
 * wholly on screen. Returns -1 only when the document has no stops. */
int nd_reading_at(const struct nd_reading *r, double doc_y, double view_h);

#endif /* NEMDOWN_READING_H */
