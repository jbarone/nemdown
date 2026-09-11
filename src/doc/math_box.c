/* math_box.c — atoms into positioned boxes, by the rules in TeXbook Appendix G.
 *
 * Every constant with a name like "shift up" or "gap" comes out of the font's
 * OpenType MATH table rather than being invented here. That is the whole point
 * of the design: STIX reports 70%/55% script scaling where Noto reports
 * 60%/50%, and a formula set with the wrong one looks subtly amateurish in a
 * way that is hard to name and easy to see.
 *
 * Boxes carry TeX's three dimensions — width, height above the baseline, and
 * depth below it — so a formula drops into a line of prose at the right place.
 */

#include "doc/math.h"

#include <hb.h>
#include <hb-ot.h>
#include <math.h>
#include <pango/pangocairo.h>
#include <stdlib.h>
#include <string.h>

#include "doc/arena.h"
#include "doc/math_box.h"
#include "util/log.h"

/* ---- the font and its constants ------------------------------------------ */

struct nd_math_font {
  double               px;
  PangoFont           *pf;
  hb_font_t           *hb;
  cairo_scaled_font_t *sf;
  double               ys;   /* divide an hb_position_t by this to get pixels */

  double axis, rule, num_up, den_dn, num_gap_min, den_gap_min;
  double sup_up, sup_up_cramped, sub_dn, sup_drop, sub_drop;
  double sup_bottom_max, sub_top_max, sup_sub_gap_min, sup_bottom_below_sub_top;
  double rad_rule, rad_gap, rad_ascender, rad_kern_before, rad_kern_after,
         rad_degree_raise;
  double acc_gap, upper_limit_gap, lower_limit_gap, upper_limit_baseline,
         lower_limit_baseline;
  double script_pct, ss_pct;
};

#define FONT_CACHE 12
static struct nd_math_font *g_cache[FONT_CACHE];
static unsigned g_ncache;
static int g_probe;   /* 0 unknown, 1 found a math font, -1 none installed */

/* In preference order. The first with a MATH table wins; without one there is
 * no typesetting to do and the caller falls back to styled source. */
static const char *FAMILIES[] = {
  "STIX Two Math", "Latin Modern Math", "XITS Math", "Asana Math",
  "TeX Gyre Termes Math", "Cambria Math", "Noto Sans Math", NULL,
};

static double konst(hb_font_t *hb, hb_ot_math_constant_t k, double ys) {
  return hb_ot_math_get_constant(hb, k) / ys;
}

static struct nd_math_font *font_build(double px) {
  PangoFontMap *fm = pango_cairo_font_map_get_default();
  PangoContext *ctx = pango_font_map_create_context(fm);
  struct nd_math_font *f = NULL;

