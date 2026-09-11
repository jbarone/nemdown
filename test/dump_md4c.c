/* dump_md4c — print md4c's raw callback stream for a markdown file.
 *
 * Exists to verify parser behaviour before any renderer is written:
 * how `![[embed]]` arrives, whether task marks and offsets are usable,
 * what entities and math spans look like. See the plan, step 0.
 *
 * Usage: dump_md4c <file.md>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <md4c.h>

static int depth;

static void indent(void) {
  for (int i = 0; i < depth; i++) fputs("  ", stdout);
}

static const char *block_name(MD_BLOCKTYPE t) {
  switch (t) {
    case MD_BLOCK_DOC:   return "DOC";
    case MD_BLOCK_QUOTE: return "QUOTE";
    case MD_BLOCK_UL:    return "UL";
    case MD_BLOCK_OL:    return "OL";
    case MD_BLOCK_LI:    return "LI";
    case MD_BLOCK_HR:    return "HR";
    case MD_BLOCK_H:     return "H";
    case MD_BLOCK_CODE:  return "CODE";
    case MD_BLOCK_HTML:  return "HTML";
    case MD_BLOCK_P:     return "P";
    case MD_BLOCK_TABLE: return "TABLE";
    case MD_BLOCK_THEAD: return "THEAD";
    case MD_BLOCK_TBODY: return "TBODY";
    case MD_BLOCK_TR:    return "TR";
    case MD_BLOCK_TH:    return "TH";
    case MD_BLOCK_TD:    return "TD";
  }
  return "?";
}

static const char *span_name(MD_SPANTYPE t) {
  switch (t) {
    case MD_SPAN_EM:                return "EM";
    case MD_SPAN_STRONG:            return "STRONG";
    case MD_SPAN_A:                 return "A";
    case MD_SPAN_IMG:               return "IMG";
    case MD_SPAN_CODE:              return "CODE";
    case MD_SPAN_DEL:               return "DEL";
    case MD_SPAN_LATEXMATH:         return "LATEXMATH";
    case MD_SPAN_LATEXMATH_DISPLAY: return "LATEXMATH_DISPLAY";
    case MD_SPAN_WIKILINK:          return "WIKILINK";
    case MD_SPAN_U:                 return "U";
  }
  return "?";
}

static const char *text_name(MD_TEXTTYPE t) {
  switch (t) {
    case MD_TEXT_NORMAL:    return "NORMAL";
    case MD_TEXT_NULLCHAR:  return "NULLCHAR";
    case MD_TEXT_BR:        return "BR";
    case MD_TEXT_SOFTBR:    return "SOFTBR";
    case MD_TEXT_ENTITY:    return "ENTITY";
    case MD_TEXT_CODE:      return "CODE";
    case MD_TEXT_HTML:      return "HTML";
    case MD_TEXT_LATEXMATH: return "LATEXMATH";
  }
  return "?";
}

/* Print a non-NUL-terminated slice with escapes, truncated for readability. */
static void put_slice(const MD_CHAR *s, MD_SIZE n) {
  MD_SIZE limit = n > 60 ? 60 : n;
  putchar('"');
  for (MD_SIZE i = 0; i < limit; i++) {
    unsigned char c = (unsigned char)s[i];
    if      (c == '\n') fputs("\\n", stdout);
    else if (c == '\t') fputs("\\t", stdout);
    else if (c == '"')  fputs("\\\"", stdout);
    else if (c < 0x20)  printf("\\x%02x", c);
    else                putchar(c);
  }
  if (limit < n) fputs("...", stdout);
  putchar('"');
}

static void put_attr(const char *label, const MD_ATTRIBUTE *a) {
  if (!a || !a->text) { printf(" %s=<null>", label); return; }
  printf(" %s=", label);
  put_slice(a->text, a->size);
}

