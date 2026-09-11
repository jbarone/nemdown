/* check_utf8.c — differential test of nd_utf8_scrub against g_utf8_validate.
 *
 * The scrub is what keeps pango_get_log_attrs() from looping forever on a
 * buffer that ends mid-sequence, so it is swept by brute force rather than
 * spot-checked: every 1-, 2- and 3-byte buffer exhaustively, every 4-byte
 * lead against a set of interesting continuation bytes, and every truncated
 * prefix sitting at the end of the buffer — the shape that actually froze the
 * viewer.
 *
 * Three properties are asserted on each input. The third matters as much as
 * the first: a scrub that damaged well-formed text would be a silent data
 * bug rather than a visible crash.
 *
 *   1. the output is well-formed UTF-8 over exactly [0, len)
 *   2. no NUL survives, since node.h promises no interior NUL
 *   3. an already-clean input comes back byte-identical, with no replacements
 *
 * Usage: build/check_utf8
 */

#include <glib.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "util/utf8.h"

static unsigned long long checked, bad_output, damaged_valid, nul_survived;

static void show(const char *tag, const unsigned char *b, size_t len) {
  fprintf(stderr, "  %s:", tag);
  for (size_t i = 0; i < len; i++) fprintf(stderr, " %02x", b[i]);
  fputc('\n', stderr);
}

static void check(const unsigned char *in, size_t len) {
  unsigned char out[8];
  memcpy(out, in, len);
  size_t replaced = nd_utf8_scrub((char *)out, len);
  checked++;

  if (!g_utf8_validate((const char *)out, (gssize)len, NULL)) {
    if (bad_output++ < 5) {
      fprintf(stderr, "still invalid after scrub:\n");
      show("in ", in, len);
      show("out", out, len);
    }
  }

  for (size_t i = 0; i < len; i++) {
    if (out[i] != 0x00) continue;
    if (nul_survived++ < 5) show("NUL survived", in, len);
    break;
  }

  /* Length preservation is structural — the scrub writes in place and never
   * moves a byte — so what is worth asserting is that a clean input is left
   * completely alone, replacement count included. */
  bool clean = g_utf8_validate((const char *)in, (gssize)len, NULL);
  for (size_t i = 0; clean && i < len; i++)
    if (in[i] == 0x00) clean = false;

  if (clean && (replaced != 0 || memcmp(in, out, len) != 0)) {
    if (damaged_valid++ < 5) {
      fprintf(stderr, "damaged a well-formed sequence:\n");
      show("in ", in, len);
      show("out", out, len);
    }
  }
}

/* Lead bytes, continuations, boundaries and the surrogate and out-of-range
 * edges — enough that the sampled 4-byte sweep still lands on every class the
 * scrub distinguishes. */
static const unsigned char interesting[] = {
  0x00, 0x01, 0x08, 0x0a, 0x20, 0x3f, 0x41, 0x7e, 0x7f, 0x80, 0x81, 0x8f,
  0x90, 0x9f, 0xa0, 0xad, 0xbe, 0xbf, 0xc0, 0xc1, 0xc2, 0xdf, 0xe0, 0xe1,
  0xed, 0xef, 0xf0, 0xf4, 0xf5, 0xf7, 0xf8, 0xfe, 0xff
};
#define NI (sizeof interesting)

int main(void) {
  unsigned char b[8];

  for (unsigned x = 0; x < 256; x++) {
    b[0] = (unsigned char)x;
    check(b, 1);
  }
  for (unsigned x = 0; x < 256; x++)
    for (unsigned y = 0; y < 256; y++) {
      b[0] = (unsigned char)x; b[1] = (unsigned char)y;
      check(b, 2);
    }
  for (unsigned x = 0; x < 256; x++)
    for (unsigned y = 0; y < 256; y++)
      for (unsigned z = 0; z < 256; z++) {
        b[0] = (unsigned char)x; b[1] = (unsigned char)y;
        b[2] = (unsigned char)z;
        check(b, 3);
      }

  for (unsigned x = 0; x < 256; x++)
    for (size_t i = 0; i < NI; i++)
      for (size_t j = 0; j < NI; j++)
        for (size_t k = 0; k < NI; k++) {
          b[0] = (unsigned char)x; b[1] = interesting[i];
          b[2] = interesting[j];   b[3] = interesting[k];
          check(b, 4);
        }

  /* Every prefix of a multi-byte sequence, ending exactly at the buffer end.
   * A lead byte promising continuations that are not there is the input that
   * hung Pango, so it gets swept on its own rather than only incidentally. */
  for (unsigned x = 0; x < 256; x++)
    for (size_t i = 0; i < NI; i++)
      for (size_t j = 0; j < NI; j++) {
        b[0] = (unsigned char)x; b[1] = interesting[i]; b[2] = interesting[j];
        check(b, 1);
        check(b, 2);
        check(b, 3);
      }

  printf("%s: %llu inputs, %llu invalid after scrub, %llu damaged valid, "
         "%llu NUL survived\n",
         (bad_output || damaged_valid || nul_survived) ? "FAIL" : "ok",
         checked, bad_output, damaged_valid, nul_survived);
  return (bad_output || damaged_valid || nul_survived) ? 1 : 0;
}
