/* pathguard.h — confine document-supplied paths to the document's tree.
 *
 * A note we did not write can name any path it likes, in an image source, an
 * embed, or a wikilink target. Without a containment check it can make the
 * viewer read and display any file the user can read, and — because the
 * checkbox write follows navigation — can steer that write outside the tree
 * too.
 */

#ifndef NEMDOWN_PATHGUARD_H
#define NEMDOWN_PATHGUARD_H

#include <stdbool.h>

/* True when `path` resolves inside `root`. Both are resolved with realpath()
 * first, so `..`, symlinks and percent-decoded traversal are all handled by
 * comparing what the kernel would actually open. `path` must exist. */
bool nd_path_within(const char *root, const char *path);

#endif /* NEMDOWN_PATHGUARD_H */
