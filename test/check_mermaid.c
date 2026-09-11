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

  if (!sweep_builds(pctx, bl)) return 1;
  g_object_unref(pctx);

  printf("ok: %ld mutated inputs through the translators, %ld produced dot "
         "and all of it balanced; %ld through the full build%s\n",
         checked, drew, bl,
         nd_mermaid_have_graphviz() ? "" : " (graphviz absent: graph dialects"
                                           " fell back, as designed)");
  return 0;
}
