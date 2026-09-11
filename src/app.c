/* app.c — application state, scrolling, and action dispatch. */

#include "app.h"

#include <libgen.h>
#include <math.h>
#include <time.h>
#include <stdlib.h>
#include <strings.h>
#include <string.h>
#include <spawn.h>
#include <sys/inotify.h>
#include <sys/mman.h>
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

/* How long the copy button shows a tick instead of its usual glyph. */
#define ND_COPIED_MS    900

/* One h/l step. Roughly three monospace characters at the default size. */
#define ND_PAN_STEP     36.0

/* The reading marker. Thin and short of the text, so it reads as a margin
 * mark rather than a rule: the point is to answer "where was I" at a glance
 * without competing with the words. */
#define ND_GUIDE_W      2.0
#define ND_GUIDE_GAP    22.0   /* left of the column */
#define ND_GUIDE_ALPHA  0.80
#define ND_GUIDE_TAU    0.050  /* it should arrive before you look for it */
#define ND_GUIDE_INSET  6.0    /* never flush against the window edge */
#define ND_GUIDE_STEP_INSET 12.0 /* where { and } land the stop they move to */

/* Scrollbars stay solid this long after the last scroll, then fade. */
#define ND_SB_HOLD_MS   800
#define ND_SB_FADE_TAU  0.12

/* Monotonic milliseconds, matching the units the frame callback reports. */
static uint32_t nd_now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static void scroll_clamp(struct nd_app *app);
static void sidebar_animate(struct nd_app *app, uint32_t time_ms);
static void reload_document(struct nd_app *app);
static void copy_to_clipboard(const char *text);
static void navigate_back(struct nd_app *app);
static void guide_step(struct nd_app *app, int delta);
static void guide_goto(struct nd_app *app, int idx);
static bool guide_drives(const struct nd_app *app);

static void mark(struct nd_app *app, unsigned bits) {
  app->dirty |= bits;
  nd_window_damage(&app->win);
}

bool nd_app_is_dirty(const struct nd_app *app) {
  /* A scrollbar that is merely HELD solid needs no frames — only one that is
   * actively fading does. Treating "visible" as dirty would render at the
   * display's full rate for the whole hold, doing nothing. */
  bool fading = app->scrollbar_alpha > 0.01 && app->scroll_activity_ms &&
                (nd_now_ms() - app->scroll_activity_ms) >= ND_SB_HOLD_MS;

  return app->dirty != ND_DIRTY_NONE || app->scroll.animating ||
         app->sidebar_anim || app->guide_anim || fading;
}

void nd_app_clear_dirty(struct nd_app *app) {
  app->dirty = ND_DIRTY_NONE;
}

/* Earliest deadline of any timed effect, or 0 for none. Animation is paced by
 * frame callbacks, so the poll timeout exists only for effects that must fire
 * with no further input to trigger a frame. */
static uint32_t next_deadline(const struct nd_app *app) {
  uint32_t best = 0;

  if (app->copied_until_ms) best = app->copied_until_ms;

  /* Nothing else would wake us to finish a debounced reload. */
  if (app->reload_at_ms && (!best || app->reload_at_ms < best))
    best = app->reload_at_ms;

  /* The scrollbar hold expires silently: nothing else would wake us to start
   * the fade. Once it IS fading, the frame callback carries it. */
  if (app->scrollbar_alpha >= 1.0 && app->scroll_activity_ms) {
    uint32_t hold = app->scroll_activity_ms + ND_SB_HOLD_MS;
    if (!best || hold < best) best = hold;
  }
  return best;
}

int nd_app_poll_timeout(const struct nd_app *app) {
  uint32_t deadline = next_deadline(app);
  if (!deadline) return -1;

  uint32_t now = nd_now_ms();
  if (now >= deadline) return 0;
  return (int)(deadline - now);
}

/* ---- scrolling ----------------------------------------------------------- */

static void scroll_clamp(struct nd_app *app) {
  struct nd_scroll *s = &app->scroll;
  if (s->target < 0.0) s->target = 0.0;
  if (s->target > s->max) s->target = s->max;
}

static void note_scroll_activity(struct nd_app *app) {
  app->scroll_activity_ms = nd_now_ms();
  app->scrollbar_alpha = 1.0;
}

