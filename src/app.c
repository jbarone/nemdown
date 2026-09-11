/* app.c — application state, scrolling, and action dispatch. */

#include "app.h"

#include <libgen.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <spawn.h>
#include <sys/inotify.h>
#include <sys/wait.h>
#include <unistd.h>

#include "ui/theme.h"
#include "wl/wl_cursor.h"
#include "util/log.h"

/* Time constant for scroll easing. Small enough to feel immediate, large
 * enough to read as motion rather than a jump. */
#define ND_SCROLL_TAU 0.055

/* Below this, the remaining distance is invisible. Snapping and stopping is
 * what lets the process reach idle: an asymptotic smoother never converges and
 * would request frames forever. */
#define ND_SCROLL_SNAP 0.35

#define ND_INOTIFY_DEBOUNCE_MS 50

/* A 1px divider is unhittable, especially at fractional scale, so the grab
 * strip is padded well past what is drawn. */
#define ND_DIVIDER_GRAB 8.0
#define ND_SIDEBAR_MIN  180.0
#define ND_SIDEBAR_MAX  520.0
#define ND_SIDEBAR_TAU  0.045
#define ND_ZOOM_MIN     0.7
#define ND_ZOOM_MAX     2.0

static void scroll_clamp(struct nd_app *app);
static void sidebar_animate(struct nd_app *app, uint32_t time_ms);
static void reload_document(struct nd_app *app);
static void copy_to_clipboard(const char *text);

static void mark(struct nd_app *app, unsigned bits) {
  app->dirty |= bits;
  nd_window_damage(&app->win);
}

