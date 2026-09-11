/* math_parse.c — LaTeX math source into an atom tree.
 *
 * Deliberately forgiving. This parses notes, not a thesis: every error
 * recovers into visible upright text rather than failing, because a formula
 * that renders wrong is debuggable and a formula that renders as nothing is
 * not. An unknown `\foo` comes out as the literal characters `\foo`, an
 * unmatched `}` is ignored, and a missing `\right` closes at the end of input.
 */

#include "doc/math.h"

#include <stdlib.h>
#include <string.h>

#include "doc/arena.h"

#define MAX_DEPTH 64   /* bounds the recursion on hostile input */

struct parser {
  struct nd_arena *a;
  const char      *s;
  size_t           n, i;
  int              depth;
  nd_math_alpha    alpha;   /* current \mathbb / \mathrm / ... scope */
};

/* ---- a growable list of atoms, copied into the arena when closed --------- */

struct alist {
  nd_atom **p;
  uint32_t  n, cap;
};

static void al_push(struct alist *l, nd_atom *at) {
  if (l->n == l->cap) {
    l->cap = l->cap ? l->cap * 2 : 8;
    l->p = realloc(l->p, l->cap * sizeof *l->p);
    if (!l->p) abort();
  }
  l->p[l->n++] = at;
}

static nd_math_list *al_commit(struct parser *p, struct alist *l) {
  nd_math_list *out = nd_arena_calloc(p->a, sizeof *out);
  if (l->n) {
    out->items = nd_arena_memdup(p->a, l->p, l->n * sizeof *l->p);
    out->n = l->n;
  }
  free(l->p);
  return out;
}

/* ---- lexing -------------------------------------------------------------- */

