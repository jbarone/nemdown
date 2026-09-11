/* math_syms.c — control-sequence names to Unicode, with TeX atom classes.
 *
 * The class is not decoration: the spacing table in math_box.c is indexed by
 * it, and it is what makes `a + b` and `a \cup b` space identically while
 * `f(x)` stays tight. Getting \mid classed REL rather than ORD is the
 * difference between `{x \mid x > 0}` reading correctly and reading as a typo.
 *
 * Sorted by name; looked up by binary search. Unknown names fall through to
 * upright text so an unsupported macro shows as `\foo` rather than vanishing.
 */

#include "doc/math.h"

#include <stdlib.h>
#include <string.h>

struct sym {
  const char   *name;
  uint32_t      cp;
  unsigned char cls;
  unsigned char large_op;
};

/* Keep sorted: bsearch depends on it. */
static const struct sym SYMS[] = {
  {"Delta",       0x0394, ND_MC_ORD,   0},
  {"Gamma",       0x0393, ND_MC_ORD,   0},
  {"Im",          0x2111, ND_MC_ORD,   0},
  {"Lambda",      0x039B, ND_MC_ORD,   0},
  {"Leftarrow",   0x21D0, ND_MC_REL,   0},
  {"Leftrightarrow", 0x21D4, ND_MC_REL, 0},
  {"Omega",       0x03A9, ND_MC_ORD,   0},
  {"Phi",         0x03A6, ND_MC_ORD,   0},
  {"Pi",          0x03A0, ND_MC_ORD,   0},
  {"Psi",         0x03A8, ND_MC_ORD,   0},
  {"Re",          0x211C, ND_MC_ORD,   0},
  {"Rightarrow",  0x21D2, ND_MC_REL,   0},
  {"Sigma",       0x03A3, ND_MC_ORD,   0},
  {"Theta",       0x0398, ND_MC_ORD,   0},
  {"Upsilon",     0x03A5, ND_MC_ORD,   0},
  {"Xi",          0x039E, ND_MC_ORD,   0},
  {"aleph",       0x2135, ND_MC_ORD,   0},
  {"alpha",       0x03B1, ND_MC_ORD,   0},
  {"amalg",       0x2A3F, ND_MC_BIN,   0},
  {"angle",       0x2220, ND_MC_ORD,   0},
  {"approx",      0x2248, ND_MC_REL,   0},
  {"ast",         0x2217, ND_MC_BIN,   0},
  {"asymp",       0x224D, ND_MC_REL,   0},
  {"beta",        0x03B2, ND_MC_ORD,   0},
  {"bigcap",      0x22C2, ND_MC_OP,    1},
  {"bigcup",      0x22C3, ND_MC_OP,    1},
  {"bigodot",     0x2A00, ND_MC_OP,    1},
  {"bigoplus",    0x2A01, ND_MC_OP,    1},
  {"bigotimes",   0x2A02, ND_MC_OP,    1},
  {"bigsqcup",    0x2A06, ND_MC_OP,    1},
  {"bigtriangledown", 0x25BD, ND_MC_BIN, 0},
  {"bigtriangleup",   0x25B3, ND_MC_BIN, 0},
  {"biguplus",    0x2A04, ND_MC_OP,    1},
  {"bigvee",      0x22C1, ND_MC_OP,    1},
  {"bigwedge",    0x22C0, ND_MC_OP,    1},
  {"bot",         0x22A5, ND_MC_ORD,   0},
  {"bullet",      0x2219, ND_MC_BIN,   0},
  {"cap",         0x2229, ND_MC_BIN,   0},
  {"cdot",        0x22C5, ND_MC_BIN,   0},
  {"cdots",       0x22EF, ND_MC_ORD,   0},
  {"chi",         0x03C7, ND_MC_ORD,   0},
  {"circ",        0x2218, ND_MC_BIN,   0},
  {"cong",        0x2245, ND_MC_REL,   0},
  {"coprod",      0x2210, ND_MC_OP,    1},
  {"cup",         0x222A, ND_MC_BIN,   0},
  {"dagger",      0x2020, ND_MC_BIN,   0},
  {"dashv",       0x22A3, ND_MC_REL,   0},
  {"ddagger",     0x2021, ND_MC_BIN,   0},
  {"ddots",       0x22F1, ND_MC_ORD,   0},
  {"delta",       0x03B4, ND_MC_ORD,   0},
  {"diamond",     0x22C4, ND_MC_BIN,   0},
  {"div",         0x00F7, ND_MC_BIN,   0},
  {"downarrow",   0x2193, ND_MC_REL,   0},
  {"ell",         0x2113, ND_MC_ORD,   0},
  {"emptyset",    0x2205, ND_MC_ORD,   0},
  {"epsilon",     0x03F5, ND_MC_ORD,   0},
  {"equiv",       0x2261, ND_MC_REL,   0},
  {"eta",         0x03B7, ND_MC_ORD,   0},
  {"exists",      0x2203, ND_MC_ORD,   0},
  {"forall",      0x2200, ND_MC_ORD,   0},
  {"gamma",       0x03B3, ND_MC_ORD,   0},
  {"ge",          0x2265, ND_MC_REL,   0},
  {"geq",         0x2265, ND_MC_REL,   0},
  {"gets",        0x2190, ND_MC_REL,   0},
  {"gg",          0x226B, ND_MC_REL,   0},
  {"hbar",        0x210F, ND_MC_ORD,   0},
  {"iff",         0x21D4, ND_MC_REL,   0},
  {"imath",       0x0131, ND_MC_ORD,   0},
  {"in",          0x2208, ND_MC_REL,   0},
  {"infty",       0x221E, ND_MC_ORD,   0},
  {"int",         0x222B, ND_MC_OP,    1},
  {"iota",        0x03B9, ND_MC_ORD,   0},
  {"kappa",       0x03BA, ND_MC_ORD,   0},
  {"lambda",      0x03BB, ND_MC_ORD,   0},
  {"land",        0x2227, ND_MC_BIN,   0},
  {"langle",      0x27E8, ND_MC_OPEN,  0},
  {"lceil",       0x2308, ND_MC_OPEN,  0},
  {"ldots",       0x2026, ND_MC_ORD,   0},
  {"le",          0x2264, ND_MC_REL,   0},
  {"leftarrow",   0x2190, ND_MC_REL,   0},
  {"leftrightarrow", 0x2194, ND_MC_REL, 0},
  {"leq",         0x2264, ND_MC_REL,   0},
  {"lfloor",      0x230A, ND_MC_OPEN,  0},
  {"ll",          0x226A, ND_MC_REL,   0},
  {"lnot",        0x00AC, ND_MC_ORD,   0},
  {"longmapsto",  0x27FC, ND_MC_REL,   0},
  {"longrightarrow", 0x27F6, ND_MC_REL, 0},
  {"lor",         0x2228, ND_MC_BIN,   0},
  {"mapsto",      0x21A6, ND_MC_REL,   0},
  {"mid",         0x2223, ND_MC_REL,   0},
  {"models",      0x22A8, ND_MC_REL,   0},
  {"mp",          0x2213, ND_MC_BIN,   0},
  {"mu",          0x03BC, ND_MC_ORD,   0},
  {"nabla",       0x2207, ND_MC_ORD,   0},
  {"ne",          0x2260, ND_MC_REL,   0},
  {"neg",         0x00AC, ND_MC_ORD,   0},
  {"neq",         0x2260, ND_MC_REL,   0},
  {"ni",          0x220B, ND_MC_REL,   0},
  {"notin",       0x2209, ND_MC_REL,   0},
  {"nu",          0x03BD, ND_MC_ORD,   0},
  {"odot",        0x2299, ND_MC_BIN,   0},
  {"oint",        0x222E, ND_MC_OP,    1},
  {"omega",       0x03C9, ND_MC_ORD,   0},
  {"ominus",      0x2296, ND_MC_BIN,   0},
  {"oplus",       0x2295, ND_MC_BIN,   0},
  {"oslash",      0x2298, ND_MC_BIN,   0},
  {"otimes",      0x2297, ND_MC_BIN,   0},
  {"parallel",    0x2225, ND_MC_REL,   0},
  {"partial",     0x2202, ND_MC_ORD,   0},
  {"perp",        0x22A5, ND_MC_REL,   0},
  {"phi",         0x03C6, ND_MC_ORD,   0},
  {"pi",          0x03C0, ND_MC_ORD,   0},
  {"pm",          0x00B1, ND_MC_BIN,   0},
  {"prec",        0x227A, ND_MC_REL,   0},
  {"preceq",      0x2AAF, ND_MC_REL,   0},
  {"prime",       0x2032, ND_MC_ORD,   0},
  {"prod",        0x220F, ND_MC_OP,    1},
  {"propto",      0x221D, ND_MC_REL,   0},
  {"psi",         0x03C8, ND_MC_ORD,   0},
  {"rangle",      0x27E9, ND_MC_CLOSE, 0},
  {"rceil",       0x2309, ND_MC_CLOSE, 0},
  {"rfloor",      0x230B, ND_MC_CLOSE, 0},
  {"rho",         0x03C1, ND_MC_ORD,   0},
  {"rightarrow",  0x2192, ND_MC_REL,   0},
  {"setminus",    0x2216, ND_MC_BIN,   0},
  {"sigma",       0x03C3, ND_MC_ORD,   0},
  {"sim",         0x223C, ND_MC_REL,   0},
  {"simeq",       0x2243, ND_MC_REL,   0},
  {"square",      0x25A1, ND_MC_ORD,   0},
  {"star",        0x22C6, ND_MC_BIN,   0},
  {"subset",      0x2282, ND_MC_REL,   0},
  {"subseteq",    0x2286, ND_MC_REL,   0},
  {"succ",        0x227B, ND_MC_REL,   0},
  {"succeq",      0x2AB0, ND_MC_REL,   0},
  {"sum",         0x2211, ND_MC_OP,    1},
  {"supset",      0x2283, ND_MC_REL,   0},
  {"supseteq",    0x2287, ND_MC_REL,   0},
  {"surd",        0x221A, ND_MC_ORD,   0},
  {"tau",         0x03C4, ND_MC_ORD,   0},
  {"theta",       0x03B8, ND_MC_ORD,   0},
  {"times",       0x00D7, ND_MC_BIN,   0},
  {"to",          0x2192, ND_MC_REL,   0},
  {"top",         0x22A4, ND_MC_ORD,   0},
  {"triangle",    0x25B3, ND_MC_ORD,   0},
  {"uparrow",     0x2191, ND_MC_REL,   0},
  {"uplus",       0x228E, ND_MC_BIN,   0},
  {"varepsilon",  0x03B5, ND_MC_ORD,   0},
  {"varphi",      0x03D5, ND_MC_ORD,   0},
  {"varpi",       0x03D6, ND_MC_ORD,   0},
  {"varrho",      0x03F1, ND_MC_ORD,   0},
  {"varsigma",    0x03C2, ND_MC_ORD,   0},
  {"vartheta",    0x03D1, ND_MC_ORD,   0},
  {"vdash",       0x22A2, ND_MC_REL,   0},
  {"vdots",       0x22EE, ND_MC_ORD,   0},
  {"vee",         0x2228, ND_MC_BIN,   0},
  {"wedge",       0x2227, ND_MC_BIN,   0},
  {"wp",          0x2118, ND_MC_ORD,   0},
  {"xi",          0x03BE, ND_MC_ORD,   0},
  {"zeta",        0x03B6, ND_MC_ORD,   0},
};

