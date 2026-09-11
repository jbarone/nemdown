/* toc.h — table of contents built from heading blocks. */

#ifndef NEMDOWN_TOC_H
#define NEMDOWN_TOC_H

#include "doc/arena.h"
#include "doc/doc.h"
#include "doc/node.h"

/* Owned by the document. The public nd_toc is a view onto `items`; `blocks`
 * runs parallel to it so y-offsets can be refreshed after a relayout without
 * rebuilding text or nesting. */
struct nd_toc_store {
  nd_toc_entry *items;
  nd_block    **blocks;
  size_t        count;
};

void nd_toc_build(struct nd_arena *a, nd_block *root,
                  struct nd_toc_store *store, nd_toc *view);

void nd_toc_refresh_offsets(struct nd_toc_store *store);

/* First H1, else the file's basename. */
char *nd_toc_document_title(struct nd_arena *a, nd_block *root, const char *path);

/* Lowercases, turns runs of non-alphanumerics into single dashes, trims. Used
 * for heading slugs AND for normalising a `#anchor` before matching one. */
char *nd_toc_slugify(struct nd_arena *a, const char *text);

#endif /* NEMDOWN_TOC_H */
