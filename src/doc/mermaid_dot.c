/* mermaid_dot.c — flowchart, stateDiagram and classDiagram, translated to dot.
 *
 * These three dialects have something in common that the sequence dialect does
 * not: they describe a set of nodes and edges and say nothing about where any
 * of it goes. Somebody has to solve that, and solving it well is a research
 * problem, not an afternoon — so graphviz solves it, and the work here is
 * translation rather than layout.
 *
 * Theming goes INTO the dot rather than being applied to the picture
 * afterwards. Catppuccin colours and Hack are graph attributes, so graphviz
 * measures the text in the font it will be drawn in and the result sits in the
 * document instead of looking pasted on top of it.
 *
 * Every label is document text and therefore untrusted, so it takes exactly
 * one of two paths out of here: nd_mm_dot_str for a quoted attribute, or
 * nd_mm_html_esc for an HTML-like one. Nothing is interpolated raw.
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "doc/mermaid_int.h"

#define MM_MAXID   128
#define MM_MAXLBL  1024
#define MM_MAXCLUS  32
#define MM_MAXNOTE  32

/* ---- a node registry, shared by all three dialects --------------------- */

typedef struct {
  char *id;
  char *label;
  int   shape;
  int   cluster;    /* index into the cluster table, or -1 */
  int   flags;
  char *extra;      /* classDiagram: members, one per line */
  char *anno;       /* classDiagram: <<interface>> and friends */
} mm_node;

/* An array of POINTERS, not of nodes. Entries are handed out and held across
 * later lookups — a flowchart statement keeps both ends of an edge while it
 * parses the next one — and growing an array of structs moves every one of
 * them. That was a use-after-free the first time this was written, found by
 * ASan on a fence with enough nodes to force the second realloc. Individually
 * allocated entries never move, so the mistake cannot be made again. */
typedef struct {
  mm_node **v;
  uint32_t  n, cap;
  bool      full;   /* latched once ND_MM_MAX_NODES is hit */
} mm_nodes;

static mm_node *nodes_find(mm_nodes *ns, const char *id) {
  for (uint32_t i = 0; i < ns->n; i++)
    if (strcmp(ns->v[i]->id, id) == 0) return ns->v[i];
  return NULL;
}

static mm_node *nodes_get(mm_nodes *ns, const char *id) {
  if (!id || !*id) return NULL;
  mm_node *e = nodes_find(ns, id);
  if (e) return e;
  if (ns->n >= ND_MM_MAX_NODES) { ns->full = true; return NULL; }
  if (ns->n == ns->cap) {
    uint32_t cap = ns->cap ? ns->cap * 2 : 16;
    mm_node **v = realloc(ns->v, cap * sizeof *v);
    if (!v) { ns->full = true; return NULL; }
    ns->v = v; ns->cap = cap;
  }
  e = calloc(1, sizeof *e);
  if (!e) { ns->full = true; return NULL; }
  e->id = strdup(id);
  e->cluster = -1;
  if (!e->id) { free(e); ns->full = true; return NULL; }
  ns->v[ns->n++] = e;
  return e;
}

static void node_set_label(mm_node *e, const char *label) {
  if (!e || !label || !*label) return;
  free(e->label);
  e->label = strdup(label);
}

static void nodes_free(mm_nodes *ns) {
  for (uint32_t i = 0; i < ns->n; i++) {
    free(ns->v[i]->id);    free(ns->v[i]->label);
    free(ns->v[i]->extra); free(ns->v[i]->anno);
    free(ns->v[i]);
  }
  free(ns->v);
  memset(ns, 0, sizeof *ns);
}

/* ---- small scanners ---------------------------------------------------- */

static void skip_ws(const char **p) {
  while (**p == ' ' || **p == '\t') (*p)++;
}

static bool id_char(char c) {
  return isalnum((unsigned char)c) || c == '_' || c == '.' ||
         (unsigned char)c >= 0x80;
}

static bool read_id(const char **p, char *out, size_t cap) {
  skip_ws(p);
  size_t n = 0;
  while (id_char(**p)) {
    if (n + 1 < cap) out[n++] = **p;
    (*p)++;
  }
  out[n] = 0;
  return n > 0;
}

static void trim(char *s) {
  size_t n = strlen(s);
  while (n && isspace((unsigned char)s[n - 1])) s[--n] = 0;
  size_t i = 0;
  while (s[i] && isspace((unsigned char)s[i])) i++;
  if (i) memmove(s, s + i, n - i + 1);
}

/* ---- shared preamble --------------------------------------------------- */

static void preamble(nd_buf *b, const char *rankdir, double nodesep,
                     double ranksep) {
  nd_buf_fmt(b,
      "digraph {\n"
      "  bgcolor=\"transparent\"\n"
      "  rankdir=%s\n"
      "  nodesep=%.2f\n  ranksep=%.2f\n"
      "  node [fontname=\"%s\", fontsize=13, fontcolor=\"%s\",\n"
      "        shape=box, style=\"rounded,filled\", fillcolor=\"%s\",\n"
      "        color=\"%s\", penwidth=1.2, margin=\"0.22,0.13\"]\n"
      "  edge [fontname=\"%s\", fontsize=11, fontcolor=\"%s\",\n"
      "        color=\"%s\", penwidth=1.2, arrowsize=0.7]\n",
      rankdir, nodesep, ranksep, ND_MM_FONT, ND_MM_TEXT, ND_MM_FILL,
      ND_MM_BORDER, ND_MM_FONT, ND_MM_DIM, ND_MM_EDGE);
}

/* =========================================================================
 * flowchart
 * ========================================================================= */

enum {
  SH_BOX, SH_ROUND, SH_STADIUM, SH_CIRCLE, SH_DIAMOND, SH_HEX,
  SH_SUBROUTINE, SH_CYLINDER, SH_PARALLEL, SH_PARALLEL2,
  SH_TRAPEZ, SH_TRAPEZ_ALT, SH_ASYM
};

/* Longest opener first, so `([` is never read as `(`. */
static const struct { const char *open, *close; int shape; } kShapes[] = {
  {"([", "])",  SH_STADIUM},
  {"[[", "]]",  SH_SUBROUTINE},
  {"[(", ")]",  SH_CYLINDER},
  {"((", "))",  SH_CIRCLE},
  {"{{", "}}",  SH_HEX},
  {"[/", "/]",  SH_PARALLEL},      /* or `\]`, which makes it a trapezium */
  {"[\\","\\]", SH_PARALLEL2},     /* or `/]` */
  {">",  "]",   SH_ASYM},
  {"[",  "]",   SH_BOX},
  {"(",  ")",   SH_ROUND},
  {"{",  "}",   SH_DIAMOND},
};