  for (unsigned i = 0; FAMILIES[i]; i++) {
    PangoFontDescription *d = pango_font_description_new();
    pango_font_description_set_family(d, FAMILIES[i]);
    pango_font_description_set_absolute_size(d, px * PANGO_SCALE);
    PangoFont *pf = pango_font_map_load_font(fm, ctx, d);
    pango_font_description_free(d);
    if (!pf) continue;

    hb_font_t *hb = pango_font_get_hb_font(pf);
    /* Fontconfig substitutes rather than failing, so asking for a font that is
     * not installed returns something usable-looking with no MATH table. This
     * check, not the load, is what decides. */
    if (!hb || !hb_ot_math_has_data(hb_font_get_face(hb))) {
      g_object_unref(pf);
      continue;
    }

    f = calloc(1, sizeof *f);
    if (!f) break;
    f->px = px;
    f->pf = pf;
    f->hb = hb;
    f->sf = pango_cairo_font_get_scaled_font(PANGO_CAIRO_FONT(pf));

    int xs, ys;
    hb_font_get_scale(hb, &xs, &ys);
    f->ys = ys / px;

    double Y = f->ys;
    f->axis        = konst(hb, HB_OT_MATH_CONSTANT_AXIS_HEIGHT, Y);
    f->rule        = konst(hb, HB_OT_MATH_CONSTANT_FRACTION_RULE_THICKNESS, Y);
    f->num_up      = konst(hb, HB_OT_MATH_CONSTANT_FRACTION_NUMERATOR_SHIFT_UP, Y);
    f->den_dn      = konst(hb, HB_OT_MATH_CONSTANT_FRACTION_DENOMINATOR_SHIFT_DOWN, Y);
    f->num_gap_min = konst(hb, HB_OT_MATH_CONSTANT_FRACTION_NUMERATOR_GAP_MIN, Y);
    f->den_gap_min = konst(hb, HB_OT_MATH_CONSTANT_FRACTION_DENOMINATOR_GAP_MIN, Y);

    f->sup_up          = konst(hb, HB_OT_MATH_CONSTANT_SUPERSCRIPT_SHIFT_UP, Y);
    f->sup_up_cramped  = konst(hb, HB_OT_MATH_CONSTANT_SUPERSCRIPT_SHIFT_UP_CRAMPED, Y);
    f->sub_dn          = konst(hb, HB_OT_MATH_CONSTANT_SUBSCRIPT_SHIFT_DOWN, Y);
    f->sup_drop        = konst(hb, HB_OT_MATH_CONSTANT_SUPERSCRIPT_BASELINE_DROP_MAX, Y);
    f->sub_drop        = konst(hb, HB_OT_MATH_CONSTANT_SUBSCRIPT_BASELINE_DROP_MIN, Y);
    f->sup_bottom_max  = konst(hb, HB_OT_MATH_CONSTANT_SUPERSCRIPT_BOTTOM_MIN, Y);
    f->sub_top_max     = konst(hb, HB_OT_MATH_CONSTANT_SUBSCRIPT_TOP_MAX, Y);
    f->sup_sub_gap_min = konst(hb, HB_OT_MATH_CONSTANT_SUB_SUPERSCRIPT_GAP_MIN, Y);
    f->sup_bottom_below_sub_top =
        konst(hb, HB_OT_MATH_CONSTANT_SUPERSCRIPT_BOTTOM_MAX_WITH_SUBSCRIPT, Y);

    f->rad_rule      = konst(hb, HB_OT_MATH_CONSTANT_RADICAL_RULE_THICKNESS, Y);
    f->rad_gap       = konst(hb, HB_OT_MATH_CONSTANT_RADICAL_VERTICAL_GAP, Y);
    f->rad_ascender  = konst(hb, HB_OT_MATH_CONSTANT_RADICAL_EXTRA_ASCENDER, Y);
    f->rad_kern_before = konst(hb, HB_OT_MATH_CONSTANT_RADICAL_KERN_BEFORE_DEGREE, Y);
    f->rad_kern_after  = konst(hb, HB_OT_MATH_CONSTANT_RADICAL_KERN_AFTER_DEGREE, Y);
    f->rad_degree_raise =
        (double)hb_ot_math_get_constant(hb, HB_OT_MATH_CONSTANT_RADICAL_DEGREE_BOTTOM_RAISE_PERCENT);

    f->acc_gap = konst(hb, HB_OT_MATH_CONSTANT_ACCENT_BASE_HEIGHT, Y);
    f->upper_limit_gap = konst(hb, HB_OT_MATH_CONSTANT_UPPER_LIMIT_GAP_MIN, Y);
    f->lower_limit_gap = konst(hb, HB_OT_MATH_CONSTANT_LOWER_LIMIT_GAP_MIN, Y);
    f->upper_limit_baseline =
        konst(hb, HB_OT_MATH_CONSTANT_UPPER_LIMIT_BASELINE_RISE_MIN, Y);
    f->lower_limit_baseline =
        konst(hb, HB_OT_MATH_CONSTANT_LOWER_LIMIT_BASELINE_DROP_MIN, Y);

    f->script_pct =
        hb_ot_math_get_constant(hb, HB_OT_MATH_CONSTANT_SCRIPT_PERCENT_SCALE_DOWN) / 100.0;
    f->ss_pct =
        hb_ot_math_get_constant(hb, HB_OT_MATH_CONSTANT_SCRIPT_SCRIPT_PERCENT_SCALE_DOWN) / 100.0;
    if (f->script_pct <= 0.1) f->script_pct = 0.7;
    if (f->ss_pct <= 0.1)     f->ss_pct = 0.5;
    break;
  }

  g_object_unref(ctx);
  return f;
}

nd_math_font *nd_math_font_get(double px) {
  /* Sizes repeat constantly — base, script, scriptscript, and the same three
   * again for every formula in the document — so a tiny cache removes almost
   * all of the face loading. */
  for (unsigned i = 0; i < g_ncache; i++)
    if (fabs(g_cache[i]->px - px) < 0.01) return g_cache[i];

  if (g_probe < 0) return NULL;
  struct nd_math_font *f = font_build(px);
  if (!f) {
    if (g_probe == 0) {
      g_probe = -1;
      nd_warn("no font with an OpenType MATH table; math will show as source");
    }
    return NULL;
  }
  g_probe = 1;
  if (g_ncache < FONT_CACHE) g_cache[g_ncache++] = f;
  return f;
}

bool nd_math_available(void) {
  if (g_probe == 0) nd_math_font_get(16.0);
  return g_probe > 0;
}

/* ---- boxes --------------------------------------------------------------- */

struct bl {                     /* a growable box list */
  nd_box **p; double *x, *y; uint32_t n, cap;
};

static void bl_add(struct bl *l, nd_box *b, double x, double y) {
  if (l->n == l->cap) {
    l->cap = l->cap ? l->cap * 2 : 8;
    l->p = realloc(l->p, l->cap * sizeof *l->p);
    l->x = realloc(l->x, l->cap * sizeof *l->x);
    l->y = realloc(l->y, l->cap * sizeof *l->y);
    if (!l->p || !l->x || !l->y) abort();
  }
  l->p[l->n] = b; l->x[l->n] = x; l->y[l->n] = y; l->n++;
}

