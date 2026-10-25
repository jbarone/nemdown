/* mermaid_pele.c — every mermaid dialect, via pele in QuickJS.
 *
 * pele (github.com/obsidianmd/pele) is Obsidian's mermaid renderer: pure
 * TypeScript, synchronous, no DOM, deterministic, with text measurement
 * delegated to a callback. That last property is what makes embedding it
 * sound rather than approximate: the callback here is Pango, the same shaper
 * librsvg draws with, so what pele measured is what gets drawn.
 *
 * It runs as JavaScript rather than as a port because it moves too fast to
 * port: nineteen releases in its first three weeks. The bundle is linked in
 * by pele_bundle.c, so following upstream is replacing one vendored file.
 * quickjs-ng is linked, not dlopened as graphviz is: it is 1.4MB with no
 * dependency beyond glibc, and JSValue crosses the API by value, so the
 * header's struct layout is part of the ABI either way and dlopen would buy
 * nothing.
 *
 * Three things pele cannot know and this file supplies:
 *
 *  - Text widths. Pango rounds glyph positions by default, which librsvg does
 *    not, so the measuring context turns rounding off. With it on, Hack at
 *    14px measures 8px a glyph and draws 8.43: a 32-glyph class member ran
 *    out through its border.
 *  - Colours. pele themes entirely with CSS custom properties, which librsvg
 *    does not implement — unresolved, every fill is black and every stroke is
 *    gone. All of them are defined once, on the root <svg>, so a flat table
 *    resolves them exactly; the public --pele-* ones are seeded with Mocha.
 *  - Bounds. The fence is untrusted input to a JS engine. The engine has no
 *    std or os module, so the bundle can compute but not reach anything; the
 *    heap is capped and a watchdog interrupts a render that overruns. Both
 *    leave the context usable, so one hostile fence costs itself, not the
 *    rest of the document.
 */

#include <math.h>
#include <quickjs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "doc/mermaid_int.h"
#include "util/log.h"

#define PELE_HEAP        (64u << 20)
#define PELE_BUDGET_MS   500.0
#define PELE_FONT_PX     14.0
#define PELE_MAX_SVG     (4u << 20)   /* characters pele may emit */
/* pele nests two deep (--_bg -> --pele-bg -> a colour); the bound is what keeps
 * a variable defined in terms of itself from recursing forever. */
#define PELE_VAR_DEPTH   4

static JSRuntime *rt;
static JSContext *jctx;
static JSValue render_fn;
static bool tried, ready;

static PangoContext *mctx;      /* unrounded; see the header */
static PangoLayout *mlay;
static double deadline;

