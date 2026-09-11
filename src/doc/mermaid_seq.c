/* mermaid_seq.c — sequenceDiagram, laid out and drawn directly.
 *
 * No graph layout engine appears in this file, and that is the point. A
 * sequence diagram is not a graph waiting to be untangled: participants are
 * columns in the order they are introduced and messages are rows in the order
 * they are written, so both axes are already decided by the source. The single
 * quantity that cannot be read off the text is how wide the text is, and Pango
 * answers that — which is precisely the capability mermaid needs a browser for.
 *
 * The work is therefore arithmetic, and it is retained: layouts are built once
 * and painting walks the result. That keeps the diagram scale-invariant for
 * free, exactly like the rest of the document, so it stays crisp at any
 * display scale with no raster anywhere.
 *
 * Column widths come from the MESSAGE labels as well as the participant names.
 * Sizing columns to the names alone is the obvious thing and it is wrong: the
 * first version did that, and "Query User Records" was drawn straight through
 * the Database lifeline. Labels also carry an opaque backing, because a
 * message spanning three columns still has to cross the one in the middle.
 */

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "doc/mermaid_int.h"
#include "ui/theme.h"

#define SQ_MARGIN     22.0
#define SQ_BOX_H      34.0
#define SQ_BOX_MINW   92.0
#define SQ_PAD_X      14.0
#define SQ_GAP        34.0    /* clear space between two participant boxes */
#define SQ_ROW        36.0
#define SQ_LBL_PAD    12.0    /* clear space each side of a message label */
#define SQ_SELF_W     32.0
#define SQ_ACT_W      10.0
#define SQ_NOTE_MAXW 220.0
#define SQ_HEAD_GAP   22.0    /* lifeline top to the first row */

#define SQ_FS_PART    13.0
#define SQ_FS_MSG     11.0
#define SQ_FS_KW      10.0
#define SQ_FS_NOTE    10.5

enum { EV_MSG, EV_ACT, EV_DEACT, EV_FRAME, EV_SECTION, EV_END, EV_NOTE };
enum { NOTE_OVER, NOTE_RIGHT, NOTE_LEFT };
/* Arrowheads, in mermaid's own vocabulary: `>>` filled, `>` open, `x` a cross,
 * `)` the async half-head, and nothing at all for a bare line. */
enum { HEAD_OPEN, HEAD_FILLED, HEAD_CROSS, HEAD_ASYNC, HEAD_NONE };

struct sq_part {
  char        *id, *label;
  bool         is_actor;
  double       x, w;
  PangoLayout *pl;
  double       lw, lh;
  /* Sorted, merged y-intervals where a message label crosses this lifeline.
   * The line is drawn around them instead of through them. */
  double      *gap;
  uint32_t     ngap;   /* pairs, so 2*ngap doubles */
};

struct sq_ev {
  int    kind;
  int    from, to;
  int    dashed, head;
  int    act;            /* +1 activate the target, -1 deactivate the source */
  bool   self;
  int    num;            /* autonumber, 0 for none */
  int    note_pos, note_lo, note_hi;
  int    match;          /* frame <-> end */
  int    depth;
  char  *text, *kw;      /* held between parsing and measuring, then freed */
  PangoLayout *pl,  *kwl;
  double lw, lh, kw_w, kw_h;
  double y, y2, h;
};

struct nd_seq {
  struct sq_part *p;
  uint32_t        np;
  struct sq_ev   *e;
  uint32_t        ne;
  double          w, h, life_top, life_bot;
};

/* ---- parsing ----------------------------------------------------------- */

static char *dupz(const char *s) { char *d = strdup(s ? s : ""); return d; }

static void sq_trim(char *s) {
  size_t n = strlen(s);
  while (n && isspace((unsigned char)s[n - 1])) s[--n] = 0;
  size_t i = 0;
  while (s[i] && isspace((unsigned char)s[i])) i++;
  if (i) memmove(s, s + i, n - i + 1);
}

static int part_index(struct nd_seq *s, const char *id) {
  char key[128];
  snprintf(key, sizeof key, "%s", id);
  sq_trim(key);
  if (!*key) return -1;
  for (uint32_t i = 0; i < s->np; i++)
    if (strcmp(s->p[i].id, key) == 0) return (int)i;
  if (s->np >= ND_MM_MAX_PARTS) return -1;
  struct sq_part *v = realloc(s->p, (s->np + 1) * sizeof *v);
  if (!v) return -1;
  s->p = v;
  struct sq_part *e = &s->p[s->np];
  memset(e, 0, sizeof *e);
  e->id = dupz(key);
  e->label = dupz(key);
  return (int)s->np++;
}

