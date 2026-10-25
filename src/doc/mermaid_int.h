/* mermaid_int.h — shared between mermaid.c and the pele renderer.
 *
 * Not part of the engine's public surface; doc.h knows only mermaid.h.
 */

#ifndef NEMDOWN_MERMAID_INT_H
#define NEMDOWN_MERMAID_INT_H

#include <librsvg/rsvg.h>
#include <pango/pangocairo.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---- caps -------------------------------------------------------------- */
/* A fence is untrusted input and graph layout is superlinear in the edge
 * count, so the problem is bounded before pele is handed anything. Past any
 * of these the fence shows its source, which is a legible outcome rather than
 * a stalled frame. */
#define ND_MM_MAX_SRC    65536u   /* bytes of fence body */
#define ND_MM_MAX_LINE    1024u   /* bytes of one measured string */
#define ND_MM_MAX_EDGES    800u   /* pele's maxEdges */
#define ND_MM_MAX_DIM     8000.0  /* laid-out diagram, either axis */

/* ---- palette ----------------------------------------------------------- */
#define ND_MM_BG      "#1e1e2e"
#define ND_MM_FILL    "#313244"
#define ND_MM_FILL2   "#45475a"
#define ND_MM_BORDER  "#585b70"
#define ND_MM_TEXT    "#cdd6f4"
#define ND_MM_DIM     "#a6adc8"
#define ND_MM_ACCENT  "#94e2d5"
#define ND_MM_FONT    "Hack Nerd Font"

/* ---- a growable byte buffer -------------------------------------------- */
/* `ok` latches false on the first allocation failure, so call sites append
 * freely and check once at the end rather than testing every write. */
typedef struct {
  char  *p;
  size_t len, cap;
  bool   ok;
} nd_buf;

void  nd_buf_add(nd_buf *b, const char *s, size_t n);
char *nd_buf_take(nd_buf *b);   /* NUL-terminated, caller frees; NULL if !ok */
void  nd_buf_free(nd_buf *b);

/* ---- pele, in QuickJS -------------------------------------------------- */
/* Every dialect pele knows. NULL if the source does not parse, the render ran
 * past its time or heap budget, or the result is past ND_MM_MAX_DIM; the
 * caller then shows the source. */
bool nd_mm_pele_available(PangoContext *pctx);
RsvgHandle *nd_mm_pele_render(PangoContext *pctx, const char *src,
                              uint32_t len, double max_w, double *w,
                              double *h);

/* The same render, stopped before theming: pele's own SVG, malloc'd. For
 * check_mermaid, which needs the input to nd_mm_pele_theme as well as its
 * output. */
char *nd_mm_pele_svg(PangoContext *pctx, const char *src, uint32_t len,
                     double max_w, double *w, double *h);

/* Resolves pele's CSS custom properties, which librsvg does not implement,
 * against the Mocha theme. Malloc'd; NULL on allocation failure. Exposed for
 * check_mermaid, which asserts it cannot unbalance the markup. */
char *nd_mm_pele_theme(const char *svg, size_t len);

/* The bundle, from pele_bundle.c. NUL-terminated, as JS_Eval requires. */
extern const char nd_pele_js[], nd_pele_js_end[];

#endif /* NEMDOWN_MERMAID_INT_H */
