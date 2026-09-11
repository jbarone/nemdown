/* doc.h — THE SEAM between the Wayland shell and the document engine.
 *
 * The engine never touches Wayland, never learns the window size, never owns a
 * scroll offset and never sees an input event. It is handed a viewport width
 * and a document-space y, and owns the readable-measure clamp and centering
 * itself. The shell never parses markdown and never touches Pango.
 *
 * LIFETIME RULE: every pointer returned from here is borrowed and is
 * invalidated by nd_doc_reload() and nd_doc_free(). After a reload the caller
 * must re-fetch the TOC and the properties. This is the likeliest
 * use-after-free in the project.
 */

#ifndef NEMDOWN_DOC_H
#define NEMDOWN_DOC_H

#include <cairo.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct nd_doc nd_doc;

/* ---- frontmatter, produced here and painted by the sidebar -------------- */

typedef enum {
  ND_PROP_TEXT, ND_PROP_LIST, ND_PROP_NUMBER,
  ND_PROP_CHECKBOX, ND_PROP_DATE, ND_PROP_DATETIME, ND_PROP_EMPTY
} nd_prop_type;

typedef struct {
  const char  *key;
  nd_prop_type type;
  const char  *text;      /* scalar display form; NULL for lists */
  double       number;
  bool         checkbox;
  const char *const *items;
  size_t       nitems;
  bool         unsupported; /* nested YAML we render dimmed rather than drop */
} nd_property;

typedef struct {
  const nd_property *items;
  size_t             count;
  bool               present;    /* was there a fence at all */
  const char        *error;      /* NULL on a clean parse */
  int                error_line; /* 1-based, within the YAML region */
} nd_props;

/* ---- table of contents -------------------------------------------------- */

typedef struct {
  const char *text;
  const char *slug;
  uint8_t     level; /* 1..6 as written */
  uint8_t     depth; /* normalised nesting, 0-based */
  double      y;     /* document-space y; rewritten by every layout */
} nd_toc_entry;

typedef struct {
  const nd_toc_entry *items;
  size_t              count;
} nd_toc;

/* ---- hit testing -------------------------------------------------------- */

typedef enum {
  ND_HIT_NONE, ND_HIT_LINK, ND_HIT_WIKILINK, ND_HIT_CHECKBOX,
  ND_HIT_IMAGE, ND_HIT_TEXT,
  /* Over a code block, and over its copy button specifically. The button is
   * only drawn on hover, so the shell needs to know about the former too. */
  ND_HIT_CODE, ND_HIT_CODE_COPY,
  ND_HIT_CALLOUT_FOLD
} nd_hit_kind;

typedef struct {
  nd_hit_kind kind;
  const char *url;
  uint32_t    source_offset; /* ND_HIT_CHECKBOX: byte to flip in the file */
  bool        checked;
  const char *text;          /* ND_HIT_CODE*: the fence's contents */
  uint32_t    text_len;
  /* Document space. For ND_HIT_CODE* this is the BUTTON's rect, not the
   * block's, so the geometry lives in one place and the shell just draws it. */
  double      x, y, w, h;
} nd_hit;

/* ---- lifecycle ---------------------------------------------------------- */

nd_doc *nd_doc_open(const char *path, char **err);
bool    nd_doc_reload(nd_doc *d, char **err);

/* Swaps in a different file, reusing the Pango context. Like reload, this
 * invalidates every borrowed pointer. */
bool    nd_doc_load(nd_doc *d, const char *path, char **err);

const char *nd_doc_path(const nd_doc *d);
void    nd_doc_free(nd_doc *d);

/* Document-space y of a heading whose slug matches, or -1. */
double  nd_doc_anchor_y(const nd_doc *d, const char *slug);

/* ---- layout and paint --------------------------------------------------- */

/* Reflow for a viewport width. Cheap and idempotent when nothing relevant
 * changed, so the shell may call it every frame. Returns content height. */
double nd_doc_layout(nd_doc *d, double viewport_w, double font_scale);
double nd_doc_content_height(const nd_doc *d);

/* Display scale affects painting only: layout is scale-invariant because
 * metrics hinting is off. This drops resolution-dependent caches. */
void nd_doc_set_scale(nd_doc *d, double scale);

/* cr's origin is the content area's top-left and it is already clipped. */
void nd_doc_paint(nd_doc *d, cairo_t *cr, double scroll_y, double viewport_h);

/* ---- stable scroll position across reload and reflow -------------------- */

