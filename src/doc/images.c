/* images.c — gdk-pixbuf and librsvg into Cairo surfaces, with an LRU cache. */

#include "doc/images.h"

#include <librsvg/rsvg.h>
#include <ctype.h>
#include <stdint.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <gdk-pixbuf/gdk-pixbuf.h>

#include "doc/pathguard.h"
#include "util/log.h"

#define ND_CACHE_MAX 48

struct entry {
  char            *key;   /* resolved path + requested size */
  cairo_surface_t *surf;
  uint64_t         used;  /* monotonic tick, for LRU eviction */
};

struct nd_image_cache {
  char        *base_dir;
  char        *root_dir;
  double       scale;
  struct entry e[ND_CACHE_MAX];
  unsigned     n;
  uint64_t     tick;
};

struct nd_image_cache *nd_images_new(const char *base_dir,
                                     const char *root_dir) {
  struct nd_image_cache *c = calloc(1, sizeof *c);
  if (!c) return NULL;
  c->base_dir = strdup(base_dir ? base_dir : ".");
  c->root_dir = strdup(root_dir ? root_dir : (base_dir ? base_dir : "."));
  c->scale = 1.0;
  return c;
}

static void entry_clear(struct entry *e) {
  if (e->surf) cairo_surface_destroy(e->surf);
  free(e->key);
  memset(e, 0, sizeof *e);
}

void nd_images_free(struct nd_image_cache *c) {
  if (!c) return;
  for (unsigned i = 0; i < c->n; i++) entry_clear(&c->e[i]);
  free(c->base_dir);
  free(c->root_dir);
  free(c);
}

void nd_images_set_scale(struct nd_image_cache *c, double scale) {
  if (!c || scale == c->scale) return;
  c->scale = scale;
  /* Surfaces were rasterised for the old scale; SVG especially would blur. */
  for (unsigned i = 0; i < c->n; i++) entry_clear(&c->e[i]);
  c->n = 0;
}

/* Percent-decoding, because md4c hands back the raw link destination. */
static void percent_decode(char *s) {
  char *w = s;
  for (char *r = s; *r; ) {
    if (r[0] == '%' && isxdigit((unsigned char)r[1]) &&
        isxdigit((unsigned char)r[2])) {
      char hex[3] = {r[1], r[2], 0};
      *w++ = (char)strtol(hex, NULL, 16);
      r += 3;
    } else {
      *w++ = *r++;
    }
  }
  *w = '\0';
}

static bool exists(const char *p) {
  struct stat st;
  return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

/* Resolve relative to the document, then try Obsidian's usual attachment
 * folders. This is a heuristic, not a vault index — nemdown has no vault. */
static char *resolve(struct nd_image_cache *c, const char *src) {
  if (!src || !*src) return NULL;

  char *dec = strdup(src);
  percent_decode(dec);

  /* A local viewer that reaches out to the network is a surprise, not a
   * feature; remote images stay placeholders. */
  if (strncmp(dec, "http://", 7) == 0 || strncmp(dec, "https://", 8) == 0) {
    free(dec);
    return NULL;
  }

  char buf[4096];

  /* An absolute path is only honoured if it lands inside the tree anyway. */
  if (dec[0] == '/') {
    if (exists(dec) && nd_path_within(c->root_dir, dec)) return dec;
    free(dec);
    return NULL;
  }

  static const char *prefixes[] = {"", "attachments/", "assets/"};
  for (unsigned i = 0; i < 3; i++) {
    int n = snprintf(buf, sizeof buf, "%s/%s%s", c->base_dir, prefixes[i], dec);
    if (n < 0 || (size_t)n >= sizeof buf) continue; /* refuse on truncation */
    if (!exists(buf)) continue;
    /* Decoding happens before this, so `%2e%2e/` is caught here too: the check
     * is on the resolved path, not on the text the document wrote. */
    if (!nd_path_within(c->root_dir, buf)) continue;
    free(dec);
    return strdup(buf);
  }

  free(dec);
  return NULL;
}

static bool is_svg(const char *path) {
  size_t n = strlen(path);
  return n > 4 && strcasecmp(path + n - 4, ".svg") == 0;
}

bool nd_images_probe(struct nd_image_cache *c, const char *src,
                     double *w, double *h) {
  char *path = resolve(c, src);
  if (!path) return false;

  bool ok = false;
  if (is_svg(path)) {
    RsvgHandle *h_ = rsvg_handle_new_from_file(path, NULL);
    if (h_) {
      gdouble iw = 0, ih = 0;
      /* Returns FALSE for viewBox-only SVGs with no absolute size. */
      if (rsvg_handle_get_intrinsic_size_in_pixels(h_, &iw, &ih) && iw > 0) {
        *w = iw; *h = ih; ok = true;
      } else {
        RsvgRectangle vb;
        gboolean has_vb = FALSE;
        rsvg_handle_get_intrinsic_dimensions(h_, NULL, NULL, NULL, NULL,
                                             &has_vb, &vb);
        if (has_vb && vb.width > 0) { *w = vb.width; *h = vb.height; ok = true; }
        else { *w = 640; *h = 360; ok = true; }
      }
      g_object_unref(h_);
    }
  } else {
    int iw = 0, ih = 0;
    /* Reads the header only: no pixels are decoded to get a size. */
    if (gdk_pixbuf_get_file_info(path, &iw, &ih) && iw > 0) {
      *w = iw; *h = ih; ok = true;
    }
  }

  free(path);
  return ok;
}

/* GdkPixbuf gives non-premultiplied RGB/RGBA rows; Cairo wants premultiplied
 * native-endian ARGB32. gdk-pixbuf ships no Cairo bridge of its own —
 * gdk_cairo_surface_create_from_pixbuf lives in GTK, which this project does
 * not link — so the conversion is done here. Little-endian only, which is the
 * only target. */
static cairo_surface_t *surface_from_pixbuf(GdkPixbuf *pb) {
  int w = gdk_pixbuf_get_width(pb);
  int h = gdk_pixbuf_get_height(pb);
  int nch = gdk_pixbuf_get_n_channels(pb);
  int srs = gdk_pixbuf_get_rowstride(pb);
  const guchar *src = gdk_pixbuf_get_pixels(pb);

  cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
  if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) return NULL;

  cairo_surface_flush(s);
  int drs = cairo_image_surface_get_stride(s);
  unsigned char *dst = cairo_image_surface_get_data(s);

  for (int y = 0; y < h; y++) {
    const guchar *sp = src + (size_t)y * srs;
    uint32_t *dp = (uint32_t *)(dst + (size_t)y * drs);
    for (int x = 0; x < w; x++, sp += nch) {
      uint32_t a = (nch == 4) ? sp[3] : 0xff;
      uint32_t r = sp[0], g = sp[1], b = sp[2];
      if (a != 0xff) {
        r = (r * a + 127) / 255;
        g = (g * a + 127) / 255;
        b = (b * a + 127) / 255;
      }
      dp[x] = (a << 24) | (r << 16) | (g << 8) | b;
    }
  }
  cairo_surface_mark_dirty(s);
  return s;
}

