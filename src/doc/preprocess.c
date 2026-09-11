/* preprocess.c — frontmatter split and Obsidian embed rewriting. */

#include "doc/preprocess.h"

#include <stdlib.h>
#include <string.h>

#include "doc/arena.h"

/* True if the line starting at p (length avail) is exactly `fence`, allowing
 * trailing spaces and a CR. */
static bool line_is(const char *p, size_t avail, const char *fence) {
  size_t n = strlen(fence);
  if (avail < n || strncmp(p, fence, n) != 0) return false;
  for (size_t i = n; i < avail; i++) {
    if (p[i] == '\n') return true;
    if (p[i] != ' ' && p[i] != '\t' && p[i] != '\r') return false;
  }
  return true; /* fence on the final line, no newline */
}

static size_t next_line(const char *buf, size_t len, size_t i) {
  while (i < len && buf[i] != '\n') i++;
  return i < len ? i + 1 : len;
}

/* Locate the frontmatter fence. Returns the body offset; 0 when there is none. */
static size_t split_frontmatter(const char *buf, size_t len,
                                const char **yaml, size_t *yaml_len) {
  *yaml = NULL;
  *yaml_len = 0;

  size_t i = 0;
  if (len >= 3 && (unsigned char)buf[0] == 0xEF &&
      (unsigned char)buf[1] == 0xBB && (unsigned char)buf[2] == 0xBF)
    i = 3; /* BOM */

  if (!line_is(buf + i, len - i, "---")) return 0;

  size_t yaml_start = next_line(buf, len, i);
  size_t p = yaml_start;
  while (p < len) {
    if (line_is(buf + p, len - p, "---") || line_is(buf + p, len - p, "...")) {
      *yaml = buf + yaml_start;
      *yaml_len = p - yaml_start;
      return next_line(buf, len, p);
    }
    p = next_line(buf, len, p);
  }

  /* A leading `---` with no terminator is not frontmatter — it is a thematic
   * break. Without this fallback the whole document would be swallowed. */
  return 0;
}

/* Growable byte buffer backed by realloc; the final contents are copied into
 * the arena once the size is known. */
struct buf {
  char  *p;
  size_t len, cap;
};

struct edits {
  struct nd_edit *p;
  size_t len, cap;
};

static void edits_put(struct edits *e, size_t new_off, size_t orig_off) {
  if (e->len == e->cap) {
    e->cap = e->cap ? e->cap * 2 : 16;
    e->p = realloc(e->p, e->cap * sizeof *e->p);
  }
  e->p[e->len].new_off = new_off;
  e->p[e->len].orig_off = orig_off;
  e->len++;
}

static void buf_put(struct buf *b, const char *s, size_t n) {
  if (b->len + n > b->cap) {
    size_t cap = b->cap ? b->cap * 2 : 4096;
    while (cap < b->len + n) cap *= 2;
    b->p = realloc(b->p, cap);
    b->cap = cap;
  }
  memcpy(b->p + b->len, s, n);
  b->len += n;
}

/* Rewrite `![[target|size]]` to `![size](<target>)`.
 *
 * Fenced code blocks and inline code spans are skipped: a document that
 * documents Obsidian syntax must survive being rendered by it. */
static void rewrite_embeds(const char *src, size_t len, struct buf *out,
                           struct edits *edits) {
  size_t i = 0;
  bool in_fence = false;
  char fence_char = 0;
  size_t fence_len = 0;
  bool at_line_start = true;

  while (i < len) {
    /* Fenced code block delimiters, which are line-oriented. */
    if (at_line_start) {
      size_t j = i, indent = 0;
      while (j < len && (src[j] == ' ' || src[j] == '\t') && indent < 4) { j++; indent++; }
      if (j < len && (src[j] == '`' || src[j] == '~')) {
        char c = src[j];
        size_t run = 0;
        while (j + run < len && src[j + run] == c) run++;
        if (run >= 3) {
          if (!in_fence) {
            in_fence = true; fence_char = c; fence_len = run;
          } else if (c == fence_char && run >= fence_len) {
            in_fence = false;
          }
          size_t eol = next_line(src, len, i);
          buf_put(out, src + i, eol - i);
          i = eol;
          continue;
        }
      }
    }

    if (in_fence) {
      size_t eol = next_line(src, len, i);
      buf_put(out, src + i, eol - i);
      i = eol;
      at_line_start = true;
      continue;
    }

    /* Inline code span: copy the backtick run and everything to its match. */
    if (src[i] == '`') {
      size_t run = 0;
      while (i + run < len && src[i + run] == '`') run++;
      size_t j = i + run;
      while (j < len) {
        if (src[j] == '`') {
          size_t r2 = 0;
          while (j + r2 < len && src[j + r2] == '`') r2++;
          if (r2 == run) { j += r2; break; }
          j += r2;
        } else {
          j++;
        }
      }
      buf_put(out, src + i, j - i);
      at_line_start = false;
      i = j;
      continue;
    }

    /* The embed itself. */
    if (src[i] == '!' && i + 2 < len && src[i + 1] == '[' && src[i + 2] == '[') {
      size_t j = i + 3;
      while (j + 1 < len && !(src[j] == ']' && src[j + 1] == ']') && src[j] != '\n')
        j++;
      if (j + 1 < len && src[j] == ']' && src[j + 1] == ']') {
        const char *inner = src + i + 3;
        size_t inner_len = j - (i + 3);

        /* `|` separates the target from an Obsidian size hint. */
        const char *bar = memchr(inner, '|', inner_len);
        size_t tlen = bar ? (size_t)(bar - inner) : inner_len;
        const char *size = bar ? bar + 1 : NULL;
        size_t slen = bar ? inner_len - tlen - 1 : 0;

        buf_put(out, "![", 2);
        if (slen) buf_put(out, size, slen); /* alt text carries the size hint */
        buf_put(out, "](<", 3);
        buf_put(out, inner, tlen);
        buf_put(out, ">)", 2);
        i = j + 2;
        /* Everything after this point is shifted; record where. */
        edits_put(edits, out->len, i);
        at_line_start = false;
        continue;
      }
    }

    at_line_start = (src[i] == '\n');
    buf_put(out, src + i, 1);
    i++;
  }
}

void nd_preprocess(struct nd_arena *a, const char *buf, size_t len,
                   struct nd_source *out) {
  memset(out, 0, sizeof *out);

  out->body_offset = split_frontmatter(buf, len, &out->yaml, &out->yaml_len);

  const char *body = buf + out->body_offset;
  size_t body_len = len - out->body_offset;

  struct buf b = {0};
  struct edits e = {0};
  rewrite_embeds(body, body_len, &b, &e);

  out->body = nd_arena_strndup(a, b.p ? b.p : "", b.len);
  out->body_len = b.len;
  free(b.p);

  if (e.len) {
    out->edits = nd_arena_memdup(a, e.p, e.len * sizeof *e.p);
    out->nedits = e.len;
  }
  free(e.p);
}

size_t nd_source_orig_offset(const struct nd_source *s, size_t body_off) {
  /* No rewrites before this point means the body is byte-identical so far. */
  size_t base_new = 0, base_orig = 0;

  size_t lo = 0, hi = s->nedits;
  while (lo < hi) {
    size_t mid = (lo + hi) / 2;
    if (s->edits[mid].new_off <= body_off) lo = mid + 1;
    else                                   hi = mid;
  }
  if (lo > 0) {
    base_new  = s->edits[lo - 1].new_off;
    base_orig = s->edits[lo - 1].orig_off;
  }
  return s->body_offset + base_orig + (body_off - base_new);
}
