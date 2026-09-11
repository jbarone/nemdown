/* wl_window.c — toplevel lifecycle, fractional scale, and frame pacing. */

#include "wl/wl_window.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "util/log.h"

#define ND_DEFAULT_W 1100
#define ND_DEFAULT_H 800
#define ND_MIN_W     480
#define ND_MIN_H     320

/* Nothing on the wire bounds a configure or a scale, and both feed buffer
 * sizing: at 30000x30000 the pool is 3.6GB and its size truncates to a
 * negative int32 on the way to wl_shm_create_pool.
 *
 * Two separate clamps, because they defend different things. ND_MAX_DIM bounds
 * the LOGICAL size, which is what flows into the page-scroll and zoom
 * arithmetic. ND_MAX_DEV bounds the DEVICE size, which is what decides the
 * allocation: at 4 bytes a pixel, 16384 square is exactly 1GiB, the largest
 * square that still fits the int32 pool extent. Clamping only the logical size
 * would not be enough -- 16384 logical at scale 1.5 is 24576 device, and 2.4GB.
 * Past that the buffer stops growing and the viewport scales it, which is a
 * soft edge rather than a blank window, at sizes no real display reaches. */
#define ND_MAX_DIM   16384
#define ND_MAX_DEV   16384
#define ND_MIN_SCALE 0.25
#define ND_MAX_SCALE 8.0

static void render(struct nd_window *win);

/* Zero keeps its protocol meaning of "the client picks", so it passes through
 * untouched and the caller's existing fallback handles it. */
static int clamp_dim(int32_t v, int min) {
  if (v == 0) return 0;
  if (v < min) return min;
  if (v > ND_MAX_DIM) return ND_MAX_DIM;
  return (int)v;
}

/* ---- frame callback ----------------------------------------------------- */

static void frame_done(void *data, struct wl_callback *cb, uint32_t time_ms);

static const struct wl_callback_listener frame_listener = {
  .done = frame_done,
};

static void frame_done(void *data, struct wl_callback *cb, uint32_t time_ms) {
  struct nd_window *win = data;
  wl_callback_destroy(cb); /* one-shot: leaking these is a slow climb */
  win->frame_cb = NULL;
  win->frame_pending = false;

  nd_app_animate(win->app, time_ms);

  if (nd_app_is_dirty(win->app)) render(win);
}

/* ---- painting ----------------------------------------------------------- */

static void render(struct nd_window *win) {
  if (!win->configured || win->closed) return;

  if (win->need_realloc) {
    int bw = (int)lround(win->w * win->scale);
    int bh = (int)lround(win->h * win->scale);
    if (bw > ND_MAX_DEV) bw = ND_MAX_DEV;
    if (bh > ND_MAX_DEV) bh = ND_MAX_DEV;
    /* The scale has to be part of the test, not just the device size. Cairo's
     * device scale is baked into the buffer at creation, so 2000x1600@1.0
     * becoming 1000x800@2.0 gives identical bw/bh, skips the realloc, and
     * leaves the surface at the old scale while paint is told the new one --
     * the document renders into a quarter of the window. */
    if (bw != win->buf_w || bh != win->buf_h || win->scale != win->buf_scale) {
      if (!nd_buffers_realloc(&win->bufs, win->app->wl.shm, bw, bh, win->scale)) {
        nd_warn("buffer allocation failed");
        return;
      }
      win->buf_w = bw;
      win->buf_h = bh;
      win->buf_scale = win->scale;
    }
    win->need_realloc = false;
  }

  struct nd_buffer *b = nd_buffers_acquire(&win->bufs);
  if (!b) return; /* both in flight; the dirty flag survives for next frame */

  nd_app_paint(win->app, b->cr, win->w, win->h, win->scale);
  cairo_surface_flush(b->cs);

  /* Opaque region lets the compositor skip blending the whole window. */
  struct wl_region *opaque = wl_compositor_create_region(win->app->wl.compositor);
  wl_region_add(opaque, 0, 0, win->w, win->h);
  wl_surface_set_opaque_region(win->surface, opaque);
  wl_region_destroy(opaque);

  /* Buffer is device-sized; the viewport destination is logical. The
   * compositor derives the ratio. buffer_scale stays 1 so the two mechanisms
   * never fight. */
  wl_surface_set_buffer_scale(win->surface, 1);
  if (win->viewport) wp_viewport_set_destination(win->viewport, win->w, win->h);

  wl_surface_attach(win->surface, b->wl, 0, 0);
  wl_surface_damage_buffer(win->surface, 0, 0, INT32_MAX, INT32_MAX);

  /* xdg_surface.configure calls render() unconditionally -- it must, having
   * acked -- so this can run with a callback already outstanding. Overwriting
   * the pointer would strand the old one: frame_done destroys its own argument
   * and NULLs the field, dropping the newer callback, and they accumulate
   * while a compositor repeats configures. Destroy before replacing. */
  if (win->frame_cb) wl_callback_destroy(win->frame_cb);
  win->frame_cb = wl_surface_frame(win->surface);
  wl_callback_add_listener(win->frame_cb, &frame_listener, win);
  win->frame_pending = true;

  wl_surface_commit(win->surface);
  b->busy = true;
  nd_app_clear_dirty(win->app);
}

