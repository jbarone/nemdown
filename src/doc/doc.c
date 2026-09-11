/* doc.c — document lifecycle and orchestration behind doc.h. */

#include "doc/doc.h"

#include <math.h>
#include <stdio.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>

#include "doc/arena.h"
#include "doc/frontmatter.h"
#include "doc/hit.h"
#include "doc/images.h"
#include "doc/layout.h"
#include "doc/paint.h"
#include "doc/pathguard.h"
#include "doc/parse.h"
#include "doc/search.h"
#include "doc/select.h"
#include "doc/postprocess.h"
#include "doc/preprocess.h"
#include "doc/reading.h"
#include "doc/toc.h"
#include "doc/wikilink.h"
#include "util/log.h"
#include "util/utf8.h"

struct nd_doc {
  char *path;
  char *dir;       /* the document's directory: wikilinks resolve against it */
  char *root_dir;  /* containment boundary: set once, survives navigation */
  char *raw;       /* the whole file, as read */
  size_t raw_len;

  struct nd_arena  arena;
  struct nd_source src;
  nd_block        *root;

  PangoContext *pctx;
  double last_viewport_w;
  double last_font_scale;
  double column_x;
  double content_h;
  bool   laid_out;

  struct nd_toc_store toc_store;
  nd_toc   toc;
  nd_props props;
  char    *title;

  struct nd_image_cache *images; /* outlives reparse: see images.h */
  nd_selection sel;
  nd_matches   matches;
  char        *needle;
  struct nd_reading reading;
};

/* Layout measures and retains a PangoLayout for every block, so cost is
 * O(document) and a reflow runs on every width change. Measured: a 100MB file
 * is 19s of layout and 7.8GB resident, repeated on every resize frame — on the
 * same thread that must answer xdg_wm_base.ping, so the compositor kills us.
 * A markdown document a person reads is orders of magnitude under this. */
#define ND_MAX_FILE_BYTES (32u * 1024u * 1024u)

static char *read_file(const char *path, size_t *len, char **err) {
  FILE *f = fopen(path, "rb");
  if (!f) {
    *err = nd_strdup_fmt("cannot open %s", path);
    return NULL;
  }
  if (fseek(f, 0, SEEK_END) != 0) { fclose(f); *err = nd_strdup_fmt("cannot seek %s", path); return NULL; }
  long n = ftell(f);
  if (n < 0) { fclose(f); *err = nd_strdup_fmt("cannot size %s", path); return NULL; }
  if ((unsigned long)n > ND_MAX_FILE_BYTES) {
    fclose(f);
    *err = nd_strdup_fmt("%s is %ld MB; nemdown declines files over %u MB",
                         path, n / (1024 * 1024), ND_MAX_FILE_BYTES / (1024 * 1024));
    return NULL;
  }
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

  memset(&d->sel, 0, sizeof d->sel); /* block indices die with the tree */
  nd_search_free(&d->matches);       /* and so do match block pointers */
  nd_preprocess(&d->arena, d->raw, d->raw_len, &d->src);

  d->root = nd_parse(&d->arena, &d->src);
  if (!d->root) {
    /* The arena was reset above, so everything these point at is gone. The
     * callers only warn and carry on, and the next frame would read them. */
    memset(&d->toc, 0, sizeof d->toc);
    memset(&d->toc_store, 0, sizeof d->toc_store);
    memset(&d->props, 0, sizeof d->props);
    d->title = NULL;
    *err = nd_strdup_fmt("failed to parse %s", d->path);
    return false;
  }

  nd_frontmatter_parse(&d->arena, d->src.yaml, d->src.yaml_len, &d->props);
  nd_postprocess(&d->arena, d->root);
  nd_wikilink_resolve_tree(&d->arena, d->root, d->dir, d->root_dir);
  nd_toc_build(&d->arena, d->root, &d->toc_store, &d->toc);
  d->title = nd_toc_document_title(&d->arena, d->root, d->path);
  return true;
}

nd_doc *nd_doc_open(const char *path, char **err) {
  nd_doc *d = calloc(1, sizeof *d);
  if (!d) { *err = nd_strdup_fmt("out of memory"); return NULL; }

  nd_arena_init(&d->arena);
  d->pctx = nd_pango_context_new();
  d->last_font_scale = 1.0;

  if (!nd_doc_load(d, path, err)) { nd_doc_free(d); return NULL; }
  return d;
}

