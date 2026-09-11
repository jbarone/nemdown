/* wl_input.c — seat handling, xkb, key repeat, pointer batching. */

#include "wl/wl_input.h"

#include <linux/input-event-codes.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include "app.h"
#include "util/log.h"

/* ---- key repeat ---------------------------------------------------------- */

static void repeat_disarm(struct nd_input *in) {
  struct itimerspec its;
  memset(&its, 0, sizeof its);
  timerfd_settime(in->repeat_fd, 0, &its, NULL);
  in->repeat_key = 0;
}

static void repeat_arm(struct nd_input *in, xkb_keysym_t sym, uint32_t key) {
  if (in->repeat_rate <= 0) return; /* 0 legitimately means "no repeat" */
  if (in->xkb_keymap && !xkb_keymap_key_repeats(in->xkb_keymap, key + 8)) return;

  in->repeat_sym = sym;
  in->repeat_key = key;

  struct itimerspec its = {
    .it_value    = {.tv_sec  = in->repeat_delay / 1000,
                    .tv_nsec = (long)(in->repeat_delay % 1000) * 1000000L},
    .it_interval = {.tv_sec = 0, .tv_nsec = 1000000000L / in->repeat_rate},
  };
  timerfd_settime(in->repeat_fd, 0, &its, NULL);
}

void nd_input_repeat_tick(struct nd_app *app) {
  struct nd_input *in = &app->input;
  uint64_t expirations = 0;
  if (read(in->repeat_fd, &expirations, sizeof expirations) != sizeof expirations)
    return;
  if (!in->repeat_key) return;

  /* Replay missed ticks so held-key scrolling keeps its velocity, but cap it:
   * after a long stall we do not want to teleport the document. */
  if (expirations > 8) expirations = 8;
  for (uint64_t i = 0; i < expirations; i++)
    nd_app_key(app, in->repeat_sym, true);
}

/* ---- keyboard ------------------------------------------------------------ */

static void kb_keymap(void *data, struct wl_keyboard *kb, uint32_t format,
                      int fd, uint32_t size) {
  (void)kb;
  struct nd_input *in = data;

  if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) { close(fd); return; }

  /* MAP_PRIVATE: the fd is sealed read-only, so MAP_SHARED fails. */
  char *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
  if (map == MAP_FAILED) { close(fd); return; }

  /* size - 1: the buffer is NUL-terminated and the parser must not see it. */
  struct xkb_keymap *km = xkb_keymap_new_from_buffer(
      in->xkb_ctx, map, size - 1, XKB_KEYMAP_FORMAT_TEXT_V1,
      XKB_KEYMAP_COMPILE_NO_FLAGS);

  munmap(map, size);
  close(fd);
  if (!km) { nd_warn("failed to compile keymap"); return; }

  xkb_state_unref(in->xkb_state);
  xkb_keymap_unref(in->xkb_keymap);
  in->xkb_keymap = km;
  in->xkb_state  = xkb_state_new(km);
}

static void kb_enter(void *data, struct wl_keyboard *kb, uint32_t serial,
                     struct wl_surface *surf, struct wl_array *keys) {
  (void)data; (void)kb; (void)serial; (void)surf; (void)keys;
  /* Deliberately ignoring `keys`: synthesising presses for already-held keys
   * produces phantom repeats. */
}

static void kb_leave(void *data, struct wl_keyboard *kb, uint32_t serial,
                     struct wl_surface *surf) {
  (void)kb; (void)serial; (void)surf;
  struct nd_input *in = data;
  /* Without this, alt-tabbing away mid-`j` leaves the document scrolling. */
  repeat_disarm(in);
}

static void kb_key(void *data, struct wl_keyboard *kb, uint32_t serial,
                   uint32_t time, uint32_t key, uint32_t state) {
  (void)kb; (void)serial; (void)time;
  struct nd_input *in = data;
  if (!in->xkb_state) return;

  /* evdev -> X11 keycode offset. */
  xkb_keysym_t sym = xkb_state_key_get_one_sym(in->xkb_state, key + 8);

  if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
    nd_app_key(in->app, sym, false);
    repeat_arm(in, sym, key);
  } else if (in->repeat_key == key) {
    repeat_disarm(in);
  }
}

