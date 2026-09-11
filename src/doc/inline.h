/* inline.h — edits to inline text that must carry run offsets with them. */

#ifndef NEMDOWN_INLINE_H
#define NEMDOWN_INLINE_H

#include "doc/arena.h"
#include "doc/node.h"

typedef struct {
  uint32_t start, end;
} nd_range;

/* Deletes byte ranges from `inl->text`, remapping every run offset so the
 * styling still covers the right characters. Ranges must be sorted and
 * non-overlapping.
 *
 * Three features need exactly this — ==highlight==, callout markers, and
 * embed detection — so it is written once and shared rather than open-coded
 * three times with three different off-by-ones. */
void nd_inline_delete(struct nd_arena *a, nd_inline *inl,
                      const nd_range *dels, uint32_t ndel);

/* Finds ==highlight== pairs outside code and math, adds ND_RUN_HIGHLIGHT over
 * the interior, and removes the four marker bytes. md4c has no flag for this
 * and Obsidian supports it. */
void nd_inline_extract_highlights(struct nd_arena *a, nd_inline *inl);

#endif /* NEMDOWN_INLINE_H */
