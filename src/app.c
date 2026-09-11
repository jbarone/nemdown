/* app.c — application state, scrolling, and action dispatch. */

#include "app.h"

#include <libgen.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <unistd.h>

#include "ui/theme.h"
#include "util/log.h"

/* Time constant for scroll easing. Small enough to feel immediate, large
 * enough to read as motion rather than a jump. */
#define ND_SCROLL_TAU 0.055

/* Below this, the remaining distance is invisible. Snapping and stopping is
 * what lets the process reach idle: an asymptotic smoother never converges and
 * would request frames forever. */
#define ND_SCROLL_SNAP 0.35

#define ND_INOTIFY_DEBOUNCE_MS 50

static void scroll_clamp(struct nd_app *app);

static void mark(struct nd_app *app, unsigned bits) {
  app->dirty |= bits;
  nd_window_damage(&app->win);
}

bool nd_app_is_dirty(const struct nd_app *app) {
  return app->dirty != ND_DIRTY_NONE || app->scroll.animating;
}

void nd_app_clear_dirty(struct nd_app *app) {
  app->dirty = ND_DIRTY_NONE;
}

int nd_app_poll_timeout(const struct nd_app *app) {
  /* Animation is paced by frame callbacks, not by the poll timeout, so there
   * is nothing to wake up for while it runs. Returning 0 here is the classic
   * way to burn a core doing nothing. */
  (void)app;
  return -1;
}

/* ---- scrolling ----------------------------------------------------------- */

static void scroll_clamp(struct nd_app *app) {
  struct nd_scroll *s = &app->scroll;
  if (s->target < 0.0) s->target = 0.0;
  if (s->target > s->max) s->target = s->max;
}

void nd_app_scroll_by(struct nd_app *app, double dy, bool immediate) {
  struct nd_scroll *s = &app->scroll;
  s->target += dy;
  scroll_clamp(app);

  if (immediate) {
    s->offset    = s->target;
    s->animating = false;
  } else {
    s->animating = true;
  }
  mark(app, ND_DIRTY_ALL);
}

void nd_app_scroll_settle(struct nd_app *app) {
  struct nd_scroll *s = &app->scroll;
  s->offset    = s->target;
  s->animating = false;
  mark(app, ND_DIRTY_ALL);
}

void nd_app_animate(struct nd_app *app, uint32_t time_ms) {
  struct nd_scroll *s = &app->scroll;
  if (!s->animating) { s->last_ms = time_ms; return; }

  double dt = (double)(time_ms - s->last_ms) / 1000.0;
  s->last_ms = time_ms;
  /* After a stall, a huge dt would teleport the view. */
  if (dt <= 0.0 || dt > 0.05) dt = 1.0 / 60.0;

  double k = 1.0 - exp(-dt / ND_SCROLL_TAU);
  s->offset += (s->target - s->offset) * k;

  if (fabs(s->target - s->offset) < ND_SCROLL_SNAP) {
    s->offset    = s->target;
    s->animating = false;
  }
  app->dirty |= ND_DIRTY_ALL;
}

void nd_app_tick(struct nd_app *app) {
  if (nd_app_is_dirty(app)) nd_window_damage(&app->win);
}

/* ---- input actions ------------------------------------------------------- */

void nd_app_key(struct nd_app *app, xkb_keysym_t sym, bool is_repeat) {
  (void)is_repeat;
  double line = app->scroll.line_height;
  double page = (double)app->win.h * 0.9;

  switch (sym) {
    case XKB_KEY_q:
    case XKB_KEY_Escape:
      nd_app_quit(app);
      break;

    case XKB_KEY_j:
    case XKB_KEY_Down:  nd_app_scroll_by(app, line, false); break;
    case XKB_KEY_k:
    case XKB_KEY_Up:    nd_app_scroll_by(app, -line, false); break;

    case XKB_KEY_Page_Down: nd_app_scroll_by(app, page, false); break;
    case XKB_KEY_Page_Up:   nd_app_scroll_by(app, -page, false); break;

    case XKB_KEY_g:
    case XKB_KEY_Home:
      app->scroll.target = 0.0;
      app->scroll.animating = true;
      mark(app, ND_DIRTY_ALL);
      break;

    case XKB_KEY_G:
    case XKB_KEY_End:
      app->scroll.target = app->scroll.max;
      app->scroll.animating = true;
      mark(app, ND_DIRTY_ALL);
      break;

    case XKB_KEY_b:
      app->sidebar_visible = !app->sidebar_visible;
      mark(app, ND_DIRTY_ALL | ND_DIRTY_RELAYOUT);
      break;

    default:
      break;
  }
}

void nd_app_pointer_motion(struct nd_app *app, double x, double y) {
  if (!app->doc) return;

  int hover = -1;
  if (app->sidebar_visible && x < app->sidebar_w) {
    hover = nd_sidebar_toc_at(&app->sidebar, app->sidebar_w, (double)app->win.h,
                              nd_doc_props(app->doc), nd_doc_toc(app->doc), x, y);
  }
  if (hover != app->sidebar.hover_toc) {
    app->sidebar.hover_toc = hover;
    mark(app, ND_DIRTY_SIDEBAR);
  }
}

void nd_app_pointer_button(struct nd_app *app, double x, double y, bool pressed) {
  if (!pressed || !app->doc) return;

  if (app->sidebar_visible && x < app->sidebar_w) {
    int idx = nd_sidebar_toc_at(&app->sidebar, app->sidebar_w,
                                (double)app->win.h, nd_doc_props(app->doc),
                                nd_doc_toc(app->doc), x, y);
    if (idx >= 0) {
      /* Set the target and let the smoother ease it there. */
      app->scroll.target = nd_toc_target_y(app->doc, idx, (double)app->win.h);
      app->scroll.animating = true;
      mark(app, ND_DIRTY_ALL);
    }
  }
}