static uint32_t utf8_next(struct parser *p) {
  unsigned char c = (unsigned char)p->s[p->i];
  /* The source was scrubbed to well-formed UTF-8 at load (util/utf8.c), so
   * this only has to decode, not validate. */
  if (c < 0x80) { p->i++; return c; }
  unsigned need = (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : 3;
  uint32_t cp = c & (0x3Fu >> need);
  if (p->i + need >= p->n + 1 && p->i + need > p->n - 1) { p->i++; return c; }
  for (unsigned k = 1; k <= need; k++)
    cp = (cp << 6) | ((unsigned char)p->s[p->i + k] & 0x3F);
  p->i += need + 1;
  return cp;
}

static void skip_space(struct parser *p) {
  while (p->i < p->n && (p->s[p->i] == ' ' || p->s[p->i] == '\t' ||
                         p->s[p->i] == '\n' || p->s[p->i] == '\r'))
    p->i++;
}

static bool is_alpha(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

/* Reads a control sequence, the backslash already consumed. A CS is either a
 * run of letters or exactly one non-letter (`\,` `\{` `\\`). */
static size_t read_cs(struct parser *p, const char **name) {
  *name = p->s + p->i;
  if (p->i >= p->n) return 0;
  if (!is_alpha(p->s[p->i])) { p->i++; return 1; }
  size_t start = p->i;
  while (p->i < p->n && is_alpha(p->s[p->i])) p->i++;
  return p->i - start;
}

static bool cs_is(const char *name, size_t len, const char *want) {
  return strlen(want) == len && memcmp(name, want, len) == 0;
}

/* ---- atom construction --------------------------------------------------- */

static nd_atom *atom_new(struct parser *p, nd_atom_kind k, nd_math_class c) {
  nd_atom *at = nd_arena_calloc(p->a, sizeof *at);
  at->kind = k;
  at->cls = c;
  return at;
}

static nd_atom *atom_run(struct parser *p, const uint32_t *cps, uint32_t n,
                         nd_math_class cls) {
  nd_atom *at = atom_new(p, ND_AT_RUN, cls);
  at->u.run.cps = nd_arena_memdup(p->a, cps, n * sizeof *cps);
  at->u.run.ncp = n;
  return at;
}

/* ASCII operators are not the characters maths uses. A hyphen-minus is a word
 * divider drawn to match lowercase; MINUS SIGN is drawn to match plus, on the
 * same axis and at the same weight. Typing `-` and getting a hyphen is one of
 * the most visible tells that something is not really typeset. */
static uint32_t math_char(uint32_t cp) {
  switch (cp) {
    case '-':  return 0x2212;   /* MINUS SIGN */
    case '*':  return 0x2217;   /* ASTERISK OPERATOR */
    case '\'': return 0x2032;   /* PRIME */
    default:   return cp;
  }
}

static nd_atom *atom_char(struct parser *p, uint32_t cp, nd_math_class cls) {
  cp = math_char(cp);
  return atom_run(p, &cp, 1, cls);
}

/* Literal text for a construct we do not understand, so it stays visible. */
static nd_atom *atom_literal(struct parser *p, const char *s, size_t len) {
  uint32_t buf[64];
  uint32_t n = 0;
  for (size_t k = 0; k < len && n < 64; k++)
    buf[n++] = nd_math_alpha_map((unsigned char)s[k], ND_MA_UPRIGHT);
  return atom_run(p, buf, n, ND_MC_ORD);
}

static nd_math_list *parse_list(struct parser *p, char stop, const char *stop_cs,
                                bool one);

/* One argument: `{...}`, or a single token when the braces are omitted, which
 * LaTeX allows and people write constantly (`\frac12`, `x^2`). */
static nd_math_list *parse_arg(struct parser *p) {
  skip_space(p);
  if (p->i < p->n && p->s[p->i] == '{') {
    p->i++;
    return parse_list(p, '}', NULL, false);
  }
  /* Braces are optional in LaTeX and people leave them off constantly --
   * `x^2`, `\frac12`, `^\infty`. Parsing exactly one atom handles every case
   * uniformly. It must be ONE: letting the list run to the end of input and
   * keeping its first atom would still have consumed the rest, which silently
   * swallowed everything after a script. */
  return parse_list(p, 0, NULL, true);
}

/* An optional `[...]`, for \sqrt[3]{x}. */
static nd_math_list *parse_opt(struct parser *p) {
  skip_space(p);
  if (p->i >= p->n || p->s[p->i] != '[') return NULL;
  p->i++;
  return parse_list(p, ']', NULL, false);
}

/* ---- delimiters ---------------------------------------------------------- */

/* The token after \left or \right. `.` means "no delimiter". */
static uint32_t parse_delim(struct parser *p) {
  skip_space(p);
  if (p->i >= p->n) return 0;
  if (p->s[p->i] == '.') { p->i++; return 0; }
  if (p->s[p->i] == '\\') {
    p->i++;
    const char *name;
    size_t len = read_cs(p, &name);
    if (cs_is(name, len, "{")) return '{';
    if (cs_is(name, len, "}")) return '}';
    if (cs_is(name, len, "|")) return 0x2016;
    nd_math_class c; bool lo;
    uint32_t cp = nd_math_symbol(name, len, &c, &lo);
    return cp;
  }
  return utf8_next(p);
}

/* ---- environments -------------------------------------------------------- */

struct env_info {
  const char *name;
  uint32_t    left, right;
  bool        align_rel;
};

static const struct env_info ENVS[] = {
  {"matrix",   0,      0,      false},
  {"pmatrix",  '(',    ')',    false},
  {"bmatrix",  '[',    ']',    false},
  {"Bmatrix",  '{',    '}',    false},
  {"vmatrix",  '|',    '|',    false},
  {"Vmatrix",  0x2016, 0x2016, false},
  {"cases",    '{',    0,      false},
  {"align",    0,      0,      true},
  {"aligned",  0,      0,      true},
  {"align*",   0,      0,      true},
  {"array",    0,      0,      false},
};

static const struct env_info *env_find(const char *name, size_t len) {
  for (unsigned i = 0; i < sizeof ENVS / sizeof ENVS[0]; i++)
    if (cs_is(name, len, ENVS[i].name)) return &ENVS[i];
  return NULL;
}

/* Reads `{name}` after \begin or \end. */
static size_t read_env_name(struct parser *p, const char **out) {
  skip_space(p);
  if (p->i >= p->n || p->s[p->i] != '{') { *out = p->s + p->i; return 0; }
  p->i++;
  *out = p->s + p->i;
  size_t start = p->i;
  while (p->i < p->n && p->s[p->i] != '}') p->i++;
  size_t len = p->i - start;
  if (p->i < p->n) p->i++;
  return len;
}

static nd_atom *parse_env(struct parser *p, const struct env_info *e) {
  /* Cells accumulate row-major into a flat scratch list; the grid is built
   * once the row count is known. */
  struct alist rows = {0};   /* each entry is a (nd_atom *)-cast row vector */
  nd_math_list **flat = NULL;
  uint32_t nflat = 0, capflat = 0;
  uint32_t cols = 0, this_row = 0, nrows = 0;

  for (;;) {
    /* stop = 0, not '&': the `stop` character is consumed by parse_list, and
     * the separator has to survive for the loop below to tell a column break
     * from a row break. Both separators are peeked, never eaten, in there. */
    nd_math_list *cell = parse_list(p, 0, "\\", false);
    if (nflat == capflat) {
      capflat = capflat ? capflat * 2 : 8;
      flat = realloc(flat, capflat * sizeof *flat);
      if (!flat) abort();
    }
    flat[nflat++] = cell;
    this_row++;

    skip_space(p);
    if (p->i < p->n && p->s[p->i] == '&') { p->i++; continue; }

    /* `\\` ends a row; \end ends the environment. */
    if (p->i + 1 < p->n && p->s[p->i] == '\\' && p->s[p->i + 1] == '\\') {
      p->i += 2;
      if (this_row > cols) cols = this_row;
      this_row = 0;
      nrows++;
      skip_space(p);
      /* A trailing `\\` before \end must not make a phantom empty row. */
      if (p->i + 4 <= p->n && memcmp(p->s + p->i, "\\end", 4) == 0) break;
      continue;
    }
    break;
  }
  if (this_row > 0) { if (this_row > cols) cols = this_row; nrows++; }
  if (nrows == 0) nrows = 1;

  /* Consume `\end{...}`. */
  skip_space(p);
  if (p->i + 4 <= p->n && memcmp(p->s + p->i, "\\end", 4) == 0) {
    p->i += 4;
    const char *nm; read_env_name(p, &nm);
  }

  nd_atom *at = atom_new(p, ND_AT_MATRIX, ND_MC_INNER);
  at->u.matrix.rows = nrows;
  at->u.matrix.cols = cols ? cols : 1;
  at->u.matrix.left = e->left;
  at->u.matrix.right = e->right;
  at->u.matrix.align_rel = e->align_rel;

  nd_math_list ***grid = nd_arena_calloc(p->a, nrows * sizeof *grid);
  uint32_t k = 0;
  for (uint32_t r = 0; r < nrows; r++) {
    grid[r] = nd_arena_calloc(p->a, at->u.matrix.cols * sizeof **grid);
    for (uint32_t c = 0; c < at->u.matrix.cols; c++)
      grid[r][c] = (k < nflat) ? flat[k++] : NULL;
  }
  at->u.matrix.cell = grid;
  free(flat);
  free(rows.p);
  return at;
}

/* ---- named operators ----------------------------------------------------- */

struct fn {
  const char *name;
  bool        limits;   /* \lim puts its subscript underneath in display */
};

static const struct fn FNS[] = {
  {"arccos", false}, {"arcsin", false}, {"arctan", false}, {"arg", false},
  {"cos", false},    {"cosh", false},   {"cot", false},    {"coth", false},
  {"csc", false},    {"deg", false},    {"det", true},     {"dim", false},
  {"exp", false},    {"gcd", true},     {"hom", false},    {"inf", true},
  {"ker", false},    {"lg", false},     {"lim", true},     {"liminf", true},
  {"limsup", true},  {"ln", false},     {"log", false},    {"max", true},
  {"min", true},     {"Pr", true},      {"sec", false},    {"sin", false},
  {"sinh", false},   {"sup", true},     {"tan", false},    {"tanh", false},
};

static const struct fn *fn_find(const char *name, size_t len) {
  for (unsigned i = 0; i < sizeof FNS / sizeof FNS[0]; i++)
    if (cs_is(name, len, FNS[i].name)) return &FNS[i];
  return NULL;
}

/* ---- accents ------------------------------------------------------------- */

static uint32_t accent_cp(const char *name, size_t len) {
  if (cs_is(name, len, "hat") || cs_is(name, len, "widehat"))   return 0x0302;
  if (cs_is(name, len, "bar") || cs_is(name, len, "overline"))  return 0x0304;
  if (cs_is(name, len, "vec"))                                  return 0x20D7;
  if (cs_is(name, len, "tilde") || cs_is(name, len, "widetilde")) return 0x0303;
  if (cs_is(name, len, "dot"))                                  return 0x0307;
  if (cs_is(name, len, "ddot"))                                 return 0x0308;
  if (cs_is(name, len, "check"))                                return 0x030C;
  if (cs_is(name, len, "breve"))                                return 0x0306;
  if (cs_is(name, len, "acute"))                                return 0x0301;
  if (cs_is(name, len, "grave"))                                return 0x0300;
  return 0;
}

/* ---- alphabets ----------------------------------------------------------- */

static bool alpha_for(const char *name, size_t len, nd_math_alpha *out) {
  if (cs_is(name, len, "mathbb"))   { *out = ND_MA_BB;      return true; }
  if (cs_is(name, len, "mathcal"))  { *out = ND_MA_CAL;     return true; }
  if (cs_is(name, len, "mathscr"))  { *out = ND_MA_CAL;     return true; }
  if (cs_is(name, len, "mathfrak")) { *out = ND_MA_FRAK;    return true; }
  if (cs_is(name, len, "mathrm"))   { *out = ND_MA_UPRIGHT; return true; }
  if (cs_is(name, len, "operatorname")) { *out = ND_MA_UPRIGHT; return true; }
  if (cs_is(name, len, "text"))     { *out = ND_MA_UPRIGHT; return true; }
  if (cs_is(name, len, "mathbf"))   { *out = ND_MA_BOLD;    return true; }
  if (cs_is(name, len, "mathsf"))   { *out = ND_MA_SANS;    return true; }
  if (cs_is(name, len, "mathtt"))   { *out = ND_MA_MONO;    return true; }
  if (cs_is(name, len, "mathit"))   { *out = ND_MA_ITALIC;  return true; }
  return false;
}

/* ---- the main loop ------------------------------------------------------- */

/* Attaches ^ and _ to the atom they follow. TeX allows either order and at
 * most one of each; a second is a syntax error we simply overwrite. */
static void attach_script(struct parser *p, struct alist *l, bool sup) {
  nd_atom *target;
  if (l->n > 0) {
    target = l->p[l->n - 1];
  } else {
    /* `^2` with nothing before it still has to hang on something. */
    target = atom_run(p, NULL, 0, ND_MC_ORD);
    al_push(l, target);
  }
  nd_math_list *arg = parse_arg(p);
  if (sup) target->sup = arg;
  else     target->sub = arg;
}

static nd_math_list *parse_list(struct parser *p, char stop, const char *stop_cs,
                                bool one) {
  struct alist l = {0};
  if (++p->depth > MAX_DEPTH) { p->depth--; return al_commit(p, &l); }
  nd_math_alpha saved_alpha = p->alpha;

  while (p->i < p->n) {
    /* An unbraced argument is exactly one atom, and stopping here rather than
     * after the push means a following ^ or _ binds to the enclosing atom, as
     * LaTeX requires: in `x^\alpha_2` the subscript belongs to x. */
    if (one && l.n > 0) break;

    char c = p->s[p->i];

    if (stop && c == stop) { p->i++; break; }
    /* `&` and `\\` end a cell without being consumed here: parse_env decides. */
    if (stop_cs && c == '&') break;
    if (stop_cs && c == '\\' && p->i + 1 < p->n && p->s[p->i + 1] == '\\') break;

    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { p->i++; continue; }

    if (c == '}') {
      /* An unmatched close: drop it rather than unwinding the whole formula. */
      if (!stop) { p->i++; continue; }
      p->i++;
      break;
    }

    if (c == '{') {
      p->i++;
      nd_math_list *inner = parse_list(p, '}', NULL, false);
      nd_atom *at = atom_new(p, ND_AT_LIST, ND_MC_ORD);
      at->u.list = inner;
      al_push(&l, at);
      continue;
    }

    if (c == '^') { p->i++; attach_script(p, &l, true);  continue; }
    if (c == '_') { p->i++; attach_script(p, &l, false); continue; }

    if (c != '\\') {
      uint32_t cp = utf8_next(p);
      nd_math_class cls = nd_math_char_class(cp);
      /* Digits stay upright; letters take the current alphabet, italic by
       * default, which is the single most visible convention in math setting. */
      nd_math_alpha use = (cp >= '0' && cp <= '9') ? ND_MA_UPRIGHT : p->alpha;
      al_push(&l, atom_char(p, nd_math_alpha_map(cp, use), cls));
      continue;
    }

    /* ---- a control sequence ---- */
    size_t cs_start = p->i;
    p->i++;
    const char *name;
    size_t len = read_cs(p, &name);
    if (len == 0) continue;

    if (stop_cs && cs_is(name, len, "\\")) { p->i = cs_start; break; }

    /* \end closes an environment; let the caller see it. */
    if (cs_is(name, len, "end")) { p->i = cs_start; break; }
    if (cs_is(name, len, "right")) { p->i = cs_start; break; }

    if (cs_is(name, len, "begin")) {
      const char *env; size_t elen = read_env_name(p, &env);
      const struct env_info *e = env_find(env, elen);
      if (!e) { al_push(&l, atom_literal(p, "\\begin", 6)); continue; }
      al_push(&l, parse_env(p, e));
      continue;
    }

    if (cs_is(name, len, "left")) {
      uint32_t left = parse_delim(p);
      nd_math_list *body = parse_list(p, 0, NULL, false);
      uint32_t right = 0;
      skip_space(p);
      if (p->i + 6 <= p->n && memcmp(p->s + p->i, "\\right", 6) == 0) {
        p->i += 6;
        right = parse_delim(p);
      }
      nd_atom *at = atom_new(p, ND_AT_FENCED, ND_MC_INNER);
      at->u.fenced.body = body;
      at->u.fenced.left = left;
      at->u.fenced.right = right;
      al_push(&l, at);
      continue;
    }

    if (cs_is(name, len, "frac") || cs_is(name, len, "dfrac") ||
        cs_is(name, len, "tfrac") || cs_is(name, len, "binom")) {
      nd_atom *at = atom_new(p, ND_AT_FRAC, ND_MC_INNER);
      at->u.frac.num = parse_arg(p);
      at->u.frac.den = parse_arg(p);
      if (cs_is(name, len, "binom")) {
        at->u.frac.no_rule = true;
        at->u.frac.left = '(';
        at->u.frac.right = ')';
      }
      al_push(&l, at);
      continue;
    }

    if (cs_is(name, len, "sqrt")) {
      nd_atom *at = atom_new(p, ND_AT_RADICAL, ND_MC_ORD);
      at->u.radical.index = parse_opt(p);
      at->u.radical.body = parse_arg(p);
      al_push(&l, at);
      continue;
    }

    uint32_t acc = accent_cp(name, len);
    if (acc) {
      nd_atom *at = atom_new(p, ND_AT_ACCENT, ND_MC_ORD);
      at->u.accent.cp = acc;
      at->u.accent.body = parse_arg(p);
      al_push(&l, at);
      continue;
    }

    nd_math_alpha na;
    if (alpha_for(name, len, &na)) {
      nd_math_alpha prev = p->alpha;
      p->alpha = na;
      nd_math_list *inner = parse_arg(p);
      p->alpha = prev;
      nd_atom *at = atom_new(p, ND_AT_LIST, ND_MC_ORD);
      at->u.list = inner;
      al_push(&l, at);
      continue;
    }

    const struct fn *f = fn_find(name, len);
    if (f) {
      uint32_t buf[16];
      uint32_t k = 0;
      for (size_t q = 0; q < len && k < 16; q++) buf[k++] = (unsigned char)name[q];
      nd_atom *at = atom_run(p, buf, k, ND_MC_OP);
      at->limits = f->limits;
      al_push(&l, at);
      continue;
    }

    /* Explicit spaces. */
    double em = 0;
    if      (cs_is(name, len, ","))     em = 3.0 / 18.0;
    else if (cs_is(name, len, ":"))     em = 4.0 / 18.0;
    else if (cs_is(name, len, ";"))     em = 5.0 / 18.0;
    else if (cs_is(name, len, "!"))     em = -3.0 / 18.0;
    else if (cs_is(name, len, " "))     em = 6.0 / 18.0;
    else if (cs_is(name, len, "quad"))  em = 1.0;
    else if (cs_is(name, len, "qquad")) em = 2.0;
    if (em != 0) {
      nd_atom *at = atom_new(p, ND_AT_SPACE, ND_MC_ORD);
      at->u.space_em = em;
      al_push(&l, at);
      continue;
    }

    /* Escaped literals. */
    if (len == 1 && strchr("{}$%&_#", name[0])) {
      al_push(&l, atom_char(p, (uint32_t)name[0], ND_MC_ORD));
      continue;
    }
    if (cs_is(name, len, "|")) {
      al_push(&l, atom_char(p, 0x2016, ND_MC_ORD));
      continue;
    }

    nd_math_class scls = ND_MC_ORD;
    bool large = false;
    uint32_t cp = nd_math_symbol(name, len, &scls, &large);
    if (cp) {
      nd_atom *at = atom_char(p, cp, scls);
      at->u.run.large_op = large;
      at->limits = large;
      al_push(&l, at);
      continue;
    }

    /* Unknown: show the source so the author can see what was not understood. */
    al_push(&l, atom_literal(p, p->s + cs_start, (size_t)(p->i - cs_start)));
  }

  p->alpha = saved_alpha;
  p->depth--;
  return al_commit(p, &l);
}

nd_math_list *nd_math_parse(struct nd_arena *a, const char *src, size_t len) {
  struct parser p = {.a = a, .s = src, .n = len, .i = 0, .depth = 0,
                     .alpha = ND_MA_ITALIC};
  return parse_list(&p, 0, NULL, false);
}
