/* wl_cursor.c — cursor-shape-v1 wrapper. */

#include "wl/wl_cursor.h"

#include "app.h"
#include "cursor-shape-v1-client-protocol.h"

void nd_cursor_release(struct nd_input *in) {
  if (!in->cursor_dev) return;
  wp_cursor_shape_device_v1_destroy(in->cursor_dev);
  in->cursor_dev = NULL;
  /* The cached shape describes a device that no longer exists. Forgetting it
   * is what lets the next nd_cursor_set actually issue a request rather than
   * short-circuit on "already set". */
  in->cursor = ND_CURSOR_DEFAULT;
}

void nd_cursor_set(struct nd_app *app, nd_cursor shape) {
  /* The cache is only valid if we can act on it: recording the shape before
   * checking for a device means a shape requested with no pointer attached is
   * remembered as current and never issued once one arrives. */
  if (!app->wl.cursor_mgr || !app->input.pointer) return;
  if (shape == app->input.cursor) return;
  app->input.cursor = shape;

  if (!app->input.cursor_dev) {
    app->input.cursor_dev = wp_cursor_shape_manager_v1_get_pointer(
        app->wl.cursor_mgr, app->input.pointer);
    if (!app->input.cursor_dev) return;
  }

  uint32_t s;
  switch (shape) {
    case ND_CURSOR_POINTER:     s = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER; break;
    case ND_CURSOR_COL_RESIZE:  s = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_COL_RESIZE; break;
    case ND_CURSOR_TEXT:        s = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT; break;
    default:                    s = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT; break;
  }
  /* set_shape is silently ignored without the most recent enter serial. */
  wp_cursor_shape_device_v1_set_shape(app->input.cursor_dev,
                                      app->input.enter_serial, s);
}