struct ctx {
  struct nd_arena *a;
  double base_px;
};

static nd_box *box_new(struct ctx *c) {
  return nd_arena_calloc(c->a, sizeof(nd_box));
}

static nd_box *box_commit(struct ctx *c, struct bl *l) {
  nd_box *b = box_new(c);
  if (l->n) {
    b->kids = nd_arena_memdup(c->a, l->p, l->n * sizeof *l->p);
    b->kx   = nd_arena_memdup(c->a, l->x, l->n * sizeof *l->x);
    b->ky   = nd_arena_memdup(c->a, l->y, l->n * sizeof *l->y);
    b->nkids = l->n;
  }
  free(l->p); free(l->x); free(l->y);
  return b;
}

/* Recomputes a container's own metrics from its placed children. */
static void box_fit(nd_box *b) {
  double h = 0, d = 0, w = 0;
  for (uint32_t i = 0; i < b->nkids; i++) {
    nd_box *k = b->kids[i];
    double top = b->ky[i] - k->h, bot = b->ky[i] + k->d;
    if (-top > h) h = -top;
    if (bot > d) d = bot;
    if (b->kx[i] + k->w > w) w = b->kx[i] + k->w;
  }
  b->w = w; b->h = h; b->d = d;
}

/* ---- styles -------------------------------------------------------------- */

static double style_px(struct ctx *c, nd_math_style s, struct nd_math_font *f) {
  switch (s) {
    case ND_MS_SCRIPT: case ND_MS_SCRIPT_CRAMPED: return c->base_px * f->script_pct;
    case ND_MS_SS:     case ND_MS_SS_CRAMPED:     return c->base_px * f->ss_pct;
    default:                                      return c->base_px;
  }
}

static nd_math_style sup_style(nd_math_style s) {
  switch (s) {
    case ND_MS_DISPLAY: case ND_MS_TEXT: return ND_MS_SCRIPT;
    case ND_MS_DISPLAY_CRAMPED: case ND_MS_TEXT_CRAMPED: return ND_MS_SCRIPT_CRAMPED;
    case ND_MS_SCRIPT: return ND_MS_SS;
    case ND_MS_SCRIPT_CRAMPED: return ND_MS_SS_CRAMPED;
    default: return s;
  }
}
static nd_math_style sub_style(nd_math_style s) {
  nd_math_style t = sup_style(s);
  return (nd_math_style)(t | 1);   /* the cramped variant is always the odd one */
}
static nd_math_style cramp(nd_math_style s) { return (nd_math_style)(s | 1); }
static bool is_cramped(nd_math_style s) { return (s & 1) != 0; }
static bool is_display(nd_math_style s) {
  return s == ND_MS_DISPLAY || s == ND_MS_DISPLAY_CRAMPED;
}
static nd_math_style frac_num_style(nd_math_style s) {
  if (is_display(s)) return ND_MS_TEXT;
  return sup_style(s);
}
static nd_math_style frac_den_style(nd_math_style s) {
  if (is_display(s)) return ND_MS_TEXT_CRAMPED;
  return sub_style(s);
}

/* ---- glyph boxes --------------------------------------------------------- */

static nd_box *glyph_box(struct ctx *c, struct nd_math_font *f,
                         const uint32_t *cps, uint32_t ncp) {
  nd_box *b = box_new(c);
  b->sf = f->sf;
  if (!ncp) return b;

  cairo_glyph_t *g = nd_arena_calloc(c->a, ncp * sizeof *g);
  double x = 0, h = 0, d = 0;
  uint32_t n = 0;
  for (uint32_t i = 0; i < ncp; i++) {
    hb_codepoint_t gi;
    if (!hb_font_get_nominal_glyph(f->hb, cps[i], &gi)) continue;
    g[n].index = gi;
    g[n].x = x;
    g[n].y = 0;
    hb_glyph_extents_t e;
    if (hb_font_get_glyph_extents(f->hb, gi, &e)) {
      double top = e.y_bearing / f->ys;
      double bot = (e.y_bearing + e.height) / f->ys;
      if (top > h) h = top;
      if (-bot > d) d = -bot;
    }
    x += hb_font_get_glyph_h_advance(f->hb, gi) / f->ys;
    n++;
  }
  b->g = g; b->ng = n;
  b->w = x; b->h = h; b->d = d;
  return b;
}

/* The slant correction that keeps a superscript clear of an italic stem. */
static double italic_corr(struct nd_math_font *f, nd_box *b) {
  if (!b->ng) return 0;
  return hb_ot_math_get_glyph_italics_correction(f->hb, b->g[b->ng - 1].index) / f->ys;
}

/* ---- stretchy glyphs ----------------------------------------------------- */