/* Points the document at a different file. Used by wikilink navigation, which
 * is why it re-homes the image cache too: relative paths in the new document
 * resolve against ITS directory, not the old one's. */
bool nd_doc_load(nd_doc *d, const char *path, char **err) {
  size_t len = 0;
  char *buf = read_file(path, &len, err);
  if (!buf) return false;

  /* Scrub before anything parses or measures it. Doing this once here is what
   * lets every consumer downstream — Pango, GLib's UTF-8 helpers, the
   * clipboard — rely on the contract node.h states. */
  size_t bad = nd_utf8_scrub(buf, len);
  if (bad) nd_warn("%s: replaced %zu malformed byte%s", path, bad,
                   bad == 1 ? "" : "s");

  free(d->raw);
  d->raw = buf;
  d->raw_len = len;

  free(d->path);
  d->path = strdup(path);

  free(d->dir);
  char *dup = strdup(path);
  char *slash = dup ? strrchr(dup, '/') : NULL;
  if (slash) *slash = '\0';
  d->dir = strdup((slash && dup) ? dup : ".");
  free(dup);

  /* The FIRST document opened fixes the boundary. Navigating within the tree
   * must not be able to widen it, or one hop through a wikilink would hand the
   * next document the whole filesystem. */
  if (!d->root_dir) d->root_dir = strdup(d->dir);

  nd_images_free(d->images);
  d->images = nd_images_new(d->dir, d->root_dir);

  return rebuild(d, err);
}

bool nd_doc_open_chosen(nd_doc *d, const char *path, char **err) {
  /* Dropping it makes nd_doc_load adopt the new file's directory, since it
   * only sets the root when there is not one already. */
  free(d->root_dir);
  d->root_dir = NULL;
  return nd_doc_load(d, path, err);
}

const char *nd_doc_path(const nd_doc *d) { return d->path; }

double nd_doc_anchor_y(const nd_doc *d, const char *anchor) {
  if (!anchor || !*anchor) return -1.0;

  /* `[[note#Some Heading]]` arrives with the heading as written, but slugs are
   * normalised — so normalise the anchor the same way before comparing. */
  struct nd_arena tmp;
  nd_arena_init(&tmp);
  char *want = nd_toc_slugify(&tmp, anchor);

  double y = -1.0;
  for (size_t i = 0; i < d->toc.count; i++) {
    if (d->toc.items[i].slug && strcmp(d->toc.items[i].slug, want) == 0) {
      y = d->toc.items[i].y;
      break;
    }
  }
  nd_arena_reset(&tmp);
  return y;
}

bool nd_doc_reload(nd_doc *d, char **err) {
  size_t len = 0;
  char *buf = read_file(d->path, &len, err);
  if (!buf) return false;

  nd_utf8_scrub(buf, len);
  free(d->raw);
  d->raw = buf;
  d->raw_len = len;
  return rebuild(d, err);
}

void nd_doc_free(nd_doc *d) {
  if (!d) return;
  nd_layout_free_tree(d->root);
  nd_arena_reset(&d->arena);
  nd_search_free(&d->matches);
  nd_reading_free(&d->reading);
  free(d->needle);
  nd_images_free(d->images);
  if (d->pctx) g_object_unref(d->pctx);
  free(d->raw);
  free(d->dir);
  free(d->root_dir);
  free(d->path);
  free(d);
}

double nd_doc_layout(nd_doc *d, double viewport_w, double font_scale) {
  if (!d->root) return 0.0; /* a failed rebuild leaves no tree to measure */
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
    .images = d->images,
    .font_scale = font_scale,
    .column_x = col_x,
    .column_w = col_w,
  };
  d->column_x = col_x;
  d->content_h = nd_layout_tree(&ctx, d->root);

  /* Pure geometry, so it is rebuilt here rather than at parse: the stops move
   * whenever the column width does. */
  nd_reading_build(&d->reading, d->root);

  nd_toc_refresh_offsets(&d->toc_store);

  /* Matches carry y offsets and block pointers, so a reflow invalidates them. */
  if (d->needle) {
    int keep = d->matches.current;
    nd_search_free(&d->matches);
    nd_search_run(d->root, d->needle, &d->matches);
    if (keep >= 0 && (uint32_t)keep < d->matches.count) d->matches.current = keep;
  }

  d->last_viewport_w = viewport_w;
  d->last_font_scale = font_scale;
  d->laid_out = true;
  return d->content_h;
}