bool nd_app_is_dirty(const struct nd_app *app) {
  return app->dirty != ND_DIRTY_NONE || app->scroll.animating ||
         app->sidebar_anim;
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
  sidebar_animate(app, time_ms);

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

static void sidebar_animate(struct nd_app *app, uint32_t time_ms) {
  if (!app->sidebar_anim) { app->sidebar_last_ms = time_ms; return; }

  double dt = (double)(time_ms - app->sidebar_last_ms) / 1000.0;
  app->sidebar_last_ms = time_ms;
  if (dt <= 0.0 || dt > 0.05) dt = 1.0 / 60.0;

  double k = 1.0 - exp(-dt / ND_SIDEBAR_TAU);
  app->sidebar_w += (app->sidebar_target - app->sidebar_w) * k;

  if (fabs(app->sidebar_target - app->sidebar_w) < 0.5) {
    app->sidebar_w = app->sidebar_target;
    app->sidebar_anim = false;
  }
  app->dirty |= ND_DIRTY_ALL;
}

void nd_app_tick(struct nd_app *app) {
  if (nd_app_is_dirty(app)) nd_window_damage(&app->win);
}

/* ---- input actions ------------------------------------------------------- */

/* Text entry for the search field. Returns true when the key was consumed. */
static bool search_key(struct nd_app *app, xkb_keysym_t sym) {
  switch (sym) {
    case XKB_KEY_Escape:
      app->searching = false;
      app->query_len = 0;
      app->query[0] = '\0';
      nd_doc_search_clear(app->doc);
      mark(app, ND_DIRTY_ALL);
      return true;

    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
      app->searching = false; /* matches stay highlighted; n/N walk them */
      mark(app, ND_DIRTY_ALL);
      return true;

    case XKB_KEY_BackSpace:
      if (app->query_len) {
        /* Step back a whole UTF-8 character, not a byte. */
        size_t i = app->query_len - 1;
        while (i > 0 && ((unsigned char)app->query[i] & 0xC0) == 0x80) i--;
        app->query_len = i;
        app->query[i] = '\0';
        nd_doc_search(app->doc, app->query);
      }
      mark(app, ND_DIRTY_ALL);
      return true;

    default: {
      if (!app->input.xkb_state) return true;
      char buf[8];
      int n = xkb_state_key_get_utf8(app->input.xkb_state,
                                     app->input.last_keycode, buf, sizeof buf);
      if (n > 0 && (unsigned char)buf[0] >= 0x20 &&
          app->query_len + (size_t)n < sizeof app->query) {
        memcpy(app->query + app->query_len, buf, (size_t)n);
        app->query_len += (size_t)n;
        app->query[app->query_len] = '\0';
        nd_doc_search(app->doc, app->query);

        double y = nd_doc_search_step(app->doc, 0, (double)app->win.h);
        if (y >= 0) { app->scroll.target = y; app->scroll.animating = true; }
      }
      mark(app, ND_DIRTY_ALL);
      return true;
    }
  }
}

void nd_app_key(struct nd_app *app, xkb_keysym_t sym, bool is_repeat) {
  (void)is_repeat;

  if (app->searching && search_key(app, sym)) return;
  double line = app->scroll.line_height;
  double page = (double)app->win.h * 0.9;

  bool ctrl = app->input.xkb_state &&
              xkb_state_mod_name_is_active(app->input.xkb_state,
                                           XKB_MOD_NAME_CTRL,
                                           XKB_STATE_MODS_EFFECTIVE);

  if (ctrl) {
    switch (sym) {
      case XKB_KEY_c:
      case XKB_KEY_C: {
        /* With nothing selected, copy the whole document source — the bytes
         * are already in memory and it is the obvious thing to want. */
        char *text = nd_doc_select_text(app->doc);
        if (text) {
          copy_to_clipboard(text);
          free(text);
        } else {
          copy_to_clipboard(nd_doc_source(app->doc));
        }
        return;
      }
      case XKB_KEY_d: nd_app_scroll_by(app, page / 2, false);  return;
      case XKB_KEY_u: nd_app_scroll_by(app, -page / 2, false); return;
      default: break;
    }
  }

  switch (sym) {
    case XKB_KEY_Escape:
      if (nd_doc_has_selection(app->doc)) {
        nd_doc_select_clear(app->doc);
        mark(app, ND_DIRTY_DOC);
        break;
      }
      nd_app_quit(app);
      break;

    case XKB_KEY_q:
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
      app->sidebar_target = app->sidebar_visible ? app->sidebar_pref : 0.0;
      app->sidebar_anim = true;
      mark(app, ND_DIRTY_ALL | ND_DIRTY_RELAYOUT);
      break;

    case XKB_KEY_plus:
    case XKB_KEY_equal:
    case XKB_KEY_KP_Add:
      nd_app_zoom(app, app->font_scale + 0.1);
      break;

    case XKB_KEY_minus:
    case XKB_KEY_KP_Subtract:
      nd_app_zoom(app, app->font_scale - 0.1);
      break;

    case XKB_KEY_0:
    case XKB_KEY_KP_0:
      nd_app_zoom(app, 1.0);
      break;

    case XKB_KEY_r:
      reload_document(app);
      break;

    case XKB_KEY_slash:
      app->searching = true;
      app->query_len = 0;
      app->query[0] = '\0';
      mark(app, ND_DIRTY_ALL);
      break;

    case XKB_KEY_n:
    case XKB_KEY_N: {
      double y = nd_doc_search_step(app->doc, sym == XKB_KEY_N ? -1 : 1,
                                    (double)app->win.h);
      if (y >= 0) {
        app->scroll.target = y;
        app->scroll.animating = true;
        mark(app, ND_DIRTY_ALL);
      }
      break;
    }

    default:
      break;
  }
}

void nd_app_zoom(struct nd_app *app, double scale) {
  if (scale < ND_ZOOM_MIN) scale = ND_ZOOM_MIN;
  if (scale > ND_ZOOM_MAX) scale = ND_ZOOM_MAX;
  if (scale == app->font_scale) return;

  /* Hold the reader's place: zooming changes every block height, so a raw
   * pixel offset would land somewhere unrelated. */
  uint64_t anchor = nd_doc_anchor_at(app->doc, app->scroll.offset);
  app->font_scale = scale;
  app->scroll.line_height = 23.0 * scale;

  double sidebar = app->sidebar_w;
  nd_doc_layout(app->doc, (double)app->win.w - sidebar, app->font_scale);
  app->scroll.offset = nd_doc_y_for_anchor(app->doc, anchor);
  app->scroll.target = app->scroll.offset;

  mark(app, ND_DIRTY_ALL | ND_DIRTY_RELAYOUT);
}

/* Reloads from disk, holding the reader's place. */
static void reload_document(struct nd_app *app) {
  char *err = NULL;
  uint64_t anchor = nd_doc_anchor_at(app->doc, app->scroll.offset);

  if (!nd_doc_reload(app->doc, &err)) {
    nd_warn("%s", err ? err : "reload failed");
    free(err);
    return;
  }

  /* Every pointer handed out by doc.h died with the reload, so the sidebar's
   * cached hover index has to go too. */
  app->sidebar.hover_toc = -1;
  app->hover.kind = ND_HIT_NONE;
  app->hover.url = NULL;

  double sidebar = app->sidebar_visible ? app->sidebar_w : 0.0;
  nd_doc_layout(app->doc, (double)app->win.w - sidebar, app->font_scale);

  app->scroll.offset = nd_doc_y_for_anchor(app->doc, anchor);
  app->scroll.target = app->scroll.offset;
  app->scroll.animating = false;

  mark(app, ND_DIRTY_ALL | ND_DIRTY_RELAYOUT);
}

/* Hands text to wl-copy rather than implementing wl_data_source. Being a
 * clipboard source means servicing send(mime, fd) writes that can block on a
 * slow receiver, which has to be folded non-blocking into the poll loop —
 * real protocol work for no user-visible difference. wl-clipboard is already
 * part of this desktop. */
static void copy_to_clipboard(const char *text) {
  if (!text || !*text) return;

  int fds[2];
  if (pipe(fds) != 0) return;

  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_adddup2(&fa, fds[0], STDIN_FILENO);
  posix_spawn_file_actions_addclose(&fa, fds[1]);

  char *argv[] = {(char *)"wl-copy", NULL};
  extern char **environ;
  pid_t pid;
  int rc = posix_spawnp(&pid, "wl-copy", &fa, NULL, argv, environ);
  posix_spawn_file_actions_destroy(&fa);
  close(fds[0]);

  if (rc != 0) { close(fds[1]); nd_warn("wl-copy not available"); return; }

  size_t len = strlen(text), off = 0;
  while (off < len) {
    ssize_t n = write(fds[1], text + off, len - off);
    if (n <= 0) break;
    off += (size_t)n;
  }
  close(fds[1]);
  /* wl-copy daemonises to own the selection; do not wait for it. */
}

/* posix_spawn with an argv array, never a shell string: a document can contain
 * any URL it likes and none of it should reach a shell. */
static void open_external(const char *url) {
  if (!url || !*url) return;
  char *argv[] = {(char *)"xdg-open", (char *)url, NULL};
  extern char **environ;
  pid_t pid;
  if (posix_spawnp(&pid, "xdg-open", NULL, NULL, argv, environ) == 0) {
    /* Reap without blocking; xdg-open forks and returns promptly. */
    waitpid(pid, NULL, WNOHANG);
  }
}

/* Document-space y for a window point in the content pane. */
static double content_y(const struct nd_app *app, double y) {
  return y + app->scroll.offset;
}

static double content_x(const struct nd_app *app, double x) {
  return x - (app->sidebar_visible ? app->sidebar_w : 0.0);
}

void nd_app_pointer_motion(struct nd_app *app, double x, double y) {
  if (!app->doc) return;

  if (app->selecting) {
    nd_doc_select_extend(app->doc, content_x(app, x), content_y(app, y));
    nd_cursor_set(app, ND_CURSOR_TEXT);
    mark(app, ND_DIRTY_DOC);
    return;
  }

  /* Drag is modal: while it is running, nothing else is hit-tested. Otherwise
   * a fast drag outruns the divider and the hit test loses track of it. */
  if (app->dragging) {
    double w = app->drag_start_w + (x - app->drag_start_x);
    double cap = (double)app->win.w - 360.0;
    if (cap > ND_SIDEBAR_MAX) cap = ND_SIDEBAR_MAX;
    if (w < ND_SIDEBAR_MIN) w = ND_SIDEBAR_MIN;
    if (w > cap) w = cap;

    app->sidebar_w = app->sidebar_target = app->sidebar_pref = w;
    /* Layout is not called here — only from the render path — so a burst of
     * motion events coalesces into one reflow per frame. */
    mark(app, ND_DIRTY_ALL | ND_DIRTY_RELAYOUT);
    nd_cursor_set(app, ND_CURSOR_COL_RESIZE);
    return;
  }

  bool on_divider = app->sidebar_visible &&
                    fabs(x - app->sidebar_w) <= ND_DIVIDER_GRAB;
  if (on_divider) {
    nd_cursor_set(app, ND_CURSOR_COL_RESIZE);
    if (app->sidebar.hover_toc != -1) {
      app->sidebar.hover_toc = -1;
      mark(app, ND_DIRTY_SIDEBAR);
    }
    return;
  }

  bool in_sidebar = app->sidebar_visible && x < app->sidebar_w;

  int toc_hover = -1;
  if (in_sidebar) {
    toc_hover = nd_sidebar_toc_at(&app->sidebar, app->sidebar_w,
                                  (double)app->win.h, nd_doc_props(app->doc),
                                  nd_doc_toc(app->doc), x, y);
  }
  if (toc_hover != app->sidebar.hover_toc) {
    app->sidebar.hover_toc = toc_hover;
    mark(app, ND_DIRTY_SIDEBAR);
  }

  nd_hit hit;
  hit.kind = ND_HIT_NONE;
  if (!in_sidebar)
    nd_doc_hit_test(app->doc, content_x(app, x), content_y(app, y), &hit);

  if (hit.kind != app->hover.kind || hit.url != app->hover.url) {
    app->hover = hit;
    mark(app, ND_DIRTY_DOC);
  }

  nd_cursor shape = ND_CURSOR_DEFAULT;
  if (toc_hover >= 0) shape = ND_CURSOR_POINTER;
  else if (hit.kind == ND_HIT_LINK || hit.kind == ND_HIT_CHECKBOX)
    shape = ND_CURSOR_POINTER;
  nd_cursor_set(app, shape);
}

void nd_app_pointer_button(struct nd_app *app, double x, double y, bool pressed,
                           uint32_t time_ms) {
  if (!app->doc) return;

  if (!pressed) {
    app->dragging = false;
    app->selecting = false;
    return;
  }

  /* Multi-click: same spot AND within the usual double-click window. Without
   * the time check, two slow clicks in one place would read as a double. */
  bool near = fabs(x - app->last_click_x) < 4.0 &&
              fabs(y - app->last_click_y) < 4.0;
  bool soon = (time_ms - app->last_click_ms) < 400;
  app->click_count = (near && soon) ? app->click_count + 1 : 1;
  app->last_click_ms = time_ms;
  app->last_click_x = x;
  app->last_click_y = y;

  if (app->sidebar_visible && fabs(x - app->sidebar_w) <= ND_DIVIDER_GRAB) {
    app->dragging = true;
    app->drag_start_x = x;
    app->drag_start_w = app->sidebar_w;
    return;
  }

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
    return;
  }

  double cx = content_x(app, x), cy = content_y(app, y);

  nd_hit hit;
  if (!nd_doc_hit_test(app->doc, cx, cy, &hit)) {
    /* Nothing actionable under the pointer: begin a text selection. */
    if (app->click_count == 2)      nd_doc_select_word(app->doc, cx, cy);
    else if (app->click_count >= 3) nd_doc_select_block(app->doc, cx, cy);
    else {
      nd_doc_select_begin(app->doc, cx, cy);
      app->selecting = true;
    }
    mark(app, ND_DIRTY_DOC);
    return;
  }

  nd_doc_select_clear(app->doc);

  switch (hit.kind) {
    case ND_HIT_LINK:
      open_external(hit.url);
      break;

    case ND_HIT_CHECKBOX:
      /* Write the byte, then reload so the tree matches the file rather than
       * guessing at what the edit did. */
      if (nd_doc_toggle_task(app->doc, hit.source_offset, !hit.checked))
        reload_document(app);
      break;

    case ND_HIT_WIKILINK:
      /* Single-file viewer: styled as a link, but there is nowhere to go. */
      break;

    default:
      break;
  }
}

