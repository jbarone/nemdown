/* utf8.c — in-place, length-preserving UTF-8 scrub. */

#include "util/utf8.h"

#include <stdbool.h>
#include <stdint.h>

size_t nd_utf8_scrub(char *buf, size_t len) {
  size_t i = 0, replaced = 0;

  while (i < len) {
    unsigned char c = (unsigned char)buf[i];

    /* NUL is a valid code point but breaks the NUL-terminated half of the
     * contract, so it goes too. */
    if (c == 0x00) { buf[i++] = '?'; replaced++; continue; }
    if (c < 0x80)  { i++; continue; }

    unsigned need;
    uint32_t cp;
    if      ((c & 0xE0) == 0xC0) { need = 1; cp = c & 0x1Fu; }
    else if ((c & 0xF0) == 0xE0) { need = 2; cp = c & 0x0Fu; }
    else if ((c & 0xF8) == 0xF0) { need = 3; cp = c & 0x07u; }
    else { /* a stray continuation byte, or 0xF8..0xFF */
      buf[i++] = '?'; replaced++; continue;
    }

    /* Truncated at the end of the buffer: this is the exact shape that hangs
     * Pango, so it must not survive. */
    if (i + need >= len + 1 || i + need > len - 1) {
      buf[i++] = '?'; replaced++; continue;
    }

    unsigned k;
    for (k = 1; k <= need; k++) {
      unsigned char cc = (unsigned char)buf[i + k];
      if ((cc & 0xC0) != 0x80) break;
      cp = (cp << 6) | (cc & 0x3Fu);
    }
    if (k <= need) { /* a continuation byte was missing */
      buf[i++] = '?'; replaced++; continue;
    }

    /* Well-formedness, not just decodability. */
    bool bad = false;
    if (need == 1 && cp < 0x80)      bad = true;   /* overlong */
    if (need == 2 && cp < 0x800)     bad = true;   /* overlong */
    if (need == 3 && cp < 0x10000)   bad = true;   /* overlong */
    if (cp > 0x10FFFF)               bad = true;   /* out of range */
    if (cp >= 0xD800 && cp <= 0xDFFF) bad = true;  /* surrogate */

    if (bad) { buf[i++] = '?'; replaced++; continue; }

    i += need + 1;
  }
  return replaced;
}
