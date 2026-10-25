/* check_mermaid — mutation sweep over the mermaid renderer.
 *
 * A mermaid fence is untrusted input, and pele turns it into SVG that librsvg
 * then parses. pele's own escaping is upstream's to get right, but
 * nd_mm_pele_theme then REWRITES that SVG's attribute values to resolve the
 * CSS variables librsvg cannot, which makes it a boundary of ours. The
 * invariant is that the rewrite touches nothing but attribute values: with
 * every quoted value blanked out, the markup before and after must be
 * byte-identical, no `<` may be added, and the count of `"` may not change.
 *
 * Three passes. Random SVG-shaped strings built to provoke the resolver run
 * hundreds of thousands of times, because the pass is string work. Everything
 * pele draws from the mutated corpus runs thousands, and there no var() may
 * survive and librsvg must accept the result. The full build runs thousands
 * more, for the fitting and the raster cache. Memory faults are not checked
 * here by hand -- that is what `make asan` and `make valgrind` are for, and
 * this harness is written to be run under both.
 *
 * As with check_utf8: if you change the theming pass, run this, then BREAK it
 * on purpose and confirm the sweep still fails. A sweep that cannot fail is
 * worth nothing. Three mutations were checked when it was written, and each is
 * caught: ignoring the tag tracking, letting a definition carry `<` into the
 * table, and dropping the recursion bound. The second slipped through the
 * first version, which is why the character counts exist.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pango/pangocairo.h>

#include "doc/mermaid.h"
#include "doc/mermaid_int.h"

/* One seed, printed on failure, so any hit reproduces exactly. */
static uint64_t rng_state = 0x9e3779b97f4a7c15ull;

static uint64_t rnd(void) {
  rng_state ^= rng_state << 13;
  rng_state ^= rng_state >> 7;
  rng_state ^= rng_state << 17;
  return rng_state;
}

/* The bytes that mean something to one grammar or another: mermaid's own
 * punctuation, SVG's and CSS's. Random noise almost never produces these in
 * the combinations that matter, so the alphabet is stacked towards them. */
static const char kNasty[] =
    "\"\\'{}[]()<>~#%|:;,&-=.>/*$+\n\t\r `!?^@";

/* Slot 0 is built at startup: a label long enough to run past the measurer's
 * ND_MM_MAX_LINE clamp. Mutation alone never grows a short label to that
 * length, so without it the clamp is simply never exercised. */
static char kLong[6000];

static const char *kCorpus[] = {
  kLong,
  "flowchart TD\n"
  "  A[Start] --> B{Choose?}\n"
  "  B -- Yes --> C([done])\n"
  "  B -->|No| D[(store)]\n"
  "  C & D --> E[[sub]]\n"
  "  subgraph g [Group]\n"
  "    E -.-> F\n"
  "  end\n"
  "  F ==> A\n",

  "sequenceDiagram\n"
  "  autonumber\n"
  "  actor A\n"
  "  participant B as The Other One\n"
  "  A->>+B: hello there\n"
  "  B-->>-A: goodbye\n"
  "  A->>A: thinks\n"
  "  Note over A,B: both of them\n"
  "  alt one\n"
  "    A-xB: no\n"
  "  else two\n"
  "    A-)B: async\n"
  "  end\n",

  "stateDiagram-v2\n"
  "  [*] --> Idle\n"
  "  Idle --> Busy : work\n"
  "  state Busy {\n"
  "    [*] --> Step1\n"
  "    Step1 --> [*]\n"
  "  }\n"
  "  state ch <<choice>>\n"
  "  Busy --> ch\n"
  "  ch --> [*]\n",

  "gantt\n"
  "  title Plan\n"
  "  dateFormat YYYY-MM-DD\n"
  "  section A\n"
  "  One :a1, 2026-01-01, 3d\n"
  "  Two :after a1, 2d\n",

  "pie title Share\n"
  "  \"Left\" : 40\n"
  "  \"Right\" : 60\n",

  "erDiagram\n"
  "  CUSTOMER ||--o{ ORDER : places\n"
  "  ORDER {\n"
  "    string id PK\n"
  "    int total\n"
  "  }\n",

  "mindmap\n"
  "  root((Centre))\n"
  "    Left\n"
  "      Deeper\n"
  "    Right[Square]\n",

  "gitGraph\n"
  "  commit\n"
  "  branch dev\n"
  "  commit id: \"x\"\n"
  "  checkout main\n"
  "  merge dev\n",

  "classDiagram\n"
  "  note for Thing \"a note\"\n"
  "  class Thing {\n"
  "    <<interface>>\n"
  "    +String name\n"
  "    #calc(int n) Map~String, Int~\n"
  "    +run() void*\n"
  "  }\n"
  "  class Other\n"
  "  Thing \"1\" *-- \"many\" Other : owns\n"
  "  Thing <|-- Other\n",
};
#define NCORPUS ((int)(sizeof kCorpus / sizeof *kCorpus))