static void emit_shape(nd_buf *b, int shape) {
  switch (shape) {
    case SH_CIRCLE:      nd_buf_puts(b, ", shape=circle, style=filled"); break;
    case SH_STADIUM:     nd_buf_fmt(b, ", shape=oval, style=filled, color=\"%s\"",
                                    ND_MM_ACCENT); break;
    case SH_DIAMOND:     nd_buf_fmt(b, ", shape=diamond, style=filled,"
                                    " fillcolor=\"%s\", color=\"%s\"",
                                    ND_MM_FILL2, ND_MM_DECIDE); break;
    case SH_HEX:         nd_buf_puts(b, ", shape=hexagon, style=filled"); break;
    case SH_SUBROUTINE:  nd_buf_puts(b, ", shape=box, peripheries=2,"
                                    " style=filled"); break;
    case SH_CYLINDER:    nd_buf_puts(b, ", shape=cylinder, style=filled"); break;
    case SH_PARALLEL:
    case SH_PARALLEL2:   nd_buf_puts(b, ", shape=parallelogram, style=filled"); break;
    case SH_TRAPEZ:      nd_buf_puts(b, ", shape=trapezium, style=filled"); break;
    case SH_TRAPEZ_ALT:  nd_buf_puts(b, ", shape=invtrapezium, style=filled"); break;
    case SH_ASYM:        nd_buf_puts(b, ", shape=cds, style=filled"); break;
    case SH_ROUND:
    case SH_BOX:
    default:             break;   /* the default node style already */
  }
}

/* Reads `id`, `id[Label]`, `id{Label}` and the rest, leaving *p after it.
 * The shape is recorded on the node the first time one is written, so a node
 * mentioned bare in one statement and shaped in another keeps its shape. */
static mm_node *read_nodespec(const char **p, mm_nodes *ns, int cluster) {
  char id[MM_MAXID];
  if (!read_id(p, id, sizeof id)) return NULL;

  mm_node *e = nodes_get(ns, id);
  if (e && e->cluster < 0) e->cluster = cluster;

  for (size_t i = 0; i < sizeof kShapes / sizeof *kShapes; i++) {
    size_t ol = strlen(kShapes[i].open);
    if (strncmp(*p, kShapes[i].open, ol) != 0) continue;

    const char *body = *p + ol;
    const char *close = NULL;
    const char *cl = kShapes[i].close;
    int shape = kShapes[i].shape;

    /* `[/ ... /]` and `[/ ... \]` share an opener and differ only in how they
     * close, so both closers are searched and the nearer one decides. */
    const char *alt = NULL;
    if (shape == SH_PARALLEL)  alt = "\\]";
    if (shape == SH_PARALLEL2) alt = "/]";

    if (*body == '"') {
      const char *q = strchr(body + 1, '"');
      if (q) {
        close = strstr(q + 1, cl);
        const char *a = alt ? strstr(q + 1, alt) : NULL;
        if (a && (!close || a < close)) { close = a; cl = alt;
                                          shape = shape == SH_PARALLEL
                                                ? SH_TRAPEZ : SH_TRAPEZ_ALT; }
      }
    } else {
      close = strstr(body, cl);
      const char *a = alt ? strstr(body, alt) : NULL;
      if (a && (!close || a < close)) { close = a; cl = alt;
                                        shape = shape == SH_PARALLEL
                                              ? SH_TRAPEZ : SH_TRAPEZ_ALT; }
    }
    if (!close) break;

    char label[MM_MAXLBL];
    size_t n = (size_t)(close - body);
    if (n > sizeof label - 1) n = sizeof label - 1;
    memcpy(label, body, n);
    label[n] = 0;
    trim(label);
    nd_mm_clean_label(label);
    node_set_label(e, label);
    if (e) e->shape = shape;
    *p = close + strlen(cl);
    break;
  }

  /* `:::className` styling, which we read past and do not act on: the theme
   * is fixed, so honouring a document's own colours would break it. */
  if (strncmp(*p, ":::", 3) == 0) {
    *p += 3;
    char cls[MM_MAXID];
    read_id(p, cls, sizeof cls);
  }
  return e;
}

typedef struct {
  int  dashed, thick, invis, head;   /* head: 0 none, 1 arrow, 2 odot, 3 tee */
  char label[MM_MAXLBL];
} mm_link;

static bool link_char(char c) {
  return c == '-' || c == '.' || c == '=' || c == '~';
}

static void link_style(mm_link *lk, const char *s, const char *e) {
  for (const char *c = s; c < e; c++) {
    if (*c == '.') lk->dashed = 1;
    if (*c == '=') lk->thick = 1;
    if (*c == '~') lk->invis = 1;
  }
}

static bool read_arrowhead(const char **p, mm_link *lk) {
  if (**p == '>') { lk->head = 1; (*p)++; return true; }
  if (**p == 'o') { lk->head = 2; (*p)++; return true; }
  if (**p == 'x') { lk->head = 3; (*p)++; return true; }
  return false;
}

static bool read_link(const char **pp, mm_link *lk) {
  const char *p = *pp;
  skip_ws(&p);
  const char *s = p;
  while (link_char(*p)) p++;
  if (p - s < 2) return false;          /* one dash is a hyphen, not a link */

  memset(lk, 0, sizeof *lk);
  link_style(lk, s, p);

  if (!read_arrowhead(&p, lk)) {
    /* Either an open link (`A --- B`) or the `-- text -->` form. The second
     * run of link characters is what tells them apart, and it must be a RUN:
     * stopping at the first `-` would read the hyphen in "e-mail" as one. */
    const char *q = p, *fq = NULL, *fr = NULL;
    while (*q) {
      if (link_char(*q)) {
        const char *r = q;
        while (link_char(*r)) r++;
        if (r - q >= 2) { fq = q; fr = r; break; }
        q = r;
      } else q++;
    }
    if (fq) {
      size_t n = (size_t)(fq - p);
      if (n > sizeof lk->label - 1) n = sizeof lk->label - 1;
      memcpy(lk->label, p, n);
      lk->label[n] = 0;
      trim(lk->label);
      nd_mm_clean_label(lk->label);
      link_style(lk, fq, fr);
      p = fr;
      read_arrowhead(&p, lk);
    }
  }

  /* The other label form, `-->|text|`. */
  skip_ws(&p);
  if (*p == '|') {
    const char *e = strchr(p + 1, '|');
    if (e) {
      size_t n = (size_t)(e - p - 1);
      if (n > sizeof lk->label - 1) n = sizeof lk->label - 1;
      memcpy(lk->label, p + 1, n);
      lk->label[n] = 0;
      trim(lk->label);
      nd_mm_clean_label(lk->label);
      p = e + 1;
    }
  }
  *pp = p;
  return true;
}

