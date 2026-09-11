/* arena.c — bump allocator. */

#include "doc/arena.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "util/log.h"

#define ND_ARENA_CHUNK (64 * 1024)
#define ND_ARENA_ALIGN 16

struct nd_arena_chunk {
  struct nd_arena_chunk *next;
  size_t used, cap;
  /* Payload follows inline; aligned because the header is a multiple of 16. */
  _Alignas(ND_ARENA_ALIGN) unsigned char data[];
};

void nd_arena_init(struct nd_arena *a) {
  a->head = NULL;
  a->total = 0;
}

static struct nd_arena_chunk *chunk_new(size_t need) {
  size_t cap = need > ND_ARENA_CHUNK ? need : ND_ARENA_CHUNK;
  struct nd_arena_chunk *c = malloc(sizeof *c + cap);
  if (!c) nd_die("out of memory");
  c->next = NULL;
  c->used = 0;
  c->cap  = cap;
  return c;
}

void *nd_arena_alloc(struct nd_arena *a, size_t n) {
  if (n == 0) n = 1;
  n = (n + ND_ARENA_ALIGN - 1) & ~(size_t)(ND_ARENA_ALIGN - 1);

  if (!a->head || a->head->used + n > a->head->cap) {
    struct nd_arena_chunk *c = chunk_new(n);
    c->next = a->head;
    a->head = c;
  }

  void *p = a->head->data + a->head->used;
  a->head->used += n;
  a->total += n;
  return p;
}

void *nd_arena_calloc(struct nd_arena *a, size_t n) {
  void *p = nd_arena_alloc(a, n);
  memset(p, 0, n);
  return p;
}

void *nd_arena_memdup(struct nd_arena *a, const void *src, size_t n) {
  void *p = nd_arena_alloc(a, n);
  if (n) memcpy(p, src, n);
  return p;
}

char *nd_arena_strndup(struct nd_arena *a, const char *src, size_t n) {
  char *p = nd_arena_alloc(a, n + 1);
  if (n) memcpy(p, src, n);
  p[n] = '\0';
  return p;
}

void nd_arena_reset(struct nd_arena *a) {
  struct nd_arena_chunk *c = a->head;
  while (c) {
    struct nd_arena_chunk *next = c->next;
    free(c);
    c = next;
  }
  a->head = NULL;
  a->total = 0;
}
