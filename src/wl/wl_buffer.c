/* wl_buffer.c — memfd-backed shm buffers wrapped in Cairo surfaces.
 *
 * XRGB8888 / CAIRO_FORMAT_RGB24 rather than ARGB: the window is fully opaque,
 * so telling the compositor there is no alpha lets it skip blending entirely.
 * The two formats are byte-identical on little-endian.
 */

#include "wl/wl_buffer.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "util/log.h"

static void buffer_free(struct nd_buffer *b) {
  if (!b) return;
  if (b->cr) cairo_destroy(b->cr);
  if (b->cs) cairo_surface_destroy(b->cs);
  if (b->wl) wl_buffer_destroy(b->wl);
  if (b->data && b->data != MAP_FAILED) munmap(b->data, b->size);
  free(b);
}

static void buffer_release(void *data, struct wl_buffer *wlb) {
  (void)wlb;
  struct nd_buffer *b = data;
  b->busy = false;
  /* Deferred free: a resize could not drop this while the compositor still
   * had it on screen. Now it can. */
  if (b->zombie) buffer_free(b);
}

static const struct wl_buffer_listener buffer_listener = {
  .release = buffer_release,
};

static struct nd_buffer *buffer_create(struct wl_shm *shm,
                                       int w, int h, double scale) {
  struct nd_buffer *b = calloc(1, sizeof *b);
  if (!b) return NULL;

  /* Cairo dictates the stride; w * 4 is not always right. */
  b->stride = cairo_format_stride_for_width(CAIRO_FORMAT_RGB24, w);
  if (b->stride <= 0) { free(b); return NULL; }
  b->size = (size_t)b->stride * (size_t)h;

  int fd = memfd_create("nemdown-shm", MFD_CLOEXEC | MFD_ALLOW_SEALING);
  if (fd < 0) { nd_warn("memfd_create failed"); free(b); return NULL; }

  if (ftruncate(fd, (off_t)b->size) < 0) { close(fd); free(b); return NULL; }
  fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_SEAL);

  b->data = mmap(NULL, b->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (b->data == MAP_FAILED) { close(fd); b->data = NULL; free(b); return NULL; }

  struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int32_t)b->size);
  b->wl = wl_shm_pool_create_buffer(pool, 0, w, h, b->stride,
                                    WL_SHM_FORMAT_XRGB8888);
  wl_shm_pool_destroy(pool); /* the wl_buffer keeps the mapping alive */
  close(fd);                 /* the compositor dup'd it */

  if (!b->wl) { munmap(b->data, b->size); b->data = NULL; free(b); return NULL; }
  wl_buffer_add_listener(b->wl, &buffer_listener, b);

  b->cs = cairo_image_surface_create_for_data(b->data, CAIRO_FORMAT_RGB24,
                                              w, h, b->stride);
  /* The whole HiDPI story, in one call: every drawing and hit-testing
   * coordinate downstream is logical, and Cairo scales to device pixels.
   * Setting it on the surface (not the context) means no cairo_restore can
   * accidentally drop it, and Pango picks it up for hinting. */
  cairo_surface_set_device_scale(b->cs, scale, scale);
  b->cr = cairo_create(b->cs);

  b->w = w;
  b->h = h;
  return b;
}

bool nd_buffers_realloc(struct nd_buffers *bs, struct wl_shm *shm,
                        int w, int h, double scale) {
  bs->shm = shm;
  if (w <= 0 || h <= 0) return false;

  for (int i = 0; i < ND_BUFFER_COUNT; i++) {
    struct nd_buffer *b = bs->b[i];
    if (!b) continue;
    if (b->busy) {
      /* Cannot unmap memory the compositor is still reading. Detach it from
       * the ring; buffer_release owns it now. */
      b->zombie = true;
    } else {
      buffer_free(b);
    }
    bs->b[i] = NULL;
  }

  bool ok = true;
  for (int i = 0; i < ND_BUFFER_COUNT; i++) {
    bs->b[i] = buffer_create(shm, w, h, scale);
    if (!bs->b[i]) ok = false;
  }
  return ok;
}

struct nd_buffer *nd_buffers_acquire(struct nd_buffers *bs) {
  for (int i = 0; i < ND_BUFFER_COUNT; i++) {
    struct nd_buffer *b = bs->b[i];
    if (b && !b->busy) return b;
  }
  nd_dbg("all buffers busy; skipping frame");
  return NULL;
}

void nd_buffers_destroy(struct nd_buffers *bs) {
  for (int i = 0; i < ND_BUFFER_COUNT; i++) {
    /* A buffer still held by the compositor is left to its release callback;
     * we are exiting, so the leak is bounded by process lifetime. */
    if (bs->b[i] && !bs->b[i]->busy) buffer_free(bs->b[i]);
    bs->b[i] = NULL;
  }
  memset(bs, 0, sizeof *bs);
}