static void emit_edge(nd_buf *b, const char *from, const char *to,
                      const mm_link *lk) {
  nd_buf_puts(b, "  ");
  nd_mm_dot_str(b, from);
  nd_buf_puts(b, " -> ");
  nd_mm_dot_str(b, to);
  nd_buf_puts(b, " [");
  if (lk->invis)  nd_buf_puts(b, "style=invis, ");
  else if (lk->dashed) nd_buf_puts(b, "style=dashed, ");
  if (lk->thick)  nd_buf_puts(b, "penwidth=2.0, ");
  switch (lk->head) {
    case 0:  nd_buf_puts(b, "arrowhead=none, "); break;
    case 2:  nd_buf_puts(b, "arrowhead=odot, "); break;
    case 3:  nd_buf_puts(b, "arrowhead=tee, ");  break;
    default: break;
  }
  if (lk->label[0]) {
    nd_buf_puts(b, "label=");
    nd_mm_dot_str(b, lk->label);
    nd_buf_puts(b, ", ");
  }
  nd_buf_puts(b, "]\n");
}

/* Lines that configure rather than describe. Read past, not acted on: the
 * theme is fixed and interactivity has nowhere to go in a viewer. */
static bool flow_skippable(const char *s) {
  static const char *kSkip[] = {"style ", "classDef ", "class ", "click ",
                                "linkStyle ", "accTitle", "accDescr", NULL};
  for (int i = 0; kSkip[i]; i++)
    if (strncmp(s, kSkip[i], strlen(kSkip[i])) == 0) return true;
  return false;
}

char *nd_mm_flow_dot(const char *src, uint32_t len) {
  nd_lines it = {.src = src, .len = len};
  char line[ND_MM_MAX_LINE];
  if (!nd_mm_line(&it, line, sizeof line)) return NULL;

  const char *rankdir = "TB";
  {
    const char *d = line;
    while (*d && !isspace((unsigned char)*d)) d++;
    skip_ws(&d);
    if      (strncmp(d, "LR", 2) == 0) rankdir = "LR";
    else if (strncmp(d, "RL", 2) == 0) rankdir = "RL";
    else if (strncmp(d, "BT", 2) == 0) rankdir = "BT";
  }

  mm_nodes ns = {0};
  nd_buf edges = {.ok = true};
  char *clabel[MM_MAXCLUS] = {0};
  int   cparent[MM_MAXCLUS];
  int   nclus = 0, stack[MM_MAXCLUS], sp = 0, cur = -1;
  uint32_t nedge = 0;

  while (nd_mm_line(&it, line, sizeof line)) {
    if (flow_skippable(line)) continue;
    if (strncmp(line, "direction", 9) == 0) continue;

    if (strncmp(line, "subgraph", 8) == 0 &&
        (line[8] == 0 || isspace((unsigned char)line[8]))) {
      const char *p = line + 8;
      skip_ws(&p);
      char title[MM_MAXLBL];
      snprintf(title, sizeof title, "%s", p);
      /* `subgraph id [Title]` — the bracketed part is the title, the bare
       * form is both id and title. */
      char *br = strchr(title, '[');
      if (br) {
        char *end = strrchr(br, ']');
        if (end) *end = 0;
        memmove(title, br + 1, strlen(br + 1) + 1);
      }
      trim(title);
      nd_mm_clean_label(title);
      if (nclus < MM_MAXCLUS && sp < MM_MAXCLUS) {
        clabel[nclus] = strdup(title);
        cparent[nclus] = cur;
        stack[sp++] = cur = nclus;
        nclus++;
      }
      continue;
    }
    if (strcmp(line, "end") == 0) {
      if (sp > 0) { sp--; cur = sp > 0 ? stack[sp - 1] : -1; }
      continue;
    }

    /* A statement is a chain: node (link node)*, with `&` for parallel ends. */
    const char *p = line;
    mm_node *prev[16];
    int nprev = 0;
    for (;;) {
      mm_node *e = read_nodespec(&p, &ns, cur);
      if (!e) break;
      if (nprev < 16) prev[nprev++] = e;
      skip_ws(&p);
      if (*p == '&') { p++; continue; }
      break;
    }
    if (!nprev) continue;

    for (;;) {
      mm_link lk;
      const char *save = p;
      if (!read_link(&p, &lk)) { p = save; break; }

      mm_node *next[16];
      int nnext = 0;
      for (;;) {
        mm_node *e = read_nodespec(&p, &ns, cur);
        if (!e) break;
        if (nnext < 16) next[nnext++] = e;
        skip_ws(&p);
        if (*p == '&') { p++; continue; }
        break;
      }
      if (!nnext) break;

      for (int a = 0; a < nprev; a++)
        for (int c = 0; c < nnext; c++) {
          if (nedge++ >= ND_MM_MAX_EDGES) goto too_big;
          emit_edge(&edges, prev[a]->id, next[c]->id, &lk);
        }
      memcpy(prev, next, sizeof next);
      nprev = nnext;
    }
  }

  if (ns.full || ns.n == 0 || !edges.ok) goto too_big;

  {
    nd_buf b = {.ok = true};
    preamble(&b, rankdir, 0.45, 0.45);

    /* Clusters first, innermost written where they are nested, so graphviz
     * sees the containment it needs. */
    for (int c = 0; c < nclus; c++) {
      if (cparent[c] != -1) continue;         /* emitted by its parent */
      int todo[MM_MAXCLUS], nt = 0;
      todo[nt++] = c;
      /* Iterative, so a pathological nesting cannot recurse the stack away. */
      for (int t = 0; t < nt; t++) {
        int k = todo[t];
        nd_buf_fmt(&b, "  subgraph cluster_%d {\n    label=", k);
        nd_mm_dot_str(&b, clabel[k] ? clabel[k] : "");
        nd_buf_fmt(&b, "\n    fontname=\"%s\"\n    fontsize=12\n"
                       "    fontcolor=\"%s\"\n    color=\"%s\"\n"
                       "    style=\"rounded\"\n    penwidth=1.0\n",
                   ND_MM_FONT, ND_MM_DIM, ND_MM_BORDER);
        for (uint32_t i = 0; i < ns.n; i++) {
          if (ns.v[i]->cluster != k) continue;
          nd_buf_puts(&b, "    ");
          nd_mm_dot_str(&b, ns.v[i]->id);
          nd_buf_puts(&b, " [label=");
          nd_mm_dot_str(&b, ns.v[i]->label ? ns.v[i]->label : ns.v[i]->id);
          emit_shape(&b, ns.v[i]->shape);
          nd_buf_puts(&b, "]\n");
        }
        for (int j = 0; j < nclus; j++)
          if (cparent[j] == k && nt < MM_MAXCLUS) todo[nt++] = j;
        nd_buf_puts(&b, "  }\n");
      }
    }

    for (uint32_t i = 0; i < ns.n; i++) {
      if (ns.v[i]->cluster >= 0) continue;
      nd_buf_puts(&b, "  ");
      nd_mm_dot_str(&b, ns.v[i]->id);
      nd_buf_puts(&b, " [label=");
      nd_mm_dot_str(&b, ns.v[i]->label ? ns.v[i]->label : ns.v[i]->id);
      emit_shape(&b, ns.v[i]->shape);
      nd_buf_puts(&b, "]\n");
    }
    nd_buf_add(&b, edges.p ? edges.p : "", edges.len);
    nd_buf_puts(&b, "}\n");

    nd_buf_free(&edges);
    nodes_free(&ns);
    for (int c = 0; c < nclus; c++) free(clabel[c]);
    return nd_buf_take(&b);
  }

too_big:
  nd_buf_free(&edges);
  nodes_free(&ns);
  for (int c = 0; c < nclus; c++) free(clabel[c]);
  return NULL;
}

