/* mermaid_gv.c — graphviz, loaded at run time rather than linked.
 *
 * dlopen keeps graphviz an optional dependency: one binary renders graph
 * diagrams where it is installed and shows their source where it is not, with
 * no second build configuration and no 11MB hard dependency on a package many
 * readers will never need. The same bargain the maths typesetter strikes with
 * its font.
 *
 * Only eight entry points are needed and every handle is opaque here, so no
 * graphviz header is included and nothing depends on its struct layouts — the
 * ABI surface is eight function signatures, which is what makes loading a
 * library this way safe rather than clever.
 *
 * The output format is `svg:cairo`, not `svg`, and that is not a detail. Plain
 * SVG writes text as text, leaving the renderer to shape it — and librsvg does
 * not shape it the way graphviz measured it. graphviz measures through Pango
 * with hinting at 96dpi, where Hack's 10.66px advance hints down to 10px, then
 * writes the result as points; librsvg draws the same string unhinted at 13px
 * and gets 4% more. The difference is invisible on a node with a wide margin
 * and obvious in a class-diagram cell, where the last member ran out through
 * the right border. `svg:cairo` emits every glyph as a positioned outline, so
 * what graphviz measured is exactly what is drawn and the mismatch cannot
 * exist. The cost is a larger SVG and no selectable text in it, and neither
 * matters to a picture that gets rasterised anyway.
 */

#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>

#include "doc/mermaid_int.h"
#include "util/log.h"

typedef void GVC;
typedef void AGraph;

static struct {
  bool tried, ok;
  void *lib;
  GVC    *(*context)(void);
  int     (*free_context)(GVC *);
  AGraph *(*memread)(const char *);
  int     (*close)(AGraph *);
  int     (*layout)(GVC *, AGraph *, const char *);
  int     (*free_layout)(GVC *, AGraph *);
  int     (*render_data)(GVC *, AGraph *, const char *, char **, size_t *);
  void    (*free_render_data)(char *);
  int     (*nnodes)(AGraph *);
  int     (*nedges)(AGraph *);
  int     (*seterr)(int);          /* agseterr; AGMAX == 2 silences it */
} gv;

/* The soname moves with major releases — graphviz 13 shipped libgvc.so.6,
 * 16 ships .so.7 — so the unversioned link is tried first (Arch keeps it in
 * the main package) and the known versions after it. */
static const char *kLibs[] = {
  "libgvc.so", "libgvc.so.7", "libgvc.so.6", "libgvc.so.8", NULL
};

static void gv_load(void) {
  if (gv.tried) return;
  gv.tried = true;

  for (int i = 0; kLibs[i] && !gv.lib; i++)
    gv.lib = dlopen(kLibs[i], RTLD_LAZY | RTLD_LOCAL);
  if (!gv.lib) {
    nd_info("graphviz not found; mermaid graph diagrams will show their source");
    return;
  }

  /* agmemread and friends live in libcgraph, which libgvc depends on, and
   * dlsym on a handle searches its dependency tree — so one dlopen suffices. */
  #define SYM(field, name) \
    *(void **)&gv.field = dlsym(gv.lib, name); \
    if (!gv.field) { nd_warn("graphviz: missing %s", name); goto fail; }
  SYM(context,          "gvContext")
  SYM(free_context,     "gvFreeContext")
  SYM(memread,          "agmemread")
  SYM(close,            "agclose")
  SYM(layout,           "gvLayout")
  SYM(free_layout,      "gvFreeLayout")
  SYM(render_data,      "gvRenderData")
  SYM(free_render_data, "gvFreeRenderData")
  #undef SYM
  /* Optional: only used for the size caps and for quieting the library. */
  *(void **)&gv.nnodes = dlsym(gv.lib, "agnnodes");
  *(void **)&gv.nedges = dlsym(gv.lib, "agnedges");
  *(void **)&gv.seterr = dlsym(gv.lib, "agseterr");

  gv.ok = true;
  return;
fail:
  dlclose(gv.lib);
  gv.lib = NULL;
}

bool nd_mm_gv_available(void) {
  gv_load();
  return gv.ok;
}

/* The plain-SVG header states the drawing's size in POINTS, which is the
 * coordinate system the dot was written in — a fontsize of 13 there is 13
 * logical pixels here. The cairo SVG states its own size in 96dpi pixels
 * instead, so rather than assume the ratio between them, both are asked for
 * and the point size is used as the viewport librsvg fits the drawing into. */
static bool viewbox_of(const char *svg, size_t len, double *w, double *h) {
  size_t scan = len < 2048 ? len : 2048;
  for (size_t i = 0; i + 9 < scan; i++) {
    if (memcmp(svg + i, "viewBox=\"", 9) != 0) continue;

    /* Copied out and terminated rather than scanned in place: gvRenderData's
     * buffer comes with a length, and sscanf only stops at a NUL. */
    char nums[80];
    size_t n = len - (i + 9);
    if (n > sizeof nums - 1) n = sizeof nums - 1;
    memcpy(nums, svg + i + 9, n);
    nums[n] = 0;

    double a, b, c, d;
    if (sscanf(nums, "%lf %lf %lf %lf", &a, &b, &c, &d) == 4 && c > 0 && d > 0) {
      *w = c; *h = d;
      return true;
    }
    return false;
  }
  return false;
}

RsvgHandle *nd_mm_gv_render(const char *dot, double *w, double *h) {
  gv_load();
  if (!gv.ok || !dot) return NULL;

  /* The dot is ours, so a parse error is our bug — but the labels in it came
   * from the document, and a viewer that prints to stderr on a malformed note
   * is a viewer that spams the terminal. AGMAX turns the messages off. */
  if (gv.seterr) gv.seterr(2 /* AGMAX */);

  GVC *ctx = gv.context();
  if (!ctx) return NULL;

  RsvgHandle *out = NULL, *rh = NULL;
  char   *plain = NULL, *drawn = NULL;
  size_t  plen = 0, dlen = 0;
  bool    laid = false;
  GError *err = NULL;

  AGraph *g = gv.memread(dot);
  if (!g) goto done;

  if (gv.nnodes && gv.nedges &&
      (gv.nnodes(g) > (int)ND_MM_MAX_NODES || gv.nedges(g) > (int)ND_MM_MAX_EDGES))
    goto done;

  if (gv.layout(ctx, g, "dot") != 0) goto done;
  laid = true;

  if (gv.render_data(ctx, g, "svg", &plain, &plen) != 0 || !plain || !plen)
    goto done;
  if (!viewbox_of(plain, plen, w, h)) goto done;

  if (!isfinite(*w) || !isfinite(*h) ||
      *w > ND_MM_MAX_DIM || *h > ND_MM_MAX_DIM) goto done;

  /* A graphviz built without its cairo plugin still draws, just with the
   * shaping mismatch described above — a worse picture beats no picture. */
  if (gv.render_data(ctx, g, "svg:cairo", &drawn, &dlen) != 0 || !drawn || !dlen) {
    drawn = NULL;
    rh = rsvg_handle_new_from_data((const guint8 *)plain, plen, &err);
  } else {
    rh = rsvg_handle_new_from_data((const guint8 *)drawn, dlen, &err);
  }
  if (!rh) {
    nd_warn("mermaid: librsvg rejected graphviz output: %s",
            err ? err->message : "?");
    if (err) g_error_free(err);
    goto done;
  }
  out = rh;

done:
  if (plain) gv.free_render_data(plain);
  if (drawn) gv.free_render_data(drawn);
  if (g) {
    if (laid) gv.free_layout(ctx, g);
    gv.close(g);
  }
  gv.free_context(ctx);
  return out;
}