static void kb_modifiers(void *data, struct wl_keyboard *kb, uint32_t serial,
                         uint32_t depressed, uint32_t latched, uint32_t locked,
                         uint32_t group) {
  (void)kb; (void)serial;
  struct nd_input *in = data;
  if (in->xkb_state)
    xkb_state_update_mask(in->xkb_state, depressed, latched, locked, 0, 0, group);
}

static void kb_repeat_info(void *data, struct wl_keyboard *kb, int32_t rate,
                           int32_t delay) {
  (void)kb;
  struct nd_input *in = data;
  in->repeat_rate  = rate;
  in->repeat_delay = delay;
}

static const struct wl_keyboard_listener kb_listener = {
  .keymap      = kb_keymap,
  .enter       = kb_enter,
  .leave       = kb_leave,
  .key         = kb_key,
  .modifiers   = kb_modifiers,
  .repeat_info = kb_repeat_info,
};

/* ---- pointer ------------------------------------------------------------- */

static void ptr_enter(void *data, struct wl_pointer *p, uint32_t serial,
                      struct wl_surface *surf, wl_fixed_t sx, wl_fixed_t sy) {
  (void)p; (void)surf;
  struct nd_input *in = data;
  in->enter_serial = serial; /* set_shape is ignored without the latest one */
  in->px = wl_fixed_to_double(sx);
  in->py = wl_fixed_to_double(sy);
  in->has_pointer = true;
  nd_app_pointer_motion(in->app, in->px, in->py);
}

static void ptr_leave(void *data, struct wl_pointer *p, uint32_t serial,
                      struct wl_surface *surf) {
  (void)p; (void)serial; (void)surf;
  struct nd_input *in = data;
  in->has_pointer = false;
  nd_app_pointer_leave(in->app);
}

static void ptr_motion(void *data, struct wl_pointer *p, uint32_t time,
                       wl_fixed_t sx, wl_fixed_t sy) {
  (void)p; (void)time;
  struct nd_input *in = data;
  in->pending.have_motion = true;
  /* Surface-local LOGICAL coordinates: never multiply by scale. */
  in->pending.x = wl_fixed_to_double(sx);
  in->pending.y = wl_fixed_to_double(sy);
}

static void ptr_button(void *data, struct wl_pointer *p, uint32_t serial,
                       uint32_t time, uint32_t button, uint32_t state) {
  (void)p;
  struct nd_input *in = data;
  in->pending.have_button = true;
  in->pending.button      = button;
  in->pending.btn_state   = state;
  in->pending.serial      = serial;
  in->pending.time        = time;
}

static void ptr_axis(void *data, struct wl_pointer *p, uint32_t time,
                     uint32_t axis, wl_fixed_t value) {
  (void)p; (void)time;
  struct nd_input *in = data;
  if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL) return;
  in->pending.have_axis = true;
  in->pending.axis_v    = wl_fixed_to_double(value);
}

static void ptr_axis_source(void *data, struct wl_pointer *p, uint32_t source) {
  (void)p;
  struct nd_input *in = data;
  in->pending.source = source;
}

static void ptr_axis_stop(void *data, struct wl_pointer *p, uint32_t time,
                          uint32_t axis) {
  (void)p; (void)time;
  struct nd_input *in = data;
  if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) in->pending.have_stop = true;
}

static void ptr_axis_discrete(void *data, struct wl_pointer *p, uint32_t axis,
                              int32_t discrete) {
  (void)p;
  struct nd_input *in = data;
  /* Deprecated since seat v8, but it is what older compositors send. */
  if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL && !in->pending.have_v120) {
    in->pending.have_v120 = true;
    in->pending.v120      = discrete * 120;
  }
}

static void ptr_axis_value120(void *data, struct wl_pointer *p, uint32_t axis,
                              int32_t value120) {
  (void)p;
  struct nd_input *in = data;
  if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL) return;
  in->pending.have_v120 = true;
  in->pending.v120      = value120;
}

