/* arena.h — bump allocator for the document tree.
 *
 * The tree has exactly one lifetime: built once, destroyed wholesale on reload.
 * That makes per-node free() pure overhead, and makes leaking or double-freeing
 * a node structurally impossible.
 *
 * Anything with a real destructor (PangoLayout, cairo_surface_t) must NOT live
 * here; those are tracked separately and torn down before the arena resets.
 */

#ifndef NEMDOWN_ARENA_H
#define NEMDOWN_ARENA_H

#include <stddef.h>

struct nd_arena_chunk;

struct nd_arena {
  struct nd_arena_chunk *head;
  size_t total;
};

void  nd_arena_init(struct nd_arena *a);
void *nd_arena_alloc(struct nd_arena *a, size_t n);
void *nd_arena_calloc(struct nd_arena *a, size_t n);
void *nd_arena_memdup(struct nd_arena *a, const void *src, size_t n);
/* Copies n bytes and appends a NUL: md4c strings are not NUL-terminated. */
char *nd_arena_strndup(struct nd_arena *a, const char *src, size_t n);
void  nd_arena_reset(struct nd_arena *a);

#endif /* NEMDOWN_ARENA_H */