/* Picks the smallest variant of `cp` at least `target` tall, or assembles one
 * from parts when the font provides them and no variant is big enough. */
static nd_box *stretch_glyph(struct ctx *c, struct nd_math_font *f, uint32_t cp,
                             double target) {
  hb_codepoint_t base;
  if (!cp || !hb_font_get_nominal_glyph(f->hb, cp, &base))
    return NULL;

  hb_codepoint_t chosen = base;
  hb_ot_math_glyph_variant_t var[16];
  unsigned nv = 16;
  hb_ot_math_get_glyph_variants(f->hb, base, HB_DIRECTION_TTB, 0, &nv, var);
  for (unsigned i = 0; i < nv; i++) {
    chosen = var[i].glyph;
    if (var[i].advance / f->ys >= target) break;
  }

  /* Variants top out; a tall matrix needs a brace built from repeated parts. */
  double best = 0;
  if (nv) best = var[nv - 1].advance / f->ys;
  if (nv == 0 || best < target - 0.5) {
    unsigned np = 0;
    hb_ot_math_get_glyph_assembly(f->hb, base, HB_DIRECTION_TTB, 0, &np, NULL, NULL);
    if (np > 0) {
      hb_ot_math_glyph_part_t parts[16];
      unsigned want = np > 16 ? 16 : np;
      hb_ot_math_get_glyph_assembly(f->hb, base, HB_DIRECTION_TTB, 0, &want, parts, NULL);
      double overlap = hb_ot_math_get_min_connector_overlap(f->hb, HB_DIRECTION_TTB) / f->ys;

      /* Grow the extenders until the column is tall enough. */
      double fixed = 0, ext = 0;
      unsigned n_ext = 0;
      for (unsigned i = 0; i < want; i++) {
        double len = parts[i].full_advance / f->ys;
        if (parts[i].flags & HB_OT_MATH_GLYPH_PART_FLAG_EXTENDER) { ext += len; n_ext++; }
        else fixed += len;
      }
      unsigned reps = 1;
      if (n_ext && ext > overlap)
        while (fixed + reps * (ext - overlap) < target && reps < 64) reps++;

      struct bl l = {0};
      double y = 0;
      for (unsigned i = 0; i < want; i++) {
        unsigned times = (parts[i].flags & HB_OT_MATH_GLYPH_PART_FLAG_EXTENDER) ? reps : 1;
        for (unsigned k = 0; k < times; k++) {
          uint32_t one = 0;
          nd_box *pb = box_new(c);
          pb->sf = f->sf;
          pb->g = nd_arena_calloc(c->a, sizeof *pb->g);
          pb->g[0].index = parts[i].glyph;
          pb->ng = 1;
          pb->w = hb_font_get_glyph_h_advance(f->hb, parts[i].glyph) / f->ys;
          double len = parts[i].full_advance / f->ys;
          pb->h = 0; pb->d = len;
          bl_add(&l, pb, 0, y);
          y += len - overlap;
          (void)one;
        }
      }
      nd_box *asm_box = box_commit(c, &l);
      box_fit(asm_box);
      return asm_box;
    }
  }

  uint32_t one = 0;
  nd_box *b = box_new(c);
  b->sf = f->sf;
  b->g = nd_arena_calloc(c->a, sizeof *b->g);
  b->g[0].index = chosen;
  b->ng = 1;
  b->w = hb_font_get_glyph_h_advance(f->hb, chosen) / f->ys;
  hb_glyph_extents_t e;
  if (hb_font_get_glyph_extents(f->hb, chosen, &e)) {
    b->h = e.y_bearing / f->ys;
    b->d = -(e.y_bearing + e.height) / f->ys;
  }
  (void)one;
  return b;
}

/* Centres a delimiter on the maths axis, which is where TeX puts them. */
static nd_box *axis_delim(struct ctx *c, struct nd_math_font *f, uint32_t cp,
                          double half) {
  if (!cp) return NULL;
  nd_box *g = stretch_glyph(c, f, cp, half * 2);
  if (!g) return NULL;

  /* The glyph's visual middle sits (h-d)/2 above its baseline; shifting the
   * baseline DOWN by that much minus the axis height lands the middle exactly
   * on the axis. Adding the axis instead drops the delimiter by 2*axis, which
   * reads as parentheses sagging below the fraction they enclose. */
  double centre = (g->h - g->d) / 2;
  double shift = centre - f->axis;
  struct bl l = {0};
  bl_add(&l, g, 0, shift);
  nd_box *out = box_commit(c, &l);
  box_fit(out);
  return out;
}

/* ---- inter-atom spacing -------------------------------------------------- */

/* TeXbook Chapter 18's table, in eighteenths of an em. A negative entry means
 * "only outside script styles", which is why cramped formulas set tighter. */
