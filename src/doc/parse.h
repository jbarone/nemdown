/* parse.h — md4c callback stream into a retained tree. */

#ifndef NEMDOWN_PARSE_H
#define NEMDOWN_PARSE_H

#include "doc/arena.h"
#include "doc/node.h"
#include "doc/preprocess.h"

/* Builds the tree from an already-preprocessed source. Returns NULL on a
 * parser failure (which md4c makes very hard to provoke). */
nd_block *nd_parse(struct nd_arena *a, const struct nd_source *src);

#endif /* NEMDOWN_PARSE_H */
