/* render_png — render a markdown file to a PNG with no compositor involved.
 *
 * This is how the engine gets looked at and diffed: it links the document
 * engine only, so it needs neither Wayland nor a display.
 *
 * Usage: render_png <file.md> <out.png> [width] [scale]
 */

#include <stdio.h>
#include <stdlib.h>

#include <cairo.h>

#include "doc/doc.h"
#include "ui/theme.h"

int main(int argc, char **argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: render_png <file.md> <out.png> [width] [scale]\n");
    return 2;
  }

  int    width = argc > 3 ? atoi(argv[3]) : 900;
  double scale = argc > 4 ? atof(argv[4]) : 1.0;

  char *err = NULL;
  nd_doc *doc = nd_doc_open(argv[1], &err);
  if (!doc) {
    fprintf(stderr, "error: %s\n", err ? err : "cannot open");
    return 1;
  }

  double content_h = nd_doc_layout(doc, width, 1.0);
  int height = (int)content_h;
  if (height < 1) height = 1;
  if (height > 20000) height = 20000; /* keep a runaway document viewable */

  cairo_surface_t *cs = cairo_image_surface_create(
      CAIRO_FORMAT_ARGB32, (int)(width * scale), (int)(height * scale));
  cairo_surface_set_device_scale(cs, scale, scale);
  cairo_t *cr = cairo_create(cs);

  nd_src(cr, ND_BG_CONTENT);
  cairo_paint(cr);

  nd_doc_paint(doc, cr, 0, height);

  cairo_destroy(cr);
  cairo_surface_write_to_png(cs, argv[2]);
  cairo_surface_destroy(cs);

  printf("%s: %dx%d @ %.2gx  (content %.0fpx, %zu toc entries)\n",
         argv[2], width, height, scale, content_h, nd_doc_toc(doc)->count);

  nd_doc_free(doc);
  return 0;
}