static cairo_surface_t *render_svg(const char *path, double w, double h,
                                   double scale) {
  RsvgHandle *handle = rsvg_handle_new_from_file(path, NULL);
  if (!handle) return NULL;

  /* Rasterise at device resolution: letting Cairo upscale a 1x raster is how
   * vector diagrams end up blurry. */
  int pw = (int)(w * scale + 0.5), ph = (int)(h * scale + 0.5);
  cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, pw, ph);
  cairo_t *cr = cairo_create(s);
  cairo_scale(cr, scale, scale);

  RsvgRectangle viewport = {.x = 0, .y = 0, .width = w, .height = h};
  gboolean ok = rsvg_handle_render_document(handle, cr, &viewport, NULL);

  cairo_destroy(cr);
  g_object_unref(handle);

  if (!ok) { cairo_surface_destroy(s); return NULL; }
  cairo_surface_set_device_scale(s, scale, scale);
  return s;
}

static struct entry *cache_slot(struct nd_image_cache *c, const char *key) {
  for (unsigned i = 0; i < c->n; i++)
    /* key can be NULL for a slot that was claimed but never populated, so this
     * test has to come first. */
    if (c->e[i].key && strcmp(c->e[i].key, key) == 0) {
      c->e[i].used = ++c->tick;
      return &c->e[i];
    }

  if (c->n < ND_CACHE_MAX) return &c->e[c->n++];

  unsigned lru = 0;
  for (unsigned i = 1; i < c->n; i++)
    if (c->e[i].used < c->e[lru].used) lru = i;
  entry_clear(&c->e[lru]);
  return &c->e[lru];
}

cairo_surface_t *nd_images_get(struct nd_image_cache *c, const char *src,
                               double w, double h) {
  /* Written as !(>=) so NaN is rejected: NaN fails every ordered comparison,
   * so `w < 1` would let it straight through. */
  if (!c || !(w >= 1.0) || !(h >= 1.0) || !isfinite(w) || !isfinite(h))
    return NULL;

  char *path = resolve(c, src);
  if (!path) return NULL;

  char key[4200];
  snprintf(key, sizeof key, "%s|%dx%d@%.2f", path, (int)w, (int)h, c->scale);

  struct entry *slot = cache_slot(c, key);
  if (slot->key && strcmp(slot->key, key) == 0) {
    /* A hit, which may be a NULL surface: that is a remembered failure. */
    free(path);
    return slot->surf;
  }

  cairo_surface_t *surf = NULL;
  if (is_svg(path)) {
    surf = render_svg(path, w, h, c->scale);
  } else {
    /* Decoding and scaling in one call looks markedly better than decoding
     * full size and letting Cairo downscale. */
    GdkPixbuf *pb = gdk_pixbuf_new_from_file_at_scale(
        path, (int)(w * c->scale + 0.5), (int)(h * c->scale + 0.5), TRUE, NULL);
    if (pb) {
      surf = surface_from_pixbuf(pb);
      if (surf) cairo_surface_set_device_scale(surf, c->scale, c->scale);
      g_object_unref(pb);
    }
  }

  free(path);

  /* Record the outcome either way. A failed decode is cached as a NULL
   * surface, which does two things: it stops the next lookup from finding a
   * claimed-but-unpopulated slot with a NULL key and calling strcmp on it, and
   * it stops a broken file being re-decoded on every single frame.
   *
   * A header can parse while the pixel data does not — gdk_pixbuf_get_file_info
   * succeeds on a valid IHDR with a corrupt IDAT — so this path is reachable
   * from any document that ships a malformed image. */
  entry_clear(slot);
  slot->key = strdup(key);
  slot->surf = surf;
  slot->used = ++c->tick;

  if (!slot->key) {
    /* Out of memory: leave nothing half-initialised behind. */
    if (surf) cairo_surface_destroy(surf);
    memset(slot, 0, sizeof *slot);
    return NULL;
  }
  return surf;
}
