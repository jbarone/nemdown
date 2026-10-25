/* mermaid.c — fence detection, the shared buffer, and the raster cache. */

#include "doc/mermaid.h"

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "doc/mermaid_int.h"

/* ---- buffer ------------------------------------------------------------ */

static bool buf_grow(nd_buf *b, size_t need) {
  if (!b->ok) return false;
  if (b->len + need + 1 <= b->cap) return true;
  size_t cap = b->cap ? b->cap : 1024;
  while (cap < b->len + need + 1) {
    if (cap > (size_t)1 << 26) { b->ok = false; return false; }  /* 64MB of SVG */
    cap *= 2;
  }
  char *p = realloc(b->p, cap);
  if (!p) { b->ok = false; return false; }
  b->p = p; b->cap = cap;
  return true;
}

void nd_buf_add(nd_buf *b, const char *s, size_t n) {
  if (!buf_grow(b, n)) return;
  memcpy(b->p + b->len, s, n);
  b->len += n;
  b->p[b->len] = 0;
}

char *nd_buf_take(nd_buf *b) {
  if (!b->ok || !b->p) { nd_buf_free(b); return NULL; }
  char *p = b->p;
  b->p = NULL; b->len = b->cap = 0;
  return p;
}

void nd_buf_free(nd_buf *b) {
  free(b->p);
  b->p = NULL; b->len = b->cap = 0;
}

/* ---- the diagram ------------------------------------------------------- */

struct nd_mermaid {
  double w, h;          /* as drawn, after fitting to the column */
  double nat_w, nat_h;  /* as laid out */
  double fit;           /* <= 1; diagrams are never enlarged */

  RsvgHandle      *svg;
  cairo_surface_t *raster;
  double           raster_scale;
  size_t           raster_bytes;
  struct nd_mermaid *older, *newer;   /* LRU */
  bool             linked;            /* explicitly, so unlinking twice is safe */
};

/* Rasters are the only unbounded memory a document can ask for here — a file
 * may hold any number of fences — so they share one budget across every live
 * diagram and the least recently painted is dropped first. Painting is the
 * only thing that creates one, so a diagram never scrolled to costs nothing. */
#define ND_MM_RASTER_BUDGET ((size_t)48 << 20)
static struct nd_mermaid *lru_new, *lru_old;
static size_t lru_bytes;

/* The list runs newest to oldest through `older`, so the end with no `newer`
 * is the head and the end with no `older` is the tail. Getting those two the
 * wrong way round still works for a list of one, which is exactly why it is
 * worth stating: it would have gone unnoticed until a document held enough
 * diagrams to need eviction. */
static void lru_unlink(struct nd_mermaid *m) {
  if (!m->linked) return;
  if (m->newer) m->newer->older = m->older; else lru_new = m->older;
  if (m->older) m->older->newer = m->newer; else lru_old = m->newer;
  m->older = m->newer = NULL;
  m->linked = false;
}

static void raster_drop(struct nd_mermaid *m) {
  if (!m->raster) return;
  lru_unlink(m);
  lru_bytes -= m->raster_bytes;
  cairo_surface_destroy(m->raster);
  m->raster = NULL;
  m->raster_bytes = 0;
}

static void lru_touch(struct nd_mermaid *m) {
  if (lru_new == m) return;
  lru_unlink(m);
  m->older = lru_new;
  if (lru_new) lru_new->newer = m;
  lru_new = m;
  if (!lru_old) lru_old = m;
  m->linked = true;
}

bool nd_mermaid_is_fence(const char *lang) {
  if (!lang) return false;
  while (*lang == ' ' || *lang == '\t') lang++;
  size_t n = 0;
  while (lang[n] && !isspace((unsigned char)lang[n])) n++;
  return n == 7 && g_ascii_strncasecmp(lang, "mermaid", 7) == 0;
}