static struct sq_ev *ev_new(struct nd_seq *s, int kind) {
  if (s->ne >= ND_MM_MAX_EVENTS) return NULL;
  struct sq_ev *v = realloc(s->e, (s->ne + 1) * sizeof *v);
  if (!v) return NULL;
  s->e = v;
  struct sq_ev *e = &s->e[s->ne++];
  memset(e, 0, sizeof *e);
  e->kind = kind;
  e->match = -1;
  e->from = e->to = -1;
  return e;
}

static const struct { const char *tok; int dashed, head; } kArrows[] = {
  {"-->>", 1, HEAD_FILLED}, {"->>", 0, HEAD_FILLED},
  {"--x",  1, HEAD_CROSS},  {"-x",  0, HEAD_CROSS},
  {"--)",  1, HEAD_ASYNC},  {"-)",  0, HEAD_ASYNC},
  {"-->",  1, HEAD_OPEN},   {"->",  0, HEAD_OPEN},
  {"--",   1, HEAD_NONE},   {"-",   0, HEAD_NONE},
};

/* Frame keywords: everything that opens a labelled box closed by `end`. */
static const char *kFrames[] = {"alt", "opt", "loop", "par", "critical",
                                "break", "rect", "box", NULL};
static const char *kSections[] = {"else", "and", "option", NULL};

static bool word_is(const char *s, const char *w, const char **rest) {
  size_t n = strlen(w);
  if (strncmp(s, w, n) != 0) return false;
  if (s[n] && !isspace((unsigned char)s[n])) return false;
  if (rest) { const char *p = s + n; while (*p == ' ' || *p == '\t') p++;
              *rest = p; }
  return true;
}