/* Encodes "which block, and how far into it" rather than a raw pixel offset,
 * so a reload that changes block heights above the viewport does not move the
 * reader. Without this, every save jumps you back to the top. */
uint64_t nd_doc_anchor_at(const nd_doc *d, double doc_y);
double   nd_doc_y_for_anchor(const nd_doc *d, uint64_t anchor);

/* ---- interaction -------------------------------------------------------- */

bool nd_doc_hit_test(nd_doc *d, double x, double doc_y, nd_hit *out);

/* Collapses or expands the callout whose fold chevron was hit. */
void nd_doc_toggle_fold(nd_doc *d, double x, double doc_y);

/* Writes a single byte to the source file, flipping a task checkbox. */
bool nd_doc_toggle_task(nd_doc *d, uint32_t source_offset, bool now_checked);

/* ---- horizontal panning ------------------------------------------------- */

/* Code fences do not wrap; they clip and pan, as Obsidian's do. Returns true
 * if a fence under the point actually moved, so the caller knows whether to
 * consume the scroll event rather than passing it to the page. */
bool nd_doc_pan_code(nd_doc *d, double x, double doc_y, double dx);

/* Keyboard equivalent: pans the visible fence nearest the viewport centre.
 * Fences that do not overflow are skipped, so h/l never appear to do nothing
 * while a pannable block is on screen. */
bool nd_doc_pan_focused(nd_doc *d, double scroll_y, double viewport_h, double dx);

/* ---- selection ---------------------------------------------------------- */

void  nd_doc_select_begin(nd_doc *d, double x, double doc_y);
void  nd_doc_select_extend(nd_doc *d, double x, double doc_y);
void  nd_doc_select_word(nd_doc *d, double x, double doc_y);
void  nd_doc_select_block(nd_doc *d, double x, double doc_y);
void  nd_doc_select_clear(nd_doc *d);
bool  nd_doc_has_selection(const nd_doc *d);
/* Rendered text, not markdown source — see select.h. Caller frees with free(). */
char *nd_doc_select_text(nd_doc *d);
/* The document's own bytes, borrowed. */
const char *nd_doc_source(const nd_doc *d);

/* ---- search ------------------------------------------------------------- */

/* Re-runs over the whole document; matches are re-anchored on every layout. */
void   nd_doc_search(nd_doc *d, const char *needle);
void   nd_doc_search_clear(nd_doc *d);
size_t nd_doc_search_count(const nd_doc *d);
int    nd_doc_search_current(const nd_doc *d);
/* Steps the active match and returns the y to scroll to, or -1. */
double nd_doc_search_step(nd_doc *d, int delta, double viewport_h);

/* ---- sidebar data ------------------------------------------------------- */

/* The sidebar draws text too; sharing one context keeps font options and the
 * scale-invariance guarantee identical on both sides. */
struct _PangoContext *nd_doc_pango_context(const nd_doc *d);

/* ---- reading guide ------------------------------------------------------- */

/* A flat, document-ordered list of the units a person reads one at a time --
 * paragraphs, headings, list items, code fences, tables, callouts. The engine
 * supplies the geometry only: which one is current, whether a marker is shown
 * and how it moves are the shell's business, like hover. */
uint32_t nd_doc_reading_count(const nd_doc *d);

/* Left edge of the readable column, in document space. The marker uses this
 * rather than each stop's own x: a list item is indented, and a marker that
 * stepped sideways as you read down a list would be exactly the fidget the
 * thing is meant to prevent. */
double nd_doc_column_x(const nd_doc *d);

/* Geometry of stop `i`, in document space. False when `i` is out of range. */
bool nd_doc_reading_rect(const nd_doc *d, uint32_t i, double *x, double *y,
                         double *w, double *h);

/* The stop being read in the viewport [doc_y, doc_y + viewport_h): the first
 * one WHOLLY visible, so the marker never points at a block that is cut off.
 * When nothing fits -- a block taller than the window, or the end of the
 * document -- it is whichever visible block fills most of the viewport.
 * Returns -1 only for a document with no readable content. */
int nd_doc_reading_at(const nd_doc *d, double doc_y, double viewport_h);

const nd_props *nd_doc_props(const nd_doc *d);
const nd_toc   *nd_doc_toc(const nd_doc *d);
const char     *nd_doc_title(const nd_doc *d);
int             nd_toc_active(const nd_doc *d, double scroll_y);
double          nd_toc_target_y(const nd_doc *d, int index, double viewport_h);

#endif /* NEMDOWN_DOC_H */
