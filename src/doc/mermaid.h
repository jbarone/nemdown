/* mermaid.h — mermaid diagrams, rendered rather than shown as source.
 *
 * Every dialect is drawn by pele, Obsidian's mermaid renderer, run in QuickJS
 * (mermaid_pele.c). Behind it sit the native renderers, which predate it and
 * draw four dialects: `flowchart`, `stateDiagram-v2` and `classDiagram` are
 * graphs, laid out by graphviz, and `sequenceDiagram` is a table of columns
 * and rows that Pango measures directly. A fence pele refuses goes to them,
 * and one that both refuse falls back to the ordinary syntax-highlighted code
 * panel, which is the honest answer: the reader sees exactly what the file
 * says.
 *
 * graphviz is loaded with dlopen at RUN time, not linked at build time:
 * installed, the fallback can draw graphs; absent, they degrade to source and
 * nothing else changes. quickjs-ng is linked; see mermaid_pele.c for why.
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
 * Returns NULL when neither pele nor the native renderers can draw it: the
 * dialect is unknown, the source does not parse, the diagram is beyond the
 * size caps, or graphviz is needed and absent. In every one of those cases the
 * caller shows the source instead. */
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