#define MAXBUF 8192

static uint32_t mutate(const char *src, char *out) {
  size_t n = strlen(src);
  if (n > MAXBUF - 1) n = MAXBUF - 1;
  memcpy(out, src, n);

  int rounds = 1 + (int)(rnd() % 12);
  for (int r = 0; r < rounds && n > 0; r++) {
    size_t at = (size_t)(rnd() % n);
    switch (rnd() % 5) {
      case 0:                                        /* overwrite */
        out[at] = kNasty[rnd() % (sizeof kNasty - 1)];
        break;
      case 1:                                        /* delete */
        memmove(out + at, out + at + 1, n - at - 1);
        n--;
        break;
      case 2:                                        /* insert */
        if (n + 1 < MAXBUF) {
          memmove(out + at + 1, out + at, n - at);
          out[at] = kNasty[rnd() % (sizeof kNasty - 1)];
          n++;
        }
        break;
      case 3:                                        /* truncate */
        n = at;
        break;
      default: {                                     /* duplicate a run */
        size_t len = 1 + (size_t)(rnd() % 24);
        if (at + len > n) len = n - at;
        if (n + len < MAXBUF) {
          memmove(out + at + len, out + at, n - at);
          n += len;
        }
        break;
      }
    }
  }
  out[n] = 0;
  return (uint32_t)n;
}

/* ---- pele's theming pass ----------------------------------------------- */

/* The markup with every quoted attribute value emptied: what nd_mm_pele_theme
 * must leave exactly as it found it. Written from the XML reading, not copied
 * from the code under test. */
static char *skeleton(const char *s) {
  size_t n = strlen(s);
  char *out = malloc(n + 1), *o = out;
  bool tag = false, val = false;
  for (const char *p = s; *p; p++) {
    if (val) { if (*p == '"') { val = false; *o++ = '"'; } continue; }
    if (tag && *p == '"') { val = true; *o++ = '"'; continue; }
    if (*p == '<') tag = true;
    else if (*p == '>') tag = false;
    *o++ = *p;
  }
  *o = 0;
  return out;
}

/* True if any attribute value still holds a var(). */
static bool var_left(const char *s) {
  bool tag = false, val = false;
  for (const char *p = s; *p; p++) {
    if (val) {
      if (*p == '"') val = false;
      else if (!strncmp(p, "var(", 4)) return true;
      continue;
    }
    if (tag && *p == '"') val = true;
    else if (*p == '<') tag = true;
    else if (*p == '>') tag = false;
  }
  return false;
}