static int enter_block(MD_BLOCKTYPE type, void *detail, void *ud) {
  (void)ud;
  indent();
  printf("+ %s", block_name(type));
  switch (type) {
    case MD_BLOCK_H:
      printf(" level=%u", ((MD_BLOCK_H_DETAIL *)detail)->level);
      break;
    case MD_BLOCK_CODE: {
      MD_BLOCK_CODE_DETAIL *d = detail;
      put_attr("lang", &d->lang);
      put_attr("info", &d->info);
      printf(" fence='%c'", d->fence_char ? d->fence_char : '-');
      break;
    }
    case MD_BLOCK_UL: {
      MD_BLOCK_UL_DETAIL *d = detail;
      printf(" tight=%d mark='%c'", d->is_tight, d->mark);
      break;
    }
    case MD_BLOCK_OL: {
      MD_BLOCK_OL_DETAIL *d = detail;
      printf(" start=%u tight=%d delim='%c'", d->start, d->is_tight, d->mark_delimiter);
      break;
    }
    case MD_BLOCK_LI: {
      MD_BLOCK_LI_DETAIL *d = detail;
      printf(" is_task=%d", d->is_task);
      if (d->is_task)
        printf(" mark='%c' mark_offset=%u", d->task_mark, d->task_mark_offset);
      break;
    }
    case MD_BLOCK_TH:
    case MD_BLOCK_TD:
      printf(" align=%d", ((MD_BLOCK_TD_DETAIL *)detail)->align);
      break;
    default:
      break;
  }
  putchar('\n');
  depth++;
  return 0;
}

static int leave_block(MD_BLOCKTYPE type, void *detail, void *ud) {
  (void)detail; (void)ud;
  depth--;
  indent();
  printf("- %s\n", block_name(type));
  return 0;
}

static int enter_span(MD_SPANTYPE type, void *detail, void *ud) {
  (void)ud;
  indent();
  printf("  <%s", span_name(type));
  if (type == MD_SPAN_A) {
    MD_SPAN_A_DETAIL *d = detail;
    put_attr("href", &d->href);
    put_attr("title", &d->title);
  } else if (type == MD_SPAN_IMG) {
    MD_SPAN_IMG_DETAIL *d = detail;
    put_attr("src", &d->src);
    put_attr("title", &d->title);
  } else if (type == MD_SPAN_WIKILINK) {
    MD_SPAN_WIKILINK_DETAIL *d = detail;
    put_attr("target", &d->target);
  }
  printf(">\n");
  depth++;
  return 0;
}

static int leave_span(MD_SPANTYPE type, void *detail, void *ud) {
  (void)detail; (void)ud;
  depth--;
  indent();
  printf("  </%s>\n", span_name(type));
  return 0;
}

static int text_cb(MD_TEXTTYPE type, const MD_CHAR *text, MD_SIZE size, void *ud) {
  (void)ud;
  indent();
  printf("  . %-9s ", text_name(type));
  put_slice(text, size);
  putchar('\n');
  return 0;
}

int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: dump_md4c <file.md>\n");
    return 2;
  }

  FILE *f = fopen(argv[1], "rb");
  if (!f) { perror(argv[1]); return 1; }
  fseek(f, 0, SEEK_END);
  long len = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *buf = malloc((size_t)len + 1);
  if (!buf || fread(buf, 1, (size_t)len, f) != (size_t)len) {
    fprintf(stderr, "read failed\n");
    return 1;
  }
  buf[len] = '\0';
  fclose(f);

  /* Skip YAML frontmatter exactly as the real parser will: the fence is
   * located lexically, and a missing terminator means there is none. */
  size_t off = 0;
  if (len >= 4 && strncmp(buf, "---\n", 4) == 0) {
    const char *end = strstr(buf + 4, "\n---\n");
    if (end) {
      off = (size_t)(end - buf) + 5;
      printf("[frontmatter: %zu bytes stripped, body_offset=%zu]\n\n", off - 4, off);
    } else {
      printf("[leading --- but no terminator: treating whole file as markdown]\n\n");
    }
  }

  MD_PARSER parser = {
    .abi_version = 0,
    .flags = MD_FLAG_COLLAPSEWHITESPACE | MD_FLAG_PERMISSIVEATXHEADERS |
             MD_FLAG_PERMISSIVEAUTOLINKS | MD_FLAG_NOINDENTEDCODEBLOCKS |
             MD_FLAG_NOHTML | MD_FLAG_TABLES | MD_FLAG_STRIKETHROUGH |
             MD_FLAG_TASKLISTS | MD_FLAG_LATEXMATHSPANS | MD_FLAG_WIKILINKS,
    .enter_block = enter_block,
    .leave_block = leave_block,
    .enter_span  = enter_span,
    .leave_span  = leave_span,
    .text        = text_cb,
    .debug_log   = NULL,
    .syntax      = NULL,
  };

  int rc = md_parse(buf + off, (MD_SIZE)((size_t)len - off), &parser, NULL);
  free(buf);
  if (rc != 0) fprintf(stderr, "md_parse failed: %d\n", rc);
  return rc;
}
