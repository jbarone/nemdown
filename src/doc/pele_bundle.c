/* pele_bundle.c — pele's JavaScript, linked into the binary.
 *
 * Embedded rather than installed beside the binary so that the two cannot
 * drift: the bundle a build was tested with is the one it runs, and there is
 * no runtime path to resolve or to go missing. Updating pele is replacing
 * vendor/pele/pele.global.js (tools/update-pele.sh) and rebuilding; the
 * Makefile lists that file as a prerequisite of this object, because .incbin
 * is invisible to the compiler's dependency output.
 *
 * .incbin rather than a generated array: 709KB of bytes as C source is a
 * five-megabyte file that is slow to compile and says nothing. The path is
 * relative to the directory make runs in, which is the repository root.
 *
 * A trailing NUL is appended because JS_Eval requires one past the length.
 */

#include "doc/mermaid_int.h"

__asm__(
  "  .section .rodata\n"
  "  .balign 16\n"
  "  .global nd_pele_js\n"
  "  .hidden nd_pele_js\n"
  "  .type nd_pele_js, @object\n"
  "nd_pele_js:\n"
  "  .incbin \"vendor/pele/pele.global.js\"\n"
  "  .global nd_pele_js_end\n"
  "  .hidden nd_pele_js_end\n"
  "nd_pele_js_end:\n"
  "  .byte 0\n"
  "  .previous\n");