void nd_window_damage(struct nd_window *win) {
  if (!win->frame_pending) render(win);
}

/* ---- xdg_surface / xdg_toplevel ----------------------------------------- */

static void xdg_surface_configure(void *data, struct xdg_surface *xs,
                                  uint32_t serial) {
  struct nd_window *win = data;
  xdg_surface_ack_configure(xs, serial);

  int nw = win->pending_w ? win->pending_w : (win->w ? win->w : ND_DEFAULT_W);
  int nh = win->pending_h ? win->pending_h : (win->h ? win->h : ND_DEFAULT_H);

  if (nw != win->w || nh != win->h) {
    win->w = nw;
    win->h = nh;
    win->need_realloc = true;
    nd_app_resized(win->app, nw, nh);
  }

  win->configured = true;
  /* Having acked, we must attach a buffer and commit or the compositor will
   * not map us. */
  render(win);
}

static const struct xdg_surface_listener xdg_surface_listener = {
  .configure = xdg_surface_configure,
};

static void toplevel_configure(void *data, struct xdg_toplevel *tl,
                               int32_t w, int32_t h, struct wl_array *states) {
  (void)tl;
  struct nd_window *win = data;
  /* Pending only: xdg_surface.configure is the commit point. A negative
   * dimension would otherwise reach win->w, where it survives the realloc
   * guard and flows on into the page-scroll and zoom arithmetic; zero keeps
   * its existing meaning of "you choose". */
  win->pending_w = clamp_dim(w, ND_MIN_W);
  win->pending_h = clamp_dim(h, ND_MIN_H);

  win->activated = false;
  uint32_t *st;
  wl_array_for_each(st, states) {
    if (*st == XDG_TOPLEVEL_STATE_ACTIVATED) win->activated = true;
  }
}

static void toplevel_close(void *data, struct xdg_toplevel *tl) {
  (void)tl;
  struct nd_window *win = data;
  win->closed = true;
  nd_app_quit(win->app);
}

static void toplevel_configure_bounds(void *data, struct xdg_toplevel *tl,
                                      int32_t w, int32_t h) {
  (void)data; (void)tl; (void)w; (void)h;
}

static void toplevel_wm_capabilities(void *data, struct xdg_toplevel *tl,
                                     struct wl_array *caps) {
  (void)data; (void)tl; (void)caps;
}

static const struct xdg_toplevel_listener toplevel_listener = {
  .configure        = toplevel_configure,
  .close            = toplevel_close,
  .configure_bounds = toplevel_configure_bounds,
  .wm_capabilities  = toplevel_wm_capabilities,
};

/* ---- fractional scale ---------------------------------------------------- */