static double now_ms(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static int watchdog(JSRuntime *r, void *op) {
  (void)r; (void)op;
  return deadline > 0 && now_ms() > deadline;
}

/* ---- the measurer ------------------------------------------------------ */

/* pele's TextMeasurer style bits (src/text/measurer.ts). */
enum { PS_BOLD = 1, PS_ITALIC = 2, PS_MONO = 4, PS_SUB = 8, PS_SUP = 16 };

static JSValue js_measure(JSContext *ctx, JSValueConst this_val, int argc,
                          JSValueConst *argv) {
  (void)this_val;
  if (argc < 2) return JS_NewFloat64(ctx, 0);
  size_t len;
  const char *s = JS_ToCStringLen(ctx, &len, argv[0]);
  if (!s) return JS_EXCEPTION;
  double size = PELE_FONT_PX;
  int32_t style = 0;
  JS_ToFloat64(ctx, &size, argv[1]);
  if (argc > 2) JS_ToInt32(ctx, &style, argv[2]);
  if (!(size > 0.5) || !(size < 500)) size = PELE_FONT_PX;
  if (style & (PS_SUB | PS_SUP)) size /= 1.2;   /* pele's SMALLER */

  PangoFontDescription *fd = pango_font_description_new();
  pango_font_description_set_family(fd, ND_MM_FONT);
  pango_font_description_set_absolute_size(fd, size * PANGO_SCALE);
  if (style & PS_BOLD) pango_font_description_set_weight(fd, PANGO_WEIGHT_BOLD);
  if (style & PS_ITALIC) pango_font_description_set_style(fd, PANGO_STYLE_ITALIC);
  pango_layout_set_font_description(mlay, fd);
  pango_font_description_free(fd);
  pango_layout_set_text(mlay, s, len > ND_MM_MAX_LINE ? (int)ND_MM_MAX_LINE
                                                       : (int)len);
  JS_FreeCString(ctx, s);
  PangoRectangle log;
  pango_layout_get_extents(mlay, NULL, &log);
  return JS_NewFloat64(ctx, (double)log.width / PANGO_SCALE);
}

/* ---- setup ------------------------------------------------------------- */

/* Always consumes the pending exception, which would otherwise be reported
 * against the next call. A fence that does not parse is a debug message, not a
 * warning: it is laid out again on every resize, and its source is on screen
 * already. */
static void drop_exception(const char *what, bool loud) {
  JSValue e = JS_GetException(jctx);
  const char *s = JS_ToCString(jctx, e);
  if (loud) nd_warn("pele: %s: %s", what, s ? s : "(no message)");
  else nd_dbg("pele: %s: %s", what, s ? s : "(no message)");
  JS_FreeCString(jctx, s);
  JS_FreeValue(jctx, e);
}

static const char shim[] =
  "globalThis.__nd_render = (src, maxw, px, font, maxe, maxo) => {"
  "  const r = pele.render(src, {"
  "    measurer: { width: __nd_measure }, fontFamily: font,"
  "    fontFamilyMono: font, fontSize: px, maxWidth: maxw,"
  "    maxEdges: maxe, outputLimit: maxo, links: false, images: false,"
  "    now: 0 });"
  "  return [r.svg, r.width, r.height];"
  "};";

static bool setup(PangoContext *pctx) {
  /* Its own context off the document's font map, so rounding can be turned
   * off here without moving a single glyph in the document. */
  mctx = pango_font_map_create_context(pango_context_get_font_map(pctx));
  pango_cairo_context_set_font_options(mctx,
                                       pango_cairo_context_get_font_options(pctx));
  pango_context_set_round_glyph_positions(mctx, FALSE);
  mlay = pango_layout_new(mctx);

  rt = JS_NewRuntime();
  if (!rt) return false;
  JS_SetMemoryLimit(rt, PELE_HEAP);
  JS_SetMaxStackSize(rt, 2u << 20);
  JS_SetInterruptHandler(rt, watchdog, NULL);
  jctx = JS_NewContext(rt);
  if (!jctx) return false;

  double t0 = now_ms();
  deadline = t0 + 5000;
  JSValue r = JS_Eval(jctx, nd_pele_js, (size_t)(nd_pele_js_end - nd_pele_js),
                      "pele.global.js", JS_EVAL_TYPE_GLOBAL);
  if (JS_IsException(r)) { drop_exception("loading the bundle", true); deadline = 0; return false; }
  JS_FreeValue(jctx, r);

  JSValue g = JS_GetGlobalObject(jctx);
  JS_SetPropertyStr(jctx, g, "__nd_measure",
                    JS_NewCFunction(jctx, js_measure, "__nd_measure", 3));
  r = JS_Eval(jctx, shim, sizeof shim - 1, "shim", JS_EVAL_TYPE_GLOBAL);
  deadline = 0;
  if (JS_IsException(r)) { drop_exception("shim", true); JS_FreeValue(jctx, g); return false; }
  JS_FreeValue(jctx, r);
  render_fn = JS_GetPropertyStr(jctx, g, "__nd_render");
  JS_FreeValue(jctx, g);
  nd_dbg("pele: bundle loaded in %.1f ms", now_ms() - t0);
  return JS_IsFunction(jctx, render_fn);
}

/* ---- var() ------------------------------------------------------------- */

struct cvar { const char *name; size_t nl; const char *val; size_t vl; };

static const char *const theme[][2] = {
  {"--pele-bg", ND_MM_BG},          {"--pele-fg", ND_MM_TEXT},
  {"--pele-muted", ND_MM_DIM},      {"--pele-line", "#9399b2"},
  {"--pele-surface", ND_MM_FILL},   {"--pele-surface-alt", ND_MM_FILL2},
  {"--pele-border", ND_MM_BORDER},  {"--pele-accent", ND_MM_ACCENT},
  {"--pele-critical", "#f38ba8"},
  {"--pele-series-1", "#94e2d5"},   {"--pele-series-2", "#cba6f7"},
  {"--pele-series-3", "#fab387"},   {"--pele-series-4", "#89b4fa"},
  {"--pele-series-5", "#a6e3a1"},   {"--pele-series-6", "#f5c2e7"},
  {"--pele-series-7", "#f9e2af"},   {"--pele-series-8", "#89dceb"},
  {"--pele-series-9", "#eba0ac"},   {"--pele-series-10", "#b4befe"},
  {"--pele-series-11", "#f2cdcd"},  {"--pele-series-12", "#74c7ec"},
};

#define MAX_VARS 96
struct vtab { struct cvar v[MAX_VARS]; int n; };

/* First definition wins, so the theme seeded first beats pele's fallbacks. */
static void vt_set(struct vtab *t, const char *name, size_t nl,
                   const char *val, size_t vl) {
  for (int i = 0; i < t->n; i++)
    if (t->v[i].nl == nl && !memcmp(t->v[i].name, name, nl)) return;
  if (t->n < MAX_VARS) t->v[t->n++] = (struct cvar){name, nl, val, vl};
}

static const struct cvar *vt_get(const struct vtab *t, const char *name,
                                 size_t nl) {
  for (int i = 0; i < t->n; i++)
    if (t->v[i].nl == nl && !memcmp(t->v[i].name, name, nl)) return &t->v[i];
  return NULL;
}

static void resolve(const struct vtab *t, nd_buf *out, const char *s, size_t n,
                    int depth) {
  for (size_t i = 0; i < n; ) {
    size_t a = i + 4, b = a + 2;
    if (depth < PELE_VAR_DEPTH && n - i > 6 && !memcmp(s + i, "var(--", 6))
      while (b < n && (g_ascii_isalnum(s[b]) || s[b] == '-' || s[b] == '_')) b++;
    /* Only a well-formed reference -- an identifier, then `)` or `,` -- is
     * resolved. Anything else is copied as written, as CSS would leave it. */
    if (b > a + 2 && b < n && (s[b] == ')' || s[b] == ',')) {
      size_t j = b, nest = 0;
      for (; j < n; j++) {
        if (s[j] == '(') nest++;
        else if (s[j] == ')') { if (!nest) break; nest--; }
      }
      if (j >= n) { nd_buf_add(out, s + i, n - i); return; }
      const struct cvar *v = vt_get(t, s + a, b - a);
      if (v) resolve(t, out, v->val, v->vl, depth + 1);
      else if (b < j) resolve(t, out, s + b + 1, j - b - 1, depth + 1);
      i = j + 1;
    } else {
      size_t k = i + 1;
      while (k < n && s[k] != 'v') k++;
      nd_buf_add(out, s + i, k - i);
      i = k;
    }
  }
}

/* Substitutes inside quoted attribute values only, so a label that reads
 * "var(--x)" is left as the author wrote it. Every replacement comes from the
 * theme or from the root's own style attribute, neither of which can hold a
 * raw quote, so quoting is preserved by construction -- and check_mermaid
 * asserts it anyway, over everything pele draws from mutated fences. */
char *nd_mm_pele_theme(const char *svg, size_t len) {
  struct vtab t = {.n = 0};
  for (size_t i = 0; i < sizeof theme / sizeof theme[0]; i++)
    vt_set(&t, theme[i][0], strlen(theme[i][0]), theme[i][1],
           strlen(theme[i][1]));
  const char *gt = memchr(svg, '>', len);
  const char *st = gt ? memmem(svg, (size_t)(gt - svg), " style=\"", 8) : NULL;
  if (st) {
    const char *p = st + 8, *e = memchr(p, '"', (size_t)(gt - p));
    while (e && p < e) {
      const char *semi = memchr(p, ';', (size_t)(e - p));
      if (!semi) semi = e;
      const char *colon = memchr(p, ':', (size_t)(semi - p));
      /* A value holding markup is ignored rather than trusted: pele escapes
       * it, but the guarantee that this pass never changes the markup should
       * not rest on that. */
      if (colon && semi - p > 2 && p[0] == '-' && p[1] == '-' &&
          !memchr(colon, '<', (size_t)(semi - colon)) &&
          !memchr(colon, '>', (size_t)(semi - colon)))
        vt_set(&t, p, (size_t)(colon - p), colon + 1, (size_t)(semi - colon - 1));
      p = semi + 1;
    }
  }

  /* Allocated up front, so empty input is an empty string and NULL keeps
   * meaning only that allocation failed. */
  nd_buf out = {.ok = true};
  nd_buf_add(&out, "", 0);
  bool tag = false;
  for (size_t i = 0; i < len; i++) {
    char c = svg[i];
    if (tag && c == '"') {
      const char *e = memchr(svg + i + 1, '"', len - i - 1);
      if (!e) { nd_buf_add(&out, svg + i, len - i); break; }
      nd_buf_add(&out, "\"", 1);
      resolve(&t, &out, svg + i + 1, (size_t)(e - svg) - i - 1, 0);
      nd_buf_add(&out, "\"", 1);
      i = (size_t)(e - svg);
      continue;
    }
    if (c == '<') tag = true;
    else if (c == '>') tag = false;
    nd_buf_add(&out, &c, 1);
  }
  return nd_buf_take(&out);
}

/* ---- render ------------------------------------------------------------ */

bool nd_mm_pele_available(PangoContext *pctx) {
  if (!tried) { tried = true; ready = setup(pctx); }
  return ready;
}

char *nd_mm_pele_svg(PangoContext *pctx, const char *src, uint32_t len,
                     double max_w, double *w, double *h) {
  *w = *h = 0;
  if (!nd_mm_pele_available(pctx)) return NULL;

  JSValue args[6] = {
    JS_NewStringLen(jctx, src, len),
    JS_NewFloat64(jctx, max_w),
    JS_NewFloat64(jctx, PELE_FONT_PX),
    JS_NewString(jctx, ND_MM_FONT),
    JS_NewFloat64(jctx, ND_MM_MAX_EDGES),
    JS_NewFloat64(jctx, PELE_MAX_SVG),
  };
  double t0 = now_ms();
  deadline = t0 + PELE_BUDGET_MS;
  JSValue r = JS_Call(jctx, render_fn, JS_UNDEFINED, 6, args);
  deadline = 0;
  for (int i = 0; i < 6; i++) JS_FreeValue(jctx, args[i]);
  if (JS_IsException(r)) { drop_exception("render", false); return NULL; }

  JSValue sv = JS_GetPropertyUint32(jctx, r, 0);
  JSValue wv = JS_GetPropertyUint32(jctx, r, 1);
  JSValue hv = JS_GetPropertyUint32(jctx, r, 2);
  JS_FreeValue(jctx, r);
  size_t slen = 0;
  const char *svg = JS_ToCStringLen(jctx, &slen, sv);
  if (!svg) drop_exception("render result", false);
  JS_ToFloat64(jctx, w, wv);
  JS_ToFloat64(jctx, h, hv);
  JS_FreeValue(jctx, sv);
  JS_FreeValue(jctx, wv);
  JS_FreeValue(jctx, hv);
  nd_dbg("pele: %.1f ms, %zu bytes", now_ms() - t0, slen);

  char *out = NULL;
  if (svg && *w > 0 && *h > 0 && *w <= ND_MM_MAX_DIM && *h <= ND_MM_MAX_DIM &&
      (out = malloc(slen + 1))) {
    memcpy(out, svg, slen);
    out[slen] = 0;
  }
  JS_FreeCString(jctx, svg);
  return out;
}

RsvgHandle *nd_mm_pele_render(PangoContext *pctx, const char *src,
                              uint32_t len, double max_w, double *w,
                              double *h) {
  char *svg = nd_mm_pele_svg(pctx, src, len, max_w, w, h);
  if (!svg) return NULL;
  char *t = nd_mm_pele_theme(svg, strlen(svg));
  free(svg);
  if (!t) return NULL;

  GError *err = NULL;
  RsvgHandle *rh = rsvg_handle_new_from_data((const guint8 *)t, strlen(t), &err);
  if (!rh) {
    nd_warn("pele: librsvg rejected the SVG: %s", err ? err->message : "?");
    g_clear_error(&err);
  }
  free(t);
  return rh;
}
