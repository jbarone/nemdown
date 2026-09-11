/* sidebar.h — the split sidebar: properties above, contents below. */

#ifndef NEMDOWN_SIDEBAR_H
#define NEMDOWN_SIDEBAR_H

#include <cairo.h>
#include <stdbool.h>
#include <pango/pangocairo.h>

#include "doc/doc.h"

typedef enum { ND_PANE_NONE, ND_PANE_PROPS, ND_PANE_TOC } nd_pane;

struct nd_sidebar {
  PangoContext *pctx;

  double props_scroll, toc_scroll;
  double props_h;          /* height of the properties PANE (clipped) */
  double props_content_h;  /* height of its CONTENT, which may exceed the pane */
  double toc_content_h;
  double view_h;           /* sidebar height as of the last paint */

  int    hover_toc;        /* index under the pointer, or -1 */
  int    active_toc;

  /* Overlay scrollbars fade out when scrolling stops. */
  double props_alpha, toc_alpha;
};

void nd_sidebar_init(struct nd_sidebar *sb, PangoContext *pctx);

void nd_sidebar_paint(struct nd_sidebar *sb, cairo_t *cr, double w, double h,
                      const nd_props *props, const nd_toc *toc,
                      const char *title, double scale);

/* Returns the TOC index at a sidebar-local point, or -1. */
int nd_sidebar_toc_at(const struct nd_sidebar *sb, double w, double h,
                      const nd_props *props, const nd_toc *toc,
                      double x, double y);

/* Which pane a sidebar-local y falls in, so the wheel can be routed to it. */
nd_pane nd_sidebar_pane_at(const struct nd_sidebar *sb, double y);

/* Scrolls one pane, clamped to its content. Returns true if it moved. */
bool nd_sidebar_scroll(struct nd_sidebar *sb, nd_pane pane, double dy);

#endif /* NEMDOWN_SIDEBAR_H */
