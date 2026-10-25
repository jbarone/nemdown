/* check_mermaid — mutation sweep over the mermaid translators and renderers.
 *
 * A mermaid fence is untrusted input running through four hand-written
 * parsers, and three of them do not draw anything themselves: they WRITE DOT
 * that graphviz is then asked to parse. That makes label quoting a structural
 * question rather than a cosmetic one — a label that escaped its quotes would
 * not merely look wrong, it would change the graph. So the invariant this
 * sweep asserts is exactly that one: whatever bytes go in, the dot that comes
 * out is still balanced, both in its quotes and in its braces.
 *
 * Two passes, because they cost very different amounts. The translators are
 * string work and run hundreds of thousands of times; the full build behind
 * them runs graphviz, librsvg and Pango and runs thousands. Memory faults are
 * not checked here by hand — that is what `make asan` and `make valgrind` are
 * for, and this harness is written to be run under both.
 *
 * As with check_utf8: if you change a parser, run this, then BREAK it on
 * purpose and confirm the sweep still fails. A sweep that cannot fail is worth
 * nothing. Four mutations were checked when it was written, and each is caught:
 * emitting a label unquoted, dropping the backslash case from nd_mm_dot_str,
 * and skipping nd_mm_clean_label's control-byte filter all fail here directly;
 * removing the line-length bound in nd_mm_line fails under `make asan`, which
 * is the pass that owns memory faults.
 *
 * Its first run found one: a classDiagram member goes straight from the source
 * line into an HTML-like label without passing through nd_mm_clean_label, so a
 * control byte in a member reached the dot. That is why the filter now lives in
 * the two escapers rather than in the cleaner.
 *
 * pele adds a second boundary of the same shape. pele writes the SVG, and its
 * escaping is upstream's to get right, but nd_mm_pele_theme then REWRITES that
 * SVG's attribute values to resolve the CSS variables librsvg cannot. The
 * invariant there is that the rewrite touches nothing but attribute values:
 * with every quoted value blanked out, the markup before and after must be
 * byte-identical. It is asserted twice -- over random SVG-shaped strings built
 * to provoke the resolver, and over everything pele draws from the mutated
 * corpus, where in addition no var() may survive and librsvg must accept the
 * result. Breaking the tag tracking, letting a value carry `<` into the table,
 * and dropping the recursion bound were each checked to fail here.
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
 * punctuation, dot's, and HTML's. Random noise almost never produces these in
 * the combinations that matter, so the alphabet is stacked towards them. */
static const char kNasty[] =
    "\"\\'{}[]()<>~#%|:;,&-=.>/*$+\n\t\r `!?^@";

/* Slot 0 is built at startup: a label long enough to run past every internal
 * text buffer. Mutation alone never grows a short label to that length, so
 * without it the length clamps are simply never exercised. */
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

  /* Dialects only pele draws. The translators reject them, which is itself
   * worth sweeping; the pele pass below is what they are here for. */
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

/* Scans generated dot the way dot's own lexer would: a `"` opens a string, a
 * backslash inside one escapes the next byte, and braces outside one nest.
 * Anything a label smuggled through its quoting shows up here as an imbalance.
 * HTML-like labels open with `<` right after `label=` and are skipped by
 * angle-bracket depth, which is how dot reads them too. */
static bool dot_balanced(const char *dot, const char **why) {
  int braces = 0;
  bool in_str = false;

  for (const char *p = dot; *p; p++) {
    if (in_str) {
      if (*p == '\\' && p[1]) { p++; continue; }
      if (*p == '"') in_str = false;
      continue;
    }
    if (*p == '"') { in_str = true; continue; }

    if (*p == '<' && p > dot && p[-1] == '=') {
      int depth = 0;
      for (; *p; p++) {
        if (*p == '<') depth++;
        else if (*p == '>' && --depth == 0) break;
      }
      if (!*p) { *why = "unterminated HTML-like label"; return false; }
      continue;
    }

    if (*p == '{') braces++;
    else if (*p == '}' && --braces < 0) { *why = "unbalanced }"; return false; }
  }
  /* No raw control bytes either. Newline is dot's statement separator and is
   * ours to write; a label carrying one must have come out as the two
   * characters `\n`, and anything else below 0x20 never belongs in the
   * output at all. */
  for (const char *p = dot; *p; p++)
    if ((unsigned char)*p < 0x20 && *p != '\n') {
      *why = "raw control byte in the dot";
      return false;
    }
  if (in_str)   { *why = "unterminated string"; return false; }
  if (braces)   { *why = "unbalanced {"; return false; }
  return true;
}

static long checked, drew;

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

static bool sweep_translators(long rounds) {
  char buf[MAXBUF];

  for (long i = 0; i < rounds; i++) {
    uint64_t seed = rng_state;
    int which = (int)(rnd() % NCORPUS);
    uint32_t len = mutate(kCorpus[which], buf);

    /* Every translator gets every input, not only the one its corpus entry
     * came from: a flowchart parser handed a class diagram is exactly the
     * case a real document produces when a dialect line is mistyped. */
    char *(*fns[3])(const char *, uint32_t) = {
      nd_mm_flow_dot, nd_mm_state_dot, nd_mm_class_dot
    };
    static const char *names[3] = {"flow", "state", "class"};

    for (int f = 0; f < 3; f++) {
      char *dot = fns[f](buf, len);
      checked++;
      if (!dot) continue;
      drew++;

      const char *why = NULL;
      if (!dot_balanced(dot, &why)) {
        printf("FAIL: %s produced unbalanced dot (%s)\n", names[f], why);
        printf("  seed 0x%016llx\n", (unsigned long long)seed);
        printf("  --- input ---\n%.*s\n", (int)len, buf);
        printf("  --- dot ---\n%s\n", dot);
        free(dot);
        return false;
      }
      free(dot);
    }
  }
  return true;
}

/* The whole path, graphviz and librsvg and Pango included. Far slower, so far
 * fewer rounds — but this is the only pass that exercises the sequence
 * renderer's layout arithmetic and the raster cache. */
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
  long tr = argc > 1 ? atol(argv[1]) : 60000;
  long bl = argc > 2 ? atol(argv[2]) : 1500;

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

  if (!sweep_translators(tr)) return 1;

  PangoFontMap *fm = pango_cairo_font_map_get_default();
  PangoContext *pctx = pango_font_map_create_context(fm);
  pango_cairo_context_set_resolution(pctx, 96.0);

  long th = tr * 2;
  if (!sweep_theme(th)) return 1;
  if (!sweep_pele(pctx, bl)) return 1;

  /* The full build twice: through pele, which is what the viewer does, and
   * with pele skipped, so the native renderers behind it stay covered. */
  if (!sweep_builds(pctx, bl)) return 1;
  setenv("NEMDOWN_NO_PELE", "1", 1);
  if (!sweep_builds(pctx, bl)) return 1;
  unsetenv("NEMDOWN_NO_PELE");
  g_object_unref(pctx);

  printf("ok: %ld mutated inputs through the translators, %ld produced dot "
         "and all of it balanced; %ld through the full build%s\n",
         checked, drew, bl,
         nd_mermaid_have_graphviz() ? "" : " (graphviz absent: graph dialects"
                                           " fell back, as designed)");
  printf("ok: %ld synthetic SVGs through the theming pass, %ld of %ld pele "
         "renders themed with the markup intact; full build with and "
         "without pele\n", th, pele_drew, bl);
  return 0;
}