static void seq_parse(struct nd_seq *s, const char *src, uint32_t len) {
  nd_lines it = {.src = src, .len = len};
  char line[ND_MM_MAX_LINE];
  bool autonum = false;
  int  seq = 0, depth = 0;
  int  stack[32], sp = 0;

  nd_mm_line(&it, line, sizeof line);        /* the `sequenceDiagram` header */

  while (nd_mm_line(&it, line, sizeof line)) {
    const char *rest;

    if (strcmp(line, "autonumber") == 0) { autonum = true; continue; }
    if (word_is(line, "title", &rest) || word_is(line, "accTitle", &rest) ||
        word_is(line, "accDescr", &rest) || word_is(line, "links", &rest) ||
        word_is(line, "link", &rest) || word_is(line, "properties", &rest))
      continue;

    if (word_is(line, "participant", &rest) || word_is(line, "actor", &rest)) {
      bool actor = line[0] == 'a';
      char buf[256];
      snprintf(buf, sizeof buf, "%s", rest);
      char *as = strstr(buf, " as ");
      char id[128];
      if (as) snprintf(id, sizeof id, "%.*s", (int)(as - buf), buf);
      else    snprintf(id, sizeof id, "%s", buf);
      int i = part_index(s, id);
      if (i < 0) continue;
      if (as) {
        char lbl[192];
        snprintf(lbl, sizeof lbl, "%s", as + 4);
        sq_trim(lbl);
        nd_mm_clean_label(lbl);
        free(s->p[i].label);
        s->p[i].label = dupz(lbl);
      }
      s->p[i].is_actor = actor;
      continue;
    }
    /* `create`/`destroy participant X` — the participant is real even though
     * the lifeline's start and end are not drawn. */
    if (word_is(line, "create", &rest) || word_is(line, "destroy", &rest)) {
      const char *r2;
      if (word_is(rest, "participant", &r2) || word_is(rest, "actor", &r2))
        part_index(s, r2);
      else
        part_index(s, rest);
      continue;
    }

    if (word_is(line, "activate", &rest) || word_is(line, "deactivate", &rest)) {
      int i = part_index(s, rest);
      if (i < 0) continue;
      struct sq_ev *e = ev_new(s, line[0] == 'a' ? EV_ACT : EV_DEACT);
      if (!e) return;
      e->from = i;
      e->depth = depth;
      continue;
    }

    if (g_ascii_strncasecmp(line, "note", 4) == 0 &&
        (line[4] == 0 || isspace((unsigned char)line[4]))) {
      const char *p = line + 4;
      while (*p == ' ') p++;
      int pos = NOTE_OVER;
      if      (word_is(p, "over", &rest))     { pos = NOTE_OVER;  p = rest; }
      else if (word_is(p, "right", &rest))    { pos = NOTE_RIGHT; p = rest;
                                                if (word_is(p, "of", &rest)) p = rest; }
      else if (word_is(p, "left", &rest))     { pos = NOTE_LEFT;  p = rest;
                                                if (word_is(p, "of", &rest)) p = rest; }
      const char *colon = strchr(p, ':');
      if (!colon) continue;
      char who[192];
      snprintf(who, sizeof who, "%.*s", (int)(colon - p), p);
      char *comma = strchr(who, ',');
      int lo, hi;
      if (comma) { *comma = 0; lo = part_index(s, who); hi = part_index(s, comma + 1); }
      else       { lo = hi = part_index(s, who); }
      if (lo < 0 || hi < 0) continue;
      if (lo > hi) { int t = lo; lo = hi; hi = t; }

      struct sq_ev *e = ev_new(s, EV_NOTE);
      if (!e) return;
      e->note_pos = pos; e->note_lo = lo; e->note_hi = hi; e->depth = depth;
      char txt[512];
      snprintf(txt, sizeof txt, "%s", colon + 1);
      sq_trim(txt);
      nd_mm_clean_label(txt);
      e->text = dupz(txt);
      continue;
    }

    {
      bool handled = false;
      for (int f = 0; kFrames[f] && !handled; f++) {
        if (!word_is(line, kFrames[f], &rest)) continue;
        struct sq_ev *e = ev_new(s, EV_FRAME);
        if (!e) return;
        e->depth = depth++;
        if (sp < 32) stack[sp++] = (int)(s->ne - 1);
        char txt[512];
        snprintf(txt, sizeof txt, "%s", rest);
        sq_trim(txt);
        nd_mm_clean_label(txt);
        /* `rect rgb(...)` names a colour, not a caption, and the theme is
         * fixed — so the box is drawn and the colour is dropped. */
        if (strcmp(kFrames[f], "rect") == 0) txt[0] = 0;
        e->text = dupz(txt);
        e->kw   = dupz(kFrames[f]);
        handled = true;
      }
      if (handled) continue;

      for (int f = 0; kSections[f] && !handled; f++) {
        if (!word_is(line, kSections[f], &rest)) continue;
        struct sq_ev *e = ev_new(s, EV_SECTION);
        if (!e) return;
        e->depth = depth > 0 ? depth - 1 : 0;
        char txt[512];
        snprintf(txt, sizeof txt, "%s", rest);
        sq_trim(txt);
        nd_mm_clean_label(txt);
        e->text = dupz(txt);
        e->kw   = dupz(kSections[f]);
        handled = true;
      }
      if (handled) continue;
    }

    if (strcmp(line, "end") == 0) {
      struct sq_ev *e = ev_new(s, EV_END);
      if (!e) return;
      if (depth > 0) depth--;
      e->depth = depth;
      if (sp > 0) {
        int open = stack[--sp];
        e->match = open;
        s->e[open].match = (int)(s->ne - 1);
      }
      continue;
    }

    /* A message. The arrow is looked for only BEFORE the colon, so a hyphen
     * in the message text cannot be read as one. */
    {
      const char *colon = strchr(line, ':');
      size_t limit = colon ? (size_t)(colon - line) : strlen(line);
      int at = -1, ai = -1;
      for (size_t i = 0; i < limit && at < 0; i++)
        for (size_t a = 0; a < sizeof kArrows / sizeof *kArrows; a++) {
          size_t tl = strlen(kArrows[a].tok);
          if (i + tl <= limit &&
              strncmp(line + i, kArrows[a].tok, tl) == 0) {
            at = (int)i; ai = (int)a; break;
          }
        }
      if (at < 0) continue;

      char lhs[128];
      snprintf(lhs, sizeof lhs, "%.*s", at, line);
      const char *rp = line + at + strlen(kArrows[ai].tok);
      char rhs[128];
      snprintf(rhs, sizeof rhs, "%.*s", (int)(limit - (size_t)(rp - line)), rp);
      sq_trim(rhs);

      /* `+` activates the target, `-` deactivates the sender. */
      int act = 0;
      if (rhs[0] == '+') { act = 1;  memmove(rhs, rhs + 1, strlen(rhs)); }
      else if (rhs[0] == '-') { act = -1; memmove(rhs, rhs + 1, strlen(rhs)); }
      sq_trim(rhs);

      /* Resolved in order and checked in between: `->>Z: x` has no sender, and
       * looking Z up anyway would conjure a column for a line we then drop. */
      int from = part_index(s, lhs);
      if (from < 0) continue;
      int to = part_index(s, rhs);
      if (to < 0) continue;

      struct sq_ev *e = ev_new(s, EV_MSG);
      if (!e) return;
      e->from = from; e->to = to;
      e->dashed = kArrows[ai].dashed;
      e->head = kArrows[ai].head;
      e->act = act;
      e->self = from == to;
      e->depth = depth;
      if (autonum) e->num = ++seq;

      char txt[512] = {0};
      if (colon) {
        snprintf(txt, sizeof txt, "%s", colon + 1);
        sq_trim(txt);
        nd_mm_clean_label(txt);
      }
      if (e->num) {
        char num[544];
        snprintf(num, sizeof num, "%d. %s", e->num, txt);
        e->text = dupz(num);
      } else {
        e->text = dupz(txt);
      }
    }
  }
}