static bool theme_preserves(const char *in, const char *out, const char **why) {
  char *a = skeleton(in), *b = skeleton(out);
  bool same = strcmp(a, b) == 0;
  free(a);
  free(b);
  if (!same) { *why = "markup outside attribute values changed"; return false; }
  /* The skeleton blanks values, so it cannot see markup carried INTO one. A
   * raw `<` in a value is malformed XML and a `"` would end it, so the pass
   * may never add the first and never change the count of the second. It may
   * REMOVE a `<`: a resolved var() drops its fallback, and in malformed input
   * the fallback can hold one. */
  size_t lt_in = 0, lt_out = 0, q_in = 0, q_out = 0;
  for (const char *p = in; *p; p++) { lt_in += *p == '<'; q_in += *p == '"'; }
  for (const char *p = out; *p; p++) { lt_out += *p == '<'; q_out += *p == '"'; }
  if (lt_out > lt_in) { *why = "the pass added a < to a value"; return false; }
  if (q_out != q_in) { *why = "the pass changed the quoting"; return false; }
  /* Fan-out through nested variables is the only way out grows. The depth
   * bound keeps it polynomial; this keeps it honest. */
  if (strlen(out) > 64 * strlen(in) + 4096) { *why = "output blew up"; return false; }
  return true;
}

/* Tokens that steer the resolver into its corners: definitions that refer to
 * themselves and to each other, unterminated var(, nested parentheses, quotes
 * and angle brackets where a value should be. */
static const char *kSvgTok[] = {
  "<", ">", "\"", " ", "x", "=", ";", ":", ",", "(", ")", "#fff",
  "var(", "var(--", "var(--a)", "var(--b,", "var(--_c)", "var(--pele-bg)",
  "var(--pele-fg,#000)", "--a:", "--b:", "--_c:", " style=\"", "<g fill=\"",
  "<text>", "</text>", "/>",
};
#define NSVGTOK ((int)(sizeof kSvgTok / sizeof *kSvgTok))

static const char *kSvgRoot[] = {
  "<svg style=\"--a:var(--b);--b:var(--a);--_c:var(--pele-bg,#fff)\">",
  "<svg style=\"--a:var(--a) var(--a) var(--a);--b:<x>\">",
  "<svg style=\"--a:<g;--b:var(--a)\">",
  "<svg style=\"--a:var(--b,var(--b,var(--b,var(--b,var(--b)))))\">",
  "<svg>",
  "",
};
#define NSVGROOT ((int)(sizeof kSvgRoot / sizeof *kSvgRoot))

static bool sweep_theme(long rounds) {
  char buf[MAXBUF];
  for (long i = 0; i < rounds; i++) {
    uint64_t seed = rng_state;
    size_t n = (size_t)snprintf(buf, sizeof buf, "%s",
                                kSvgRoot[rnd() % NSVGROOT]);
    int toks = (int)(rnd() % 80);
    for (int t = 0; t < toks; t++) {
      const char *k = kSvgTok[rnd() % NSVGTOK];
      size_t kl = strlen(k);
      if (n + kl + 1 >= sizeof buf) break;
      memcpy(buf + n, k, kl);
      n += kl;
    }
    buf[n] = 0;

    char *out = nd_mm_pele_theme(buf, n);
    const char *why = NULL;
    if (!out || !theme_preserves(buf, out, &why)) {
      printf("FAIL: nd_mm_pele_theme: %s\n", out ? why : "returned NULL");
      printf("  seed 0x%016llx\n", (unsigned long long)seed);
      printf("  --- input ---\n%s\n  --- output ---\n%s\n", buf,
             out ? out : "(null)");
      free(out);
      return false;
    }
    free(out);
  }
  return true;
}

static long pele_drew;

/* Everything pele draws from the mutated corpus goes through the theming pass
 * and then librsvg, which is the path the viewer takes. */
