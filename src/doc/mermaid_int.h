/* mermaid_int.h — shared between the mermaid translators and renderers.
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
/* A fence is untrusted input and graph layout is superlinear in the node
 * count, so every dimension of the problem is bounded before graphviz is
 * handed anything. Past any of these the fence shows its source, which is a
 * legible outcome rather than a stalled frame. */
#define ND_MM_MAX_SRC    65536u   /* bytes of fence body */
#define ND_MM_MAX_LINE    1024u   /* a single logical line */
#define ND_MM_MAX_NODES    400u
#define ND_MM_MAX_EDGES    800u
#define ND_MM_MAX_PARTS     40u   /* sequence columns */
#define ND_MM_MAX_EVENTS   600u   /* sequence rows */
#define ND_MM_MAX_DIM     8000.0  /* laid-out diagram, either axis */

/* ---- palette, as dot-ready strings ------------------------------------- */
#define ND_MM_BG      "#1e1e2e"
#define ND_MM_FILL    "#313244"
#define ND_MM_FILL2   "#45475a"
#define ND_MM_BORDER  "#585b70"
#define ND_MM_TEXT    "#cdd6f4"
#define ND_MM_DIM     "#a6adc8"
#define ND_MM_EDGE    "#7f849c"
#define ND_MM_ACCENT  "#94e2d5"
#define ND_MM_DECIDE  "#f9e2af"
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
void  nd_buf_puts(nd_buf *b, const char *s);
void  nd_buf_fmt(nd_buf *b, const char *fmt, ...)
      __attribute__((format(printf, 2, 3)));
char *nd_buf_take(nd_buf *b);   /* NUL-terminated, caller frees; NULL if !ok */
void  nd_buf_free(nd_buf *b);

/* ---- line iteration over a fence body ---------------------------------- */
typedef struct {
  const char *src;
  uint32_t    len, pos;
} nd_lines;

/* Copies the next meaningful line into `out`: trimmed, `%%` comments removed,
 * a trailing `;` dropped, blank lines skipped. False at end of input. */
bool nd_mm_line(nd_lines *it, char *out, size_t cap);

/* ---- label text -------------------------------------------------------- */
/* In place: strips one layer of matching quotes, turns `<br>` and `<br/>` into
 * newlines, decodes the `#nn;` escapes mermaid uses for punctuation it would
 * otherwise eat, and removes control characters. */
void nd_mm_clean_label(char *s);

/* Appends `s` as a dot quoted string, escapes included. */
void nd_mm_dot_str(nd_buf *b, const char *s);
/* Appends `s` with `&<>` escaped, for an HTML-like label. */
void nd_mm_html_esc(nd_buf *b, const char *s);

/* ---- graphviz, behind dlopen ------------------------------------------- */
bool nd_mm_gv_available(void);

/* Lays `dot` out and parses the SVG it produces. `*w`/`*h` come back as the
 * viewBox size, which is the coordinate system the font sizes in the dot were
 * written in — so a diagram drawn at that size has text the size we asked for.
 * NULL if graphviz is missing, the dot does not parse, or the result is past
 * ND_MM_MAX_DIM. */
RsvgHandle *nd_mm_gv_render(const char *dot, double *w, double *h);

/* ---- the three graph dialects ------------------------------------------ */
/* Each returns malloc'd dot source, or NULL if the source is not that dialect
 * or carries nothing to draw. */
char *nd_mm_flow_dot(const char *src, uint32_t len);
char *nd_mm_state_dot(const char *src, uint32_t len);
char *nd_mm_class_dot(const char *src, uint32_t len);

/* ---- the sequence renderer --------------------------------------------- */
struct nd_seq;
struct nd_seq *nd_mm_seq_build(PangoContext *pctx, const char *src,
                               uint32_t len);
void nd_mm_seq_size(const struct nd_seq *s, double *w, double *h);
void nd_mm_seq_paint(const struct nd_seq *s, cairo_t *cr);
void nd_mm_seq_free(struct nd_seq *s);

#endif /* NEMDOWN_MERMAID_INT_H */