static const signed char SPACE[8][8] = {
  /*         Ord  Op  Bin  Rel Open Close Punct Inner */
  /*Ord  */ {  0,  1,  -2,  -3,  0,   0,    0,   -1 },
  /*Op   */ {  1,  1,   0,  -3,  0,   0,    0,   -1 },
  /*Bin  */ { -2, -2,   0,   0, -2,   0,    0,   -2 },
  /*Rel  */ { -3, -3,   0,   0, -3,   0,    0,   -3 },
  /*Open */ {  0,  0,   0,   0,  0,   0,    0,    0 },
  /*Close*/ {  0,  1,  -2,  -3,  0,   0,    0,   -1 },
  /*Punct*/ { -1, -1,   0,  -1, -1,  -1,   -1,   -1 },
  /*Inner*/ { -1,  1,  -2,  -3, -1,   0,   -1,   -1 },
};

static double space_between(nd_math_class l, nd_math_class r, nd_math_style s,
                            double em) {
  signed char v = SPACE[l][r];
  if (v == 0) return 0;
  if (v < 0) {
    if (s >= ND_MS_SCRIPT) return 0;   /* suppressed in script styles */
    v = (signed char)-v;
  }
  return em * v / 18.0;
}

static nd_box *layout_list(struct ctx *c, nd_math_list *l, nd_math_style s);

/* ---- scripts ------------------------------------------------------------- */

static nd_box *attach_scripts(struct ctx *c, struct nd_math_font *f, nd_box *nucleus,
                              nd_atom *a, nd_math_style s, double ic) {
  if (!a->sup && !a->sub) return nucleus;

  nd_box *sup = a->sup ? layout_list(c, a->sup, sup_style(s)) : NULL;
  nd_box *sub = a->sub ? layout_list(c, a->sub, sub_style(s)) : NULL;

  struct bl l = {0};
  bl_add(&l, nucleus, 0, 0);
  double x = nucleus->w;

  if (sup && !sub) {
    double up = is_cramped(s) ? f->sup_up_cramped : f->sup_up;
    if (up < nucleus->h - f->sup_drop) up = nucleus->h - f->sup_drop;
    if (up < sup->d + f->sup_bottom_max) up = sup->d + f->sup_bottom_max;
    bl_add(&l, sup, x + ic, -up);
  } else if (sub && !sup) {
    double dn = f->sub_dn;
    if (dn < nucleus->d + f->sub_drop) dn = nucleus->d + f->sub_drop;
    if (dn < sub->h - f->sub_top_max) dn = sub->h - f->sub_top_max;
    bl_add(&l, sub, x, dn);
  } else if (sup && sub) {
    double up = is_cramped(s) ? f->sup_up_cramped : f->sup_up;
    if (up < nucleus->h - f->sup_drop) up = nucleus->h - f->sup_drop;
    if (up < sup->d + f->sup_bottom_max) up = sup->d + f->sup_bottom_max;
    double dn = f->sub_dn;
    if (dn < nucleus->d + f->sub_drop) dn = nucleus->d + f->sub_drop;

    /* Keep them from touching, then make sure the superscript is not so low
     * that the pair looks like one blob. */
    double gap = (up - sup->d) - (sub->h - dn);
    if (gap < f->sup_sub_gap_min) {
      dn += f->sup_sub_gap_min - gap;
      double bottom = f->sup_bottom_below_sub_top - (up - sup->d);
      if (bottom > 0) { up += bottom; dn -= bottom; }
    }
    /* The superscript clears the italic slant; the subscript sits square. */
    bl_add(&l, sup, x + ic, -up);
    bl_add(&l, sub, x, dn);
  }

  nd_box *out = box_commit(c, &l);
  box_fit(out);
  return out;
}

/* \sum and \lim in display style stack their scripts instead. */
static nd_box *attach_limits(struct ctx *c, struct nd_math_font *f, nd_box *nucleus,
                             nd_atom *a, nd_math_style s) {
  nd_box *up = a->sup ? layout_list(c, a->sup, sup_style(s)) : NULL;
  nd_box *dn = a->sub ? layout_list(c, a->sub, sub_style(s)) : NULL;
  if (!up && !dn) return nucleus;

  double w = nucleus->w;
  if (up && up->w > w) w = up->w;
  if (dn && dn->w > w) w = dn->w;

  struct bl l = {0};
  bl_add(&l, nucleus, (w - nucleus->w) / 2, 0);
  if (up) {
    double rise = f->upper_limit_baseline;
    if (rise < nucleus->h + f->upper_limit_gap + up->d)
      rise = nucleus->h + f->upper_limit_gap + up->d;
    bl_add(&l, up, (w - up->w) / 2, -rise);
  }
  if (dn) {
    double drop = f->lower_limit_baseline;
    if (drop < nucleus->d + f->lower_limit_gap + dn->h)
      drop = nucleus->d + f->lower_limit_gap + dn->h;
    bl_add(&l, dn, (w - dn->w) / 2, drop);
  }
  nd_box *out = box_commit(c, &l);
  box_fit(out);
  out->w = w;
  return out;
}