void nd_app_pointer_leave(struct nd_app *app) {
  bool dirty = false;
  if (app->sidebar.hover_toc != -1) { app->sidebar.hover_toc = -1; dirty = true; }
  if (app->hover.kind != ND_HIT_NONE) {
    app->hover.kind = ND_HIT_NONE;
    app->hover.url = NULL;
    dirty = true;
  }
  nd_cursor_set(app, ND_CURSOR_DEFAULT);
  if (dirty) mark(app, ND_DIRTY_ALL);
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

/* A slim bar along the bottom, shown while searching or while matches stand. */
static void paint_search_bar(struct nd_app *app, cairo_t *cr, int w, int h,
                             double scale) {
  if (!app->searching && nd_doc_search_count(app->doc) == 0) return;

  double bar_h = 34.0;
  double y = (double)h - bar_h;

  nd_src(cr, CTP_CRUST);
  cairo_rectangle(cr, 0, y, w, bar_h);
  cairo_fill(cr);
  nd_src(cr, app->searching ? ND_ACCENT : CTP_SURFACE0);
  cairo_rectangle(cr, 0, nd_snap(y, scale), w, 1.0 / scale);
  cairo_fill(cr);

  cairo_select_font_face(cr, ND_MONO, CAIRO_FONT_SLANT_NORMAL,
                         CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, 13.0);

  char line[192];
  size_t n = nd_doc_search_count(app->doc);
  int cur = nd_doc_search_current(app->doc);
  if (n)
    snprintf(line, sizeof line, "/%s   %d of %zu", app->query, cur + 1, n);
  else if (app->query_len)
    snprintf(line, sizeof line, "/%s   no matches", app->query);
  else
    snprintf(line, sizeof line, "/");

  nd_src(cr, n || !app->query_len ? CTP_TEXT : ND_URGENT);
  cairo_move_to(cr, 16.0, y + 22.0);
  cairo_show_text(cr, line);
}

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
    nd_src(cr, app->dragging ? ND_ACCENT : ND_DIVIDER);
    cairo_rectangle(cr, nd_snap(sidebar, scale), 0, 1.0 / scale, h);
    cairo_fill(cr);
  }

  paint_search_bar(app, cr, w, h, scale);

  cairo_save(cr);
  cairo_rectangle(cr, sidebar, 0, content_w, content_h);
  cairo_clip(cr);
  cairo_translate(cr, sidebar, 0);
  nd_doc_paint(app->doc, cr, app->scroll.offset, content_h);

  /* Hover feedback lives here rather than in the engine, which stays stateless
   * with respect to input. */
  if (app->hover.kind == ND_HIT_LINK || app->hover.kind == ND_HIT_WIKILINK) {
    double hy = app->hover.y - app->scroll.offset + app->hover.h - 1.0;
    nd_src(cr, app->hover.kind == ND_HIT_LINK ? CTP_BLUE : CTP_LAVENDER);
    cairo_rectangle(cr, app->hover.x, nd_snap(hy, scale),
                    app->hover.w, 1.0 / scale);
    cairo_fill(cr);
  }
  cairo_restore(cr);
}

