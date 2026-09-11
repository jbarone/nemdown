/* wikilink.c — wikilink resolution. */

#include "doc/wikilink.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static bool is_file(const char *p) {
  struct stat st;
  return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

char *nd_wikilink_split(struct nd_arena *a, const char *target,
                        const char **anchor) {
  *anchor = NULL;
  if (!target) return NULL;

  const char *hash = strchr(target, '#');
  size_t plen = hash ? (size_t)(hash - target) : strlen(target);

  char *path = nd_arena_strndup(a, target, plen);
  if (hash && hash[1]) *anchor = hash + 1;
  return path;
}

/* Tries `<dir>/<name>.md` then `<dir>/<name>`, so both `[[note]]` and
 * `[[note.md]]` work. Returns an arena-owned absolute-ish path, or NULL. */
static char *resolve(struct nd_arena *a, const char *dir, const char *name) {
  if (!name || !*name) return NULL;

  char buf[4096];

  snprintf(buf, sizeof buf, "%s/%s.md", dir, name);
  if (is_file(buf)) return nd_arena_strndup(a, buf, strlen(buf));

  snprintf(buf, sizeof buf, "%s/%s", dir, name);
  if (is_file(buf)) return nd_arena_strndup(a, buf, strlen(buf));

  return NULL;
}

static void resolve_inline(struct nd_arena *a, nd_inline *inl, const char *dir) {
  for (uint32_t i = 0; i < inl->nruns; i++) {
    nd_run *r = &inl->runs[i];
    if (!(r->flags & ND_RUN_WIKILINK) || !r->href) continue;

    const char *anchor = NULL;
    char *name = nd_wikilink_split(a, r->href, &anchor);
    char *path = resolve(a, dir, name);

    if (!path) {
      /* Styled differently rather than dropped: the document should still
       * read correctly, but a link that goes nowhere should look like one. */
      r->flags |= ND_RUN_WIKILINK_DEAD;
      continue;
    }

    /* Keep the anchor attached so the click can scroll to it after loading. */
    if (anchor) {
      char joined[4096];
      snprintf(joined, sizeof joined, "%s#%s", path, anchor);
      r->href = nd_arena_strndup(a, joined, strlen(joined));
    } else {
      r->href = path;
    }
  }
}

void nd_wikilink_resolve_tree(struct nd_arena *a, nd_block *root,
                              const char *dir) {
  resolve_inline(a, &root->inl, dir);
  resolve_inline(a, &root->title, dir);
  for (uint32_t i = 0; i < root->nkids; i++)
    nd_wikilink_resolve_tree(a, root->kids[i], dir);
}