/* ---- composites ---------------------------------------------------------- */

static nd_box *layout_frac(struct ctx *c, struct nd_math_font *f, nd_atom *a,
                           nd_math_style s) {
  nd_box *num = layout_list(c, a->u.frac.num, frac_num_style(s));
  nd_box *den = layout_list(c, a->u.frac.den, frac_den_style(s));
  double rule = a->u.frac.no_rule ? 0 : f->rule;

  double w = num->w > den->w ? num->w : den->w;
  double up = f->num_up, dn = f->den_dn;

  /* Push both clear of the rule by at least the font's minimum gaps. */
  double num_bottom = up - num->d;
  double rule_top = f->axis + rule / 2;
  if (num_bottom < rule_top + f->num_gap_min)
    up += (rule_top + f->num_gap_min) - num_bottom;

  double den_top = den->h - dn;
  double rule_bot = f->axis - rule / 2;
  if (den_top > rule_bot - f->den_gap_min)
    dn += den_top - (rule_bot - f->den_gap_min);

  struct bl l = {0};
  bl_add(&l, num, (w - num->w) / 2, -up);
  bl_add(&l, den, (w - den->w) / 2, dn);
  if (rule > 0) {
    nd_box *r = box_new(c);
    r->is_rule = true;
    r->w = w; r->h = rule / 2; r->d = rule / 2;
    bl_add(&l, r, 0, -f->axis);
  }
  nd_box *body = box_commit(c, &l);
  box_fit(body);
  body->w = w;

  if (!a->u.frac.left && !a->u.frac.right) return body;

  /* \binom: parentheses sized to the stack. */
  double half = (body->h + body->d) / 2 + f->axis;
  nd_box *lp = axis_delim(c, f, a->u.frac.left, half);
  nd_box *rp = axis_delim(c, f, a->u.frac.right, half);
  struct bl o = {0};
  double x = 0;
  if (lp) { bl_add(&o, lp, x, 0); x += lp->w; }
  bl_add(&o, body, x, 0); x += body->w;
  if (rp) { bl_add(&o, rp, x, 0); x += rp->w; }
  nd_box *out = box_commit(c, &o);
  box_fit(out);
  out->w = x;
  return out;
}

static nd_box *layout_radical(struct ctx *c, struct nd_math_font *f, nd_atom *a,
                              nd_math_style s) {
  nd_box *body = layout_list(c, a->u.radical.body, cramp(s));

  double need = body->h + body->d + f->rad_gap + f->rad_rule;
  nd_box *sign = stretch_glyph(c, f, 0x221A, need);
  if (!sign) return body;

  /* Sit the radical so its bar clears the body by the font's vertical gap. */
  double bar_y = -(body->h + f->rad_gap + f->rad_rule / 2);
  double sign_shift = -(bar_y) - sign->h + (sign->h + sign->d) - (sign->h + sign->d);
  sign_shift = -bar_y - sign->h;   /* align the sign's top with the bar */

  struct bl l = {0};
  double x = 0;

  nd_math_list *idx = a->u.radical.index;
  nd_box *ib = NULL;
  if (idx) {
    ib = layout_list(c, idx, ND_MS_SS);
    x += f->rad_kern_before;
  }

  double sign_x = x;
  bl_add(&l, sign, sign_x, sign_shift);

  if (ib) {
    /* The degree rides high on the radical's left arm. */
    double raise = (sign->h + sign->d) * (f->rad_degree_raise / 100.0);
    bl_add(&l, ib, x - f->rad_kern_before, sign_shift - (sign->h + sign->d) + raise - ib->d);
  }

  double body_x = sign_x + sign->w;
  bl_add(&l, body, body_x, 0);

  nd_box *bar = box_new(c);
  bar->is_rule = true;
  bar->w = body->w;
  bar->h = f->rad_rule / 2;
  bar->d = f->rad_rule / 2;
  bl_add(&l, bar, body_x, bar_y);

  nd_box *out = box_commit(c, &l);
  box_fit(out);
  out->h += f->rad_ascender;
  return out;
}

static nd_box *layout_fenced(struct ctx *c, struct nd_math_font *f, nd_atom *a,
                             nd_math_style s) {
  nd_box *body = layout_list(c, a->u.fenced.body, s);

  /* TeX sizes a \left..\right pair from how far the content reaches from the
   * axis, not from its total height, so `(x)` and `(x^2)` differ but stay
   * symmetric about the axis. */
  double above = body->h - f->axis, below = body->d + f->axis;
  double reach = above > below ? above : below;
  double half = reach * 1.0 + f->px * 0.1;

  nd_box *lp = axis_delim(c, f, a->u.fenced.left, half);
  nd_box *rp = axis_delim(c, f, a->u.fenced.right, half);

  struct bl l = {0};
  double x = 0;
  if (lp) { bl_add(&l, lp, x, 0); x += lp->w; }
  bl_add(&l, body, x, 0); x += body->w;
  if (rp) { bl_add(&l, rp, x, 0); x += rp->w; }
  nd_box *out = box_commit(c, &l);
  box_fit(out);
  out->w = x;
  return out;
}

