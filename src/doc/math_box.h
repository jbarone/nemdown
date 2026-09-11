/* math_box.h — the laid-out box, shared between math_box.c and math_paint.c.
 *
 * Private to the math engine: everything outside it sees only nd_box as an
 * opaque type through math.h.
 */

#ifndef NEMDOWN_MATH_BOX_H
#define NEMDOWN_MATH_BOX_H

#include <cairo.h>
#include <stdbool.h>
#include <stdint.h>

/* TeX's three dimensions. Height is above the baseline and depth below, so a
 * box drops into a line of prose knowing exactly where its baseline sits. */
struct nd_box {
  double w, h, d;

  cairo_scaled_font_t *sf;      /* set when this box draws glyphs */
  cairo_glyph_t       *g;
  uint32_t             ng;

  bool is_rule;                 /* a filled rect: fraction bars, radical bars */

  struct nd_box **kids;
  double         *kx, *ky;      /* child offsets; ky is a baseline shift, down+ */
  uint32_t        nkids;
};

#endif /* NEMDOWN_MATH_BOX_H */