/* =========================================================================
 * stateDiagram-v2
 * ========================================================================= */

enum { ST_PLAIN, ST_START, ST_END, ST_CHOICE, ST_FORK };

/* `[*]` is a start when it is the source of a transition and an end when it is
 * the target, and each composite state gets its own pair — which is what
 * mermaid draws, and what keeps two unrelated end states from being merged
 * into one circle halfway across the diagram. */
static mm_node *state_marker(mm_nodes *ns, int cluster, bool is_start) {
  char id[MM_MAXID];
  snprintf(id, sizeof id, "__%s_%d", is_start ? "start" : "end", cluster);
  mm_node *e = nodes_get(ns, id);
  if (e) {
    e->flags = is_start ? ST_START : ST_END;
    if (e->cluster < 0) e->cluster = cluster;
  }
  return e;
}

static mm_node *state_ref(const char **p, mm_nodes *ns, int cluster,
                          bool is_source) {
  skip_ws(p);
  if (strncmp(*p, "[*]", 3) == 0) {
    *p += 3;
    return state_marker(ns, cluster, is_source);
  }
  char id[MM_MAXID];
  if (!read_id(p, id, sizeof id)) return NULL;
  mm_node *e = nodes_get(ns, id);
  if (e && e->cluster < 0) e->cluster = cluster;
  return e;
}

static void emit_state_node(nd_buf *b, const mm_node *e) {
  if (e->flags == -1) return;          /* a composite: its cluster is drawn */
  nd_buf_puts(b, "  ");
  nd_mm_dot_str(b, e->id);
  switch (e->flags) {
    case ST_START:
      nd_buf_fmt(b, " [label=\"\", shape=circle, style=filled,"
                    " fillcolor=\"%s\", color=\"%s\","
                    " width=0.20, height=0.20, fixedsize=true]\n",
                 ND_MM_TEXT, ND_MM_TEXT);
      break;
    case ST_END:
      nd_buf_fmt(b, " [label=\"\", shape=doublecircle, style=filled,"
                    " fillcolor=\"%s\", color=\"%s\","
                    " width=0.16, height=0.16, fixedsize=true]\n",
                 ND_MM_TEXT, ND_MM_TEXT);
      break;
    case ST_CHOICE:
      nd_buf_fmt(b, " [label=\"\", shape=diamond, style=filled,"
                    " fillcolor=\"%s\", color=\"%s\","
                    " width=0.34, height=0.34, fixedsize=true]\n",
                 ND_MM_FILL2, ND_MM_DECIDE);
      break;
    case ST_FORK:
      nd_buf_fmt(b, " [label=\"\", shape=box, style=filled,"
                    " fillcolor=\"%s\", color=\"%s\","
                    " width=1.10, height=0.07, fixedsize=true]\n",
                 ND_MM_TEXT, ND_MM_TEXT);
      break;
    default:
      nd_buf_puts(b, " [label=");
      nd_mm_dot_str(b, e->label ? e->label : e->id);
      nd_buf_puts(b, "]\n");
      break;
  }
}

