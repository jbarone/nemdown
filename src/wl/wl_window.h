/* wl_window.h — the toplevel surface, its scale, and frame pacing. */

#ifndef NEMDOWN_WL_WINDOW_H
#define NEMDOWN_WL_WINDOW_H

#include <stdbool.h>
#include <stdint.h>

#include "wl/wl_buffer.h"
#include "wl/wl_shell.h"

struct nd_app;

struct nd_window {
  struct nd_app *app;

  struct wl_surface   *surface;
  struct xdg_surface  *xdg_surface;
  struct xdg_toplevel *toplevel;
  struct wl_callback  *frame_cb;

  struct wp_viewport                 *viewport;
  struct wp_fractional_scale_v1      *frac;
  struct zxdg_toplevel_decoration_v1 *deco;

  struct nd_buffers bufs;

  int    w, h;                 /* logical size, in use */
  int    pending_w, pending_h; /* from xdg_toplevel.configure */
  double scale;                /* 1.0, 1.25, 1.5, ... */
  int    buf_w, buf_h;         /* device size the buffers were built at */

  bool configured;
  bool frame_pending;
  bool need_realloc;
  bool activated;
  bool closed;
};

bool nd_window_create(struct nd_window *win, struct nd_app *app, const char *title);
void nd_window_destroy(struct nd_window *win);

/* Mark the surface as needing a repaint. Renders immediately when no frame
 * callback is outstanding, so the first input event costs no extra latency. */
void nd_window_damage(struct nd_window *win);

#endif /* NEMDOWN_WL_WINDOW_H */