static void ptr_axis_relative_direction(void *data, struct wl_pointer *p,
                                        uint32_t axis, uint32_t direction) {
  (void)data; (void)p; (void)axis; (void)direction;
}

static void ptr_frame(void *data, struct wl_pointer *p) {
  (void)p;
  struct nd_input *in = data;
  struct nd_pointer_frame *f = &in->pending;

  if (f->have_motion) {
    in->px = f->x;
    in->py = f->y;
    nd_app_pointer_motion(in->app, f->x, f->y);
  }

  if (f->have_axis || f->have_v120) {
    bool finger = (f->source == WL_POINTER_AXIS_SOURCE_FINGER ||
                   f->source == WL_POINTER_AXIS_SOURCE_CONTINUOUS);
    if (finger) {
      /* Under a finger there is nothing to animate: track it 1:1. */
      nd_app_scroll_by(in->app, f->axis_v, true);
    } else if (f->have_v120) {
      /* 120 units == one detent. Ease it. */
      nd_app_scroll_by(in->app, (f->v120 / 120.0) * ND_SCROLL_LINES_PER_DETENT, false);
    } else if (f->have_axis) {
      nd_app_scroll_by(in->app, f->axis_v, false);
    }
  }

  if (f->have_stop) nd_app_scroll_settle(in->app);

  if (f->have_button && f->button == BTN_LEFT) {
    nd_app_pointer_button(in->app, in->px, in->py,
                          f->btn_state == WL_POINTER_BUTTON_STATE_PRESSED,
                          f->time);
  }

  memset(f, 0, sizeof *f);
}

static const struct wl_pointer_listener ptr_listener = {
  .enter                   = ptr_enter,
  .leave                   = ptr_leave,
  .motion                  = ptr_motion,
  .button                  = ptr_button,
  .axis                    = ptr_axis,
  .frame                   = ptr_frame,
  .axis_source             = ptr_axis_source,
  .axis_stop               = ptr_axis_stop,
  .axis_discrete           = ptr_axis_discrete,
  .axis_value120           = ptr_axis_value120,
  .axis_relative_direction = ptr_axis_relative_direction,
};

/* ---- seat ---------------------------------------------------------------- */

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps) {
  struct nd_input *in = data;

  bool kb = caps & WL_SEAT_CAPABILITY_KEYBOARD;
  if (kb && !in->kb) {
    in->kb = wl_seat_get_keyboard(seat);
    wl_keyboard_add_listener(in->kb, &kb_listener, in);
  } else if (!kb && in->kb) {
    wl_keyboard_release(in->kb); /* the destructor request, not _destroy */
    in->kb = NULL;
    repeat_disarm(in);
  }

  bool ptr = caps & WL_SEAT_CAPABILITY_POINTER;
  if (ptr && !in->pointer) {
    in->pointer = wl_seat_get_pointer(seat);
    wl_pointer_add_listener(in->pointer, &ptr_listener, in);
  } else if (!ptr && in->pointer) {
    wl_pointer_release(in->pointer);
    in->pointer = NULL;
  }
}

static void seat_name(void *data, struct wl_seat *seat, const char *name) {
  (void)data; (void)seat; (void)name;
}

static const struct wl_seat_listener seat_listener = {
  .capabilities = seat_capabilities,
  .name         = seat_name,
};

void nd_input_bind_seat(struct nd_app *app, struct wl_seat *seat) {
  struct nd_input *in = &app->input;
  in->seat = seat;
  wl_seat_add_listener(seat, &seat_listener, in);
}

bool nd_input_init(struct nd_input *in, struct nd_app *app) {
  in->app = app;
  in->repeat_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
  if (in->repeat_fd < 0) return false;

  in->xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
  return in->xkb_ctx != NULL;
}

void nd_input_finish(struct nd_input *in) {
  if (in->kb)      wl_keyboard_release(in->kb);
  if (in->pointer) wl_pointer_release(in->pointer);
  xkb_state_unref(in->xkb_state);
  xkb_keymap_unref(in->xkb_keymap);
  if (in->xkb_ctx) xkb_context_unref(in->xkb_ctx);
  if (in->repeat_fd >= 0) close(in->repeat_fd);
  memset(in, 0, sizeof *in);
}