static int sym_cmp(const void *key, const void *el) {
  return strcmp(key, ((const struct sym *)el)->name);
}

uint32_t nd_math_symbol(const char *name, size_t len, nd_math_class *cls,
                        bool *large_op) {
  char buf[32];
  if (len == 0 || len >= sizeof buf) return 0;
  memcpy(buf, name, len);
  buf[len] = '\0';

  const struct sym *s = bsearch(buf, SYMS, sizeof SYMS / sizeof SYMS[0],
                                sizeof SYMS[0], sym_cmp);
  if (!s) return 0;
  if (cls) *cls = (nd_math_class)s->cls;
  if (large_op) *large_op = s->large_op != 0;
  return s->cp;
}

nd_math_class nd_math_char_class(uint32_t cp) {
  switch (cp) {
    case '+': case '-': case '*': case '/':
      return ND_MC_BIN;
    case '=': case '<': case '>':
      return ND_MC_REL;
    case '(': case '[':
      return ND_MC_OPEN;
    case ')': case ']':
      return ND_MC_CLOSE;
    case ',': case ';':
      return ND_MC_PUNCT;
    default:
      return ND_MC_ORD;
  }
}

/* The Mathematical Alphanumeric Symbols block is not contiguous: Unicode had
 * already encoded a handful of these letters in the Letterlike Symbols block,
 * so the ranges have holes where those letters were removed. Every table below
 * lists its holes rather than pretending the arithmetic is uniform, because a
 * miss lands on a reserved codepoint and renders as .notdef. */
