/* math.h — TeX math typesetting onto Cairo.
 *
 * Not a TeX port and not a browser engine: a direct implementation of the
 * layout rules in Appendix G of The TeXbook, driven by the OpenType MATH table
 * of whatever math font is installed. HarfBuzz exposes that table
 * (hb_ot_math_*) and is already linked through pangocairo, so this costs no
 * new dependency — the font supplies the axis height, the rule thicknesses,
 * the script shifts and the stretchy delimiter variants, and we supply the
 * boxes.
 *
 * The pipeline mirrors TeX's own: source -> atoms -> boxes -> glyphs.
 *
 *   math_parse.c   LaTeX text            -> nd_atom tree   (structure)
 *   math_box.c     nd_atom tree + style  -> nd_box tree    (positions)
 *   math_paint.c   nd_box tree           -> cairo glyphs   (pixels)
 *
 * Every pointer here is arena-owned and dies with the document, exactly like
 * the block tree in node.h.
 */

#ifndef NEMDOWN_MATH_H
#define NEMDOWN_MATH_H

#include <cairo.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct nd_arena;

/* TeX's eight atom classes. These exist for one reason: the space between two
 * atoms is a function of their classes, and getting that table right is most
 * of what separates typeset math from characters in a row. */
typedef enum {
  ND_MC_ORD,    /* x, 1, \alpha            */
  ND_MC_OP,     /* \sum, \int, \lim        */
  ND_MC_BIN,    /* +, -, \times            */
  ND_MC_REL,    /* =, \le, \to             */
  ND_MC_OPEN,   /* (, [, \{                */
  ND_MC_CLOSE,  /* ), ], \}                */
  ND_MC_PUNCT,  /* , ;                     */
  ND_MC_INNER,  /* \frac, a fenced group   */
} nd_math_class;

/* TeX's four styles, each with a cramped variant. Cramped matters: it is what
 * stops a superscript inside a square root from colliding with the overbar. */
typedef enum {
  ND_MS_DISPLAY, ND_MS_DISPLAY_CRAMPED,
  ND_MS_TEXT,    ND_MS_TEXT_CRAMPED,
  ND_MS_SCRIPT,  ND_MS_SCRIPT_CRAMPED,
  ND_MS_SS,      ND_MS_SS_CRAMPED,
} nd_math_style;

/* Alphabet selection. LaTeX's \mathbb, \mathcal and friends are not fonts here
 * — they are ranges in the Unicode Mathematical Alphanumeric Symbols block, so
 * `\mathbb{R}` becomes U+211D and shapes from the same face. */
typedef enum {
  ND_MA_ITALIC,   /* the default for single letters */
  ND_MA_UPRIGHT,  /* digits, \mathrm, function names */
  ND_MA_BB,       /* \mathbb */
  ND_MA_CAL,      /* \mathcal */
  ND_MA_FRAK,     /* \mathfrak */
  ND_MA_BOLD,
  ND_MA_SANS,
  ND_MA_MONO,
} nd_math_alpha;

typedef enum {
  ND_AT_RUN,      /* literal codepoints, already alphabet-mapped */
  ND_AT_LIST,     /* a group: {...} */
  ND_AT_FRAC,     /* \frac, \binom, \over */
  ND_AT_RADICAL,  /* \sqrt, \sqrt[n] */
  ND_AT_FENCED,   /* \left ... \right */
  ND_AT_MATRIX,   /* pmatrix, bmatrix, cases, align */
  ND_AT_ACCENT,   /* \hat, \vec, \bar, \tilde, \dot */
  ND_AT_SPACE,    /* \, \: \; \quad \qquad \! */
} nd_atom_kind;

typedef struct nd_atom nd_atom;

/* A horizontal list of atoms — the unit the layout rules operate on. */
typedef struct {
  nd_atom **items;
  uint32_t  n;
} nd_math_list;

struct nd_atom {
  nd_atom_kind  kind;
  nd_math_class cls;

  /* Scripts attach to any atom. */
  nd_math_list *sup, *sub;
  /* \sum in display style puts them above and below instead of beside. */
  bool          limits;

  union {
    struct {                       /* ND_AT_RUN */
      uint32_t *cps;
      uint32_t  ncp;
      bool      large_op;          /* \sum: takes the display-size variant */
    } run;
    nd_math_list *list;            /* ND_AT_LIST */
    struct {                       /* ND_AT_FRAC */
      nd_math_list *num, *den;
      bool          no_rule;       /* \binom */
      uint32_t      left, right;   /* \binom's parentheses, 0 for \frac */
    } frac;
    struct {                       /* ND_AT_RADICAL */
      nd_math_list *body, *index;
    } radical;
    struct {                       /* ND_AT_FENCED */
      nd_math_list *body;
      uint32_t      left, right;   /* 0 means \left. — an invisible fence */
    } fenced;
    struct {                       /* ND_AT_MATRIX */
      nd_math_list ***cell;        /* [row][col] */
      uint32_t        rows, cols;
      uint32_t        left, right; /* surrounding delimiters, 0 for none */
      bool            align_rel;   /* align: odd columns right, even left */
    } matrix;
    struct {                       /* ND_AT_ACCENT */
      nd_math_list *body;
      uint32_t      cp;
    } accent;
    double space_em;               /* ND_AT_SPACE */
  } u;
};

/* ---- the font, and the constants it carries ------------------------------ */

/* One per (family, size). Built lazily and cached, because loading a face and
 * reading its MATH table is far too expensive to do per formula. */
typedef struct nd_math_font nd_math_font;

/* Returns NULL when no font with a MATH table is installed, which is the one
 * case the caller must handle: math then falls back to styled source text. */
nd_math_font *nd_math_font_get(double size_px);

/* True once a usable math font has been found. Cheap after the first call. */
bool nd_math_available(void);

/* ---- boxes --------------------------------------------------------------- */

/* The laid-out result. Dimensions are TeX's: width, and height/depth measured
 * from the baseline, so a formula drops into a line of prose correctly. */
typedef struct nd_box nd_box;

typedef struct {
  double width, height, depth;
} nd_math_metrics;

/* ---- the API the rest of the engine uses --------------------------------- */

/* Parse LaTeX source into an atom list. Never fails: anything it cannot
 * understand becomes literal upright text, so a typo degrades to visible
 * characters rather than to nothing. `len` bytes, need not be NUL-terminated. */
nd_math_list *nd_math_parse(struct nd_arena *a, const char *src, size_t len);

/* Lay the list out at a size and style. Returns NULL if no math font exists. */
nd_box *nd_math_layout(struct nd_arena *a, nd_math_list *l, double size_px,
                       bool display);

void nd_math_measure(const nd_box *b, nd_math_metrics *out);

/* Paint with (x, y) at the left end of the BASELINE, matching Pango. */
void nd_math_paint(const nd_box *b, cairo_t *cr, double x, double y);

/* ---- symbol table -------------------------------------------------------- */

/* Look up a control-sequence name without its backslash: "alpha" -> U+03B1.
 * Returns 0 when unknown. Fills the atom class and whether it is a large
 * operator. */
uint32_t nd_math_symbol(const char *name, size_t len, nd_math_class *cls,
                        bool *large_op);

/* Map an ASCII letter or digit into a Mathematical Alphanumeric codepoint. */
uint32_t nd_math_alpha_map(uint32_t cp, nd_math_alpha alpha);

/* The class of a bare ASCII character: '+' is BIN, '=' is REL, and so on. */
nd_math_class nd_math_char_class(uint32_t cp);

#endif /* NEMDOWN_MATH_H */
