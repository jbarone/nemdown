/* hit.c — point to link, checkbox, or image. */

#include "doc/hit.h"

#include <pango/pangocairo.h>

#include "ui/typography.h"

static bool hit_block(nd_block *b, double x, double y, nd_hit *out);

static bool hit_children(nd_block *b, double x, double y, nd_hit *out) {
  for (uint32_t i = 0; i < b->nkids; i++) {
    nd_block *k = b->kids[i];
    if (y < k->lay.y || y > k->lay.y + k->lay.h) continue;
    if (hit_block(k, x, y, out)) return true;
  }
  return false;
}

/* Cheap, unambiguous rect tests come first; text is the fallback. */
static bool hit_checkbox(nd_block *b, double x, double y, nd_hit *out) {
  if (!b->item_is_task) return false;
  const nd_style *bs = &nd_styles[ND_ST_BODY];
  double s = bs->size;
  double bx = b->lay.x + 6.0;
  double by = b->lay.y + (bs->line_height - s) / 2.0;

  /* A checkbox is a small target, so the hit area is padded. */
  if (x < bx - 3 || x > bx + s + 3 || y < by - 3 || y > by + s + 3) return false;

  out->kind = ND_HIT_CHECKBOX;
  out->source_offset = b->item_mark_offset;
  out->checked = b->item_checked != 0;
  out->x = bx; out->y = by; out->w = s; out->h = s;
  return true;
}

static bool hit_text(nd_block *b, double x, double y, nd_hit *out) {
  if (!b->lay.pl || b->inl.nruns == 0) return false;

  double lx = x - b->lay.x, ly = y - b->lay.y;

  PangoRectangle ink, logical;
  pango_layout_get_pixel_extents(b->lay.pl, &ink, &logical);

  /* MANDATORY: pango_layout_xy_to_index returns FALSE for an out-of-bounds
   * point but still writes the NEAREST index. Without this guard, clicking
   * 200px right of a line "hits" the last link on it, which reads as the app
   * behaving at random. */
  if (lx < logical.x || lx > logical.x + logical.width ||
      ly < logical.y || ly > logical.y + logical.height)
    return false;

  int index = 0, trailing = 0;
  pango_layout_xy_to_index(b->lay.pl, (int)(lx * PANGO_SCALE),
                           (int)(ly * PANGO_SCALE), &index, &trailing);

  /* Runs overlap, so prefer the innermost match. */
  const nd_run *best = NULL;
  for (uint32_t i = 0; i < b->inl.nruns; i++) {
    const nd_run *r = &b->inl.runs[i];
    if (!(r->flags & (ND_RUN_LINK | ND_RUN_WIKILINK))) continue;
    if ((uint32_t)index < r->start || (uint32_t)index >= r->end) continue;
    if (!best || (r->end - r->start) < (best->end - best->start)) best = r;
  }
  if (!best || !best->href) return false;

  out->kind = (best->flags & ND_RUN_WIKILINK) ? ND_HIT_WIKILINK : ND_HIT_LINK;
  out->url = best->href;

  /* Bounds for the shell's hover underline: the run's own extents. */
  PangoRectangle rr;
  pango_layout_index_to_pos(b->lay.pl, (int)best->start, &rr);
  PangoRectangle re;
  pango_layout_index_to_pos(b->lay.pl, (int)best->end, &re);
  out->x = b->lay.x + (double)rr.x / PANGO_SCALE;
  out->y = b->lay.y + (double)rr.y / PANGO_SCALE;
  out->w = ((double)(re.x - rr.x)) / PANGO_SCALE;
  out->h = (double)rr.height / PANGO_SCALE;
  if (out->w <= 0) out->w = 8.0; /* run wrapped across lines */
  return true;
}

/* Top-right of the code panel. The language label sits top-LEFT, so they never
 * collide. Kept here rather than in the painter so hit area and drawn area
 * cannot drift apart. */
#define ND_COPY_BTN   22.0
#define ND_COPY_INSET  8.0

static void copy_button_rect(const nd_block *b, double *bx, double *by) {
  *bx = b->lay.x + b->lay.w - ND_COPY_INSET - ND_COPY_BTN;
  *by = b->lay.y + ND_COPY_INSET;
}

static bool hit_code(nd_block *b, double x, double y, nd_hit *out) {
  if (b->kind != ND_CODE) return false;
  if (x < b->lay.x || x > b->lay.x + b->lay.w) return false;
  if (y < b->lay.y || y > b->lay.y + b->lay.h) return false;

  double bx, by;
  copy_button_rect(b, &bx, &by);

  bool on_button = x >= bx && x <= bx + ND_COPY_BTN &&
                   y >= by && y <= by + ND_COPY_BTN;

  out->kind = on_button ? ND_HIT_CODE_COPY : ND_HIT_CODE;
  out->text = b->code_text;
  out->text_len = b->code_len;
  out->x = bx; out->y = by;
  out->w = ND_COPY_BTN; out->h = ND_COPY_BTN;
  return true;
}

/* The chevron sits at the right end of the title row. */
#define ND_FOLD_BTN 20.0

static void fold_button_rect(const nd_block *b, double *bx, double *by) {
  *bx = b->lay.x + b->lay.w - 16.0 - ND_FOLD_BTN;
  *by = b->lay.y + 10.0;
}

static bool hit_fold(nd_block *b, double x, double y, nd_hit *out) {
  if (b->kind != ND_CALLOUT) return false;

  double bx, by;
  fold_button_rect(b, &bx, &by);
  if (x < bx || x > bx + ND_FOLD_BTN || y < by || y > by + ND_FOLD_BTN)
    return false;

  out->kind = ND_HIT_CALLOUT_FOLD;
  out->checked = b->callout_folded != 0;
  out->x = bx; out->y = by;
  out->w = ND_FOLD_BTN; out->h = ND_FOLD_BTN;
  return true;
}

static bool hit_block(nd_block *b, double x, double y, nd_hit *out) {
  if (b->kind == ND_CODE) return hit_code(b, x, y, out);
  if (b->kind == ND_CALLOUT && hit_fold(b, x, y, out)) return true;

  if (b->kind == ND_ITEM && hit_checkbox(b, x, y, out)) return true;

  if (b->kind == ND_IMAGE && b->lay.img_w > 0 &&
      x >= b->lay.x && x <= b->lay.x + b->lay.img_w) {
    out->kind = ND_HIT_IMAGE;
    out->url = b->img_src;
    out->x = b->lay.x; out->y = b->lay.y;
    out->w = b->lay.img_w; out->h = b->lay.img_h;
    return true;
  }

  if (hit_children(b, x, y, out)) return true;
  return hit_text(b, x, y, out);
}

bool nd_hit_tree(nd_block *root, double x, double doc_y, nd_hit *out) {
  out->kind = ND_HIT_NONE;
  out->url = NULL;

  /* Same binary search the painter uses: top-level blocks only, because
   * containers span their children and break the ordering precondition. */
  uint32_t lo = 0, hi = root->nkids;
  while (lo < hi) {
    uint32_t mid = (lo + hi) / 2;
    nd_block *k = root->kids[mid];
    if (k->lay.y + k->lay.h <= doc_y) lo = mid + 1;
    else                              hi = mid;
  }
  for (uint32_t i = lo; i < root->nkids; i++) {
    nd_block *k = root->kids[i];
    if (k->lay.y > doc_y) break;
    if (hit_block(k, x, doc_y, out)) return true;
  }
  return false;
}
