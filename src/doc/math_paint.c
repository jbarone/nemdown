/* math_paint.c — walk the box tree and put glyphs on the surface.
 *
 * Everything interesting already happened in math_box.c; this is a recursive
 * translate-and-draw. The one wrinkle is that glyph positions are stored
 * relative to their own box, so they have to be offset into surface space
 * without mutating the box — the tree is laid out once and painted on every
 * frame, and a formula scrolled twice must not drift.
 */

#include "doc/math.h"

#include <stdlib.h>

#include "doc/math_box.h"

#define STACK_GLYPHS 64

static void paint_box(const nd_box *b, cairo_t *cr, double x, double y) {
  if (!b) return;

  if (b->is_rule) {
    /* Rules are the fraction bars and radical overbars. They are filled in the
     * current source, which the caller sets, so math inherits the text colour
     * and selection highlighting works over it. */
    cairo_rectangle(cr, x, y - b->h, b->w, b->h + b->d);
    cairo_fill(cr);
  } else if (b->ng && b->sf) {
    cairo_glyph_t stackbuf[STACK_GLYPHS];
    cairo_glyph_t *g = b->ng <= STACK_GLYPHS ? stackbuf
                                             : malloc(b->ng * sizeof *g);
    if (g) {
      for (uint32_t i = 0; i < b->ng; i++) {
        g[i].index = b->g[i].index;
        g[i].x = b->g[i].x + x;
        g[i].y = b->g[i].y + y;
      }
      cairo_set_scaled_font(cr, b->sf);
      cairo_show_glyphs(cr, g, (int)b->ng);
      if (g != stackbuf) free(g);
    }
  }

  for (uint32_t i = 0; i < b->nkids; i++)
    paint_box(b->kids[i], cr, x + b->kx[i], y + b->ky[i]);
}

void nd_math_paint(const nd_box *b, cairo_t *cr, double x, double y) {
  if (!b) return;
  cairo_save(cr);
  paint_box(b, cr, x, y);
  cairo_restore(cr);
}
