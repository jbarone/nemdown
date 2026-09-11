/* callout.c — the callout type table.
 *
 * Icons are Font Awesome codepoints that Nerd Fonts preserve, so they come
 * from the same Hack Nerd Font as the text and need no fallback family.
 */

#include "doc/callout.h"

#include <string.h>

const nd_callout_type nd_callout_types[] = {
  /* The first entry is the fallback for unknown types. */
  {"note",      0x89b4fa, "\xef\x81\x80"}, /* U+F040 pencil       blue     */
  {"abstract",  0x94e2d5, "\xef\x83\xaa"}, /* U+F0EA clipboard    teal     */
  {"summary",   0x94e2d5, "\xef\x83\xaa"},
  {"tldr",      0x94e2d5, "\xef\x83\xaa"},
  {"info",      0x74c7ec, "\xef\x81\x9a"}, /* U+F05A info-circle  sapphire */
  {"todo",      0x74c7ec, "\xef\x81\x98"}, /* U+F058 check-circle sapphire */
  {"tip",       0x94e2d5, "\xef\x83\xab"}, /* U+F0EB lightbulb    teal     */
  {"hint",      0x94e2d5, "\xef\x83\xab"},
  {"important", 0x94e2d5, "\xef\x83\xab"},
  {"success",   0xa6e3a1, "\xef\x80\x8c"}, /* U+F00C check        green    */
  {"check",     0xa6e3a1, "\xef\x80\x8c"},
  {"done",      0xa6e3a1, "\xef\x80\x8c"},
  {"question",  0xf9e2af, "\xef\x81\x99"}, /* U+F059 question     yellow   */
  {"help",      0xf9e2af, "\xef\x81\x99"},
  {"faq",       0xf9e2af, "\xef\x81\x99"},
  {"warning",   0xfab387, "\xef\x81\xb1"}, /* U+F071 triangle     peach    */
  {"caution",   0xfab387, "\xef\x81\xb1"},
  {"attention", 0xfab387, "\xef\x81\xb1"},
  {"failure",   0xf38ba8, "\xef\x81\x97"}, /* U+F057 times-circle red      */
  {"fail",      0xf38ba8, "\xef\x81\x97"},
  {"missing",   0xf38ba8, "\xef\x81\x97"},
  {"danger",    0xf38ba8, "\xef\x83\xa7"}, /* U+F0E7 bolt         red      */
  {"error",     0xf38ba8, "\xef\x83\xa7"},
  {"bug",       0xeba0ac, "\xef\x86\x88"}, /* U+F188 bug          maroon   */
  {"example",   0xcba6f7, "\xef\x83\x8b"}, /* U+F0CB list-ol      mauve    */
  {"quote",     0x9399b2, "\xef\x84\x8d"}, /* U+F10D quote-left   overlay2 */
  {"cite",      0x9399b2, "\xef\x84\x8d"},
};

const unsigned nd_callout_type_count =
    sizeof nd_callout_types / sizeof nd_callout_types[0];

uint8_t nd_callout_lookup(const char *name, size_t len) {
  for (unsigned i = 0; i < nd_callout_type_count; i++) {
    const char *n = nd_callout_types[i].name;
    if (strlen(n) == len && strncasecmp(n, name, len) == 0) return (uint8_t)i;
  }
  return 0; /* unknown types render as `note`, matching Obsidian */
}
