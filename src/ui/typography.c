/* typography.c — the type scale. */

#include "ui/typography.h"

#include "ui/theme.h"

const nd_style nd_styles[ND_ST_COUNT] = {
  /*                  family    size  line  bold  ital  colour          before after */
  [ND_ST_BODY]        = {ND_SANS, 14,   23,   false, false, CTP_TEXT,      0,  16},
  [ND_ST_H1]          = {ND_SANS, 30,   39,   true,  false, CTP_TEAL,     32,  16},
  [ND_ST_H2]          = {ND_SANS, 25,   33,   true,  false, CTP_TEAL,     28,  12},
  [ND_ST_H3]          = {ND_SANS, 21,   28,   true,  false, CTP_LAVENDER, 24,  10},
  [ND_ST_H4]          = {ND_SANS, 18,   24,   true,  false, CTP_LAVENDER, 20,   8},
  [ND_ST_H5]          = {ND_SANS, 16,   22,   true,  false, CTP_SUBTEXT1, 18,   6},
  [ND_ST_H6]          = {ND_SANS, 14,   20,   true,  false, CTP_SUBTEXT0, 16,   6},
  [ND_ST_CODE]        = {ND_MONO, 13,   19,   false, false, CTP_TEXT,     16,  16},
  [ND_ST_CODE_LABEL]  = {ND_MONO, 11,   14,   false, false, CTP_OVERLAY0,  0,   0},
  [ND_ST_QUOTE]       = {ND_SANS, 16,   26,   false, false, CTP_SUBTEXT1, 16,  16},
  [ND_ST_CALLOUT_TITLE]={ND_SANS, 16,   21,   true,  false, CTP_TEXT,      0,   6},
  [ND_ST_TABLE_HEAD]  = {ND_SANS, 14.4, 21,   true,  false, CTP_SUBTEXT1,  0,   0},
  [ND_ST_TABLE_CELL]  = {ND_SANS, 14.4, 22,   false, false, CTP_TEXT,      0,   0},
  [ND_ST_CAPTION]     = {ND_SANS, 13,   18,   false, false, CTP_OVERLAY1,  6,  16},
  [ND_ST_MATH]        = {ND_MONO, 14.4, 22,   false, true,  CTP_LAVENDER, 16,  16},
};