char *nd_mm_state_dot(const char *src, uint32_t len) {
  nd_lines it = {.src = src, .len = len};
  char line[ND_MM_MAX_LINE];
  if (!nd_mm_line(&it, line, sizeof line)) return NULL;

  const char *rankdir = "TB";
  mm_nodes ns = {0};
  nd_buf edges = {.ok = true};
  char *clabel[MM_MAXCLUS] = {0};
  int cparent[MM_MAXCLUS];
  int nclus = 0, stack[MM_MAXCLUS], sp = 0, cur = -1;
  uint32_t nedge = 0;
  bool in_note = false;

  while (nd_mm_line(&it, line, sizeof line)) {
    if (in_note) { if (strcmp(line, "end note") == 0) in_note = false; continue; }
    if (strncmp(line, "note", 4) == 0) {
      /* A single-line note carries its text after a colon; without one the
       * note runs until `end note`. Either way it is not drawn: a floating
       * annotation costs a node and an edge and buys very little. */
      if (!strchr(line, ':')) in_note = true;
      continue;
    }
    if (strncmp(line, "direction", 9) == 0) {
      const char *d = line + 9;
      skip_ws(&d);
      if      (strncmp(d, "LR", 2) == 0) rankdir = "LR";
      else if (strncmp(d, "RL", 2) == 0) rankdir = "RL";
      else if (strncmp(d, "BT", 2) == 0) rankdir = "BT";
      continue;
    }
    if (strncmp(line, "classDef", 8) == 0 || strncmp(line, "class ", 6) == 0 ||
        strncmp(line, "style ", 6) == 0 || strcmp(line, "--") == 0)
      continue;

    if (strcmp(line, "}") == 0) {
      if (sp > 0) { sp--; cur = sp > 0 ? stack[sp - 1] : -1; }
      continue;
    }

    if (strncmp(line, "state ", 6) == 0) {
      const char *p = line + 6;
      skip_ws(&p);

      /* state "long description" as id */
      if (*p == '"') {
        const char *q = strchr(p + 1, '"');
        if (q) {
          char desc[MM_MAXLBL];
          size_t n = (size_t)(q - p - 1);
          if (n > sizeof desc - 1) n = sizeof desc - 1;
          memcpy(desc, p + 1, n); desc[n] = 0;
          nd_mm_clean_label(desc);
          const char *r = q + 1;
          skip_ws(&r);
          if (strncmp(r, "as", 2) == 0) {
            r += 2;
            char id[MM_MAXID];
            if (read_id(&r, id, sizeof id)) {
              mm_node *e = nodes_get(&ns, id);
              if (e && e->cluster < 0) e->cluster = cur;
              node_set_label(e, desc);
            }
          }
          continue;
        }
      }

      char id[MM_MAXID];
      if (!read_id(&p, id, sizeof id)) continue;
      mm_node *e = nodes_get(&ns, id);
      if (e && e->cluster < 0) e->cluster = cur;
      skip_ws(&p);

      if (strncmp(p, "<<", 2) == 0) {
        if      (strncmp(p, "<<choice>>", 10) == 0) { if (e) e->flags = ST_CHOICE; }
        else if (strncmp(p, "<<fork>>", 8) == 0 ||
                 strncmp(p, "<<join>>", 8) == 0)    { if (e) e->flags = ST_FORK; }
        continue;
      }
      if (*p == '{') {
        /* A composite state is a cluster whose label is the state's own. */
        if (nclus < MM_MAXCLUS && sp < MM_MAXCLUS) {
          clabel[nclus] = strdup(e && e->label ? e->label : id);
          cparent[nclus] = cur;
          stack[sp++] = cur = nclus;
          nclus++;
          /* The state itself is not drawn; the cluster stands for it. */
          if (e) e->flags = -1;
        }
        continue;
      }
      continue;
    }

    /* A transition, or a bare `id : description`. */
    const char *arrow = strstr(line, "-->");
    if (!arrow) {
      char *colon = strchr(line, ':');
      if (colon) {
        *colon = 0;
        const char *p = line;
        char id[MM_MAXID];
        if (read_id(&p, id, sizeof id)) {
          mm_node *e = nodes_get(&ns, id);
          if (e && e->cluster < 0) e->cluster = cur;
          char desc[MM_MAXLBL];
          snprintf(desc, sizeof desc, "%s", colon + 1);
          trim(desc);
          nd_mm_clean_label(desc);
          node_set_label(e, desc);
        }
      }
      continue;
    }

    {
      char head[ND_MM_MAX_LINE];
      size_t hn = (size_t)(arrow - line);
      if (hn > sizeof head - 1) hn = sizeof head - 1;
      memcpy(head, line, hn); head[hn] = 0;

      const char *hp = head;
      mm_node *from = state_ref(&hp, &ns, cur, true);

      char tail[ND_MM_MAX_LINE];
      snprintf(tail, sizeof tail, "%s", arrow + 3);
      char *colon = strchr(tail, ':');
      char label[MM_MAXLBL] = {0};
      if (colon) {
        *colon = 0;
        snprintf(label, sizeof label, "%s", colon + 1);
        trim(label);
        nd_mm_clean_label(label);
      }
      const char *tp = tail;
      mm_node *to = state_ref(&tp, &ns, cur, false);

      if (from && to) {
        if (nedge++ >= ND_MM_MAX_EDGES) goto too_big;
        mm_link lk = {.head = 1};
        snprintf(lk.label, sizeof lk.label, "%s", label);
        emit_edge(&edges, from->id, to->id, &lk);
      }
    }
  }

  if (ns.full || ns.n == 0 || !edges.ok) goto too_big;

  {
    nd_buf b = {.ok = true};
    preamble(&b, rankdir, 0.40, 0.42);

    for (int pass = 0; pass < 2; pass++) {
      /* Pass 0 writes the clusters, pass 1 everything left at the top level. */
      for (int c = 0; pass == 0 && c < nclus; c++) {
        if (cparent[c] != -1) continue;
        int todo[MM_MAXCLUS], nt = 0;
        todo[nt++] = c;
        for (int t = 0; t < nt; t++) {
          int k = todo[t];
          nd_buf_fmt(&b, "  subgraph cluster_%d {\n    label=", k);
          nd_mm_dot_str(&b, clabel[k] ? clabel[k] : "");
          nd_buf_fmt(&b, "\n    fontname=\"%s\"\n    fontsize=12\n"
                         "    fontcolor=\"%s\"\n    color=\"%s\"\n"
                         "    style=\"rounded\"\n    penwidth=1.0\n",
                     ND_MM_FONT, ND_MM_DIM, ND_MM_BORDER);
          for (uint32_t i = 0; i < ns.n; i++)
            if (ns.v[i]->cluster == k) {
              nd_buf_puts(&b, "  ");
              emit_state_node(&b, ns.v[i]);
            }
          for (int j = 0; j < nclus; j++)
            if (cparent[j] == k && nt < MM_MAXCLUS) todo[nt++] = j;
          nd_buf_puts(&b, "  }\n");
        }
      }
      for (uint32_t i = 0; pass == 1 && i < ns.n; i++)
        if (ns.v[i]->cluster < 0) emit_state_node(&b, ns.v[i]);
    }

    nd_buf_add(&b, edges.p ? edges.p : "", edges.len);
    nd_buf_puts(&b, "}\n");

    nd_buf_free(&edges);
    nodes_free(&ns);
    for (int c = 0; c < nclus; c++) free(clabel[c]);
    return nd_buf_take(&b);
  }

too_big:
  nd_buf_free(&edges);
  nodes_free(&ns);
  for (int c = 0; c < nclus; c++) free(clabel[c]);
  return NULL;
}

