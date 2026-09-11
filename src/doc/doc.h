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
  ND_HIT_IMAGE, ND_HIT_TEXT
} nd_hit_kind;

typedef struct {
  nd_hit_kind kind;
  const char *url;
  uint32_t    source_offset; /* ND_HIT_CHECKBOX: byte to flip in the file */
  bool        checked;
  double      x, y, w, h;    /* document space, for the shell's hover paint */
} nd_hit;

/* ---- lifecycle ---------------------------------------------------------- */

nd_doc *nd_doc_open(const char *path, char **err);
bool    nd_doc_reload(nd_doc *d, char **err);
void    nd_doc_free(nd_doc *d);

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

/* ---- interaction -------------------------------------------------------- */

bool nd_doc_hit_test(nd_doc *d, double x, double doc_y, nd_hit *out);

/* ---- sidebar data ------------------------------------------------------- */

/* The sidebar draws text too; sharing one context keeps font options and the
 * scale-invariance guarantee identical on both sides. */
struct _PangoContext *nd_doc_pango_context(const nd_doc *d);

const nd_props *nd_doc_props(const nd_doc *d);
const nd_toc   *nd_doc_toc(const nd_doc *d);
const char     *nd_doc_title(const nd_doc *d);
int             nd_toc_active(const nd_doc *d, double scroll_y);
double          nd_toc_target_y(const nd_doc *d, int index, double viewport_h);

#endif /* NEMDOWN_DOC_H */
