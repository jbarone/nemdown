/* doc.c — document lifecycle and orchestration behind doc.h. */

#include "doc/doc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "doc/arena.h"
#include "doc/frontmatter.h"
#include "doc/layout.h"
#include "doc/paint.h"
#include "doc/parse.h"
#include "doc/postprocess.h"
#include "doc/preprocess.h"
#include "doc/toc.h"
#include "util/log.h"

struct nd_doc {
  char *path;
  char *raw;       /* the whole file, as read */
  size_t raw_len;

  struct nd_arena  arena;
  struct nd_source src;
  nd_block        *root;

  PangoContext *pctx;
  double last_viewport_w;
  double last_font_scale;
  double content_h;
  bool   laid_out;

  struct nd_toc_store toc_store;
  nd_toc   toc;
  nd_props props;
  char    *title;
};

static char *read_file(const char *path, size_t *len, char **err) {
  FILE *f = fopen(path, "rb");
  if (!f) {
    *err = nd_strdup_fmt("cannot open %s", path);
    return NULL;
  }
  if (fseek(f, 0, SEEK_END) != 0) { fclose(f); *err = nd_strdup_fmt("cannot seek %s", path); return NULL; }
  long n = ftell(f);
  if (n < 0) { fclose(f); *err = nd_strdup_fmt("cannot size %s", path); return NULL; }
  rewind(f);

  char *buf = malloc((size_t)n + 1);
  if (!buf) { fclose(f); *err = nd_strdup_fmt("out of memory"); return NULL; }

  size_t got = fread(buf, 1, (size_t)n, f);
  fclose(f);
  buf[got] = '\0';
  *len = got;
  return buf;
}

/* Rebuilds everything downstream of the file bytes. */
static bool rebuild(nd_doc *d, char **err) {
  nd_layout_free_tree(d->root);
  nd_arena_reset(&d->arena);
  nd_arena_init(&d->arena);
  d->root = NULL;
  d->laid_out = false;

  nd_preprocess(&d->arena, d->raw, d->raw_len, &d->src);

  d->root = nd_parse(&d->arena, &d->src);
  if (!d->root) {
    *err = nd_strdup_fmt("failed to parse %s", d->path);
    return false;
  }

  nd_frontmatter_parse(&d->arena, d->src.yaml, d->src.yaml_len, &d->props);
  nd_postprocess(&d->arena, d->root);
  nd_toc_build(&d->arena, d->root, &d->toc_store, &d->toc);
  d->title = nd_toc_document_title(&d->arena, d->root, d->path);
  return true;
}

nd_doc *nd_doc_open(const char *path, char **err) {
  nd_doc *d = calloc(1, sizeof *d);
  if (!d) { *err = nd_strdup_fmt("out of memory"); return NULL; }

  d->path = strdup(path);
  nd_arena_init(&d->arena);
  d->pctx = nd_pango_context_new();
  d->last_font_scale = 1.0;

  d->raw = read_file(path, &d->raw_len, err);
  if (!d->raw) { nd_doc_free(d); return NULL; }

  if (!rebuild(d, err)) { nd_doc_free(d); return NULL; }
  return d;
}

bool nd_doc_reload(nd_doc *d, char **err) {
  size_t len = 0;
  char *buf = read_file(d->path, &len, err);
  if (!buf) return false;

  free(d->raw);
  d->raw = buf;
  d->raw_len = len;
  return rebuild(d, err);
}

void nd_doc_free(nd_doc *d) {
  if (!d) return;
  nd_layout_free_tree(d->root);
  nd_arena_reset(&d->arena);
  if (d->pctx) g_object_unref(d->pctx);
  free(d->raw);
  free(d->path);
  free(d);
}

double nd_doc_layout(nd_doc *d, double viewport_w, double font_scale) {
  if (d->laid_out && viewport_w == d->last_viewport_w &&
      font_scale == d->last_font_scale)
    return d->content_h;

  /* The engine owns the readable measure, so the shell never learns that 700
   * is a number that matters. */
  double col_w = viewport_w - 2 * ND_PAGE_MARGIN;
  if (col_w > ND_COLUMN_MAX) col_w = ND_COLUMN_MAX;
  if (col_w < 120.0) col_w = 120.0;
  double col_x = (viewport_w - col_w) / 2.0;
  if (col_x < ND_PAGE_MARGIN) col_x = ND_PAGE_MARGIN;

  nd_layout_free_tree(d->root);

  struct nd_layout_ctx ctx = {
    .pctx = d->pctx,
    .arena = &d->arena,
    .font_scale = font_scale,
    .column_x = col_x,
    .column_w = col_w,
  };
  d->content_h = nd_layout_tree(&ctx, d->root);

  nd_toc_refresh_offsets(&d->toc_store);

  d->last_viewport_w = viewport_w;
  d->last_font_scale = font_scale;
  d->laid_out = true;
  return d->content_h;
}

double nd_doc_content_height(const nd_doc *d) { return d->content_h; }

void nd_doc_set_scale(nd_doc *d, double scale) {
  /* Layout is scale-invariant (metrics hinting is off), so there is nothing to
   * redo here. Once images land, this is where their cache gets dropped. */
  (void)d; (void)scale;
}

void nd_doc_paint(nd_doc *d, cairo_t *cr, double scroll_y, double viewport_h) {
  if (!d->laid_out) return;
  nd_paint_tree(cr, d->root, scroll_y, viewport_h);
}

bool nd_doc_hit_test(nd_doc *d, double x, double doc_y, nd_hit *out) {
  (void)d; (void)x; (void)doc_y;
  out->kind = ND_HIT_NONE;
  return false;
}

struct _PangoContext *nd_doc_pango_context(const nd_doc *d) { return d->pctx; }

const nd_props *nd_doc_props(const nd_doc *d) { return &d->props; }
const nd_toc   *nd_doc_toc(const nd_doc *d)   { return &d->toc; }
const char     *nd_doc_title(const nd_doc *d) { return d->title; }

int nd_toc_active(const nd_doc *d, double scroll_y) {
  const nd_toc *t = &d->toc;
  if (t->count == 0) return -1;

  /* Last entry at or above the reading line. */
  size_t lo = 0, hi = t->count;
  while (lo < hi) {
    size_t mid = (lo + hi) / 2;
    if (t->items[mid].y <= scroll_y + 80.0) lo = mid + 1;
    else                                    hi = mid;
  }
  return lo == 0 ? 0 : (int)(lo - 1);
}

double nd_toc_target_y(const nd_doc *d, int index, double viewport_h) {
  if (index < 0 || (size_t)index >= d->toc.count) return 0.0;
  double y = d->toc.items[index].y - 16.0;
  double max = d->content_h - viewport_h;
  if (max < 0) max = 0;
  if (y < 0) y = 0;
  if (y > max) y = max;
  return y;
}