void nd_app_pointer_leave(struct nd_app *app) {
  if (app->sidebar.hover_toc != -1) {
    app->sidebar.hover_toc = -1;
    mark(app, ND_DIRTY_SIDEBAR);
  }
}

void nd_app_resized(struct nd_app *app, int w, int h) {
  (void)w; (void)h;
  app->dirty |= ND_DIRTY_ALL | ND_DIRTY_RELAYOUT;
}

void nd_app_scale_changed(struct nd_app *app, double scale) {
  /* Layout is scale-invariant by design (hint metrics are off), so this only
   * needs a repaint — plus, later, dropping the decoded-image cache. */
  (void)scale;
  app->dirty |= ND_DIRTY_ALL;
}

void nd_app_quit(struct nd_app *app) {
  app->running = false;
}

/* ---- painting ------------------------------------------------------------ */

void nd_app_paint(struct nd_app *app, cairo_t *cr, int w, int h, double scale) {
  nd_src(cr, ND_BG_CONTENT);
  cairo_paint(cr);

  double sidebar = app->sidebar_visible ? app->sidebar_w : 0.0;

  if (!app->doc) return;

  double content_w = (double)w - sidebar;
  double content_h = (double)h;
  if (content_w <= 0) return;

  /* Layout first: the TOC's y-offsets, and therefore which entry is active,
   * are only meaningful once the document has been laid out at this width. */
  app->scroll.max = nd_doc_layout(app->doc, content_w, app->font_scale) - content_h;
  if (app->scroll.max < 0) app->scroll.max = 0;
  scroll_clamp(app);
  if (app->scroll.offset > app->scroll.max) app->scroll.offset = app->scroll.max;

  if (sidebar > 0.0) {
    nd_src(cr, ND_BG_SIDEBAR);
    cairo_rectangle(cr, 0, 0, sidebar, h);
    cairo_fill(cr);

    app->sidebar.active_toc = nd_toc_active(app->doc, app->scroll.offset);

    cairo_save(cr);
    cairo_rectangle(cr, 0, 0, sidebar, h);
    cairo_clip(cr);
    nd_sidebar_paint(&app->sidebar, cr, sidebar, (double)h,
                     nd_doc_props(app->doc), nd_doc_toc(app->doc),
                     nd_doc_title(app->doc), scale);
    cairo_restore(cr);

    /* Snapped, or a 1px rule straddles a half device pixel at 1.5x and blurs. */
    nd_src(cr, ND_DIVIDER);
    cairo_rectangle(cr, nd_snap(sidebar, scale), 0, 1.0 / scale, h);
    cairo_fill(cr);
  }

  cairo_save(cr);
  cairo_rectangle(cr, sidebar, 0, content_w, content_h);
  cairo_clip(cr);
  cairo_translate(cr, sidebar, 0);
  nd_doc_paint(app->doc, cr, app->scroll.offset, content_h);
  cairo_restore(cr);
}

/* ---- file watching ------------------------------------------------------- */

void nd_app_watch_drain(struct nd_app *app) {
  char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
  ssize_t len;
  while ((len = read(app->watch_fd, buf, sizeof buf)) > 0) {
    /* Events are consumed but reload is not wired up until the engine lands. */
    (void)len;
  }
}

/* ---- lifecycle ----------------------------------------------------------- */

bool nd_app_init(struct nd_app *app, const char *path, char **err) {
  memset(app, 0, sizeof *app);
  app->running         = true;
  app->sidebar_visible = true;
  app->sidebar_w       = 280.0;
  app->font_scale      = 1.0;
  app->watch_fd        = -1;
  app->watch_wd        = -1;
  app->scroll.line_height = 26.0;
  app->scroll.max         = 2000.0; /* placeholder until layout reports it */
  app->dirty              = ND_DIRTY_ALL | ND_DIRTY_RELAYOUT;

  app->path = realpath(path, NULL);
  if (!app->path) {
    *err = nd_strdup_fmt("cannot open %s", path);
    return false;
  }

  app->doc = nd_doc_open(app->path, err);
  if (!app->doc) return false;

  nd_sidebar_init(&app->sidebar, nd_doc_pango_context(app->doc));

  if (!nd_input_init(&app->input, app)) {
    *err = nd_strdup_fmt("cannot initialise input");
    return false;
  }

  if (!nd_wl_connect(&app->wl, app, err)) return false;

  /* Watch the containing directory, not the file: editors save by writing a
   * temp file and renaming over the target, which swaps the inode and makes a
   * watch on the file itself go permanently silent. */
  app->watch_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
  if (app->watch_fd >= 0) {
    char *dup = strdup(app->path);
    if (dup) {
      app->watch_wd = inotify_add_watch(app->watch_fd, dirname(dup),
                                        IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE);
      free(dup);
    }
  }

  char *dup = strdup(app->path);
  char title[512];
  snprintf(title, sizeof title, "nemdown — %s", dup ? basename(dup) : app->path);
  free(dup);

  if (!nd_window_create(&app->win, app, title)) {
    *err = nd_strdup_fmt("cannot create window");
    return false;
  }

  return true;
}

int nd_app_run(struct nd_app *app) {
  nd_window_damage(&app->win);
  while (app->running) {
    if (nd_wl_dispatch(app) < 0) break;
  }
  return 0;
}

void nd_app_finish(struct nd_app *app) {
  nd_window_destroy(&app->win);
  nd_doc_free(app->doc);
  nd_input_finish(&app->input);
  nd_wl_disconnect(&app->wl);
  if (app->watch_fd >= 0) close(app->watch_fd);
  free(app->path);
}
