/* preprocess.h — source fixups applied before md4c sees the bytes.
 *
 * Two jobs, both of which md4c cannot do for us:
 *
 *  1. Split the YAML frontmatter off. md4c would read a leading `---` as a
 *     thematic break and the line after it as a setext heading.
 *
 *  2. Rewrite Obsidian embeds. Verified with the callback dumper: `[[x]]`
 *     parses as MD_SPAN_WIKILINK, but `![[x.png]]` produces no span at all —
 *     md4c tries an image, fails, and emits the whole construct as literal
 *     text. Rewriting it to `![size](<target>)` turns embeds into ordinary
 *     MD_SPAN_IMG, which is both correct and simpler downstream.
 */

#ifndef NEMDOWN_PREPROCESS_H
#define NEMDOWN_PREPROCESS_H

#include <stdbool.h>
#include <stddef.h>

struct nd_arena;

/* Rewriting embeds shifts every byte after them, so md4c's offsets no longer
 * index the real file. Each rewrite records a breakpoint, and mapping an offset
 * back is a binary search over them. Without this, checkbox write-back would
 * corrupt any document containing an embed above the checkbox. */
struct nd_edit {
  size_t new_off;  /* offset in the rewritten body */
  size_t orig_off; /* corresponding offset in the original file */
};

struct nd_source {
  const char *yaml;      /* frontmatter region, or NULL */
  size_t      yaml_len;
  size_t      body_offset; /* byte offset of the body within the ORIGINAL file */
  char       *body;      /* markdown handed to md4c; arena-owned */
  size_t      body_len;

  struct nd_edit *edits; /* arena-owned, ascending by new_off */
  size_t          nedits;
};

/* Maps an md4c offset (into `body`) to a byte offset in the original file. */
size_t nd_source_orig_offset(const struct nd_source *s, size_t body_off);

/* Splits frontmatter and rewrites embeds. `buf` must outlive the result. */
void nd_preprocess(struct nd_arena *a, const char *buf, size_t len,
                   struct nd_source *out);

#endif /* NEMDOWN_PREPROCESS_H */