/* ---- file watching ------------------------------------------------------- */

void nd_app_watch_drain(struct nd_app *app) {
  char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
  bool touched = false;
  ssize_t len;

  while ((len = read(app->watch_fd, buf, sizeof buf)) > 0) {
    for (char *p = buf; p < buf + len; ) {
      struct inotify_event *e = (struct inotify_event *)p;
      /* The watch is on the directory, so filter to our own file. */
      if (e->len && app->base_name && strcmp(e->name, app->base_name) == 0)
        touched = true;
      p += sizeof(struct inotify_event) + e->len;
    }
  }

  /* One save emits several events; coalescing them into a single reload keeps
   * a rapid :w from reparsing three times. */
  if (touched) reload_document(app);
}

/* ---- lifecycle ----------------------------------------------------------- */

bool nd_app_init(struct nd_app *app, const char *path, char **err) {
  memset(app, 0, sizeof *app);
  app->running         = true;
  app->sidebar_visible = true;
  app->sidebar_w       = 280.0;
  app->sidebar_target  = 280.0;
  app->sidebar_pref    = 280.0;
  app->font_scale      = 1.0;
  app->watch_fd        = -1;
  app->watch_wd        = -1;
  /* Matches the body line height, so `j`/`k` move exactly one line. */
  app->scroll.line_height = 23.0;
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
  const char *base = dup ? basename(dup) : app->path;
  app->base_name = strdup(base);
  char title[512];
  snprintf(title, sizeof title, "nemdown — %s", base);
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
  free(app->base_name);
  free(app->path);
}
