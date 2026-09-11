/* hit.h — map a document-space point to something actionable. */

#ifndef NEMDOWN_HIT_H
#define NEMDOWN_HIT_H

#include "doc/doc.h"
#include "doc/node.h"

bool nd_hit_tree(nd_block *root, double x, double doc_y, nd_hit *out);

#endif /* NEMDOWN_HIT_H */