static bool sweep_pele(PangoContext *pctx, long rounds) {
  char buf[MAXBUF];
  for (long i = 0; i < rounds; i++) {
    uint64_t seed = rng_state;
    int which = (int)(rnd() % NCORPUS);
    uint32_t len = mutate(kCorpus[which], buf);

    double w, h;
    char *svg = nd_mm_pele_svg(pctx, buf, len, 700.0, &w, &h);
    if (!svg) continue;
    pele_drew++;

    char *out = nd_mm_pele_theme(svg, strlen(svg));
    const char *why = NULL;
    bool ok = out && theme_preserves(svg, out, &why);
    if (ok && var_left(out)) { why = "a var() survived"; ok = false; }
    if (ok) {
      GError *err = NULL;
      RsvgHandle *rh = rsvg_handle_new_from_data((const guint8 *)out,
                                                 strlen(out), &err);
      if (rh) g_object_unref(rh);
      else { why = "librsvg rejected it"; ok = false; g_clear_error(&err); }
    }
    if (!ok) {
      printf("FAIL: pele output after theming: %s\n", out ? why : "NULL");
      printf("  seed 0x%016llx\n", (unsigned long long)seed);
      printf("  --- input ---\n%.*s\n", (int)len, buf);
      free(out);
      free(svg);
      return false;
    }
    free(out);
    free(svg);
  }
  return true;
}

/* The whole path, as the viewer takes it: pele, the theming pass, librsvg, the
 * fit and the raster cache. */
static bool sweep_builds(PangoContext *pctx, long rounds) {
  char buf[MAXBUF];

  for (long i = 0; i < rounds; i++) {
    uint64_t seed = rng_state;
    int which = (int)(rnd() % NCORPUS);
    uint32_t len = mutate(kCorpus[which], buf);

    double zoom = 0.5 + (double)(rnd() % 250) / 100.0;
    struct nd_mermaid *m = nd_mermaid_build(pctx, buf, len, 700.0, zoom);
    if (!m) continue;

    double w = 0, h = 0;
    nd_mermaid_size(m, &w, &h);
    if (!(w > 0) || !(h > 0) || w > 700.5 || h > ND_MM_MAX_DIM) {
      printf("FAIL: built a diagram sized %g x %g\n", w, h);
      printf("  seed 0x%016llx\n", (unsigned long long)seed);
      printf("  --- input ---\n%.*s\n", (int)len, buf);
      nd_mermaid_free(m);
      return false;
    }

    /* Paint it too: the raster cache, the fit scaling and the sequence
     * painter's participant indices are only reached from here. */
    cairo_surface_t *cs =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 64, 64);
    cairo_surface_set_device_scale(cs, 1.5, 1.5);
    cairo_t *cr = cairo_create(cs);
    nd_mermaid_paint(m, cr, 0, 0);
    cairo_destroy(cr);
    cairo_surface_destroy(cs);

    nd_mermaid_free(m);
  }
  return true;
}

int main(int argc, char **argv) {
  long th = argc > 1 ? atol(argv[1]) : 120000;
  long pr = argc > 2 ? atol(argv[2]) : 3000;

  {
    char big[4200];
    for (size_t i = 0; i < sizeof big - 1; i++)
      big[i] = (char)('a' + (i % 26));
    big[sizeof big - 1] = 0;
    snprintf(kLong, sizeof kLong,
             "classDiagram\n  class Big {\n    +String %.1200s\n  }\n"
             "flowchart LR\n  N1[%.1400s] --> N2{%.1200s}\n",
             big, big + 7, big + 19);
  }

  PangoFontMap *fm = pango_cairo_font_map_get_default();
  PangoContext *pctx = pango_font_map_create_context(fm);
  pango_cairo_context_set_resolution(pctx, 96.0);

  if (!sweep_theme(th)) return 1;
  if (!sweep_pele(pctx, pr)) return 1;
  if (!sweep_builds(pctx, pr)) return 1;
  g_object_unref(pctx);

  printf("ok: %ld synthetic SVGs through the theming pass; %ld of %ld mutated "
         "fences drawn by pele, all themed with the markup intact; %ld more "
         "through the full build\n", th, pele_drew, pr, pr);
  return 0;
}