struct nd_mermaid *nd_mermaid_build(PangoContext *pctx, const char *src,
                                    uint32_t len, double max_w, double zoom) {
  if (!src || !len || len > ND_MM_MAX_SRC || !(max_w > 1)) return NULL;
  if (!(zoom > 0.05) || !(zoom < 20)) zoom = 1.0;

  struct nd_mermaid *m = calloc(1, sizeof *m);
  if (!m) return NULL;

  /* At the column width divided back out of the zoom, since zoom is applied
   * to the result below. */
  m->svg = nd_mm_pele_render(pctx, src, len, max_w / zoom, &m->nat_w,
                             &m->nat_h);
  if (!m->svg) { free(m); return NULL; }

  if (!(m->nat_w > 0) || !(m->nat_h > 0)) { nd_mermaid_free(m); return NULL; }

  /* Zoom first, then fit: a diagram grows with the reader's font scale but is
   * still never drawn wider than the column. Fitting the column *up* is not
   * the same thing and would be wrong — a three-node diagram blown out to
   * 700px reads as a mistake. */
  m->fit = zoom;
  if (m->nat_w * m->fit > max_w) m->fit = max_w / m->nat_w;
  m->w = m->nat_w * m->fit;
  m->h = m->nat_h * m->fit;
  return m;
}

void nd_mermaid_size(const struct nd_mermaid *m, double *w, double *h) {
  *w = m ? m->w : 0;
  *h = m ? m->h : 0;
}

/* The device scale is on the surface rather than the context, so it survives
 * the save/restore in the caller and no scale has to be plumbed in. */
static double device_scale_of(cairo_t *cr) {
  double sx = 1, sy = 1;
  cairo_surface_t *t = cairo_get_target(cr);
  if (t) cairo_surface_get_device_scale(t, &sx, &sy);
  if (!(sx > 0.05) || !(sx < 16)) sx = 1;
  return sx;
}

static void svg_render(struct nd_mermaid *m, cairo_t *cr) {
  RsvgRectangle vp = {.x = 0, .y = 0, .width = m->nat_w, .height = m->nat_h};
  rsvg_handle_render_document(m->svg, cr, &vp, NULL);
}

static bool raster_make(struct nd_mermaid *m, double scale) {
  int pw = (int)ceil(m->w * scale), ph = (int)ceil(m->h * scale);
  if (pw < 1 || ph < 1 || pw > 16384 || ph > 16384) return false;

  size_t bytes = (size_t)cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, pw)
               * (size_t)ph;
  if (bytes > ND_MM_RASTER_BUDGET) return false;

  cairo_surface_t *cs = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, pw, ph);
  if (cairo_surface_status(cs) != CAIRO_STATUS_SUCCESS) {
    cairo_surface_destroy(cs);
    return false;
  }
  /* Before the first draw, not after: cairo refuses a device scale on a
   * surface that has already been painted, and refuses it with an assert. The
   * scale is what makes the blit below work in logical units, the same trick
   * the window surface uses. */
  cairo_surface_set_device_scale(cs, scale, scale);
  cairo_t *c2 = cairo_create(cs);
  cairo_scale(c2, m->fit, m->fit);
  svg_render(m, c2);
  cairo_destroy(c2);

  raster_drop(m);
  m->raster = cs;
  m->raster_scale = scale;
  m->raster_bytes = bytes;
  lru_bytes += bytes;
  lru_touch(m);

  /* The loop exits on progress, not only on the budget: a tail that cannot be
   * dropped would otherwise spin forever, and a loop that cannot make progress
   * is this program's oldest lesson. */
  while (lru_bytes > ND_MM_RASTER_BUDGET && lru_old && lru_old != m) {
    struct nd_mermaid *victim = lru_old;
    raster_drop(victim);
    if (lru_old == victim) break;
  }
  return true;
}

void nd_mermaid_paint(struct nd_mermaid *m, cairo_t *cr, double x, double y) {
  if (!m) return;

  cairo_save(cr);
  cairo_translate(cr, x, y);

  double scale = device_scale_of(cr);
  if (!m->raster || fabs(m->raster_scale - scale) > 0.001)
    raster_make(m, scale);

  if (m->raster) {
    lru_touch(m);
    cairo_set_source_surface(cr, m->raster, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
    cairo_rectangle(cr, 0, 0, m->w, m->h);
    cairo_fill(cr);
  } else {
    /* Could not cache it — draw it live rather than draw nothing. */
    cairo_scale(cr, m->fit, m->fit);
    svg_render(m, cr);
  }
  cairo_restore(cr);
}

void nd_mermaid_free(struct nd_mermaid *m) {
  if (!m) return;
  raster_drop(m);
  if (m->svg) g_object_unref(m->svg);
  free(m);
}
