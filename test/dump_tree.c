/* dump_tree — parse a markdown file and print the retained block tree.
 *
 * Validates the block and span stacks with no graphics involved.
 *
 * Usage: dump_tree <file.md>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "doc/arena.h"
#include "doc/parse.h"
#include "doc/frontmatter.h"
#include "doc/postprocess.h"
#include "doc/preprocess.h"

static const char *kind_name(nd_block_kind k) {
  switch (k) {
    case ND_DOC_ROOT:   return "DOC";
    case ND_PARA:       return "PARA";
    case ND_HEADING:    return "H";
    case ND_CODE:       return "CODE";
    case ND_MATH_BLOCK: return "MATH";
    case ND_QUOTE:      return "QUOTE";
    case ND_CALLOUT:    return "CALLOUT";
    case ND_LIST:       return "LIST";
    case ND_ITEM:       return "ITEM";
    case ND_HR:         return "HR";
    case ND_TABLE:      return "TABLE";
    case ND_ROW:        return "ROW";
    case ND_CELL:       return "CELL";
    case ND_IMAGE:      return "IMAGE";
  }
  return "?";
}

static void put_flags(uint32_t f) {
  if (f & ND_RUN_EM)        fputs("em ", stdout);
  if (f & ND_RUN_STRONG)    fputs("strong ", stdout);
  if (f & ND_RUN_CODE)      fputs("code ", stdout);
  if (f & ND_RUN_DEL)       fputs("del ", stdout);
  if (f & ND_RUN_U)         fputs("u ", stdout);
  if (f & ND_RUN_LINK)      fputs("link ", stdout);
  if (f & ND_RUN_WIKILINK)  fputs("wikilink ", stdout);
  if (f & ND_RUN_MATH)      fputs("math ", stdout);
  if (f & ND_RUN_HIGHLIGHT) fputs("highlight ", stdout);
}

static void put_text(const char *s, uint32_t n) {
  uint32_t limit = n > 54 ? 54 : n;
  putchar('"');
  for (uint32_t i = 0; i < limit; i++) {
    if (s[i] == '\n') fputs("\\n", stdout);
    else putchar(s[i]);
  }
  if (limit < n) fputs("...", stdout);
  putchar('"');
}

static void walk(const nd_block *b, int depth) {
  for (int i = 0; i < depth; i++) fputs("  ", stdout);
  printf("%s", kind_name(b->kind));

  switch (b->kind) {
    case ND_HEADING: printf(" level=%u", b->level); break;
    case ND_LIST:
      printf(" %s tight=%u", b->list_ordered ? "ordered" : "bullet", b->list_tight);
      if (b->list_ordered) printf(" start=%u", b->list_start);
      break;
    case ND_ITEM:
      if (b->item_is_task)
        printf(" task checked=%u src_offset=%u", b->item_checked, b->item_mark_offset);
      break;
    case ND_CODE:  printf(" lang=%s len=%u", b->code_lang ? b->code_lang : "-", b->code_len); break;
    case ND_CELL:  printf(" align=%u", b->cell_align); break;
    case ND_ROW:   printf(" header=%u", b->row_is_header); break;
    case ND_IMAGE: printf(" src=%s", b->img_src ? b->img_src : "-"); break;
    case ND_CALLOUT:
      printf(" type=%u folded=%u title=\"%.*s\"", b->callout_type,
             b->callout_folded, (int)b->title.len,
             b->title.text ? b->title.text : "");
      break;
    default: break;
  }

  if (b->inl.len) { fputs(" ", stdout); put_text(b->inl.text, b->inl.len); }
  putchar('\n');

  for (uint32_t i = 0; i < b->inl.nruns; i++) {
    const nd_run *r = &b->inl.runs[i];
    for (int k = 0; k <= depth; k++) fputs("  ", stdout);
    printf("  run[%u,%u) d=%u ", r->start, r->end, r->depth);
    put_flags(r->flags);
    if (r->href) printf("-> %s", r->href);
    putchar('\n');
  }

  for (uint32_t i = 0; i < b->nkids; i++) walk(b->kids[i], depth + 1);
}

int main(int argc, char **argv) {
  if (argc != 2) { fprintf(stderr, "usage: dump_tree <file.md>\n"); return 2; }

  FILE *f = fopen(argv[1], "rb");
  if (!f) { perror(argv[1]); return 1; }
  fseek(f, 0, SEEK_END);
  long len = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *buf = malloc((size_t)len + 1);
  if (!buf || fread(buf, 1, (size_t)len, f) != (size_t)len) return 1;
  buf[len] = '\0';
  fclose(f);

  struct nd_arena a;
  nd_arena_init(&a);

  struct nd_source src;
  nd_preprocess(&a, buf, (size_t)len, &src);
  printf("[body_offset=%zu yaml=%zu bytes]\n", src.body_offset, src.yaml_len);

  nd_props props;
  nd_frontmatter_parse(&a, src.yaml, src.yaml_len, &props);
  static const char *tn[] = {"text","list","number","checkbox","date","datetime","empty"};
  printf("[properties: %zu%s]\n", props.count, props.error ? " WITH ERROR" : "");
  if (props.error) printf("  error line %d: %s\n", props.error_line, props.error);
  for (size_t i = 0; i < props.count; i++) {
    const nd_property *pr = &props.items[i];
    printf("  %-10s %-9s ", pr->key, tn[pr->type]);
    if (pr->type == ND_PROP_LIST) {
      for (size_t k = 0; k < pr->nitems; k++) printf("%s%s", k?", ":"", pr->items[k]);
    } else if (pr->type == ND_PROP_CHECKBOX) {
      printf("%s", pr->checkbox ? "true" : "false");
    } else if (pr->type == ND_PROP_NUMBER) {
      printf("%g", pr->number);
    } else if (pr->text) {
      printf("%s", pr->text);
    }
    putchar('\n');
  }

  nd_block *root = nd_parse(&a, &src);
  if (!root) { fprintf(stderr, "parse failed\n"); return 1; }
  nd_postprocess(&a, root);
  walk(root, 0);

  printf("[arena: %zu bytes]\n", a.total);
  nd_arena_reset(&a);
  free(buf);
  return 0;
}