static void frac_preferred_scale(void *data, struct wp_fractional_scale_v1 *fs,
                                 uint32_t scale_120) {
  (void)fs;
  struct nd_window *win = data;
  /* The protocol defines this as the numerator over a denominator of 120. */
  double s = scale_120 / 120.0;
  if (s <= 0.0 || s == win->scale) return;
  /* scale_120 is a uint32_t, so an unclamped value reaches lround(w * scale)
   * as ~2e10 and the narrowing to int is undefined before any size check sees
   * it. Clamping here is what makes that conversion well defined. */
  if (s < ND_MIN_SCALE) s = ND_MIN_SCALE;
  if (s > ND_MAX_SCALE) s = ND_MAX_SCALE;
  if (s == win->scale) return;

  nd_dbg("fractional scale -> %.4f", s);
  win->scale = s;
  win->need_realloc = true;
  /* Layout is scale-invariant (hint metrics are off), so only paint state and
   * the image cache care. */
  nd_app_scale_changed(win->app, s);
  nd_window_damage(win);
}

static const struct wp_fractional_scale_v1_listener frac_listener = {
  .preferred_scale = frac_preferred_scale,
};

/* ---- decoration ---------------------------------------------------------- */

static void deco_configure(void *data, struct zxdg_toplevel_decoration_v1 *d,
                           uint32_t mode) {
  (void)data; (void)d;
  if (mode != ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE)
    nd_warn("compositor wants client-side decorations; running undecorated");
}

static const struct zxdg_toplevel_decoration_v1_listener deco_listener = {
  .configure = deco_configure,
};

/* ---- lifecycle ----------------------------------------------------------- */

bool nd_window_create(struct nd_window *win, struct nd_app *app,
                      const char *title) {
  struct nd_wl *wl = &app->wl;

  win->app   = app;
  win->scale = 1.0;
  win->w     = ND_DEFAULT_W;
  win->h     = ND_DEFAULT_H;
  win->need_realloc = true;

  win->surface = wl_compositor_create_surface(wl->compositor);
  if (!win->surface) return false;

  win->xdg_surface = xdg_wm_base_get_xdg_surface(wl->wm_base, win->surface);
  xdg_surface_add_listener(win->xdg_surface, &xdg_surface_listener, win);

  win->toplevel = xdg_surface_get_toplevel(win->xdg_surface);
  xdg_toplevel_add_listener(win->toplevel, &toplevel_listener, win);
  xdg_toplevel_set_title(win->toplevel, title);
  xdg_toplevel_set_app_id(win->toplevel, "nemdown");
  xdg_toplevel_set_min_size(win->toplevel, ND_MIN_W, ND_MIN_H);

  if (wl->frac_mgr) {
    win->frac = wp_fractional_scale_manager_v1_get_fractional_scale(wl->frac_mgr,
                                                                   win->surface);
    wp_fractional_scale_v1_add_listener(win->frac, &frac_listener, win);
  }
  if (wl->viewporter)
    win->viewport = wp_viewporter_get_viewport(wl->viewporter, win->surface);

  if (wl->deco_mgr) {
    win->deco = zxdg_decoration_manager_v1_get_toplevel_decoration(wl->deco_mgr,
                                                                  win->toplevel);
    zxdg_toplevel_decoration_v1_add_listener(win->deco, &deco_listener, win);
    /* We draw no titlebar; say so explicitly rather than leaving it to chance. */
    zxdg_toplevel_decoration_v1_set_mode(
        win->deco, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
  }

  /* An empty commit is what asks for the first configure. */
  wl_surface_commit(win->surface);
  wl_display_roundtrip(wl->display);
  return true;
}

void nd_window_destroy(struct nd_window *win) {
  if (win->frame_cb)   wl_callback_destroy(win->frame_cb);
  nd_buffers_destroy(&win->bufs);
  if (win->deco)        zxdg_toplevel_decoration_v1_destroy(win->deco);
  if (win->frac)        wp_fractional_scale_v1_destroy(win->frac);
  if (win->viewport)    wp_viewport_destroy(win->viewport);
  if (win->toplevel)    xdg_toplevel_destroy(win->toplevel);
  if (win->xdg_surface) xdg_surface_destroy(win->xdg_surface);
  if (win->surface)     wl_surface_destroy(win->surface);
  memset(win, 0, sizeof *win);
}