/* ---- measuring --------------------------------------------------------- */

/* Every layout is built here and nowhere else, so freeing is one walk and a
 * paint never allocates. The parser parks plain strings in the layout fields;
 * this turns each of them into the real thing. */
static PangoLayout *mk(PangoContext *pctx, char *owned_text, double px,
                       bool bold, double wrap_w, double *w, double *h) {
  PangoLayout *l = pango_layout_new(pctx);
  PangoFontDescription *fd = pango_font_description_new();
  pango_font_description_set_family(fd, ND_SANS);
  pango_font_description_set_absolute_size(fd, px * PANGO_SCALE);
  if (bold) pango_font_description_set_weight(fd, PANGO_WEIGHT_BOLD);
  pango_layout_set_font_description(l, fd);
  pango_font_description_free(fd);
  pango_layout_set_text(l, owned_text ? owned_text : "", -1);
  if (wrap_w > 0) {
    pango_layout_set_width(l, (int)(wrap_w * PANGO_SCALE));
    pango_layout_set_wrap(l, PANGO_WRAP_WORD_CHAR);
  }
  int pw, ph;
  pango_layout_get_pixel_size(l, &pw, &ph);
  *w = pw; *h = ph;
  free(owned_text);
  return l;
}

static void seq_measure(struct nd_seq *s, PangoContext *pctx) {
  for (uint32_t i = 0; i < s->np; i++) {
    struct sq_part *p = &s->p[i];
    p->pl = mk(pctx, dupz(p->label), SQ_FS_PART, true, 0, &p->lw, &p->lh);
    p->w = p->lw + 2 * SQ_PAD_X;
    if (p->w < SQ_BOX_MINW) p->w = SQ_BOX_MINW;
  }
  for (uint32_t i = 0; i < s->ne; i++) {
    struct sq_ev *e = &s->e[i];
    char *txt = e->text, *kw = e->kw;
    e->text = e->kw = NULL;
    switch (e->kind) {
      case EV_MSG:
        e->pl = mk(pctx, txt, SQ_FS_MSG, false, 0, &e->lw, &e->lh);
        break;
      case EV_NOTE:
        e->pl = mk(pctx, txt, SQ_FS_NOTE, false, SQ_NOTE_MAXW, &e->lw, &e->lh);
        break;
      case EV_FRAME:
      case EV_SECTION:
        e->kwl = mk(pctx, kw, SQ_FS_KW, true, 0, &e->kw_w, &e->kw_h);
        e->pl  = mk(pctx, txt, SQ_FS_MSG, false, 0, &e->lw, &e->lh);
        break;
      default:
        free(txt); free(kw);
        break;
    }
  }
}

/* ---- horizontal layout ------------------------------------------------- */

static void seq_columns(struct nd_seq *s) {
  uint32_t n = s->np;
  double *step = calloc(n ? n : 1, sizeof *step);
  if (!step) return;

  for (uint32_t i = 0; i + 1 < n; i++)
    step[i] = (s->p[i].w + s->p[i + 1].w) / 2 + SQ_GAP;

  double right_extra = 0, left_extra = 0;

  /* Widen the columns until every label fits between the two lifelines it
   * spans. Only growth happens here, so satisfying one constraint can never
   * break another and a single pass is enough. */
  for (uint32_t k = 0; k < s->ne; k++) {
    struct sq_ev *e = &s->e[k];

    if (e->kind == EV_MSG && !e->self) {
      uint32_t i = (uint32_t)(e->from < e->to ? e->from : e->to);
      uint32_t j = (uint32_t)(e->from < e->to ? e->to : e->from);
      double need = e->lw + 2 * SQ_LBL_PAD, span = 0;
      for (uint32_t t = i; t < j; t++) span += step[t];
      if (span < need && j > i) {
        double add = (need - span) / (double)(j - i);
        for (uint32_t t = i; t < j; t++) step[t] += add;
      }
    } else if (e->kind == EV_MSG && e->self) {
      /* A self-message loops out to the right and hangs its label past that. */
      uint32_t i = (uint32_t)e->from;
      double need = SQ_SELF_W + e->lw + 2 * SQ_LBL_PAD;
      if (i + 1 < n) {
        double want = need + s->p[i + 1].w / 2;
        if (step[i] < want) step[i] = want;
      } else if (need > right_extra) {
        right_extra = need;
      }
    } else if (e->kind == EV_NOTE) {
      double need = e->lw + 24;
      uint32_t i = (uint32_t)e->note_lo, j = (uint32_t)e->note_hi;
      if (e->note_pos == NOTE_OVER && j > i) {
        double span = 0;
        for (uint32_t t = i; t < j; t++) span += step[t];
        if (span < need) {
          double add = (need - span) / (double)(j - i);
          for (uint32_t t = i; t < j; t++) step[t] += add;
        }
      } else if (e->note_pos == NOTE_RIGHT) {
        double want = need + 16;
        if (i + 1 < n) { if (step[i] < want + s->p[i + 1].w / 2)
                           step[i] = want + s->p[i + 1].w / 2; }
        else if (want > right_extra) right_extra = want;
      } else if (e->note_pos == NOTE_LEFT) {
        /* Left of the FIRST column there are no columns to widen, only margin
         * — and without this the note was simply drawn off the page. */
        double want = need + 16;
        if (i > 0) { if (step[i - 1] < want + s->p[i - 1].w / 2)
                       step[i - 1] = want + s->p[i - 1].w / 2; }
        else if (want > left_extra) left_extra = want;
      }
    }
  }

  double x = SQ_MARGIN + left_extra + (n ? s->p[0].w / 2 : 0);
  for (uint32_t i = 0; i < n; i++) {
    s->p[i].x = x;
    if (i + 1 < n) x += step[i];
  }
  s->w = (n ? s->p[n - 1].x + s->p[n - 1].w / 2 : 0) + right_extra + SQ_MARGIN;
  free(step);
}

