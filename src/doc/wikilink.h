/* wikilink.h — resolving `[[target]]` against the document's own directory.
 *
 * This is deliberately NOT a vault index. nemdown opens one file at a time, so
 * a wikilink resolves the way a relative path would: next to the document that
 * mentions it. Predictable, and no directory walk.
 */

#ifndef NEMDOWN_WIKILINK_H
#define NEMDOWN_WIKILINK_H

#include "doc/arena.h"
#include "doc/node.h"

/* Resolves every wikilink run against `dir`, rewriting href to an absolute
 * path when the file exists and flagging it dead when it does not. */
void nd_wikilink_resolve_tree(struct nd_arena *a, nd_block *root, const char *dir);

/* Splits `note#heading` into a path and an anchor. Returns the arena-owned
 * path; *anchor points into it, or NULL. */
char *nd_wikilink_split(struct nd_arena *a, const char *target,
                        const char **anchor);

#endif /* NEMDOWN_WIKILINK_H */