/* =========================================================================
 * classDiagram
 * ========================================================================= */

/* Rendered as HTML-like labels rather than graphviz's own record shape. A
 * record gives boxes with compartments but no rich text inside them, and a
 * class diagram needs both: an abstract method is italic, a static one is
 * underlined, and the class name is bold above a rule. */

static const char *vis_symbol(char c) {
  switch (c) {
    case '+': return "+";
    case '-': return "&#8722;";   /* a real minus: a hyphen reads as a dash */
    case '#': return "#";
    case '~': return "~";
    default:  return NULL;
  }
}

static void class_member(nd_buf *b, const char *raw) {
  char s[MM_MAXLBL];
  snprintf(s, sizeof s, "%s", raw);
  trim(s);
  if (!*s) return;

  const char *vis = vis_symbol(s[0]);
  char *rest = s + (vis ? 1 : 0);
  trim(rest);

  /* `*` is abstract and `$` is static, and mermaid puts either at the very
   * end — after the return type, not after the name. */
  bool abstract = false, statik = false;
  for (size_t n = strlen(rest); n; n = strlen(rest)) {
    if (rest[n - 1] == '*')      { abstract = true; rest[n - 1] = 0; }
    else if (rest[n - 1] == '$') { statik = true;   rest[n - 1] = 0; }
    else break;
  }
  trim(rest);
  if (!*rest) return;

  char body[MM_MAXLBL];
  char *paren = strchr(rest, '(');
  if (paren) {
    char *close = strrchr(rest, ')');
    if (close && close > paren) {
      char ret[MM_MAXLBL];
      snprintf(ret, sizeof ret, "%s", close + 1);
      trim(ret);
      *(close + 1) = 0;
      if (*ret) snprintf(body, sizeof body, "%s : %s", rest, ret);
      else      snprintf(body, sizeof body, "%s", rest);
    } else {
      snprintf(body, sizeof body, "%s", rest);
    }
  } else {
    /* `Type name` is how mermaid writes an attribute and `name : Type` is how
     * it draws one. Only the two-word form is turned around; anything else is
     * left as written rather than guessed at. */
    char *sp = strchr(rest, ' ');
    if (sp) {
      *sp = 0;
      char *name = sp + 1;
      trim(name);
      if (*name && !strchr(name, ' '))
        snprintf(body, sizeof body, "%s : %s", name, rest);
      else
        snprintf(body, sizeof body, "%s %s", rest, name);
    } else {
      snprintf(body, sizeof body, "%s", rest);
    }
  }

  /* mermaid spells generics `~T~`, because `<>` would collide with its own
   * `<<annotation>>` syntax. Turn them back into the brackets a reader
   * expects; the HTML escape below is what keeps them from becoming markup. */
  for (char *t = body; *t; t++) {
    if (*t != '~') continue;
    char *close = strchr(t + 1, '~');
    if (!close) break;
    *t = '<'; *close = '>';
    t = close;
  }

  if (vis) { nd_buf_puts(b, vis); nd_buf_puts(b, " "); }
  if (statik)   nd_buf_puts(b, "<U>");
  if (abstract) nd_buf_puts(b, "<I>");
  nd_mm_html_esc(b, body);
  if (abstract) nd_buf_puts(b, "</I>");
  if (statik)   nd_buf_puts(b, "</U>");
  nd_buf_puts(b, "<BR ALIGN=\"LEFT\"/>");
}

static void emit_class_node(nd_buf *b, const mm_node *e) {
  nd_buf_puts(b, "  ");
  nd_mm_dot_str(b, e->id);
  nd_buf_fmt(b, " [shape=box, margin=0, label=<<TABLE BORDER=\"0\" CELLBORDER=\"1\""
                " CELLSPACING=\"0\" CELLPADDING=\"6\" COLOR=\"%s\">",
             ND_MM_BORDER);

  nd_buf_fmt(b, "<TR><TD BGCOLOR=\"%s\" ALIGN=\"CENTER\">", ND_MM_FILL2);
  if (e->anno && *e->anno) {
    nd_buf_fmt(b, "<FONT POINT-SIZE=\"10\" COLOR=\"%s\">&#171;", ND_MM_DIM);
    nd_mm_html_esc(b, e->anno);
    nd_buf_puts(b, "&#187;</FONT><BR/>");
  }
  nd_buf_puts(b, "<B>");
  nd_mm_html_esc(b, e->label ? e->label : e->id);
  nd_buf_puts(b, "</B></TD></TR>");

  /* Attributes and operations are separate compartments, so the members are
   * partitioned by whether they take an argument list. */
  for (int pass = 0; pass < 2; pass++) {
    nd_buf cell = {.ok = true};
    for (const char *m = e->extra; m && *m;) {
      const char *nl = strchr(m, '\n');
      size_t n = nl ? (size_t)(nl - m) : strlen(m);
      char one[MM_MAXLBL];
      if (n > sizeof one - 1) n = sizeof one - 1;
      memcpy(one, m, n); one[n] = 0;
      bool is_method = strchr(one, '(') != NULL;
      if (is_method == (pass == 1)) class_member(&cell, one);
      m = nl ? nl + 1 : m + strlen(m);
    }
    if (cell.len)
      nd_buf_fmt(b, "<TR><TD BGCOLOR=\"%s\" ALIGN=\"LEFT\" BALIGN=\"LEFT\">%s"
                    "</TD></TR>", ND_MM_FILL, cell.p);
    nd_buf_free(&cell);
  }

  nd_buf_puts(b, "</TABLE>>]\n");
}

enum {
  R_INHERIT, R_INHERIT_R, R_REALIZE, R_REALIZE_R,
  R_COMPOSE, R_COMPOSE_R, R_AGGREG, R_AGGREG_R,
  R_ASSOC, R_ASSOC_R, R_DEP, R_DEP_R, R_LINK, R_LINK_D
};

/* Longest first: `--` appears inside `<|--`, and testing it first would split
 * every inheritance edge in the wrong place. */