/* ---- vertical layout --------------------------------------------------- */

static void seq_rows(struct nd_seq *s) {
  s->life_top = SQ_MARGIN + SQ_BOX_H;
  double y = s->life_top + SQ_HEAD_GAP;

  for (uint32_t i = 0; i < s->ne; i++) {
    struct sq_ev *e = &s->e[i];
    switch (e->kind) {
      case EV_MSG: {
        /* The label sits above the arrow, so the row has to be tall enough for
         * it even when it wrapped; a self-message also needs its loop. */
        double need = e->lh + 16;
        if (e->self) need += 20;
        double h = need > SQ_ROW ? need : SQ_ROW;
        y += h;
        e->y = y;
        break;
      }
      case EV_FRAME:
        y += 10;
        e->y = y;
        y += (e->kw_h > 14 ? e->kw_h : 14) + 10;
        break;
      case EV_SECTION:
        y += 8;
        e->y = y;
        y += (e->kw_h > 14 ? e->kw_h : 14) + 8;
        break;
      case EV_END:
        y += 12;
        e->y = y;
        if (e->match >= 0) s->e[e->match].y2 = y;
        y += 6;
        break;
      case EV_NOTE:
        y += 10;
        e->y = y;
        e->h = e->lh + 16;
        y += e->h + 6;
        break;
      default:                  /* activate / deactivate take no height */
        e->y = y;
        break;
    }
  }

  s->life_bot = y + 22;
  s->h = s->life_bot + SQ_BOX_H + SQ_MARGIN;
}

/* ---- lifeline gaps ------------------------------------------------------ */

/* A label centred between two lifelines still crosses every lifeline BETWEEN
 * them — "Query User Records" runs straight through the Database column. The
 * first fix was an opaque rectangle behind the label, which works and quietly
 * assumes the page colour: inside a callout the patch showed. Breaking the
 * lifeline instead assumes nothing, and is what a person drawing this by hand
 * would do. */
static void gap_add(struct sq_part *p, double y0, double y1) {
  double *g = realloc(p->gap, (p->ngap + 1) * 2 * sizeof *g);
  if (!g) return;
  p->gap = g;
  p->gap[p->ngap * 2]     = y0;
  p->gap[p->ngap * 2 + 1] = y1;
  p->ngap++;
}

static void seq_gaps(struct nd_seq *s) {
  for (uint32_t k = 0; k < s->ne; k++) {
    const struct sq_ev *e = &s->e[k];
    if (e->kind != EV_MSG || e->lw <= 0) continue;

    double lx, ly = e->y - e->lh - 6;
    if (e->self) lx = s->p[e->from].x + SQ_SELF_W + SQ_LBL_PAD;
    else         lx = (s->p[e->from].x + s->p[e->to].x) / 2 - e->lw / 2;
    if (e->self) ly = e->y - 16 - 2;

    for (uint32_t i = 0; i < s->np; i++)
      if (s->p[i].x >= lx - 5 && s->p[i].x <= lx + e->lw + 5)
        gap_add(&s->p[i], ly - 2, ly + e->lh + 2);
  }

  /* Sorted and merged, so drawing is one walk. Insertion sort: the counts here
   * are a handful per lifeline. */
  for (uint32_t i = 0; i < s->np; i++) {
    struct sq_part *p = &s->p[i];
    for (uint32_t a = 1; a < p->ngap; a++)
      for (uint32_t b = a; b > 0 && p->gap[b * 2] < p->gap[(b - 1) * 2]; b--) {
        double t0 = p->gap[b * 2], t1 = p->gap[b * 2 + 1];
        p->gap[b * 2]     = p->gap[(b - 1) * 2];
        p->gap[b * 2 + 1] = p->gap[(b - 1) * 2 + 1];
        p->gap[(b - 1) * 2]     = t0;
        p->gap[(b - 1) * 2 + 1] = t1;
      }
    uint32_t w = 0;
    for (uint32_t a = 0; a < p->ngap; a++) {
      if (w > 0 && p->gap[a * 2] <= p->gap[(w - 1) * 2 + 1]) {
        if (p->gap[a * 2 + 1] > p->gap[(w - 1) * 2 + 1])
          p->gap[(w - 1) * 2 + 1] = p->gap[a * 2 + 1];
      } else {
        p->gap[w * 2]     = p->gap[a * 2];
        p->gap[w * 2 + 1] = p->gap[a * 2 + 1];
        w++;
      }
    }
    p->ngap = w;
  }
}