void nd_app_scroll_by(struct nd_app *app, double dy, bool immediate) {
  note_scroll_activity(app);
  /* Scrolling by hand hands control of the cursor back to the viewport. */
  app->guide_pinned = false;

  /* The sidebar's panes scroll independently, and the wheel belongs to
   * whichever one the pointer is over. */
  if (app->sidebar_visible && app->input.has_pointer &&
      app->input.px < app->sidebar_w) {
    nd_pane pane = nd_sidebar_pane_at(&app->sidebar, app->input.py);
    nd_sidebar_scroll(&app->sidebar, pane, dy);
    mark(app, ND_DIRTY_SIDEBAR);
    return;
  }

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

bool nd_app_pan(struct nd_app *app, double x, double y, double dx) {
  if (!app->doc) return false;
  if (app->sidebar_visible && x < app->sidebar_w) return false;

  double cx = x - (app->sidebar_visible ? app->sidebar_w : 0.0);
  if (!nd_doc_pan_code(app->doc, cx, y + app->scroll.offset, dx)) return false;

  mark(app, ND_DIRTY_DOC);
  return true;
}

void nd_app_scroll_settle(struct nd_app *app) {
  struct nd_scroll *s = &app->scroll;
  s->offset    = s->target;
  s->animating = false;
  mark(app, ND_DIRTY_ALL);
}

/* Recomputes which stop is being read and points the marker at it. Cheap (a
 * binary search), so it runs on the paint path rather than being invalidated
 * from a dozen places. */
/* Is stop `i` wholly inside the viewport? */
static bool guide_whole(struct nd_app *app, int i, double top, double bot) {
  double x, y, w, h;
  if (i < 0 || !nd_doc_reading_rect(app->doc, (uint32_t)i, &x, &y, &w, &h))
    return false;
  return y >= top && y + h <= bot;
}

/* Points the marker at the cursor's block, and drags the cursor along only
 * when free scrolling has pushed it off screen -- by the smallest number of
 * stops that puts it back, so it never jumps across the page. Stepping with
 * { and } pins it, because mid-flight the block it is heading for is briefly
 * not visible and this would otherwise haul it back. */
static void guide_sync(struct nd_app *app) {
  if (!app->doc) return;
  uint32_t n = nd_doc_reading_count(app->doc);
  if (n == 0) { app->guide_idx = -1; return; }

  /* The pin belongs to one specific scroll, the one guide_goto asked for. If
   * anything else has moved the target since -- a search jumping to a match, a
   * contents click, a wikilink -- the pin is stale and the cursor goes back to
   * following the viewport. Checking the target rather than clearing the pin at
   * each call site means every one of them is covered, including the ones added
   * later: a search used to leave the cursor pinned to a block far above the
   * match, where it was clipped away and looked like it had vanished. */
  if (app->guide_pinned && app->scroll.target != app->guide_pin_target)
    app->guide_pinned = false;

  double top = app->scroll.offset, bot = top + (double)app->win.h;
  int idx = app->guide_idx;

  if (idx < 0 || (uint32_t)idx >= n) {
    idx = nd_doc_reading_at(app->doc, top, (double)app->win.h);
  } else if (!app->guide_pinned && !guide_whole(app, idx, top, bot)) {
    double x, y, w, h;
    nd_doc_reading_rect(app->doc, (uint32_t)idx, &x, &y, &w, &h);
    int j = idx;
    if (y < top) while (j + 1 < (int)n && !guide_whole(app, j, top, bot)) j++;
    else         while (j > 0 && !guide_whole(app, j, top, bot)) j--;
    idx = guide_whole(app, j, top, bot)
              ? j
              : nd_doc_reading_at(app->doc, top, (double)app->win.h);
  }

  app->guide_idx = idx;
  if (idx < 0) return;

  double x, y, w, h;
  if (!nd_doc_reading_rect(app->doc, (uint32_t)idx, &x, &y, &w, &h)) return;
  if (y != app->guide_ty || h != app->guide_th) {
    app->guide_ty = y;
    app->guide_th = h;
    /* First placement lands instantly; only later moves are worth animating,
     * or the marker slides in from the top of the document on open. */
    if (app->guide_h <= 0.0) {
      app->guide_y = y;
      app->guide_h = h;
    } else {
      app->guide_anim = true;
    }
  }
}

static void guide_animate(struct nd_app *app, uint32_t time_ms) {
  if (!app->guide_anim) { app->guide_last_ms = time_ms; return; }

  double dt = (double)(time_ms - app->guide_last_ms) / 1000.0;
  app->guide_last_ms = time_ms;
  if (dt <= 0.0 || dt > 0.05) dt = 1.0 / 60.0;

  double k = 1.0 - exp(-dt / ND_GUIDE_TAU);
  app->guide_y += (app->guide_ty - app->guide_y) * k;
  app->guide_h += (app->guide_th - app->guide_h) * k;

  /* Snap and stop, or the smoother asymptotes and we never reach idle. */
  if (fabs(app->guide_ty - app->guide_y) < 0.3 &&
      fabs(app->guide_th - app->guide_h) < 0.3) {
    app->guide_y = app->guide_ty;
    app->guide_h = app->guide_th;
    app->guide_anim = false;
  }
  app->dirty |= ND_DIRTY_DOC;
}

static void scrollbar_animate(struct nd_app *app, uint32_t time_ms) {
  if (app->scrollbar_alpha <= 0.0) return;

  uint32_t since = time_ms - app->scroll_activity_ms;
  if (since < ND_SB_HOLD_MS) return; /* still held solid */

  double dt = 1.0 / 60.0;
  app->scrollbar_alpha -= (1.0 - exp(-dt / ND_SB_FADE_TAU)) * app->scrollbar_alpha;
  if (app->scrollbar_alpha < 0.02) app->scrollbar_alpha = 0.0;

  app->sidebar.props_alpha = app->scrollbar_alpha;
  app->sidebar.toc_alpha = app->scrollbar_alpha;
  app->dirty |= ND_DIRTY_ALL;
}

void nd_app_animate(struct nd_app *app, uint32_t time_ms) {
  scrollbar_animate(app, time_ms);
  sidebar_animate(app, time_ms);
  guide_animate(app, time_ms);

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
  if (app->copied_until_ms && nd_now_ms() >= app->copied_until_ms) {
    app->copied_until_ms = 0;
    app->dirty |= ND_DIRTY_DOC;
  }
  if (app->reload_at_ms && nd_now_ms() >= app->reload_at_ms) {
    app->reload_at_ms = 0;
    reload_document(app);
  }
  /* The hold expiring is what starts the fade; without this the bars would sit
   * solid until some other event happened to request a frame.
   *
   * Dropping alpha below 1.0 here is what makes this deadline self-clearing,
   * like the two above. next_deadline reports it only while alpha is exactly
   * 1.0, and only the frame callback lowers alpha -- so if frames stop arriving
   * while the bar is solid (nothing composited, or both buffers stuck busy),
   * the deadline stays permanently in the past and nd_app_poll_timeout returns
   * 0 forever. That is the 100%-CPU spin. The fade itself stays frame-driven. */
  if (app->scrollbar_alpha >= 1.0 && app->scroll_activity_ms &&
      nd_now_ms() - app->scroll_activity_ms >= ND_SB_HOLD_MS) {
    app->scrollbar_alpha = 0.999;
    app->dirty |= ND_DIRTY_ALL;
  }
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
      /* snprintf-shaped: this returns the bytes REQUIRED, not the bytes
       * written, so a key level carrying several keysyms reports more than it
       * wrote. Bounding n only against app->query would then memcpy past the
       * end of buf — an 8-byte read of adjacent stack straight into the query,
       * which is painted and handed to g_utf8_casefold. The `< sizeof buf`
       * test is the load-bearing half; a larger buf would not fix it. */
      int n = xkb_state_key_get_utf8(app->input.xkb_state,
                                     app->input.last_keycode, buf, sizeof buf);
      if (n > 0 && (size_t)n < sizeof buf && (unsigned char)buf[0] >= 0x20 &&
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

    /* j/k move the cursor a block; the arrows still scroll a line, so there
     * is always a way to nudge the page without moving where you are. */
    case XKB_KEY_j:
      if (guide_drives(app)) guide_step(app, +1);
      else                   nd_app_scroll_by(app, line, false);
      break;
    case XKB_KEY_k:
      if (guide_drives(app)) guide_step(app, -1);
      else                   nd_app_scroll_by(app, -line, false);
      break;

    case XKB_KEY_Down:  nd_app_scroll_by(app, line, false); break;
    case XKB_KEY_Up:    nd_app_scroll_by(app, -line, false); break;

    case XKB_KEY_Page_Down: nd_app_scroll_by(app, page, false); break;
    case XKB_KEY_Page_Up:   nd_app_scroll_by(app, -page, false); break;

    case XKB_KEY_g:
    case XKB_KEY_Home:
      /* Taking the cursor along, or the page jumps to the top and the marker
       * is left behind at whatever block the viewport happens to pull it to. */
      if (guide_drives(app)) { guide_goto(app, 0); break; }
      app->scroll.target = 0.0;
      app->scroll.animating = true;
      mark(app, ND_DIRTY_ALL);
      break;

    case XKB_KEY_G:
    case XKB_KEY_End:
      if (guide_drives(app)) {
        guide_goto(app, (int)nd_doc_reading_count(app->doc) - 1);
        break;
      }
      app->scroll.target = app->scroll.max;
      app->scroll.animating = true;
      mark(app, ND_DIRTY_ALL);
      break;

    case XKB_KEY_o: {
      /* Start where the current document lives: opening a note almost always
       * means opening one beside it. */
      char *dir = strdup(nd_doc_path(app->doc));
      char *slash = dir ? strrchr(dir, '/') : NULL;
      if (slash) *slash = '\0';
      if (!nd_filechooser_start(&app->chooser, slash ? dir : NULL))
        nd_warn("no file chooser available (is xdg-desktop-portal running?)");
      free(dir);
      break;
    }

    case XKB_KEY_f:
      app->guide_on = !app->guide_on;
      mark(app, ND_DIRTY_DOC);
      break;

    /* vim's paragraph motions, which is what these already mean to the hands
     * they are aimed at. */
    case XKB_KEY_braceright:
      guide_step(app, +1);
      break;
    case XKB_KEY_braceleft:
      guide_step(app, -1);
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

    case XKB_KEY_BackSpace:
      navigate_back(app);
      break;

    case XKB_KEY_h:
    case XKB_KEY_l: {
      double dx = (sym == XKB_KEY_l ? 1.0 : -1.0) * ND_PAN_STEP * app->font_scale;
      if (nd_doc_pan_focused(app->doc, app->scroll.offset,
                             (double)app->win.h, dx))
        mark(app, ND_DIRTY_DOC);
      break;
    }

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

/* Re-points the inotify watch at whichever directory the document now lives
 * in, so live reload follows navigation. */
static void rewatch(struct nd_app *app) {
  if (app->watch_fd < 0) return;
  if (app->watch_wd >= 0) inotify_rm_watch(app->watch_fd, app->watch_wd);

  char *dup = strdup(nd_doc_path(app->doc));
  if (!dup) return;
  char *dirc = strdup(dup);
  app->watch_wd = inotify_add_watch(app->watch_fd, dirname(dirc),
                                    IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE);
  free(dirc);

  free(app->base_name);
  char *basec = strdup(dup);
  app->base_name = strdup(basename(basec));
  free(basec);
  free(dup);
}

static void set_title(struct nd_app *app) {
  char *dup = strdup(nd_doc_path(app->doc));
  char title[512];
  snprintf(title, sizeof title, "nemdown — %s", dup ? basename(dup) : "");
  free(dup);
  if (app->win.toplevel) xdg_toplevel_set_title(app->win.toplevel, title);
}

/* Loads a different document. `anchor` is an optional heading slug to land on.
 * Pushing the current path lets Backspace walk back out. */
static void navigate(struct nd_app *app, const char *path, const char *anchor,
                     bool push_history) {
  char *err = NULL;

  bool pushed = false;
  if (push_history &&
      app->history_n < (int)(sizeof app->history / sizeof app->history[0])) {
    char *prev = strdup(nd_doc_path(app->doc));
    if (prev) {
      app->history[app->history_n] = prev;
      app->history_scroll[app->history_n] = app->scroll.offset;
      app->history_n++;
      pushed = true;
    }
  }

  if (!nd_doc_load(app->doc, path, &err)) {
    nd_warn("%s", err ? err : "cannot open");
    free(err);
    /* Roll back only a push we actually made. Testing push_history alone
     * would pop a real entry when the stack was already full. */
    if (pushed) {
      app->history_n--;
      free(app->history[app->history_n]);
      app->history[app->history_n] = NULL;
    }
    return;
  }

  /* Every borrowed pointer died with the old tree. */
  app->sidebar.hover_toc = -1;
  app->sidebar.toc_scroll = 0;
  app->sidebar.props_scroll = 0;
  app->hover.kind = ND_HIT_NONE;
  app->hover.url = NULL;
  nd_doc_search_clear(app->doc);
  app->searching = false;
  app->query_len = 0;

  double sidebar = app->sidebar_visible ? app->sidebar_w : 0.0;
  nd_doc_layout(app->doc, (double)app->win.w - sidebar, app->font_scale);

  double y = 0.0;
  if (anchor) {
    double ay = nd_doc_anchor_y(app->doc, anchor);
    if (ay >= 0) y = ay - 16.0;
  }
  if (y < 0) y = 0;
  app->scroll.offset = app->scroll.target = y;
  app->scroll.animating = false;

  set_title(app);
  rewatch(app);
  mark(app, ND_DIRTY_ALL | ND_DIRTY_RELAYOUT);
}

static void navigate_back(struct nd_app *app) {
  if (app->history_n <= 0) return;

  app->history_n--;
  char *path = app->history[app->history_n];
  double y = app->history_scroll[app->history_n];
  app->history[app->history_n] = NULL;

  navigate(app, path, NULL, false);
  /* Restore where the reader was, not the top of the file. */
  app->scroll.offset = app->scroll.target = y;
  free(path);
  mark(app, ND_DIRTY_ALL);
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
  memset(&app->hover, 0, sizeof app->hover);
  /* The stops belong to the tree that just died. Zeroing the height makes the
   * next sync place the marker outright instead of sliding it from wherever it
   * happened to be in the previous document. */
  app->guide_h = 0.0;
  app->guide_th = 0.0;
  app->guide_anim = false;
  app->guide_idx = 0;
  app->guide_pinned = true;

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

  /* A memfd, not a pipe. A pipe holds 64KB, and Ctrl-C with no selection copies
   * the whole document -- up to 32MB, and the document is not ours. Writing
   * that through a pipe blocks the main loop until wl-copy drains it, and a
   * loop that is not running is not answering xdg_wm_base.ping, which is how
   * Hyprland decides a client is hung. A memfd takes the whole payload without
   * blocking, and wl-copy reads it as stdin at its own pace after we are gone. */
  size_t len = strlen(text);
  int fd = memfd_create("nemdown-copy", MFD_CLOEXEC);
  if (fd < 0) { nd_warn("memfd_create failed"); return; }

  for (size_t off = 0; off < len; ) {
    ssize_t n = write(fd, text + off, len - off);
    if (n <= 0) { nd_warn("could not stage the clipboard payload"); close(fd); return; }
    off += (size_t)n;
  }
  if (lseek(fd, 0, SEEK_SET) != 0) { close(fd); return; }

  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_adddup2(&fa, fd, STDIN_FILENO);

  char *argv[] = {(char *)"wl-copy", NULL};
  extern char **environ;
  pid_t pid;
  int rc = posix_spawnp(&pid, "wl-copy", &fa, NULL, argv, environ);
  posix_spawn_file_actions_destroy(&fa);
  close(fd);

  if (rc != 0) nd_warn("wl-copy not available");
  /* wl-copy daemonises to own the selection; do not wait for it. */
}

/* xdg-open dispatches any `scheme:` it recognises to x-scheme-handler/<scheme>,
 * and treats a bare path as a file to mime-dispatch. A document we did not
 * write should not be able to reach arbitrary desktop handlers, or to name a
 * local file that happens to sit next to it, on a single click. Only the
 * schemes a markdown document has any business using are allowed through. */
static bool url_scheme_allowed(const char *url) {
  static const char *ok[] = {"http://", "https://", "mailto:", NULL};
  for (const char **p = ok; *p; p++)
    if (strncasecmp(url, *p, strlen(*p)) == 0) return true;
  return false;
}

/* A scheme we allow does not make the rest of the string safe. `<https://x/ --f>`
 * is a single link destination as far as md4c is concerned, and a raw newline
 * survives the same way, so an allowlisted URL can still carry a space or a
 * control character into an argv element. This xdg-open happens to quote its
 * arguments, but that is a property of the script on this machine, not a
 * guarantee we hold -- so make it local. */
static bool url_bytes_safe(const char *url) {
  for (const unsigned char *p = (const unsigned char *)url; *p; p++)
    if (*p < 0x21 || *p == 0x7F) return false;
  return true;
}

/* posix_spawn with an argv array, never a shell string: a document can contain
 * any URL it likes and none of it should reach a shell. */
static void open_external(const char *url) {
  if (!url || !*url) return;

  /* A leading '-' would be read by xdg-open as an option, not a URL. */
  if (url[0] == '-' || !url_scheme_allowed(url)) {
    nd_warn("refusing to open '%s': only http, https and mailto links are followed",
            url);
    return;
  }
  if (!url_bytes_safe(url)) {
    nd_warn("refusing to open a link containing whitespace or control characters");
    return;
  }

  char *argv[] = {(char *)"xdg-open", (char *)url, NULL};
  extern char **environ;
  pid_t pid;
  /* No reaping here: WNOHANG immediately after a spawn returns before the
   * child has exited and reaps nothing. SIGCHLD is set to SIG_IGN in main(),
   * which makes the kernel discard the status for us. */
  posix_spawnp(&pid, "xdg-open", NULL, NULL, argv, environ);
}

/* Moves the marker a whole stop and brings it into view. The scroll lands the
 * stop just below the top edge, which is where guide_sync would have put the
 * marker anyway, so stepping and scrolling cannot disagree about where you
 * are. */
/* Puts the cursor on a specific stop and lets the page follow. */
static void guide_goto(struct nd_app *app, int idx) {
  if (!app->doc) return;
  uint32_t n = nd_doc_reading_count(app->doc);
  if (n == 0) return;
  if (idx < 0) idx = 0;
  if ((uint32_t)idx >= n) idx = (int)n - 1;

  double x, y, w, h;
  if (!nd_doc_reading_rect(app->doc, (uint32_t)idx, &x, &y, &w, &h)) return;

  app->guide_idx = idx;
  app->guide_pinned = true;

  /* Centre the cursor, then let the document's own bounds clamp it. That one
   * line is the whole behaviour: near the top the centred offset is negative
   * and clamps to 0, so the page holds still and the marker walks down it;
   * through the middle the page scrolls to keep the marker centred; near the
   * end the offset exceeds the maximum and clamps there, so the page holds
   * still again and the marker walks down to the last block. */
  app->scroll.target = y + h / 2.0 - (double)app->win.h / 2.0;
  scroll_clamp(app);            /* record the CLAMPED target, not the raw one */
  app->guide_pin_target = app->scroll.target;
  app->scroll.animating = true;
  note_scroll_activity(app);
  mark(app, ND_DIRTY_ALL);
}

static void guide_step(struct nd_app *app, int delta) {
  if (!app->doc) return;
  int idx = app->guide_idx;
  if (idx < 0) idx = nd_doc_reading_at(app->doc, app->scroll.offset,
                                       (double)app->win.h);
  guide_goto(app, idx + delta);
}

/* True when the cursor is the thing that should move. With the guide switched
 * off there is nothing on screen to follow, so the keys go back to scrolling. */
static bool guide_drives(const struct nd_app *app) {
  return app->guide_on && app->doc && nd_doc_reading_count(app->doc) > 0;
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

  if (hit.kind != app->hover.kind || hit.url != app->hover.url ||
      hit.x != app->hover.x || hit.y != app->hover.y) {
    app->hover = hit;
    mark(app, ND_DIRTY_DOC);
  }

  nd_cursor shape = ND_CURSOR_DEFAULT;
  if (toc_hover >= 0) shape = ND_CURSOR_POINTER;
  else if (hit.kind == ND_HIT_LINK || hit.kind == ND_HIT_CHECKBOX ||
           hit.kind == ND_HIT_CODE_COPY || hit.kind == ND_HIT_CALLOUT_FOLD)
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
  bool got = nd_doc_hit_test(app->doc, cx, cy, &hit);

  /* ND_HIT_CODE means "inside a fence but not on its button" — that is not an
   * action, and selecting code by dragging is worth keeping. */
  if (!got || hit.kind == ND_HIT_CODE) {
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

    case ND_HIT_CODE_COPY: {
      char *text = strndup(hit.text ? hit.text : "", hit.text_len);
      if (text) {
        copy_to_clipboard(text);
        free(text);
      }
      app->copied_until_ms = nd_now_ms() + ND_COPIED_MS;
      app->copied_x = hit.x;
      app->copied_y = hit.y;
      mark(app, ND_DIRTY_DOC);
      break;
    }

    case ND_HIT_CALLOUT_FOLD:
      nd_doc_toggle_fold(app->doc, cx, cy);
      mark(app, ND_DIRTY_ALL | ND_DIRTY_RELAYOUT);
      break;

    case ND_HIT_WIKILINK: {
      /* The engine resolved this to a real path, and appended #anchor if the
       * link had one. Unresolved wikilinks never produce a hit at all. */
      if (!hit.url) break;
      char *dup = strdup(hit.url);
      if (!dup) break;
      char *hash = strrchr(dup, '#');
      const char *anchor = NULL;
      if (hash) { *hash = '\0'; anchor = hash + 1; }
      navigate(app, dup, anchor, true);
      free(dup);
      break;
    }

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
  /* A new height can put the cursor's block off screen. */
  app->guide_pinned = false;
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
  guide_sync(app);
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
    app->sidebar.props_alpha = app->scrollbar_alpha;
    app->sidebar.toc_alpha = app->scrollbar_alpha;

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

  /* Document scrollbar, over the content pane's right edge. */
  if (app->scrollbar_alpha > 0.01 && app->scroll.max > 1.0) {
    double track = content_h - 8.0;
    double frac = content_h / (content_h + app->scroll.max);
    double thumb = track * frac;
    if (thumb < 28.0) thumb = 28.0;

    double t = app->scroll.offset / app->scroll.max;
    if (t < 0) t = 0;
    if (t > 1) t = 1;

    double bx = (double)w - 4.0 - 3.0;
    double by = 4.0 + t * (track - thumb);

    nd_src_a(cr, CTP_OVERLAY0, 0.45 * app->scrollbar_alpha);
    cairo_new_sub_path(cr);
    cairo_arc(cr, bx + 2.0, by + 2.0,         2.0, G_PI, 3 * G_PI / 2);
    cairo_arc(cr, bx + 2.0, by + thumb - 2.0, 2.0, G_PI / 2, G_PI);
    cairo_close_path(cr);
    cairo_rectangle(cr, bx, by + 2.0, 4.0, thumb - 4.0);
    cairo_fill(cr);
  }

  paint_search_bar(app, cr, w, h, scale);

  cairo_save(cr);
  cairo_rectangle(cr, sidebar, 0, content_w, content_h);
  cairo_clip(cr);
  cairo_translate(cr, sidebar, 0);
  /* Behind the text and in the margin, so it never sits on a glyph. */
  if (app->guide_on && app->guide_idx >= 0 && app->guide_h > 0.0) {
    /* Clipped to the viewport, not just culled against it. A stop taller than
     * the window -- a long fence, a big table -- would otherwise run off both
     * edges, and a stop partly scrolled past would show a stub at the very top
     * with its text already gone. Either way the mark has to stay somewhere it
     * can be seen. */
    double top = app->guide_y - app->scroll.offset;
    double bot = top + app->guide_h;
    if (top < ND_GUIDE_INSET) top = ND_GUIDE_INSET;
    if (bot > content_h - ND_GUIDE_INSET) bot = content_h - ND_GUIDE_INSET;

    if (bot - top >= 1.0) {
      double gx = nd_doc_column_x(app->doc) - ND_GUIDE_GAP;
      if (gx < 2.0) gx = 2.0;   /* a narrow window has no margin to spare */
      nd_src_a(cr, ND_ACCENT, ND_GUIDE_ALPHA);
      cairo_rectangle(cr, nd_snap(gx, scale), top, ND_GUIDE_W, bot - top);
      cairo_fill(cr);
    }
  }

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

  /* The copy button: drawn only while its fence is hovered, and by the shell
   * rather than the engine, which knows nothing about the pointer. */
  if (app->hover.kind == ND_HIT_CODE || app->hover.kind == ND_HIT_CODE_COPY) {
    bool armed = app->hover.kind == ND_HIT_CODE_COPY;
    double bx = app->hover.x;
    double by = app->hover.y - app->scroll.offset;
    double s = app->hover.w;

    cairo_new_sub_path(cr);
    double r = 6.0;
    cairo_arc(cr, bx + s - r, by + r,     r, -G_PI / 2, 0);
    cairo_arc(cr, bx + s - r, by + s - r, r, 0,          G_PI / 2);
    cairo_arc(cr, bx + r,     by + s - r, r, G_PI / 2,   G_PI);
    cairo_arc(cr, bx + r,     by + r,     r, G_PI,       3 * G_PI / 2);
    cairo_close_path(cr);

    nd_src(cr, armed ? CTP_SURFACE2 : CTP_SURFACE0);
    cairo_fill_preserve(cr);
    nd_src_a(cr, CTP_SURFACE2, armed ? 1.0 : 0.6);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);

    bool flashing = app->copied_until_ms != 0 &&
                    fabs(app->copied_x - bx) < 1.0 &&
                    fabs(app->copied_y - app->hover.y) < 1.0;

    nd_src(cr, flashing ? CTP_GREEN : (armed ? CTP_TEAL : CTP_OVERLAY1));
    cairo_select_font_face(cr, ND_SANS, CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 12.0);
    cairo_move_to(cr, bx + 5.5, by + 16.0);
    /* U+F0C5 copy, U+F00C check — both inside Hack Nerd Font's icon range. */
    cairo_show_text(cr, flashing ? "\xEF\x80\x8C" : "\xEF\x83\x85");
  }
  cairo_restore(cr);
}

/* ---- file watching ------------------------------------------------------- */

void nd_app_chooser_ready(struct nd_app *app) {
  char *path = nd_filechooser_take(&app->chooser);
  if (!path) return;   /* cancelled, or the portal failed and said so */

  /* A file the user picked is user intent, so it re-homes the containment
   * root; a path a document names never does. navigate() is not reused here
   * for exactly that reason -- it is the document-driven path. */
  char *err = NULL;
  if (!nd_doc_open_chosen(app->doc, path, &err)) {
    nd_warn("%s", err ? err : "cannot open");
    free(err);
    free(path);
    return;
  }
  free(err);

  /* Everything doc.h handed out died with the old tree. */
  app->sidebar.hover_toc = -1;
  memset(&app->hover, 0, sizeof app->hover);
  app->guide_h = 0.0;
  app->guide_th = 0.0;
  app->guide_anim = false;
  app->guide_idx = 0;
  app->guide_pinned = true;
  app->history_n = 0;   /* a new document is a new trail, not a continuation */

  app->scroll.offset = app->scroll.target = 0.0;
  app->scroll.animating = false;
  rewatch(app);
  set_title(app);
  free(path);
  mark(app, ND_DIRTY_ALL | ND_DIRTY_RELAYOUT);
}

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

  /* One save emits several events, and an editor's write-then-rename can
   * straddle two poll wake-ups. Arming a deadline coalesces the burst into a
   * single reload instead of reparsing two or three times. */
  if (touched) app->reload_at_ms = nd_now_ms() + ND_INOTIFY_DEBOUNCE_MS;
}

/* ---- lifecycle ----------------------------------------------------------- */

bool nd_app_init(struct nd_app *app, const char *path, char **err) {
  memset(app, 0, sizeof *app);
  app->running         = true;
  app->sidebar_visible = true;
  /* On by default: it is the sort of thing that only helps if it is already
   * there when you start reading. `f` turns it off. */
  app->chooser.fd      = -1;   /* idle; poll ignores a negative fd */
  app->chooser.wfd     = -1;
  app->guide_on        = true;
  app->guide_idx       = 0;      /* a document opens marked at its first block */
  app->guide_pinned    = true;
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
    if (nd_wl_dispatch(app) < 0) {
      /* Returning 0 here made a fatal protocol error indistinguishable from
       * the user pressing q: the process just vanished, status 0, no
       * diagnostic. libwayland has already recorded why. */
      int err = nd_wl_display_error(&app->wl);
      if (err) nd_warn("disconnected from the compositor: %s", strerror(err));
      return 1;
    }
  }
  return 0;
}

void nd_app_finish(struct nd_app *app) {
  /* Joins the worker if a dialog is still open, rather than leaving a thread
   * writing into a pipe whose read end is about to go. */
  nd_filechooser_finish(&app->chooser);
  nd_window_destroy(&app->win);
  nd_doc_free(app->doc);
  nd_input_finish(&app->input);
  nd_wl_disconnect(&app->wl);
  if (app->watch_fd >= 0) close(app->watch_fd);
  for (int i = 0; i < app->history_n; i++) free(app->history[i]);
  free(app->base_name);
  free(app->path);
}