static const struct { const char *tok; int kind; } kRel[] = {
  {"<|--", R_INHERIT},  {"<|..", R_REALIZE},
  {"--|>", R_INHERIT_R},{"..|>", R_REALIZE_R},
  {"*--",  R_COMPOSE},  {"--*",  R_COMPOSE_R},
  {"o--",  R_AGGREG},   {"--o",  R_AGGREG_R},
  {"<--",  R_ASSOC_R},  {"-->",  R_ASSOC},
  {"<..",  R_DEP_R},    {"..>",  R_DEP},
  {"--",   R_LINK},     {"..",   R_LINK_D},
};

/* Strips a leading or trailing `"multiplicity"` off one side of a relation. */
static void split_mult(char *side, char *mult, size_t cap, bool trailing) {
  mult[0] = 0;
  trim(side);
  size_t n = strlen(side);
  if (!n) return;
  if (trailing && side[n - 1] == '"') {
    char *open = memrchr(side, '"', n - 1);
    if (open) {
      size_t mn = (size_t)(side + n - 1 - open - 1);
      if (mn > cap - 1) mn = cap - 1;
      memcpy(mult, open + 1, mn); mult[mn] = 0;
      *open = 0;
    }
  } else if (!trailing && side[0] == '"') {
    char *close = strchr(side + 1, '"');
    if (close) {
      size_t mn = (size_t)(close - side - 1);
      if (mn > cap - 1) mn = cap - 1;
      memcpy(mult, side + 1, mn); mult[mn] = 0;
      memmove(side, close + 1, strlen(close + 1) + 1);
    }
  }
  trim(side);
  nd_mm_clean_label(mult);
}

static void emit_class_edge(nd_buf *b, int kind, const char *a, const char *bb,
                            const char *ma, const char *mb, const char *label) {
  /* Half the relations are the same edge read the other way round, so they are
   * normalised by swapping the ends — which also puts the parent class above
   * the child, since dot ranks by edge direction. */
  bool swap = kind == R_INHERIT_R || kind == R_REALIZE_R ||
              kind == R_COMPOSE_R || kind == R_AGGREG_R || kind == R_ASSOC_R ||
              kind == R_DEP_R;
  if (swap) {
    const char *t = a; a = bb; bb = t;
    const char *m = ma; ma = mb; mb = m;
    switch (kind) {
      case R_INHERIT_R: kind = R_INHERIT; break;
      case R_REALIZE_R: kind = R_REALIZE; break;
      case R_COMPOSE_R: kind = R_COMPOSE; break;
      case R_AGGREG_R:  kind = R_AGGREG;  break;
      case R_ASSOC_R:   kind = R_ASSOC;   break;
      default:          kind = R_DEP;     break;
    }
  }

  nd_buf_puts(b, "  ");
  nd_mm_dot_str(b, a);
  nd_buf_puts(b, " -> ");
  nd_mm_dot_str(b, bb);
  nd_buf_puts(b, " [");
  switch (kind) {
    /* dir=back draws the decoration at the TAIL, which is where UML puts the
     * triangle and the diamond: at the parent and at the whole, not at the
     * child and the part. */
    case R_INHERIT: nd_buf_puts(b, "dir=back, arrowtail=empty, "); break;
    case R_REALIZE: nd_buf_puts(b, "dir=back, arrowtail=empty, style=dashed, "); break;
    case R_COMPOSE: nd_buf_puts(b, "dir=back, arrowtail=diamond, "); break;
    case R_AGGREG:  nd_buf_puts(b, "dir=back, arrowtail=odiamond, "); break;
    case R_ASSOC:   nd_buf_puts(b, "arrowhead=vee, "); break;
    case R_DEP:     nd_buf_puts(b, "arrowhead=vee, style=dashed, "); break;
    case R_LINK_D:  nd_buf_puts(b, "arrowhead=none, style=dashed, "); break;
    default:        nd_buf_puts(b, "arrowhead=none, "); break;
  }
  if (label && *label) {
    nd_buf_puts(b, "label="); nd_mm_dot_str(b, label); nd_buf_puts(b, ", ");
  }
  if (ma && *ma) {
    nd_buf_puts(b, "taillabel="); nd_mm_dot_str(b, ma); nd_buf_puts(b, ", ");
  }
  if (mb && *mb) {
    nd_buf_puts(b, "headlabel="); nd_mm_dot_str(b, mb); nd_buf_puts(b, ", ");
  }
  nd_buf_puts(b, "labeldistance=2.4, labelangle=25, labelfontsize=10]\n");
}