/* ---- build ------------------------------------------------------------- */

struct nd_seq *nd_mm_seq_build(PangoContext *pctx, const char *src,
                               uint32_t len) {
  struct nd_seq *s = calloc(1, sizeof *s);
  if (!s) return NULL;
  seq_parse(s, src, len);
  if (s->np == 0 || s->ne == 0) { nd_mm_seq_free(s); return NULL; }
  seq_measure(s, pctx);
  seq_columns(s);
  seq_rows(s);
  seq_gaps(s);
  if (!(s->w > 0) || !(s->h > 0) ||
      s->w > ND_MM_MAX_DIM || s->h > ND_MM_MAX_DIM) {
    nd_mm_seq_free(s);
    return NULL;
  }
  return s;
}

void nd_mm_seq_size(const struct nd_seq *s, double *w, double *h) {
  *w = s->w; *h = s->h;
}

void nd_mm_seq_free(struct nd_seq *s) {
  if (!s) return;
  for (uint32_t i = 0; i < s->np; i++) {
    free(s->p[i].id); free(s->p[i].label); free(s->p[i].gap);
    if (s->p[i].pl) g_object_unref(s->p[i].pl);
  }
  /* A build abandoned before measuring still has the parked strings, which
   * is why the two are separate fields rather than one reused pointer. */
  for (uint32_t i = 0; i < s->ne; i++) {
    struct sq_ev *e = &s->e[i];
    free(e->text); free(e->kw);
    if (e->pl)  g_object_unref(e->pl);
    if (e->kwl) g_object_unref(e->kwl);
  }
  free(s->p); free(s->e);
  free(s);
}

/* ---- painting ---------------------------------------------------------- */

static void sq_round(cairo_t *cr, double x, double y, double w, double h,
                     double r) {
  if (r > w / 2) r = w / 2;
  if (r > h / 2) r = h / 2;
  cairo_new_sub_path(cr);
  cairo_arc(cr, x + w - r, y + r,     r, -G_PI / 2, 0);
  cairo_arc(cr, x + w - r, y + h - r, r, 0,         G_PI / 2);
  cairo_arc(cr, x + r,     y + h - r, r, G_PI / 2,  G_PI);
  cairo_arc(cr, x + r,     y + r,     r, G_PI,      3 * G_PI / 2);
  cairo_close_path(cr);
}

static void sq_text(cairo_t *cr, PangoLayout *pl, uint32_t colour,
                    double x, double y) {
  if (!pl) return;
  nd_src(cr, colour);
  cairo_move_to(cr, x, y);
  pango_cairo_show_layout(cr, pl);
}

static void sq_frame_x(const struct nd_seq *s, int depth, double *x0,
                       double *x1) {
  double inset = 10.0 - (double)depth * 8.0;
  *x0 = (s->np ? s->p[0].x - s->p[0].w / 2 : SQ_MARGIN) - inset;
  *x1 = s->w - SQ_MARGIN + inset;
  if (*x1 - *x0 < 40) { *x0 = SQ_MARGIN; *x1 = s->w - SQ_MARGIN; }
}

static void sq_arrowhead(cairo_t *cr, double x, double y, double dir, int head) {
  switch (head) {
    case HEAD_FILLED:
      cairo_move_to(cr, x, y);
      cairo_line_to(cr, x + dir * 9, y - 4);
      cairo_line_to(cr, x + dir * 9, y + 4);
      cairo_close_path(cr);
      cairo_fill(cr);
      break;
    case HEAD_CROSS:
      cairo_set_line_width(cr, 1.4);
      cairo_move_to(cr, x + dir * 8, y - 5); cairo_line_to(cr, x, y + 5);
      cairo_move_to(cr, x + dir * 8, y + 5); cairo_line_to(cr, x, y - 5);
      cairo_stroke(cr);
      break;
    case HEAD_NONE:
      break;
    default:                        /* open, and the async half-head */
      cairo_set_line_width(cr, 1.4);
      cairo_move_to(cr, x + dir * 9, y - 5);
      cairo_line_to(cr, x, y);
      cairo_line_to(cr, x + dir * 9, y + 5);
      cairo_stroke(cr);
      break;
  }
}