static nd_box *layout_matrix(struct ctx *c, struct nd_math_font *f, nd_atom *a,
                             nd_math_style s) {
  uint32_t R = a->u.matrix.rows, C = a->u.matrix.cols;
  nd_math_style cell_style = is_display(s) ? ND_MS_TEXT : s;

  nd_box **cells = calloc((size_t)R * C, sizeof *cells);
  double *colw = calloc(C, sizeof *colw);
  double *rowh = calloc(R, sizeof *rowh), *rowd = calloc(R, sizeof *rowd);
  if (!cells || !colw || !rowh || !rowd) abort();

  for (uint32_t r = 0; r < R; r++)
    for (uint32_t cc = 0; cc < C; cc++) {
      nd_math_list *ml = a->u.matrix.cell[r][cc];
      nd_box *b = ml ? layout_list(c, ml, cell_style) : box_new(c);
      cells[r * C + cc] = b;
      if (b->w > colw[cc]) colw[cc] = b->w;
      if (b->h > rowh[r]) rowh[r] = b->h;
      if (b->d > rowd[r]) rowd[r] = b->d;
    }

  /* An align environment's columns meet AT the alignment point -- the relation
   * is the first thing in the even column -- so a gap there would push the
   * `=` away from what it relates. A matrix's columns do want air. */
  double colgap = a->u.matrix.align_rel ? 0.0 : f->px * 0.6;
  double rowgap = f->px * 0.35;

  double total_h = 0;
  for (uint32_t r = 0; r < R; r++) total_h += rowh[r] + rowd[r];
  total_h += rowgap * (R > 0 ? R - 1 : 0);

  struct bl l = {0};
  double x0 = 0;

  double half = total_h / 2 + f->px * 0.1;
  nd_box *lp = axis_delim(c, f, a->u.matrix.left, half);
  if (lp) { bl_add(&l, lp, x0, 0); x0 += lp->w + f->px * 0.1; }

  /* Rows are centred on the axis as a block. */
  double y = -(total_h / 2) + f->axis;
  for (uint32_t r = 0; r < R; r++) {
    y += rowh[r];
    double x = x0;
    for (uint32_t cc = 0; cc < C; cc++) {
      nd_box *b = cells[r * C + cc];
      double off;
      if (a->u.matrix.align_rel)
        off = (cc % 2 == 0) ? colw[cc] - b->w : 0;   /* right, then left */
      else
        off = (colw[cc] - b->w) / 2;                 /* centred */
      bl_add(&l, b, x + off, y);
      x += colw[cc] + colgap;
    }
    y += rowd[r] + rowgap;
  }

  double width = 0;
  for (uint32_t cc = 0; cc < C; cc++) width += colw[cc];
  width += colgap * (C > 0 ? C - 1 : 0);

  double xr = x0 + width;
  nd_box *rp = axis_delim(c, f, a->u.matrix.right, half);
  if (rp) { xr += f->px * 0.1; bl_add(&l, rp, xr, 0); xr += rp->w; }

  free(cells); free(colw); free(rowh); free(rowd);

  nd_box *out = box_commit(c, &l);
  box_fit(out);
  out->w = xr;
  return out;
}

static nd_box *layout_accent(struct ctx *c, struct nd_math_font *f, nd_atom *a,
                             nd_math_style s) {
  nd_box *body = layout_list(c, a->u.accent.body, cramp(s));

  hb_codepoint_t gi;
  if (!hb_font_get_nominal_glyph(f->hb, a->u.accent.cp, &gi)) return body;

  /* A combining mark has zero advance and is designed to be stacked by the
   * shaper onto a preceding base. Drawn standalone it lands at the origin, so
   * its INK box is what has to be positioned -- using the box width puts the
   * mark at x=0 and its own height puts it far too high. */
  hb_glyph_extents_t e;
  if (!hb_font_get_glyph_extents(f->hb, gi, &e)) return body;
  double ink_left = e.x_bearing / f->ys;
  double ink_w    = e.width / f->ys;
  double ink_top  = e.y_bearing / f->ys;
  double ink_bot  = (e.y_bearing + e.height) / f->ys;

  nd_box *acc = box_new(c);
  acc->sf = f->sf;
  acc->g = nd_arena_calloc(c->a, sizeof *acc->g);
  acc->g[0].index = gi;
  acc->ng = 1;
  acc->w = 0;
  acc->h = ink_top;
  acc->d = -ink_bot;

  /* Horizontally: centre the mark's ink over the base's accent attachment
   * point when the font gives one, otherwise over the base's middle. */
  double attach = body->w / 2;
  if (body->ng == 1) {
    hb_position_t ta = hb_ot_math_get_glyph_top_accent_attachment(f->hb, body->g[0].index);
    if (ta) attach = ta / f->ys;
  }
  double kx = attach - (ink_left + ink_w / 2);

  /* Vertically: sit the mark's ink bottom on the top of the base. ky is a
   * baseline shift measured downwards, and the ink bottom sits ink_bot above
   * the mark's own baseline, so this lands it exactly on the base's height. */
  double ky = ink_bot - body->h;

  struct bl l = {0};
  bl_add(&l, body, 0, 0);
  bl_add(&l, acc, kx, ky);
  nd_box *out = box_commit(c, &l);
  box_fit(out);
  out->w = body->w;   /* an accent must not widen the atom it decorates */
  return out;
}

