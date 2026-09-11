/* wl_shell.h — Wayland connection, globals, and the main loop.
 *
 * Owns the display and every bound global. The window (wl_window.c) and input
 * (wl_input.c) layers hang off this.
 */

#ifndef NEMDOWN_WL_SHELL_H
#define NEMDOWN_WL_SHELL_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-client.h>

#include "cursor-shape-v1-client-protocol.h"
#include "fractional-scale-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

struct nd_app;

struct nd_wl {
  struct wl_display    *display;
  struct wl_registry   *registry;
  struct wl_compositor *compositor;
  struct wl_shm        *shm;
  struct wl_seat       *seat;
  struct xdg_wm_base   *wm_base;

  struct wp_viewporter                 *viewporter;
  struct wp_fractional_scale_manager_v1 *frac_mgr;
  struct wp_cursor_shape_manager_v1     *cursor_mgr;
  struct zxdg_decoration_manager_v1     *deco_mgr;

  struct nd_app *app; /* back-pointer for listeners */
};

bool nd_wl_connect(struct nd_wl *wl, struct nd_app *app, char **err);
void nd_wl_disconnect(struct nd_wl *wl);

/* One iteration of the poll loop. Returns <0 to stop. */
int nd_wl_dispatch(struct nd_app *app);

/* errno-style reason the display died, or 0 if it did not. A fatal protocol
 * error is reported here and nowhere else, so a caller that stops dispatching
 * has to ask or it exits with nothing to say. */
int nd_wl_display_error(struct nd_wl *wl);

#endif /* NEMDOWN_WL_SHELL_H */
