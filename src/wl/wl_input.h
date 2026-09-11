/* wl_input.h — seat, keyboard with xkb, key repeat, and pointer. */

#ifndef NEMDOWN_WL_INPUT_H
#define NEMDOWN_WL_INPUT_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "wl/wl_cursor.h"

struct nd_app;

/* Pointer events arrive scattered and are only coherent at wl_pointer.frame,
 * so they are accumulated here and applied in one go. */
struct nd_pointer_frame {
  bool     have_motion;
  double   x, y;
  bool     have_axis;
  double   axis_v;
  bool     have_haxis;   /* horizontal wheel / two-finger sideways */
  double   axis_h;
  bool     have_v120;
  int32_t  v120;
  bool     have_stop;
  uint32_t source;
  bool     have_button;
  uint32_t button, btn_state, serial, time;
};

struct nd_input {
  struct nd_app *app;

  struct wl_seat     *seat;
  struct wl_keyboard *kb;
  struct wl_pointer  *pointer;

  struct xkb_context *xkb_ctx;
  struct xkb_keymap  *xkb_keymap;
  struct xkb_state   *xkb_state;

  int      repeat_fd;
  int32_t  repeat_rate;  /* keys/sec; 0 disables */
  int32_t  repeat_delay; /* ms */
  uint32_t repeat_key;   /* raw wayland keycode */
  uint32_t last_keycode; /* xkb keycode of the key being handled, for get_utf8 */
  xkb_keysym_t repeat_sym;

  struct nd_pointer_frame pending;
  double   px, py;        /* last known pointer position, logical */
  uint32_t enter_serial;  /* required by cursor-shape set_shape */
  bool     has_pointer;

  bool shift_held; /* shift+wheel pans an overflowing block sideways */

  struct wp_cursor_shape_device_v1 *cursor_dev;
  nd_cursor cursor;
};

bool nd_input_init(struct nd_input *in, struct nd_app *app);
void nd_input_finish(struct nd_input *in);
void nd_input_bind_seat(struct nd_app *app, struct wl_seat *seat);
void nd_input_repeat_tick(struct nd_app *app);

#endif /* NEMDOWN_WL_INPUT_H */