char *nd_mm_class_dot(const char *src, uint32_t len) {
  nd_lines it = {.src = src, .len = len};
  char line[ND_MM_MAX_LINE];
  if (!nd_mm_line(&it, line, sizeof line)) return NULL;

  mm_nodes ns = {0};
  nd_buf edges = {.ok = true};
  mm_node *open_class = NULL;
  uint32_t nedge = 0, nnote = 0;
  char *note_for[MM_MAXNOTE] = {0}, *note_txt[MM_MAXNOTE] = {0};

  while (nd_mm_line(&it, line, sizeof line)) {
    if (open_class) {
      if (strcmp(line, "}") == 0) { open_class = NULL; continue; }
      if (strncmp(line, "<<", 2) == 0) {
        char a[MM_MAXID];
        snprintf(a, sizeof a, "%s", line + 2);
        char *e = strstr(a, ">>");
        if (e) *e = 0;
        free(open_class->anno);
        open_class->anno = strdup(a);
        continue;
      }
      nd_buf mb = {.ok = true};
      if (open_class->extra) { nd_buf_puts(&mb, open_class->extra);
                               nd_buf_puts(&mb, "\n"); }
      nd_buf_puts(&mb, line);
      free(open_class->extra);
      open_class->extra = nd_buf_take(&mb);
      continue;
    }

    if (strncmp(line, "direction", 9) == 0 || strncmp(line, "style", 5) == 0 ||
        strncmp(line, "cssClass", 8) == 0 || strncmp(line, "click", 5) == 0 ||
        strncmp(line, "link", 4) == 0 || strncmp(line, "callback", 8) == 0 ||
        strncmp(line, "namespace", 9) == 0 || strcmp(line, "}") == 0)
      continue;

    if (strncmp(line, "note", 4) == 0) {
      /* `note for X "text"` — a real annotation attached to a class, which is
       * worth drawing; the unattached `note "text"` form has nowhere to go.
       * Held until the end rather than emitted here, because mermaid lets the
       * note come BEFORE the class it is for, and the fixture does exactly
       * that: checking against the classes seen so far silently dropped it. */
      const char *p = line + 4;
      skip_ws(&p);
      if (strncmp(p, "for", 3) != 0) continue;
      p += 3;
      char target[MM_MAXID];
      if (!read_id(&p, target, sizeof target)) continue;
      skip_ws(&p);
      char text[MM_MAXLBL];
      snprintf(text, sizeof text, "%s", p);
      trim(text);
      nd_mm_clean_label(text);
      if (!*text || nnote >= MM_MAXNOTE) continue;
      note_for[nnote] = strdup(target);
      note_txt[nnote] = strdup(text);
      nnote++;
      continue;
    }

    if (strncmp(line, "class ", 6) == 0) {
      const char *p = line + 6;
      char id[MM_MAXID];
      if (!read_id(&p, id, sizeof id)) continue;
      mm_node *e = nodes_get(&ns, id);
      if (!e) goto too_big;
      skip_ws(&p);
      if (*p == '[') {                       /* class X["Display Name"] */
        const char *close = strrchr(p, ']');
        if (close) {
          char lbl[MM_MAXLBL];
          size_t n = (size_t)(close - p - 1);
          if (n > sizeof lbl - 1) n = sizeof lbl - 1;
          memcpy(lbl, p + 1, n); lbl[n] = 0;
          trim(lbl);
          nd_mm_clean_label(lbl);
          node_set_label(e, lbl);
          p = close + 1;
          skip_ws(&p);
        }
      }
      if (*p == '{') open_class = e;
      continue;
    }

    /* A relationship, or the one-line member form `Class : +member`. */
    int kind = -1;
    size_t at = 0, toklen = 0;
    for (size_t i = 0; line[i] && kind < 0; i++)
      for (size_t r = 0; r < sizeof kRel / sizeof *kRel; r++) {
        size_t tl = strlen(kRel[r].tok);
        if (strncmp(line + i, kRel[r].tok, tl) == 0) {
          kind = kRel[r].kind; at = i; toklen = tl;
          break;
        }
      }

    if (kind < 0) {
      char *colon = strchr(line, ':');
      if (!colon) continue;
      *colon = 0;
      const char *p = line;
      char id[MM_MAXID];
      if (!read_id(&p, id, sizeof id)) continue;
      mm_node *e = nodes_get(&ns, id);
      if (!e) goto too_big;
      char *member = colon + 1;
      trim(member);
      if (strncmp(member, "<<", 2) == 0) {
        char *end = strstr(member, ">>");
        if (end) *end = 0;
        free(e->anno);
        e->anno = strdup(member + 2);
        continue;
      }
      nd_buf mb = {.ok = true};
      if (e->extra) { nd_buf_puts(&mb, e->extra); nd_buf_puts(&mb, "\n"); }
      nd_buf_puts(&mb, member);
      free(e->extra);
      e->extra = nd_buf_take(&mb);
      continue;
    }

    {
      char left[ND_MM_MAX_LINE], right[ND_MM_MAX_LINE], label[MM_MAXLBL] = {0};
      size_t ln = at;
      if (ln > sizeof left - 1) ln = sizeof left - 1;
      memcpy(left, line, ln); left[ln] = 0;
      snprintf(right, sizeof right, "%s", line + at + toklen);

      char *colon = strchr(right, ':');
      if (colon) {
        *colon = 0;
        snprintf(label, sizeof label, "%s", colon + 1);
        trim(label);
        nd_mm_clean_label(label);
      }

      char ma[64], mb2[64];
      split_mult(left,  ma,  sizeof ma,  true);
      split_mult(right, mb2, sizeof mb2, false);

      const char *lp = left, *rp = right;
      char aid[MM_MAXID], bid[MM_MAXID];
      if (!read_id(&lp, aid, sizeof aid) || !read_id(&rp, bid, sizeof bid))
        continue;
      if (!nodes_get(&ns, aid) || !nodes_get(&ns, bid)) goto too_big;
      if (nedge++ >= ND_MM_MAX_EDGES) goto too_big;
      emit_class_edge(&edges, kind, aid, bid, ma, mb2, label);
    }
  }

  for (uint32_t i = 0; i < nnote; i++) {
    if (!note_for[i] || !note_txt[i] || !nodes_find(&ns, note_for[i])) continue;
    nd_buf_fmt(&edges, "  __note%u [shape=note, style=filled,"
                       " fillcolor=\"%s\", color=\"%s\", fontsize=10,"
                       " fontcolor=\"%s\", margin=\"0.14,0.09\", label=",
               i, ND_MM_FILL2, ND_MM_BORDER, ND_MM_DIM);
    nd_mm_dot_str(&edges, note_txt[i]);
    nd_buf_puts(&edges, "]\n  ");
    nd_mm_dot_str(&edges, note_for[i]);
    nd_buf_fmt(&edges, " -> __note%u [style=dashed, arrowhead=none,"
                       " color=\"%s\", constraint=false]\n",
               i, ND_MM_BORDER);
    /* Same rank as its class, or the unconstrained edge lets the note drift
     * to wherever there happens to be room and trails a leader across the
     * diagram to get back. */
    nd_buf_puts(&edges, "  { rank=same; ");
    nd_mm_dot_str(&edges, note_for[i]);
    nd_buf_fmt(&edges, "; __note%u }\n", i);
  }

  if (ns.full || ns.n == 0 || !edges.ok) goto too_big;

  {
    nd_buf b = {.ok = true};
    preamble(&b, "TB", 0.55, 0.85);
    for (uint32_t i = 0; i < ns.n; i++) emit_class_node(&b, ns.v[i]);
    nd_buf_add(&b, edges.p ? edges.p : "", edges.len);
    nd_buf_puts(&b, "}\n");
    nd_buf_free(&edges);
    nodes_free(&ns);
    for (uint32_t i = 0; i < nnote; i++) { free(note_for[i]); free(note_txt[i]); }
    return nd_buf_take(&b);
  }

too_big:
  nd_buf_free(&edges);
  nodes_free(&ns);
  for (uint32_t i = 0; i < nnote; i++) { free(note_for[i]); free(note_txt[i]); }
  return NULL;
}