double nd_doc_content_height(const nd_doc *d) { return d->content_h; }

void nd_doc_set_scale(nd_doc *d, double scale) {
  /* Layout is scale-invariant (metrics hinting is off), so only the
   * resolution-dependent raster cache needs to go. */
  nd_images_set_scale(d->images, scale);
}

void nd_doc_paint(nd_doc *d, cairo_t *cr, double scroll_y, double viewport_h) {
  if (!d->laid_out || !d->root) return;

  /* Selection and search highlights go down first, so glyphs sit on top. */
  if (d->sel.active || d->matches.count) {
    cairo_save(cr);
    cairo_translate(cr, 0, -scroll_y);
    if (d->sel.active) nd_select_paint(cr, d->root, &d->sel);
    if (d->matches.count)
      nd_search_paint(cr, &d->matches, scroll_y, scroll_y + viewport_h);
    cairo_restore(cr);
  }
  nd_paint_tree(cr, d->root, scroll_y, viewport_h, d->images);
}

void nd_doc_select_begin(nd_doc *d, double x, double doc_y) {
  nd_point p;
  if (!d->laid_out || !d->root || !nd_select_point_at(d->root, x, doc_y, &p)) {
    nd_doc_select_clear(d);
    return;
  }
  d->sel.anchor = d->sel.focus = p;
  d->sel.active = true;
}

void nd_doc_select_extend(nd_doc *d, double x, double doc_y) {
  nd_point p;
  if (!d->sel.active || !d->laid_out || !d->root) return;
  if (nd_select_point_at(d->root, x, doc_y, &p)) d->sel.focus = p;
}

void nd_doc_select_word(nd_doc *d, double x, double doc_y) {
  nd_point p;
  if (!d->laid_out || !d->root || !nd_select_point_at(d->root, x, doc_y, &p)) return;
  nd_select_word_at(d->root, p, &d->sel.anchor, &d->sel.focus);
  d->sel.active = true;
}

void nd_doc_select_block(nd_doc *d, double x, double doc_y) {
  nd_point p;
  if (!d->laid_out || !d->root || !nd_select_point_at(d->root, x, doc_y, &p)) return;
  nd_select_block_at(d->root, p, &d->sel.anchor, &d->sel.focus);
  d->sel.active = true;
}

void nd_doc_select_clear(nd_doc *d) {
  memset(&d->sel, 0, sizeof d->sel);
}

bool nd_doc_has_selection(const nd_doc *d) {
  return d->sel.active && d->sel.anchor.valid && d->sel.focus.valid;
}

char *nd_doc_select_text(nd_doc *d) {
  if (!nd_doc_has_selection(d)) return NULL;
  return nd_select_copy(d->root, &d->sel);
}

/* Top-level block index in the high bits, pixels into that block in the low
 * bits. Block indices survive a reparse far better than absolute offsets do. */
uint64_t nd_doc_anchor_at(const nd_doc *d, double doc_y) {
  if (!d->laid_out || !d->root || d->root->nkids == 0) return 0;

  for (uint32_t i = 0; i < d->root->nkids; i++) {
    nd_block *k = d->root->kids[i];
    if (doc_y < k->lay.y + k->lay.h) {
      double into = doc_y - k->lay.y;
      if (into < 0) into = 0;
      if (into > 65535) into = 65535;
      return ((uint64_t)i << 32) | (uint64_t)(into + 0.5);
    }
  }
  return ((uint64_t)(d->root->nkids - 1) << 32);
}

double nd_doc_y_for_anchor(const nd_doc *d, uint64_t anchor) {
  if (!d->laid_out || !d->root || d->root->nkids == 0) return 0.0;

  uint32_t index = (uint32_t)(anchor >> 32);
  double   into  = (double)(anchor & 0xffffffffu);

  /* The document may have shrunk since the anchor was taken. */
  if (index >= d->root->nkids) index = d->root->nkids - 1;

  double y = d->root->kids[index]->lay.y + into;
  double max = d->content_h - 1.0;
  if (y < 0) y = 0;
  if (max > 0 && y > max) y = max;
  return y;
}

