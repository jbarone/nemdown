/* render_app — render the whole window, sidebar included, to a PNG.
 *
 * Same composition the Wayland paint path uses, minus the compositor, so the
 * full layout can be reviewed without a screenshot.
 *
 * Usage: render_app <file.md> <out.png> [w] [h] [scale]
 */

#include <stdio.h>
#include <stdlib.h>

#include <cairo.h>

#include "doc/doc.h"
#include "ui/sidebar.h"
#include "ui/theme.h"

int main(int argc, char **argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: render_app <file.md> <out.png> [w] [h] [scale] [scroll]\n");
    return 2;
  }
  int    w     = argc > 3 ? atoi(argv[3]) : 1100;
  int    h     = argc > 4 ? atoi(argv[4]) : 900;
  double scale  = argc > 5 ? atof(argv[5]) : 1.0;
  double scroll = argc > 6 ? atof(argv[6]) : 0.0;

  char *err = NULL;
  nd_doc *doc = nd_doc_open(argv[1], &err);
  if (!doc) { fprintf(stderr, "error: %s\n", err ? err : "?"); return 1; }

  struct nd_sidebar sb;
  nd_sidebar_init(&sb, nd_doc_pango_context(doc));

  cairo_surface_t *cs = cairo_image_surface_create(
      CAIRO_FORMAT_ARGB32, (int)(w * scale), (int)(h * scale));
  cairo_surface_set_device_scale(cs, scale, scale);
  cairo_t *cr = cairo_create(cs);

  double sidebar_w = 280.0;

  nd_src(cr, ND_BG_CONTENT);
  cairo_paint(cr);

  nd_src(cr, ND_BG_SIDEBAR);
  cairo_rectangle(cr, 0, 0, sidebar_w, h);
  cairo_fill(cr);

  /* Layout before the sidebar: TOC offsets are undefined until then. */
  double content_w = w - sidebar_w;
  nd_doc_layout(doc, content_w, 1.0);

  sb.active_toc = nd_toc_active(doc, 0);
  cairo_save(cr);
  cairo_rectangle(cr, 0, 0, sidebar_w, h);
  cairo_clip(cr);
  nd_sidebar_paint(&sb, cr, sidebar_w, h, nd_doc_props(doc), nd_doc_toc(doc),
                   nd_doc_title(doc), scale);
  cairo_restore(cr);

  nd_src(cr, ND_DIVIDER);
  cairo_rectangle(cr, nd_snap(sidebar_w, scale), 0, 1.0 / scale, h);
  cairo_fill(cr);

  cairo_save(cr);
  cairo_rectangle(cr, sidebar_w, 0, content_w, h);
  cairo_clip(cr);
  cairo_translate(cr, sidebar_w, 0);
  /* The reading marker, drawn the way app.c draws it, so what this harness
   * shows is what the window shows. */
  int stop = nd_doc_reading_at(doc, scroll, (double)h);
  double sx, sy, sw, sh;
  if (stop >= 0 && nd_doc_reading_rect(doc, (uint32_t)stop, &sx, &sy, &sw, &sh)) {
    double gx = nd_doc_column_x(doc) - 22.0;
    if (gx < 2.0) gx = 2.0;
    double top = sy - scroll, bot = top + sh;
    if (top < 6.0) top = 6.0;
    if (bot > h - 6.0) bot = h - 6.0;
    if (bot - top >= 1.0) {
      cairo_set_source_rgba(cr, ND_R(ND_ACCENT), ND_G(ND_ACCENT), ND_B(ND_ACCENT), 0.80);
      cairo_rectangle(cr, gx, top, 2.0, bot - top);
      cairo_fill(cr);
    }
  }

  nd_doc_paint(doc, cr, scroll, h);
  cairo_restore(cr);

  cairo_destroy(cr);
  cairo_surface_write_to_png(cs, argv[2]);
  cairo_surface_destroy(cs);
  printf("%s: %dx%d @ %.2gx\n", argv[2], w, h, scale);
  nd_doc_free(doc);
  return 0;
}
