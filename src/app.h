/* app.h — application state and the actions the shell layers dispatch into. */

#ifndef NEMDOWN_APP_H
#define NEMDOWN_APP_H

#include <cairo.h>
#include <stdbool.h>
#include <stdint.h>
#include <xkbcommon/xkbcommon.h>

#include "doc/doc.h"
#include "ui/sidebar.h"
#include "wl/wl_input.h"
#include "wl/wl_shell.h"
#include "wl/wl_window.h"

/* One wheel detent scrolls three lines, matching every other document viewer. */
#define ND_SCROLL_LINES_PER_DETENT 3.0

/* Relayout is far costlier than repaint, so they are tracked separately. */
enum {
  ND_DIRTY_NONE     = 0,
  ND_DIRTY_DOC      = 1 << 0,
  ND_DIRTY_SIDEBAR  = 1 << 1,
  ND_DIRTY_RELAYOUT = 1 << 2,
  ND_DIRTY_ALL      = ND_DIRTY_DOC | ND_DIRTY_SIDEBAR,
};

struct nd_scroll {
  double offset;
  double target;
  double max;
  double line_height;
  bool   animating;
  uint32_t last_ms;
};

struct nd_app {
  struct nd_wl     wl;
  struct nd_window win;
  struct nd_input  input;

  char    *path;
  char    *base_name; /* inotify watches the directory, so events need filtering */
  nd_doc  *doc;
  int   watch_fd;
  int   watch_wd;

  struct nd_scroll  scroll;
  struct nd_sidebar sidebar;

  nd_hit   hover;      /* what the pointer is over, in document space */
  unsigned dirty;
  bool     running;
  bool     sidebar_visible;
  double   sidebar_w;      /* animated current width; 0 when fully collapsed */
  double   sidebar_target; /* where the toggle animation is heading */
  double   sidebar_pref;   /* user's chosen width, preserved across toggles */
  bool     sidebar_anim;
  uint32_t sidebar_last_ms;

  bool     dragging;       /* divider drag is modal, see nd_app_pointer_button */
  double   drag_start_x, drag_start_w;

  double   font_scale;
};

bool nd_app_init(struct nd_app *app, const char *path, char **err);
void nd_app_finish(struct nd_app *app);
int  nd_app_run(struct nd_app *app);

/* Called from the shell layers. */
void nd_app_paint(struct nd_app *app, cairo_t *cr, int w, int h, double scale);
void nd_app_resized(struct nd_app *app, int w, int h);
void nd_app_scale_changed(struct nd_app *app, double scale);
void nd_app_key(struct nd_app *app, xkb_keysym_t sym, bool is_repeat);
void nd_app_pointer_motion(struct nd_app *app, double x, double y);
void nd_app_pointer_button(struct nd_app *app, double x, double y, bool pressed);
void nd_app_pointer_leave(struct nd_app *app);
void nd_app_zoom(struct nd_app *app, double scale);
void nd_app_scroll_by(struct nd_app *app, double dy, bool immediate);
void nd_app_scroll_settle(struct nd_app *app);
void nd_app_animate(struct nd_app *app, uint32_t time_ms);
void nd_app_tick(struct nd_app *app);
void nd_app_watch_drain(struct nd_app *app);
void nd_app_quit(struct nd_app *app);

bool nd_app_is_dirty(const struct nd_app *app);
void nd_app_clear_dirty(struct nd_app *app);
int  nd_app_poll_timeout(const struct nd_app *app);

#endif /* NEMDOWN_APP_H */