/* Finds the code fence containing a point, if any. */
static nd_block *code_at(nd_block *b, double x, double doc_y) {
  if (nd_block_is_code_panel(b)) {
    if (x >= b->lay.x && x <= b->lay.x + b->lay.w &&
        doc_y >= b->lay.y && doc_y <= b->lay.y + b->lay.h)
      return b;
    return NULL;
  }
  for (uint32_t i = 0; i < b->nkids; i++) {
    nd_block *k = b->kids[i];
    if (doc_y < k->lay.y || doc_y > k->lay.y + k->lay.h) continue;
    nd_block *found = code_at(k, x, doc_y);
    if (found) return found;
  }
  return NULL;
}

bool nd_doc_pan_code(nd_doc *d, double x, double doc_y, double dx) {
  if (!d->laid_out || !d->root) return false;

  nd_block *b = code_at(d->root, x, doc_y);
  if (!b) return false;

  /* Only the part that does not fit can move. */
  double visible = b->lay.w - 2 * 16.0; /* panel padding, see layout.c */
  double overflow = b->lay.natural_w - visible;
  if (overflow <= 0) return false;

  double before = b->lay.hscroll;
  b->lay.hscroll += dx;
  if (b->lay.hscroll < 0) b->lay.hscroll = 0;
  if (b->lay.hscroll > overflow) b->lay.hscroll = overflow;
  return b->lay.hscroll != before;
}

/* Overflow in logical px, or 0 when the fence fits. */
static double code_overflow(const nd_block *b) {
  double visible = b->lay.w - 2 * 16.0; /* panel padding, see layout.c */
  double over = b->lay.natural_w - visible;
  return over > 0 ? over : 0;
}

static void find_pannable(nd_block *b, double y0, double y1, double centre,
                          nd_block **best, double *best_dist) {
  if (nd_block_is_code_panel(b)) {
    /* Must be at least partly on screen and actually have somewhere to go. */
    if (b->lay.y < y1 && b->lay.y + b->lay.h > y0 && code_overflow(b) > 0) {
      double mid = b->lay.y + b->lay.h / 2.0;
      double dist = fabs(mid - centre);
      if (!*best || dist < *best_dist) { *best = b; *best_dist = dist; }
    }
    return;
  }
  for (uint32_t i = 0; i < b->nkids; i++)
    find_pannable(b->kids[i], y0, y1, centre, best, best_dist);
}

bool nd_doc_pan_focused(nd_doc *d, double scroll_y, double viewport_h,
                        double dx) {
  if (!d->laid_out || !d->root) return false;

  nd_block *best = NULL;
  double dist = 0;
  find_pannable(d->root, scroll_y, scroll_y + viewport_h,
                scroll_y + viewport_h / 2.0, &best, &dist);
  if (!best) return false;

  double over = code_overflow(best);
  double before = best->lay.hscroll;
  best->lay.hscroll += dx;
  if (best->lay.hscroll < 0) best->lay.hscroll = 0;
  if (best->lay.hscroll > over) best->lay.hscroll = over;
  return best->lay.hscroll != before;
}

bool nd_doc_hit_test(nd_doc *d, double x, double doc_y, nd_hit *out) {
  out->kind = ND_HIT_NONE;
  if (!d->laid_out || !d->root) return false;
  return nd_hit_tree(d->root, x, doc_y, out);
}

static nd_block *callout_at(nd_block *b, double x, double doc_y) {
  if (b->kind == ND_CALLOUT &&
      x >= b->lay.x && x <= b->lay.x + b->lay.w &&
      doc_y >= b->lay.y && doc_y <= b->lay.y + b->lay.h)
    return b;
  for (uint32_t i = 0; i < b->nkids; i++) {
    nd_block *f = callout_at(b->kids[i], x, doc_y);
    if (f) return f;
  }
  return NULL;
}

