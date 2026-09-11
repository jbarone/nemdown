/* sidebar.h — the split sidebar: properties above, contents below. */

#ifndef NEMDOWN_SIDEBAR_H
#define NEMDOWN_SIDEBAR_H

#include <cairo.h>
#include <pango/pangocairo.h>

#include "doc/doc.h"

struct nd_sidebar {
  PangoContext *pctx;
  double props_scroll;
  double toc_scroll;
  double props_h;   /* measured height of the properties pane content */
  int    hover_toc; /* index under the pointer, or -1 */
  int    active_toc;
};

void nd_sidebar_init(struct nd_sidebar *sb, PangoContext *pctx);

void nd_sidebar_paint(struct nd_sidebar *sb, cairo_t *cr, double w, double h,
                      const nd_props *props, const nd_toc *toc,
                      const char *title, double scale);

/* Returns the TOC index at a sidebar-local point, or -1. */
int nd_sidebar_toc_at(const struct nd_sidebar *sb, double w, double h,
                      const nd_props *props, const nd_toc *toc,
                      double x, double y);

#endif /* NEMDOWN_SIDEBAR_H */
