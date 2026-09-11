/* wl_shell.c — connection, registry binding, and the poll loop. */

#include "wl/wl_shell.h"

#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "util/log.h"
#include "wl/wl_input.h"
#include "wl/wl_window.h"

/* A compositor may advertise a newer version than we understand. Binding above
 * what we were generated against is a protocol error that kills the client, so
 * every bind clamps. */
#define ND_MIN(a, b) ((a) < (b) ? (a) : (b))
#define BIND(field, iface, want) \
  wl->field = wl_registry_bind(reg, name, &iface##_interface, \
                               ND_MIN(version, (want)))

static void wm_base_ping(void *data, struct xdg_wm_base *base, uint32_t serial) {
  (void)data;
  /* Unconditional and immediate: Hyprland force-closes a client that stalls. */
  xdg_wm_base_pong(base, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {
  .ping = wm_base_ping,
};

static void registry_global(void *data, struct wl_registry *reg, uint32_t name,
                            const char *iface, uint32_t version) {
  struct nd_wl *wl = data;

  if (strcmp(iface, wl_compositor_interface.name) == 0) {
    BIND(compositor, wl_compositor, 6);
  } else if (strcmp(iface, wl_shm_interface.name) == 0) {
    BIND(shm, wl_shm, 1);
  } else if (strcmp(iface, xdg_wm_base_interface.name) == 0) {
    BIND(wm_base, xdg_wm_base, 5);
    xdg_wm_base_add_listener(wl->wm_base, &wm_base_listener, wl);
  } else if (strcmp(iface, wl_seat_interface.name) == 0) {
    BIND(seat, wl_seat, 9);
    nd_input_bind_seat(wl->app, wl->seat);
  } else if (strcmp(iface, wp_viewporter_interface.name) == 0) {
    BIND(viewporter, wp_viewporter, 1);
  } else if (strcmp(iface, wp_fractional_scale_manager_v1_interface.name) == 0) {
    BIND(frac_mgr, wp_fractional_scale_manager_v1, 1);
  } else if (strcmp(iface, wp_cursor_shape_manager_v1_interface.name) == 0) {
    BIND(cursor_mgr, wp_cursor_shape_manager_v1, 1);
  } else if (strcmp(iface, zxdg_decoration_manager_v1_interface.name) == 0) {
    BIND(deco_mgr, zxdg_decoration_manager_v1, 1);
  }
}

static void registry_global_remove(void *data, struct wl_registry *reg, uint32_t name) {
  (void)data; (void)reg; (void)name;
  /* Single-window viewer: nothing we bind is hot-removable in a way we can act
   * on usefully. Log nothing, crash never. */
}

static const struct wl_registry_listener registry_listener = {
  .global        = registry_global,
  .global_remove = registry_global_remove,
};

bool nd_wl_connect(struct nd_wl *wl, struct nd_app *app, char **err) {
  wl->app = app;

  wl->display = wl_display_connect(NULL);
  if (!wl->display) {
    *err = nd_strdup_fmt("cannot connect to a Wayland compositor "
                         "(is WAYLAND_DISPLAY set?)");
    return false;
  }

  wl->registry = wl_display_get_registry(wl->display);
  wl_registry_add_listener(wl->registry, &registry_listener, wl);

  /* First roundtrip collects globals; the second collects the events they emit
   * in response to binding (seat capabilities, shm formats). */
  wl_display_roundtrip(wl->display);
  wl_display_roundtrip(wl->display);

  if (!wl->compositor || !wl->shm || !wl->wm_base) {
    *err = nd_strdup_fmt("compositor is missing required interfaces:%s%s%s",
                         wl->compositor ? "" : " wl_compositor",
                         wl->shm ? "" : " wl_shm",
                         wl->wm_base ? "" : " xdg_wm_base");
    return false;
  }

  if (!wl->frac_mgr || !wl->viewporter) {
    nd_warn("no fractional-scale support; falling back to integer scale");
  }

  return true;
}

int nd_wl_display_error(struct nd_wl *wl) {
  if (!wl || !wl->display) return 0;
  return wl_display_get_error(wl->display);
}

void nd_wl_disconnect(struct nd_wl *wl) {
  if (wl->deco_mgr)   zxdg_decoration_manager_v1_destroy(wl->deco_mgr);
  if (wl->cursor_mgr) wp_cursor_shape_manager_v1_destroy(wl->cursor_mgr);
  if (wl->frac_mgr)   wp_fractional_scale_manager_v1_destroy(wl->frac_mgr);
  if (wl->viewporter) wp_viewporter_destroy(wl->viewporter);
  if (wl->wm_base)    xdg_wm_base_destroy(wl->wm_base);
  if (wl->compositor) wl_compositor_destroy(wl->compositor);
  if (wl->shm)        wl_shm_destroy(wl->shm);
  if (wl->registry)   wl_registry_destroy(wl->registry);
  if (wl->display)    wl_display_disconnect(wl->display);
  memset(wl, 0, sizeof *wl);
}

/* The prepare_read/read_events dance, rather than wl_display_dispatch, because
 * we poll other fds too and cannot let libwayland do its own blocking read.
 *
 * The invariant that matters: every path below reaches exactly one of
 * wl_display_read_events() or wl_display_cancel_read(). read_events releases
 * the read intent itself, even on failure, so it is never paired with a cancel.
 */
int nd_wl_dispatch(struct nd_app *app) {
  struct wl_display *dpy = app->wl.display;
  int wl_fd = wl_display_get_fd(dpy);

  while (wl_display_prepare_read(dpy) != 0) {
    if (wl_display_dispatch_pending(dpy) < 0) return -1;
  }

  /* Flush after prepare (so nothing queued in between is stranded) and before
   * poll (or a commit never reaches the compositor and we block forever on a
   * frame callback that cannot arrive). */
  while (wl_display_flush(dpy) < 0) {
    if (errno != EAGAIN) {
      wl_display_cancel_read(dpy);
      return -1; /* EPIPE: compositor is gone */
    }
    struct pollfd wp = {.fd = wl_fd, .events = POLLOUT};
    if (poll(&wp, 1, -1) < 0 && errno != EINTR) {
      wl_display_cancel_read(dpy);
      return -1;
    }
  }

  struct pollfd pfds[] = {
    {.fd = wl_fd,                 .events = POLLIN},
    {.fd = app->input.repeat_fd,  .events = POLLIN},
    {.fd = app->watch_fd,         .events = POLLIN},
  };
  const int nfds = (int)(sizeof pfds / sizeof pfds[0]);

  int n = poll(pfds, (nfds_t)nfds, nd_app_poll_timeout(app));
  if (n < 0) {
    wl_display_cancel_read(dpy);
    return (errno == EINTR) ? 0 : -1;
  }

  if (pfds[0].revents & POLLIN) {
    if (wl_display_read_events(dpy) < 0) return -1;
    if (wl_display_dispatch_pending(dpy) < 0) return -1;
  } else {
    wl_display_cancel_read(dpy);
  }

  if (pfds[0].revents & (POLLERR | POLLHUP)) return -1;
  if (pfds[1].revents & POLLIN) nd_input_repeat_tick(app);
  if (pfds[2].revents & POLLIN) nd_app_watch_drain(app);

  nd_app_tick(app);
  return 0;
}
