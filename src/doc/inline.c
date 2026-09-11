/* inline.c — offset-preserving edits to inline text. */

#include "doc/inline.h"

#include <stdlib.h>
#include <string.h>

/* Maps an original offset to its post-deletion position. */
static uint32_t remap(uint32_t off, const nd_range *dels, uint32_t ndel) {
  uint32_t shift = 0;
  for (uint32_t i = 0; i < ndel; i++) {
    if (dels[i].end <= off) {
      shift += dels[i].end - dels[i].start;
    } else if (dels[i].start < off) {
      /* Offset falls inside a deleted range: clamp to where it collapses. */
      shift += off - dels[i].start;
    } else {
      break;
    }
  }
  return off - shift;
}

void nd_inline_delete(struct nd_arena *a, nd_inline *inl,
                      const nd_range *dels, uint32_t ndel) {
  if (!ndel || !inl->text) return;

  uint32_t removed = 0;
  for (uint32_t i = 0; i < ndel; i++) removed += dels[i].end - dels[i].start;
  if (removed == 0 || removed > inl->len) return;

  char *out = nd_arena_alloc(a, inl->len - removed + 1);
  uint32_t w = 0, r = 0;
  for (uint32_t i = 0; i < ndel; i++) {
    if (dels[i].start > r) {
      memcpy(out + w, inl->text + r, dels[i].start - r);
      w += dels[i].start - r;
    }
    r = dels[i].end;
  }
  if (r < inl->len) {
    memcpy(out + w, inl->text + r, inl->len - r);
    w += inl->len - r;
  }
  out[w] = '\0';

  inl->text = out;
  inl->len = w;

  for (uint32_t i = 0; i < inl->nruns; i++) {
    inl->runs[i].start = remap(inl->runs[i].start, dels, ndel);
    inl->runs[i].end   = remap(inl->runs[i].end, dels, ndel);
  }
}

/* True if `off` sits inside a run whose literal text must be left alone. */
static bool in_literal(const nd_inline *inl, uint32_t off) {
  for (uint32_t i = 0; i < inl->nruns; i++) {
    const nd_run *r = &inl->runs[i];
    if ((r->flags & (ND_RUN_CODE | ND_RUN_MATH)) && off >= r->start && off < r->end)
      return true;
  }
  return false;
}

void nd_inline_extract_highlights(struct nd_arena *a, nd_inline *inl) {
  if (!inl->text || inl->len < 5) return;

  nd_range dels[64];
  nd_run   adds[32];
  uint32_t ndel = 0, nadd = 0;

  uint32_t i = 0;
  while (i + 1 < inl->len && nadd < 32 && ndel + 2 <= 64) {
    if (inl->text[i] != '=' || inl->text[i + 1] != '=' || in_literal(inl, i)) {
      i++;
      continue;
    }
    uint32_t open = i;
    uint32_t j = i + 2;
    while (j + 1 < inl->len &&
           !(inl->text[j] == '=' && inl->text[j + 1] == '=' && !in_literal(inl, j)))
      j++;
    if (j + 1 >= inl->len) break; /* unterminated: leave it literal */

    adds[nadd].start = open + 2;
    adds[nadd].end   = j;
    adds[nadd].flags = ND_RUN_HIGHLIGHT;
    adds[nadd].depth = 0;
    adds[nadd].href  = NULL;
    nadd++;

    dels[ndel++] = (nd_range){open, open + 2};
    dels[ndel++] = (nd_range){j, j + 2};
    i = j + 2;
  }

  if (!nadd) return;

  /* Append the new runs before the deletion shifts everything, so they get
   * remapped along with the existing ones. */
  nd_run *merged = nd_arena_alloc(a, (inl->nruns + nadd) * sizeof *merged);
  if (inl->nruns) memcpy(merged, inl->runs, inl->nruns * sizeof *merged);
  memcpy(merged + inl->nruns, adds, nadd * sizeof *adds);
  inl->runs = merged;
  inl->nruns += nadd;

  nd_inline_delete(a, inl, dels, ndel);
}
