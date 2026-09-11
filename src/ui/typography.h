/* typography.h — the type scale, as data.
 *
 * Sizes are logical pixels at 1x. They are never multiplied by the display
 * scale: the Cairo surface carries that, and doing it twice is the classic
 * "text is 2.25x too large at 1.5x" bug.
 *
 * Hack ships only 400 and 700, so the heading hierarchy comes from size and
 * colour rather than from weight.
 */

#ifndef NEMDOWN_TYPOGRAPHY_H
#define NEMDOWN_TYPOGRAPHY_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
  const char *family;
  double      size;         /* px at 1x */
  double      line_height;  /* px at 1x */
  bool        bold;
  bool        italic;
  uint32_t    color;
  double      space_before;
  double      space_after;
} nd_style;

/* Page geometry. Obsidian uses a readable measure rather than full bleed, and
 * so do we: long lines are the single fastest way to make a document unpleasant. */
#define ND_COLUMN_MAX   700.0
#define ND_PAGE_MARGIN  40.0
#define ND_PAD_TOP      32.0
#define ND_PAD_BOTTOM   64.0

typedef enum {
  ND_ST_BODY, ND_ST_H1, ND_ST_H2, ND_ST_H3, ND_ST_H4, ND_ST_H5, ND_ST_H6,
  ND_ST_CODE, ND_ST_CODE_LABEL, ND_ST_QUOTE, ND_ST_CALLOUT_TITLE,
  ND_ST_TABLE_HEAD, ND_ST_TABLE_CELL, ND_ST_CAPTION, ND_ST_MATH,
  ND_ST_COUNT
} nd_style_id;

extern const nd_style nd_styles[ND_ST_COUNT];

static inline const nd_style *nd_heading_style(unsigned level) {
  if (level < 1) level = 1;
  if (level > 6) level = 6;
  return &nd_styles[ND_ST_H1 + (level - 1)];
}

#endif /* NEMDOWN_TYPOGRAPHY_H */
