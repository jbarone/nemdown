/* entity.c — numeric and named HTML entity decoding.
 *
 * The full HTML5 table is 2,231 entries. This ships the ~40 that actually turn
 * up in markdown prose; anything else passes through verbatim, which renders as
 * the literal source and is a perfectly honest failure mode.
 */

#include "doc/entity.h"

#include <stdlib.h>
#include <string.h>

struct ent {
  const char *name;
  unsigned    cp;
};

/* Sorted by name for bsearch. */
static const struct ent entities[] = {
  {"AMP", 38},      {"COPY", 169},    {"GT", 62},       {"LT", 60},
  {"QUOT", 34},     {"REG", 174},     {"amp", 38},      {"apos", 39},
  {"bull", 8226},   {"copy", 169},    {"dagger", 8224}, {"deg", 176},
  {"ge", 8805},     {"gt", 62},       {"harr", 8596},   {"hellip", 8230},
  {"infin", 8734},  {"laquo", 171},   {"larr", 8592},   {"ldquo", 8220},
  {"le", 8804},     {"lsquo", 8216},  {"lt", 60},       {"mdash", 8212},
  {"middot", 183},  {"nbsp", 160},    {"ndash", 8211},  {"ne", 8800},
  {"para", 182},    {"plusmn", 177},  {"quot", 34},     {"raquo", 187},
  {"rarr", 8594},   {"rdquo", 8221},  {"reg", 174},     {"rsquo", 8217},
  {"sect", 167},    {"times", 215},   {"trade", 8482},
};

static int ent_cmp(const void *key, const void *elem) {
  return strcmp(key, ((const struct ent *)elem)->name);
}

static int utf8_encode(unsigned cp, char out[8]) {
  if (cp < 0x80) {
    out[0] = (char)cp;
    return 1;
  }
  if (cp < 0x800) {
    out[0] = (char)(0xC0 | (cp >> 6));
    out[1] = (char)(0x80 | (cp & 0x3F));
    return 2;
  }
  if (cp < 0x10000) {
    out[0] = (char)(0xE0 | (cp >> 12));
    out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[2] = (char)(0x80 | (cp & 0x3F));
    return 3;
  }
  if (cp <= 0x10FFFF) {
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
  }
  return 0;
}

int nd_entity_decode(const char *src, size_t n, char out[8]) {
  if (n < 3 || src[0] != '&' || src[n - 1] != ';') return 0;

  const char *body = src + 1;
  size_t blen = n - 2;

  if (blen > 1 && body[0] == '#') {
    unsigned cp = 0;
    if (body[1] == 'x' || body[1] == 'X') {
      if (blen < 3) return 0;
      for (size_t i = 2; i < blen; i++) {
        char c = body[i];
        unsigned d;
        if (c >= '0' && c <= '9')      d = (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (unsigned)(c - 'A' + 10);
        else return 0;
        cp = cp * 16 + d;
        if (cp > 0x10FFFF) return 0;
      }
    } else {
      for (size_t i = 1; i < blen; i++) {
        if (body[i] < '0' || body[i] > '9') return 0;
        cp = cp * 10 + (unsigned)(body[i] - '0');
        if (cp > 0x10FFFF) return 0;
      }
    }
    if (cp == 0) cp = 0xFFFD;
    return utf8_encode(cp, out);
  }

  char name[32];
  if (blen >= sizeof name) return 0;
  memcpy(name, body, blen);
  name[blen] = '\0';

  const struct ent *e = bsearch(name, entities,
                                sizeof entities / sizeof entities[0],
                                sizeof entities[0], ent_cmp);
  return e ? utf8_encode(e->cp, out) : 0;
}
