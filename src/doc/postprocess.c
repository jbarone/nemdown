/* postprocess.c — callout detection, highlight extraction, image promotion. */

#include "doc/postprocess.h"

#include <string.h>

#include "doc/callout.h"
#include "doc/inline.h"

/* A callout is a blockquote whose first paragraph opens with `[!type]`.
 *
 * The dumper showed the marker, the title and the body arrive as ONE paragraph
 * separated by a soft break — not as separate paragraphs — so splitting at the
 * first newline is the normal path here, not an edge case. */
static bool detect_callout(struct nd_arena *a, nd_block *q) {
  if (q->kind != ND_QUOTE || q->nkids == 0) return false;

  nd_block *first = q->kids[0];
  if (first->kind != ND_PARA || first->inl.len < 4) return false;

  const char *t = first->inl.text;
  uint32_t n = first->inl.len;
  if (t[0] != '[' || t[1] != '!') return false;

  uint32_t close = 2;
  while (close < n && t[close] != ']' && t[close] != '\n') close++;
  if (close >= n || t[close] != ']') return false;

  q->kind = ND_CALLOUT;
  q->callout_type = nd_callout_lookup(t + 2, close - 2);

  uint32_t p = close + 1;
  if (p < n && (t[p] == '-' || t[p] == '+')) {
    q->callout_folded = (t[p] == '-');
    p++;
  }

  /* The title runs to the first source line break. Soft breaks were folded to
   * spaces during parsing, so the recorded offset is the only way back to it.
   * Establish the boundary BEFORE trimming, or the trim walks past it and a
   * title-less callout swallows its own body. */
  uint32_t eol = first->inl.first_break;
  if (eol < p) {
    eol = p;
    while (eol < n && t[eol] != '\n') eol++;
  }

  while (p < eol && (t[p] == ' ' || t[p] == '\t')) p++;

  /* Title keeps its own formatting runs, so build it by deleting everything
   * that is not the title rather than by copying the bytes out. */
  nd_inline title = first->inl;
  title.runs = nd_arena_memdup(a, first->inl.runs,
                               first->inl.nruns * sizeof(nd_run));
  title.text = nd_arena_strndup(a, first->inl.text, first->inl.len);

  nd_range tdel[2];
  uint32_t ntdel = 0;
  if (p > 0)   tdel[ntdel++] = (nd_range){0, p};
  if (eol < n) tdel[ntdel++] = (nd_range){eol, n};
  nd_inline_delete(a, &title, tdel, ntdel);
  q->title = title;

  if (title.len == 0) {
    /* Title-less callouts show the type name, as Obsidian does. */
    const char *nm = nd_callout_types[q->callout_type].name;
    char *cap = nd_arena_strndup(a, nm, strlen(nm));
    if (cap[0] >= 'a' && cap[0] <= 'z') cap[0] = (char)(cap[0] - 'a' + 'A');
    q->title.text = cap;
    q->title.len = (uint32_t)strlen(cap);
    q->title.runs = NULL;
    q->title.nruns = 0;
  }

  /* Strip the title off the body paragraph, keeping the remainder. */
  nd_range bdel = {0, eol < n ? eol + 1 : n};
  nd_inline_delete(a, &first->inl, &bdel, 1);

  return true;
}

/* A paragraph whose only content is an image reads better as its own block:
 * it can be centred and captioned instead of sitting on a text baseline. */
static void promote_images(nd_block *b) {
  if (b->kind != ND_PARA || b->nkids == 0) return;
  bool text_empty = true;
  for (uint32_t i = 0; i < b->inl.len; i++) {
    char c = b->inl.text[i];
    if (c != ' ' && c != '\n' && c != '\t') { text_empty = false; break; }
  }
  if (text_empty) b->inl.len = 0;
}

static void walk(struct nd_arena *a, nd_block *b) {
  if (b->kind == ND_QUOTE) detect_callout(a, b);
  if (b->inl.len) nd_inline_extract_highlights(a, &b->inl);
  promote_images(b);
  for (uint32_t i = 0; i < b->nkids; i++) walk(a, b->kids[i]);
}

void nd_postprocess(struct nd_arena *a, nd_block *root) {
  walk(a, root);
}
