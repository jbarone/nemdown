/* node.h — the retained document tree.
 *
 * md4c is a streaming SAX parser, but layout runs on every resize and paint
 * runs every frame, so the callback stream is accumulated into a tree once and
 * kept. Re-parsing per frame is not viable: a 100KB document is ~30ms of
 * parsing against a 16ms frame budget.
 */

#ifndef NEMDOWN_NODE_H
#define NEMDOWN_NODE_H

#include <pango/pangocairo.h>
#include <stdbool.h>
#include <stdint.h>

typedef enum {
  ND_DOC_ROOT, ND_PARA, ND_HEADING, ND_CODE, ND_MATH_BLOCK, ND_QUOTE,
  ND_CALLOUT, ND_LIST, ND_ITEM, ND_HR, ND_TABLE, ND_ROW, ND_CELL, ND_IMAGE
} nd_block_kind;

/* Inline style flags. Runs deliberately OVERLAP rather than being flattened
 * into a style cross-product: PangoAttrList consumes overlapping ranges
 * natively and intersects them per character, so bold-inside-italic-inside-a-
 * link composes for free. Flattening would make nesting quadratic and make
 * offset remapping for ==highlight== nearly impossible. */
typedef enum {
  ND_RUN_EM        = 1u << 0,
  ND_RUN_STRONG    = 1u << 1,
  ND_RUN_CODE      = 1u << 2,
  ND_RUN_DEL       = 1u << 3,
  ND_RUN_U         = 1u << 4,
  ND_RUN_LINK      = 1u << 5,
  ND_RUN_WIKILINK  = 1u << 6,
  ND_RUN_MATH      = 1u << 7,
  ND_RUN_HIGHLIGHT = 1u << 8,
} nd_run_flags;

typedef struct {
  uint32_t start, end;  /* byte offsets into nd_inline.text */
  uint32_t flags;
  uint16_t depth;       /* span nesting depth at enter; drives attribute order */
  char    *href;        /* link target, arena-owned; NULL otherwise */
} nd_run;

typedef struct {
  char    *text;        /* UTF-8, NUL-terminated, no interior NUL */
  uint32_t len;
  nd_run  *runs;
  uint32_t nruns;
  /* Byte offset of the first soft break, or 0 for none. Soft breaks render as
   * spaces (Obsidian's default), so the source line boundary would otherwise be
   * unrecoverable — and callout titles are delimited by exactly that. */
  uint32_t first_break;
} nd_inline;

/* Per-block layout results, recomputed when the column width changes. */
typedef struct {
  double x, y, w, h;
  PangoLayout *pl;      /* primary text; NULL for containers and rules */
  PangoLayout *marker;  /* list bullet or number */
  double *colw;         /* tables: resolved column widths */
  double *rowy;         /* tables: row offsets */
  double  img_w, img_h;
  double  natural_w;    /* code blocks: unwrapped width, for overflow */
} nd_layout;

typedef struct nd_block nd_block;

struct nd_block {
  nd_block_kind kind;
  nd_block    **kids;
  uint32_t      nkids;
  nd_block     *parent;

  nd_inline inl;    /* leaf content; empty for containers */
  nd_inline title;  /* callouts only */

  uint8_t  level;        /* heading 1..6 */
  uint8_t  list_ordered, list_tight;
  uint32_t list_start;
  uint8_t  item_is_task, item_checked;
  uint32_t item_mark_offset; /* source byte of the char between the brackets */
  uint8_t  cell_align;
  uint8_t  row_is_header;
  uint8_t  callout_type, callout_folded;
  char    *code_lang;
  char    *code_text;
  uint32_t code_len;
  char    *img_src;
  char    *img_size;     /* Obsidian `|400` or `|400x300` hint, or NULL */

  nd_layout lay;
};

#endif /* NEMDOWN_NODE_H */
