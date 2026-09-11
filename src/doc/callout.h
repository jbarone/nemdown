/* callout.h — Obsidian callouts: `> [!type] Title`. */

#ifndef NEMDOWN_CALLOUT_H
#define NEMDOWN_CALLOUT_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
  const char *name;
  uint32_t    color;
  const char *icon; /* Nerd Font glyph, UTF-8 */
} nd_callout_type;

extern const nd_callout_type nd_callout_types[];
extern const unsigned nd_callout_type_count;

/* Index into nd_callout_types; falls back to `note` for unknown names. */
uint8_t nd_callout_lookup(const char *name, size_t len);

#endif /* NEMDOWN_CALLOUT_H */
