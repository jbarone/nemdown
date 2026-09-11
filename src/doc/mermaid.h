/* mermaid.h — mermaid diagrams, rendered rather than shown as source.
 *
 * Four dialects are supported, because they are the four the author of this
 * program draws: `flowchart`, `sequenceDiagram`, `stateDiagram-v2` and
 * `classDiagram`. Anything else — and anything that fails to parse — falls
 * back to the ordinary syntax-highlighted code panel, which is the honest
 * answer: the reader sees exactly what the file says.
 *
 * Two renderers sit behind this one header, and the split is not arbitrary.
 * Three of the four dialects are *graphs*: a set of nodes and edges with no
 * inherent geometry, so something has to solve a layout for them, and that
 * something is graphviz. The fourth is not a graph at all — a sequence diagram
 * is a table of columns and rows whose only unknown is how wide the text is,
 * and Pango already answers that. Running it through a graph layout engine
 * would be both harder and worse.
 *
 * graphviz is loaded with dlopen at RUN time, not linked at build time, which
 * is the same bargain the maths typesetter strikes with its font: installed,
 * the diagrams draw; absent, they degrade to source and nothing else changes.
 * That keeps one binary working either way and keeps graphviz an `optdepends`
 * rather than an 11MB hard dependency for a feature many documents never use.
 * Sequence diagrams need neither, and so always render.
 */

#ifndef NEMDOWN_MERMAID_H
#define NEMDOWN_MERMAID_H

#include <cairo.h>
#include <pango/pangocairo.h>
#include <stdbool.h>
#include <stdint.h>

struct nd_mermaid;

/* True for a fence whose info string names mermaid, e.g. ```mermaid. */
bool nd_mermaid_is_fence(const char *lang);

/* Builds a diagram drawn at `zoom` times its natural size, and never wider
 * than `max_w`. `zoom` is the document's font scale: a reader who presses `+`
 * because the text is small means the figures too, and a diagram that alone
 * refused to grow would read as a bug.
 *
 * Returns NULL when the dialect is unknown, the source does not parse, the
 * diagram is beyond the size caps, or graphviz is needed and absent — in every
 * one of those cases the caller shows the source instead. */
struct nd_mermaid *nd_mermaid_build(PangoContext *pctx, const char *src,
                                    uint32_t len, double max_w, double zoom);

void nd_mermaid_size(const struct nd_mermaid *m, double *w, double *h);

/* Draws with the diagram's top-left at (x, y), in logical units. */
void nd_mermaid_paint(struct nd_mermaid *m, cairo_t *cr, double x, double y);

void nd_mermaid_free(struct nd_mermaid *m);

/* Whether the graph dialects can render at all on this machine. Used only to
 * word the one-line hint on a fence that would have drawn. */
bool nd_mermaid_have_graphviz(void);

#endif /* NEMDOWN_MERMAID_H */
