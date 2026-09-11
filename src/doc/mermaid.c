/* mermaid.c — dialect detection, the shared helpers, and the raster cache. */

#include "doc/mermaid.h"

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "doc/mermaid_int.h"
#include "util/log.h"

/* ---- buffer ------------------------------------------------------------ */

static bool buf_grow(nd_buf *b, size_t need) {
  if (!b->ok) return false;
  if (b->len + need + 1 <= b->cap) return true;
  size_t cap = b->cap ? b->cap : 1024;
  while (cap < b->len + need + 1) {
    if (cap > (size_t)1 << 26) { b->ok = false; return false; }  /* 64MB of dot */
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

void nd_buf_puts(nd_buf *b, const char *s) { nd_buf_add(b, s, strlen(s)); }

void nd_buf_fmt(nd_buf *b, const char *fmt, ...) {
  if (!b->ok) return;
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  if (n < 0 || !buf_grow(b, (size_t)n)) { b->ok = false; return; }
  va_start(ap, fmt);
  vsnprintf(b->p + b->len, (size_t)n + 1, fmt, ap);
  va_end(ap);
  b->len += (size_t)n;
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

/* ---- lines ------------------------------------------------------------- */

bool nd_mm_line(nd_lines *it, char *out, size_t cap) {
  while (it->pos < it->len) {
    uint32_t start = it->pos;
    while (it->pos < it->len && it->src[it->pos] != '\n') it->pos++;
    uint32_t end = it->pos;
    if (it->pos < it->len) it->pos++;            /* step over the newline */
    if (end > start && it->src[end - 1] == '\r') end--;

    /* `%%` is mermaid's line comment, and `%%{init: ...}%%` — a directive we
     * do not act on — is swallowed by the same rule. */
    for (uint32_t i = start; i + 1 < end; i++)
      if (it->src[i] == '%' && it->src[i + 1] == '%') { end = i; break; }

    while (start < end && isspace((unsigned char)it->src[start])) start++;
    while (end > start && isspace((unsigned char)it->src[end - 1])) end--;
    if (end > start && it->src[end - 1] == ';') {
      end--;
      while (end > start && isspace((unsigned char)it->src[end - 1])) end--;
    }
    if (end == start) continue;

    size_t n = end - start;
    if (n > cap - 1) n = cap - 1;
    memcpy(out, it->src + start, n);
    out[n] = 0;
    return true;
  }
  return false;
}

/* ---- labels ------------------------------------------------------------ */

/* The `#nn;` forms mermaid documents for characters its own grammar would
 * otherwise consume. Only the ones that appear in practice; anything else is
 * left alone rather than guessed at. */
static const struct { const char *name; char ch; } kEsc[] = {
  {"quot", '"'}, {"semi", ';'}, {"colon", ':'}, {"amp", '&'},
  {"lt", '<'}, {"gt", '>'}, {"lbrace", '{'}, {"rbrace", '}'},
  {"lpar", '('}, {"rpar", ')'}, {"lbrack", '['}, {"rbrack", ']'},
  {"num", '#'}, {"hash", '#'},
};

void nd_mm_clean_label(char *s) {
  size_t n = strlen(s);

  /* One layer of matching quotes, which mermaid uses to protect punctuation. */
  if (n >= 2 && ((s[0] == '"' && s[n - 1] == '"') ||
                 (s[0] == '\'' && s[n - 1] == '\''))) {
    memmove(s, s + 1, n - 2);
    s[n - 2] = 0;
    n -= 2;
  }

  char *w = s;
  for (const char *r = s; *r;) {
    if ((r[0] == '<' || r[0] == '\\') && (r[1] == 'b' || r[1] == 'B') &&
        (r[2] == 'r' || r[2] == 'R')) {
      /* <br>, <br/>, <br />, and the \n some documents write literally. */
      const char *e = r + 3;
      while (*e == ' ') e++;
      if (*e == '/') e++;
      if (*e == '>') { *w++ = '\n'; r = e + 1; continue; }
    }
    if (r[0] == '\\' && r[1] == 'n') { *w++ = '\n'; r += 2; continue; }

    if (r[0] == '#') {
      const char *semi = strchr(r, ';');
      if (semi && semi - r <= 9) {
        size_t len = (size_t)(semi - r - 1);
        bool hit = false;
        for (size_t i = 0; i < sizeof kEsc / sizeof *kEsc && !hit; i++)
          if (strlen(kEsc[i].name) == len &&
              strncmp(r + 1, kEsc[i].name, len) == 0) {
            *w++ = kEsc[i].ch; r = semi + 1; hit = true;
          }
        if (hit) continue;
        if (len > 0 && len < 8 && isdigit((unsigned char)r[1])) {
          /* `#35;` — a decimal codepoint. Only Latin-1 is admitted, because a
           * wider value would need encoding and these escapes exist for
           * punctuation. */
          long v = strtol(r + 1, NULL, 10);
          if (v >= 32 && v < 127) { *w++ = (char)v; r = semi + 1; continue; }
        }
      }
    }

    unsigned char c = (unsigned char)*r;
    if (c < 0x20 && c != '\n') { r++; continue; }   /* control bytes go */
    *w++ = *r++;
  }
  *w = 0;
}

/* These two are the only ways document text reaches the dot, and they are the
 * last gate rather than the first: not every caller runs nd_mm_clean_label
 * first — a classDiagram member is accumulated straight off the line — so the
 * control-byte filter belongs HERE, where nothing can get past it. That was
 * found by check_mermaid on its first run, which is what it is for. */
void nd_mm_dot_str(nd_buf *b, const char *s) {
  nd_buf_puts(b, "\"");
  for (; *s; s++) {
    if (*s == '"' || *s == '\\') { nd_buf_puts(b, "\\"); nd_buf_add(b, s, 1); }
    else if (*s == '\n')          nd_buf_puts(b, "\\n");
    else if (*s == '\t')          nd_buf_puts(b, " ");
    else if ((unsigned char)*s >= 0x20) nd_buf_add(b, s, 1);
  }
  nd_buf_puts(b, "\"");
}

void nd_mm_html_esc(nd_buf *b, const char *s) {
  for (; *s; s++) {
    switch (*s) {
      case '&':  nd_buf_puts(b, "&amp;");  break;
      case '<':  nd_buf_puts(b, "&lt;");   break;
      case '>':  nd_buf_puts(b, "&gt;");   break;
      case '"':  nd_buf_puts(b, "&quot;"); break;
      case '\n': nd_buf_puts(b, "<BR/>");  break;
      case '\t': nd_buf_puts(b, " ");       break;
      default:
        if ((unsigned char)*s >= 0x20) nd_buf_add(b, s, 1);
        break;
    }
  }
}

/* ---- the diagram ------------------------------------------------------- */

struct nd_mermaid {
  bool   is_seq;
  double w, h;          /* as drawn, after fitting to the column */
  double nat_w, nat_h;  /* as laid out */
  double fit;           /* <= 1; diagrams are never enlarged */

  RsvgHandle      *svg;         /* graph dialects */
  cairo_surface_t *raster;
  double           raster_scale;
  size_t           raster_bytes;
  struct nd_mermaid *older, *newer;   /* LRU */
  bool             linked;            /* explicitly, so unlinking twice is safe */

  struct nd_seq *seq;           /* sequence dialect */
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

bool nd_mermaid_have_graphviz(void) { return nd_mm_gv_available(); }

/* The dialect is named by the first meaningful line, which is also how mermaid
 * itself decides. */
enum { MM_NONE, MM_FLOW, MM_SEQ, MM_STATE, MM_CLASS };

static int dialect_of(const char *src, uint32_t len) {
  nd_lines it = {.src = src, .len = len};
  char line[ND_MM_MAX_LINE];
  if (!nd_mm_line(&it, line, sizeof line)) return MM_NONE;
  if (strncmp(line, "flowchart", 9) == 0) return MM_FLOW;
  if (strncmp(line, "graph", 5) == 0 &&
      (line[5] == 0 || isspace((unsigned char)line[5]))) return MM_FLOW;
  if (strncmp(line, "sequenceDiagram", 15) == 0) return MM_SEQ;
  if (strncmp(line, "stateDiagram", 12) == 0) return MM_STATE;
  if (strncmp(line, "classDiagram", 12) == 0) return MM_CLASS;
  return MM_NONE;
}

struct nd_mermaid *nd_mermaid_build(PangoContext *pctx, const char *src,
                                    uint32_t len, double max_w, double zoom) {
  if (!src || !len || len > ND_MM_MAX_SRC || !(max_w > 1)) return NULL;
  if (!(zoom > 0.05) || !(zoom < 20)) zoom = 1.0;

  int d = dialect_of(src, len);
  if (d == MM_NONE) return NULL;

  struct nd_mermaid *m = calloc(1, sizeof *m);
  if (!m) return NULL;

  if (d == MM_SEQ) {
    m->is_seq = true;
    m->seq = nd_mm_seq_build(pctx, src, len);
    if (!m->seq) { free(m); return NULL; }
    nd_mm_seq_size(m->seq, &m->nat_w, &m->nat_h);
  } else {
    char *dot = d == MM_FLOW  ? nd_mm_flow_dot(src, len)
              : d == MM_STATE ? nd_mm_state_dot(src, len)
                              : nd_mm_class_dot(src, len);
    if (!dot) { free(m); return NULL; }
    m->svg = nd_mm_gv_render(dot, &m->nat_w, &m->nat_h);
    free(dot);
    if (!m->svg) { free(m); return NULL; }
  }

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

  if (m->is_seq) {
    /* Nothing to cache: the sequence renderer is retained Pango layouts and a
     * few dozen Cairo primitives, so it draws straight into the frame and is
     * crisp at any scale for free. */
    if (m->fit < 1.0) cairo_scale(cr, m->fit, m->fit);
    nd_mm_seq_paint(m->seq, cr);
  } else {
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
  }
  cairo_restore(cr);
}

void nd_mermaid_free(struct nd_mermaid *m) {
  if (!m) return;
  raster_drop(m);
  if (m->svg) g_object_unref(m->svg);
  if (m->seq) nd_mm_seq_free(m->seq);
  free(m);
}