/* ---- the list ------------------------------------------------------------ */

static nd_box *layout_atom(struct ctx *c, struct nd_math_font *f, nd_atom *a,
                           nd_math_style s, double *ic_out) {
  *ic_out = 0;
  switch (a->kind) {
    case ND_AT_RUN: {
      struct nd_math_font *use = f;
      if (a->u.run.large_op && is_display(s)) {
        /* A display \sum is drawn from the font's larger variant, not scaled. */
        nd_box *big = stretch_glyph(c, f, a->u.run.cps[0], f->px * 1.6);
        if (big) {
          /* Centre it on the axis like TeX does. */
          double centre = (big->h - big->d) / 2;
          struct bl l = {0};
          bl_add(&l, big, 0, centre - f->axis);
          nd_box *o = box_commit(c, &l);
          box_fit(o);
          return o;
        }
      }
      nd_box *b = glyph_box(c, use, a->u.run.cps, a->u.run.ncp);
      *ic_out = italic_corr(use, b);
      return b;
    }
    case ND_AT_LIST:    return layout_list(c, a->u.list, s);
    case ND_AT_FRAC:    return layout_frac(c, f, a, s);
    case ND_AT_RADICAL: return layout_radical(c, f, a, s);
    case ND_AT_FENCED:  return layout_fenced(c, f, a, s);
    case ND_AT_MATRIX:  return layout_matrix(c, f, a, s);
    case ND_AT_ACCENT:  return layout_accent(c, f, a, s);
    case ND_AT_SPACE: {
      nd_box *b = box_new(c);
      b->w = a->u.space_em * f->px;
      return b;
    }
  }
  return box_new(c);
}

static nd_box *layout_list(struct ctx *c, nd_math_list *ml, nd_math_style s) {
  /* Two lookups, deliberately: the script percentages live in the BASE font,
   * and only then do we know which size this style wants. Both are cached. */
  struct nd_math_font *base = nd_math_font_get(c->base_px);
  if (!base) return NULL;
  struct nd_math_font *f = nd_math_font_get(style_px(c, s, base));
  if (!f) return NULL;

  struct bl l = {0};
  double x = 0;
  nd_math_class prev = (nd_math_class)-1;

  for (uint32_t i = 0; ml && i < ml->n; i++) {
    nd_atom *a = ml->items[i];

    /* TeXbook rule 5: a Bin that cannot be binary is really an Ord. `-b` opens
     * a formula, `2^{-n}` opens a script, `(-x)` follows an Open -- all of
     * them set tight, not with the space around a true binary operator. */
    nd_math_class cls = a->cls;
    if (cls == ND_MC_BIN) {
      if (prev == (nd_math_class)-1 || prev == ND_MC_BIN || prev == ND_MC_REL ||
          prev == ND_MC_OPEN || prev == ND_MC_OP || prev == ND_MC_PUNCT)
        cls = ND_MC_ORD;
    }

    if (prev != (nd_math_class)-1)
      x += space_between(prev, cls, s, f->px);

    double ic = 0;
    nd_box *nuc = layout_atom(c, f, a, s, &ic);
    if (!nuc) continue;

    bool stack = a->limits && is_display(s) && (a->sup || a->sub);
    nd_box *withscripts = stack ? attach_limits(c, f, nuc, a, s)
                                : attach_scripts(c, f, nuc, a, s, ic);

    bl_add(&l, withscripts, x, 0);
    x += withscripts->w;
    prev = cls;
  }

  nd_box *out = box_commit(c, &l);
  box_fit(out);
  out->w = x;
  return out;
}

/* ---- entry points -------------------------------------------------------- */

nd_box *nd_math_layout(struct nd_arena *a, nd_math_list *l, double size_px,
                       bool display) {
  if (!nd_math_font_get(size_px)) return NULL;
  struct ctx c = {.a = a, .base_px = size_px};
  return layout_list(&c, l, display ? ND_MS_DISPLAY : ND_MS_TEXT);
}

void nd_math_measure(const nd_box *b, nd_math_metrics *out) {
  if (!b) { out->width = out->height = out->depth = 0; return; }
  out->width = b->w;
  out->height = b->h;
  out->depth = b->d;
}
