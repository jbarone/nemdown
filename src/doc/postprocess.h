/* postprocess.h — tree fixups that need the whole block, not a callback. */

#ifndef NEMDOWN_POSTPROCESS_H
#define NEMDOWN_POSTPROCESS_H

#include "doc/arena.h"
#include "doc/node.h"

/* Converts qualifying blockquotes into callouts, extracts ==highlight==, and
 * promotes image-only paragraphs to standalone image blocks. */
void nd_postprocess(struct nd_arena *a, nd_block *root);

#endif /* NEMDOWN_POSTPROCESS_H */