void nd_doc_toggle_fold(nd_doc *d, double x, double doc_y) {
  if (!d->laid_out || !d->root) return;
  nd_block *c = callout_at(d->root, x, doc_y);
  if (!c) return;

  c->callout_folded = !c->callout_folded;
  /* Folding changes heights from here down, so the whole document reflows. */
  d->laid_out = false;
  nd_doc_layout(d, d->last_viewport_w, d->last_font_scale);
}

/* Flips a `[ ]` / `[x]` in the source file. md4c gives the exact byte offset of
 * the character between the brackets — the one place in its API that exposes a
 * source position — so this is a single-byte write, not a re-serialisation. */
bool nd_doc_toggle_task(nd_doc *d, uint32_t source_offset, bool now_checked) {
  if (source_offset >= d->raw_len) return false;

  FILE *f = fopen(d->path, "r+b");
  if (!f) return false;

  /* raw_len is the size at PARSE time. If the file shrank since — an edit the
   * watcher has not caught up with yet, or one on a filesystem inotify does
   * not cover — seeking to a stale offset and writing would extend the file
   * through a hole of NULs. Check the size as it is now. */
  struct stat st;
  if (fstat(fileno(f), &st) != 0 || (off_t)source_offset >= st.st_size) {
    fclose(f);
    return false;
  }

  if (fseek(f, (long)source_offset, SEEK_SET) != 0) { fclose(f); return false; }

  /* Confirm we are about to overwrite a checkbox and not something else the
   * file now holds at that offset. */
  int cur = fgetc(f);
  if (cur != ' ' && cur != 'x' && cur != 'X') { fclose(f); return false; }
  if (fseek(f, (long)source_offset, SEEK_SET) != 0) { fclose(f); return false; }

  char c = now_checked ? 'x' : ' ';
  bool ok = fwrite(&c, 1, 1, f) == 1;
  fclose(f);

  if (ok) d->raw[source_offset] = c;
  return ok;
}

struct _PangoContext *nd_doc_pango_context(const nd_doc *d) { return d->pctx; }

void nd_doc_search(nd_doc *d, const char *needle) {
  nd_search_free(&d->matches);
  nd_reading_free(&d->reading);
  free(d->needle);
  d->needle = needle && *needle ? strdup(needle) : NULL;
  if (d->needle && d->laid_out) nd_search_run(d->root, d->needle, &d->matches);
}

void nd_doc_search_clear(nd_doc *d) {
  nd_search_free(&d->matches);
  nd_reading_free(&d->reading);
  free(d->needle);
  d->needle = NULL;
}

size_t nd_doc_search_count(const nd_doc *d)  { return d->matches.count; }
int    nd_doc_search_current(const nd_doc *d) { return d->matches.current; }

double nd_doc_search_step(nd_doc *d, int delta, double viewport_h) {
  if (d->matches.count == 0) return -1.0;

  int n = (int)d->matches.count;
  int cur = d->matches.current + delta;
  while (cur < 0) cur += n;      /* wrap, so n/N cycle rather than stop */
  cur %= n;
  d->matches.current = cur;

  double y = d->matches.items[cur].y - viewport_h / 3.0;
  double max = d->content_h - viewport_h;
  if (max < 0) max = 0;
  if (y < 0) y = 0;
  if (y > max) y = max;
  return y;
}

const char *nd_doc_source(const nd_doc *d) { return d->raw; }

const nd_props *nd_doc_props(const nd_doc *d) { return &d->props; }
const nd_toc   *nd_doc_toc(const nd_doc *d)   { return &d->toc; }
const char     *nd_doc_title(const nd_doc *d) { return d->title; }

uint32_t nd_doc_reading_count(const nd_doc *d) { return d->reading.n; }

double nd_doc_column_x(const nd_doc *d) { return d->column_x; }

bool nd_doc_reading_rect(const nd_doc *d, uint32_t i, double *x, double *y,
                         double *w, double *h) {
  if (i >= d->reading.n) return false;
  const nd_para *p = &d->reading.stops[i];
  *x = p->x; *y = p->y; *w = p->w; *h = p->h;
  return true;
}

int nd_doc_reading_at(const nd_doc *d, double doc_y, double viewport_h) {
  return nd_reading_at(&d->reading, doc_y, viewport_h);
}

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