static void sq_paint_frames(const struct nd_seq *s, cairo_t *cr) {
  static const double dash[] = {5.0, 4.0};

  for (uint32_t i = 0; i < s->ne; i++) {
    const struct sq_ev *e = &s->e[i];
    if (e->kind != EV_FRAME || e->y2 <= e->y) continue;
    double x0, x1;
    sq_frame_x(s, e->depth, &x0, &x1);

    nd_src(cr, CTP_SURFACE1);
    cairo_set_line_width(cr, 1.0);
    cairo_rectangle(cr, x0, e->y, x1 - x0, e->y2 - e->y);
    cairo_stroke(cr);

    /* The keyword tab, then the frame's own condition beside it. */
    double tw = e->kw_w + 16, th = e->kw_h + 6;
    nd_src(cr, CTP_SURFACE1);
    cairo_rectangle(cr, x0, e->y, tw, th);
    cairo_fill(cr);
    sq_text(cr, e->kwl, CTP_TEXT, x0 + 8, e->y + 3);
    if (e->lw > 0)
      sq_text(cr, e->pl, CTP_SUBTEXT0, x0 + tw + 10, e->y + 3);
  }

  for (uint32_t i = 0; i < s->ne; i++) {
    const struct sq_ev *e = &s->e[i];
    if (e->kind != EV_SECTION) continue;
    double x0, x1;
    sq_frame_x(s, e->depth, &x0, &x1);

    cairo_save(cr);
    cairo_set_dash(cr, dash, 2, 0);
    nd_src(cr, CTP_SURFACE1);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, x0, e->y);
    cairo_line_to(cr, x1, e->y);
    cairo_stroke(cr);
    cairo_restore(cr);

    sq_text(cr, e->kwl, CTP_OVERLAY2, x0 + 8, e->y + 4);
    if (e->lw > 0)
      sq_text(cr, e->pl, CTP_SUBTEXT0, x0 + 8 + e->kw_w + 10, e->y + 4);
  }
}

static void sq_paint_activations(const struct nd_seq *s, cairo_t *cr) {
  /* Activations nest: a participant can be re-entered while already active,
   * and mermaid steps the bar sideways for each level. The stack is what makes
   * that work, and what keeps an unmatched `activate` from swallowing the
   * diagram — it simply runs to the foot of the lifeline. */
  int    nopen[ND_MM_MAX_PARTS] = {0};
  double open_y[ND_MM_MAX_PARTS][8];

  for (uint32_t i = 0; i < s->ne; i++) {
    const struct sq_ev *e = &s->e[i];
    int on = -1, off = -1;
    if (e->kind == EV_ACT)        on = e->from;
    else if (e->kind == EV_DEACT) off = e->from;
    else if (e->kind == EV_MSG && e->act > 0) on = e->to;
    else if (e->kind == EV_MSG && e->act < 0) off = e->from;

    if (on >= 0 && nopen[on] < 8) open_y[on][nopen[on]++] = e->y;

    if (off >= 0 && nopen[off] > 0) {
      int d = --nopen[off];
      double y0 = open_y[off][d], y1 = e->y;
      double x = s->p[off].x - SQ_ACT_W / 2 + d * 4;
      nd_src(cr, CTP_SURFACE2);
      cairo_rectangle(cr, x, y0 - 6, SQ_ACT_W, y1 - y0 + 12);
      cairo_fill_preserve(cr);
      nd_src(cr, CTP_TEAL);
      cairo_set_line_width(cr, 1.0);
      cairo_stroke(cr);
    }
  }

  for (uint32_t p = 0; p < s->np; p++)
    while (nopen[p] > 0) {
      int d = --nopen[p];
      double x = s->p[p].x - SQ_ACT_W / 2 + d * 4;
      nd_src(cr, CTP_SURFACE2);
      cairo_rectangle(cr, x, open_y[p][d] - 6, SQ_ACT_W,
                      s->life_bot - open_y[p][d] + 6);
      cairo_fill_preserve(cr);
      nd_src(cr, CTP_TEAL);
      cairo_set_line_width(cr, 1.0);
      cairo_stroke(cr);
    }
}

