/* wl_buffer.h — shm buffer ring bound to Cairo image surfaces. */

#ifndef NEMDOWN_WL_BUFFER_H
#define NEMDOWN_WL_BUFFER_H

#include <cairo.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <wayland-client.h>

struct nd_buffer {
  struct wl_buffer *wl;
  uint8_t          *data;
  size_t            size;   /* mmap length */
  int               w, h;   /* device pixels */
  int               stride;
  bool              busy;   /* committed, not yet released */
  bool              zombie; /* superseded by a resize; free on release */
  cairo_surface_t  *cs;
  cairo_t          *cr;
};

#define ND_BUFFER_COUNT 2

/* Slots are heap pointers, not inline structs: on resize a buffer the
 * compositor still holds is detached from the ring and left to its own release
 * callback to free. The listener's data pointer must stay valid until then, so
 * it cannot live inside an array we reuse. */
struct nd_buffers {
  struct nd_buffer *b[ND_BUFFER_COUNT];
  struct wl_shm    *shm;
};

bool nd_buffers_realloc(struct nd_buffers *bs, struct wl_shm *shm,
                        int w, int h, double scale);

/* NULL when every buffer is still held by the compositor: skip the frame
 * rather than drawing into one mid-scanout. */
struct nd_buffer *nd_buffers_acquire(struct nd_buffers *bs);

void nd_buffers_destroy(struct nd_buffers *bs);

#endif /* NEMDOWN_WL_BUFFER_H */
