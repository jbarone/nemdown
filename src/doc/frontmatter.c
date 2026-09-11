/* frontmatter.c — libyaml event-driven parse of Obsidian properties.
 *
 * The event API rather than the document API: frontmatter is a flat mapping, so
 * a three-state loop needs no DOM, and — the deciding reason — the scalar event
 * carries `plain_implicit`, which is the only way to tell `true` (a checkbox)
 * from `"true"` (text), or a date from a quoted string that looks like one.
 */

#include "doc/frontmatter.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <yaml.h>

struct pvec {
  nd_property *p;
  size_t len, cap;
};

static nd_property *pvec_push(struct pvec *v) {
  if (v->len == v->cap) {
    v->cap = v->cap ? v->cap * 2 : 8;
    v->p = realloc(v->p, v->cap * sizeof *v->p);
  }
  memset(&v->p[v->len], 0, sizeof v->p[v->len]);
  return &v->p[v->len++];
}

static bool is_bool(const char *s, bool *val) {
  static const char *yes[] = {"true", "yes", "on"};
  static const char *no[]  = {"false", "no", "off"};
  for (unsigned i = 0; i < 3; i++) {
    if (strcasecmp(s, yes[i]) == 0) { *val = true;  return true; }
    if (strcasecmp(s, no[i])  == 0) { *val = false; return true; }
  }
  return false;
}

static bool all_digits(const char *s, int n) {
  for (int i = 0; i < n; i++)
    if (!isdigit((unsigned char)s[i])) return false;
  return true;
}

/* YYYY-MM-DD, optionally followed by a time. */
static nd_prop_type date_shape(const char *s, size_t n) {
  if (n < 10) return ND_PROP_TEXT;
  if (!all_digits(s, 4) || s[4] != '-' || !all_digits(s + 5, 2) ||
      s[7] != '-' || !all_digits(s + 8, 2))
    return ND_PROP_TEXT;
  if (n == 10) return ND_PROP_DATE;
  if ((s[10] == 'T' || s[10] == ' ') && n >= 16) return ND_PROP_DATETIME;
  return ND_PROP_TEXT;
}

/* Type inference runs ONLY on unquoted scalars. A quoted value is always text,
 * which is what lets a user write `version: "1.0"` and keep the string. */
static void classify(struct nd_arena *a, nd_property *prop, const char *val,
                     size_t len, bool plain) {
  prop->text = nd_arena_strndup(a, val, len);

  if (!plain) { prop->type = ND_PROP_TEXT; return; }

  if (len == 0 || strcmp(prop->text, "null") == 0 ||
      strcmp(prop->text, "~") == 0) {
    prop->type = ND_PROP_EMPTY;
    return;
  }

  bool b;
  if (is_bool(prop->text, &b)) {
    prop->type = ND_PROP_CHECKBOX;
    prop->checkbox = b;
    return;
  }

  char *end = NULL;
  double d = strtod(prop->text, &end);
  if (end && *end == '\0' && end != prop->text) {
    prop->type = ND_PROP_NUMBER;
    prop->number = d;
    return;
  }

  prop->type = date_shape(prop->text, len);
}

void nd_frontmatter_parse(struct nd_arena *a, const char *yaml, size_t len,
                          nd_props *out) {
  memset(out, 0, sizeof *out);
  if (!yaml || len == 0) return;
  out->present = true;

  yaml_parser_t parser;
  if (!yaml_parser_initialize(&parser)) return;
  yaml_parser_set_input_string(&parser, (const unsigned char *)yaml, len);

  enum { WANT_KEY, WANT_VALUE, IN_SEQUENCE } state = WANT_KEY;
  int depth = 0;

  struct pvec props = {0};
  nd_property *cur = NULL;

  struct { char **p; size_t len, cap; } items = {0};

  for (;;) {
    yaml_event_t ev;
    if (!yaml_parser_parse(&parser, &ev)) {
      /* Keep whatever parsed cleanly and report the rest; the fence was found
       * lexically, so the body is unaffected by bad YAML. */
      out->error = nd_arena_strndup(a, parser.problem ? parser.problem : "invalid YAML",
                                    strlen(parser.problem ? parser.problem : "invalid YAML"));
      out->error_line = (int)parser.problem_mark.line + 1;
      break;
    }

    if (ev.type == YAML_STREAM_END_EVENT) { yaml_event_delete(&ev); break; }

    switch (ev.type) {
      case YAML_MAPPING_START_EVENT:
        depth++;
        break;

      case YAML_MAPPING_END_EVENT:
        depth--;
        break;

      case YAML_SEQUENCE_START_EVENT:
        if (state == WANT_VALUE && cur) {
          state = IN_SEQUENCE;
          items.len = 0;
        }
        break;

      case YAML_SEQUENCE_END_EVENT:
        if (state == IN_SEQUENCE && cur) {
          cur->type = ND_PROP_LIST;
          cur->nitems = items.len;
          cur->items = items.len
              ? nd_arena_memdup(a, items.p, items.len * sizeof(char *))
              : NULL;
          state = WANT_KEY;
          cur = NULL;
        }
        break;

      case YAML_SCALAR_EVENT: {
        const char *val = (const char *)ev.data.scalar.value;
        size_t vlen = ev.data.scalar.length;
        bool plain = ev.data.scalar.plain_implicit != 0;

        if (state == WANT_KEY) {
          if (depth > 1) break; /* nested mapping: not a top-level property */
          cur = pvec_push(&props);
          cur->key = nd_arena_strndup(a, val, vlen);
          state = WANT_VALUE;
        } else if (state == WANT_VALUE && cur) {
          classify(a, cur, val, vlen, plain);
          state = WANT_KEY;
          cur = NULL;
        } else if (state == IN_SEQUENCE) {
          if (items.len == items.cap) {
            items.cap = items.cap ? items.cap * 2 : 8;
            items.p = realloc(items.p, items.cap * sizeof *items.p);
          }
          items.p[items.len++] = nd_arena_strndup(a, val, vlen);
        }
        break;
      }

      default:
        break;
    }

    yaml_event_delete(&ev);
  }

  yaml_parser_delete(&parser);

  /* A key that never received a value (nested structure we do not model). */
  for (size_t i = 0; i < props.len; i++) {
    if (props.p[i].key && props.p[i].type == ND_PROP_TEXT && !props.p[i].text) {
      props.p[i].type = ND_PROP_EMPTY;
      props.p[i].unsupported = true;
    }
  }

  out->count = props.len;
  out->items = props.len
      ? nd_arena_memdup(a, props.p, props.len * sizeof *props.p)
      : NULL;

  free(props.p);
  free(items.p);
}