static void sq_paint_note(const struct nd_seq *s, cairo_t *cr,
                          const struct sq_ev *e) {
  double w = e->lw + 24, x0;
  if (e->note_pos == NOTE_OVER) {
    double lo = s->p[e->note_lo].x, hi = s->p[e->note_hi].x;
    double span = hi - lo + 40;
    if (span > w) w = span;
    x0 = (lo + hi) / 2 - w / 2;
  } else if (e->note_pos == NOTE_RIGHT) {
    x0 = s->p[e->note_lo].x + 16;
  } else {
    x0 = s->p[e->note_lo].x - 16 - w;
  }

  nd_src(cr, CTP_SURFACE0);
  sq_round(cr, x0, e->y, w, e->h, 4.0);
  cairo_fill_preserve(cr);
  nd_src(cr, CTP_YELLOW);
  cairo_set_line_width(cr, 1.0);
  cairo_stroke(cr);
  sq_text(cr, e->pl, CTP_SUBTEXT1, x0 + 12, e->y + 8);
}

static void sq_paint_msg(const struct nd_seq *s, cairo_t *cr,
                         const struct sq_ev *e) {
  static const double dash[] = {5.0, 4.0};

  if (e->self) {
    double x = s->p[e->from].x, top = e->y - 16;
    cairo_save(cr);
    if (e->dashed) cairo_set_dash(cr, dash, 2, 0);
    nd_src(cr, CTP_TEXT);
    cairo_set_line_width(cr, 1.3);
    cairo_move_to(cr, x + 5, top);
    cairo_line_to(cr, x + SQ_SELF_W, top);
    cairo_line_to(cr, x + SQ_SELF_W, e->y);
    cairo_line_to(cr, x + 11, e->y);
    cairo_stroke(cr);
    cairo_restore(cr);

    nd_src(cr, CTP_TEXT);
    sq_arrowhead(cr, x + 6, e->y, 1.0, e->head);
    sq_text(cr, e->pl, CTP_SUBTEXT1, x + SQ_SELF_W + SQ_LBL_PAD, top - 2);
    return;
  }

  double x0 = s->p[e->from].x, x1 = s->p[e->to].x;
  double dir = x1 > x0 ? 1.0 : -1.0;
  double ax0 = x0 + dir * 5, ax1 = x1 - dir * 5;

  cairo_save(cr);
  if (e->dashed) cairo_set_dash(cr, dash, 2, 0);
  nd_src(cr, CTP_TEXT);
  cairo_set_line_width(cr, 1.3);
  cairo_move_to(cr, ax0, e->y);
  cairo_line_to(cr, ax1, e->y);
  cairo_stroke(cr);
  cairo_restore(cr);

  nd_src(cr, CTP_TEXT);
  sq_arrowhead(cr, ax1, e->y, -dir, e->head);

  sq_text(cr, e->pl, CTP_SUBTEXT1, (x0 + x1) / 2 - e->lw / 2,
          e->y - e->lh - 6);
}

void nd_mm_seq_paint(const struct nd_seq *s, cairo_t *cr) {
  static const double dash[] = {4.0, 4.0};
  if (!s) return;

  sq_paint_frames(s, cr);

  cairo_save(cr);
  cairo_set_dash(cr, dash, 2, 0);
  nd_src(cr, CTP_SURFACE2);
  cairo_set_line_width(cr, 1.0);
  for (uint32_t i = 0; i < s->np; i++) {
    const struct sq_part *p = &s->p[i];
    double at = s->life_top;
    for (uint32_t g = 0; g < p->ngap; g++) {
      double g0 = p->gap[g * 2], g1 = p->gap[g * 2 + 1];
      if (g1 <= at) continue;
      if (g0 > at) { cairo_move_to(cr, p->x, at); cairo_line_to(cr, p->x, g0); }
      at = g1;
    }
    if (at < s->life_bot) {
      cairo_move_to(cr, p->x, at);
      cairo_line_to(cr, p->x, s->life_bot);
    }
  }
  cairo_stroke(cr);
  cairo_restore(cr);

  sq_paint_activations(s, cr);

  for (uint32_t i = 0; i < s->ne; i++) {
    const struct sq_ev *e = &s->e[i];
    if (e->kind == EV_NOTE)     sq_paint_note(s, cr, e);
    else if (e->kind == EV_MSG) sq_paint_msg(s, cr, e);
  }

  for (uint32_t i = 0; i < s->np; i++) {
    const struct sq_part *p = &s->p[i];
    for (int pass = 0; pass < 2; pass++) {
      double by = pass ? s->h - SQ_MARGIN - SQ_BOX_H : SQ_MARGIN;
      nd_src(cr, p->is_actor ? CTP_SURFACE1 : CTP_SURFACE0);
      sq_round(cr, p->x - p->w / 2, by, p->w, SQ_BOX_H, 5.0);
      cairo_fill_preserve(cr);
      nd_src(cr, p->is_actor ? CTP_TEAL : CTP_SURFACE2);
      cairo_set_line_width(cr, 1.2);
      cairo_stroke(cr);
      sq_text(cr, p->pl, CTP_TEXT, p->x - p->lw / 2,
              by + (SQ_BOX_H - p->lh) / 2);
    }
  }
}
