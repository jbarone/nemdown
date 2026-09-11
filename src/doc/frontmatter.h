/* frontmatter.h — YAML properties, as Obsidian uses them. */

#ifndef NEMDOWN_FRONTMATTER_H
#define NEMDOWN_FRONTMATTER_H

#include "doc/arena.h"
#include "doc/doc.h"

/* Parses the frontmatter region into properties. Never fails destructively:
 * malformed YAML yields the properties gathered so far plus an error string,
 * and the document body still renders. */
void nd_frontmatter_parse(struct nd_arena *a, const char *yaml, size_t len,
                          nd_props *out);

#endif /* NEMDOWN_FRONTMATTER_H */
