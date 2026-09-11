/* toc.c — heading collection, level normalisation, and slugs. */

#include "doc/toc.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static void collect(nd_block *b, nd_block ***vec, size_t *n, size_t *cap) {
  if (b->kind == ND_HEADING) {
    if (*n == *cap) {
      *cap = *cap ? *cap * 2 : 32;
      *vec = realloc(*vec, *cap * sizeof **vec);
    }
    (*vec)[(*n)++] = b;
  }
  for (uint32_t i = 0; i < b->nkids; i++) collect(b->kids[i], vec, n, cap);
}

char *nd_toc_slugify(struct nd_arena *a, const char *text) {
  size_t n = strlen(text);
  char *out = nd_arena_alloc(a, n + 1);
  size_t k = 0;
  bool prev_dash = true; /* suppress a leading dash */

  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)text[i];
    if (isalnum(c)) {
      out[k++] = (char)tolower(c);
      prev_dash = false;
    } else if (!prev_dash) {
      out[k++] = '-';
      prev_dash = true;
    }
  }
  while (k > 0 && out[k - 1] == '-') k--;
  out[k] = '\0';
  return out;
}

void nd_toc_build(struct nd_arena *a, nd_block *root,
                  struct nd_toc_store *store, nd_toc *view) {
  nd_block **heads = NULL;
  size_t n = 0, cap = 0;
  for (uint32_t i = 0; i < root->nkids; i++) collect(root->kids[i], &heads, &n, &cap);

  store->items  = n ? nd_arena_calloc(a, n * sizeof *store->items) : NULL;
  store->blocks = n ? nd_arena_memdup(a, heads, n * sizeof *heads) : NULL;
  store->count  = n;

  /* Normalise levels with a stack so an H1 -> H4 jump indents one step rather
   * than three. Authors number headings inconsistently; a TOC should not. */
  uint8_t stack[8];
  int sp = 0;

  for (size_t i = 0; i < n; i++) {
    nd_block *h = heads[i];
    uint8_t level = h->level ? h->level : 1;

    while (sp > 0 && stack[sp - 1] >= level) sp--;
    store->items[i].depth = (uint8_t)sp;
    if (sp < (int)(sizeof stack)) stack[sp++] = level;

    store->items[i].level = level;
    store->items[i].text  = h->inl.text ? h->inl.text : "";
    store->items[i].slug  = nd_toc_slugify(a, store->items[i].text);
    store->items[i].y     = h->lay.y;
  }

  free(heads);
  view->items = store->items;
  view->count = store->count;
}

void nd_toc_refresh_offsets(struct nd_toc_store *store) {
  for (size_t i = 0; i < store->count; i++)
    store->items[i].y = store->blocks[i]->lay.y;
}

char *nd_toc_document_title(struct nd_arena *a, nd_block *root,
                            const char *path) {
  for (uint32_t i = 0; i < root->nkids; i++) {
    nd_block *k = root->kids[i];
    if (k->kind == ND_HEADING && k->level == 1 && k->inl.text && k->inl.len)
      return nd_arena_strndup(a, k->inl.text, k->inl.len);
  }
  const char *base = strrchr(path, '/');
  base = base ? base + 1 : path;
  return nd_arena_strndup(a, base, strlen(base));
}