static uint32_t alpha_range(uint32_t cp, uint32_t upper, uint32_t lower,
                            const uint32_t *holes, const uint32_t *subs) {
  uint32_t out = 0;
  if (cp >= 'A' && cp <= 'Z')      out = upper + (cp - 'A');
  else if (cp >= 'a' && cp <= 'z') out = lower + (cp - 'a');
  else return 0;

  for (unsigned i = 0; holes && holes[i]; i++)
    if (holes[i] == out) return subs[i];
  return out;
}

uint32_t nd_math_alpha_map(uint32_t cp, nd_math_alpha alpha) {
  /* Digits only vary for a few alphabets; the rest keep ASCII. */
  if (cp >= '0' && cp <= '9') {
    switch (alpha) {
      case ND_MA_BB:   return 0x1D7D8 + (cp - '0');
      case ND_MA_BOLD: return 0x1D7CE + (cp - '0');
      case ND_MA_SANS: return 0x1D7E2 + (cp - '0');
      case ND_MA_MONO: return 0x1D7F6 + (cp - '0');
      default:         return cp;
    }
  }

  switch (alpha) {
    case ND_MA_UPRIGHT:
      return cp;

    case ND_MA_ITALIC: {
      /* U+1D455 (italic h) is reserved; PLANCK CONSTANT U+210E took its place. */
      static const uint32_t holes[] = {0x1D455, 0};
      static const uint32_t subs[]  = {0x210E,  0};
      uint32_t r = alpha_range(cp, 0x1D434, 0x1D44E, holes, subs);
      return r ? r : cp;
    }
    case ND_MA_BOLD: {
      uint32_t r = alpha_range(cp, 0x1D400, 0x1D41A, NULL, NULL);
      return r ? r : cp;
    }
    case ND_MA_BB: {
      static const uint32_t holes[] = {0x1D53A, 0x1D53F, 0x1D545, 0x1D547,
                                       0x1D548, 0x1D549, 0x1D551, 0};
      static const uint32_t subs[]  = {0x2102,  0x210D,  0x2115,  0x2119,
                                       0x211A,  0x211D,  0x2124,  0};
      uint32_t r = alpha_range(cp, 0x1D538, 0x1D552, holes, subs);
      return r ? r : cp;
    }
    case ND_MA_CAL: {
      static const uint32_t holes[] = {0x1D49D, 0x1D4A0, 0x1D4A1, 0x1D4A3,
                                       0x1D4A4, 0x1D4A7, 0x1D4A8, 0x1D4AD,
                                       0x1D4BA, 0x1D4BC, 0x1D4C4, 0};
      static const uint32_t subs[]  = {0x212C,  0x2130,  0x2131,  0x210B,
                                       0x2110,  0x2112,  0x2133,  0x211B,
                                       0x212F,  0x210A,  0x2134,  0};
      uint32_t r = alpha_range(cp, 0x1D49C, 0x1D4B6, holes, subs);
      return r ? r : cp;
    }
    case ND_MA_FRAK: {
      static const uint32_t holes[] = {0x1D506, 0x1D50B, 0x1D50C, 0x1D515,
                                       0x1D51D, 0};
      static const uint32_t subs[]  = {0x212D,  0x210C,  0x2111,  0x211C,
                                       0x2128,  0};
      uint32_t r = alpha_range(cp, 0x1D504, 0x1D51E, holes, subs);
      return r ? r : cp;
    }
    case ND_MA_SANS: {
      uint32_t r = alpha_range(cp, 0x1D5A0, 0x1D5BA, NULL, NULL);
      return r ? r : cp;
    }
    case ND_MA_MONO: {
      uint32_t r = alpha_range(cp, 0x1D670, 0x1D68A, NULL, NULL);
      return r ? r : cp;
    }
  }
  return cp;
}
