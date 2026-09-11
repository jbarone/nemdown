/* parse.c — md4c callbacks accumulated into the block tree.
 *
 * Three md4c rules drive the shape of this file:
 *   - callbacks are strictly nested and in document order, so a stack suffices
 *   - `detail` and text pointers are valid only for the duration of the call
 *   - strings are NOT NUL-terminated, so every copy carries a length
 */

#include "doc/parse.h"

#include "doc/mermaid.h"

#include <md4c.h>
#include <stdlib.h>
#include <string.h>

#include "doc/entity.h"
#include "util/log.h"

/* Deep enough for any document a person writes, and bounded so the recursive
 * walks over the tree (layout, paint, hit testing) cannot run the C stack out
 * on hostile input. */
#define MAX_BLOCK_DEPTH 128
#define MAX_SPAN_DEPTH  32

/* Scratch vectors grow with realloc, then get copied into the arena at their
 * final size. A naive arena plus doubling vectors wastes most of the heap. */
struct vec {
  void  *p;
  size_t len, cap, esz;
};

static void vec_init(struct vec *v, size_t esz) {
  v->p = NULL; v->len = 0; v->cap = 0; v->esz = esz;
}

static void *vec_push(struct vec *v) {
  if (v->len == v->cap) {
    v->cap = v->cap ? v->cap * 2 : 16;
    v->p = realloc(v->p, v->cap * v->esz);
    if (!v->p) nd_die("out of memory");
  }
  return (char *)v->p + (v->len++) * v->esz;
}

struct span_frame {
  uint32_t flags;
  uint32_t start;
  uint16_t depth;
  char    *href;
  bool     suppress_text; /* IMG alt text belongs to the image, not the prose */
};

/* One entry per open block. The children vector lives here rather than in a
 * file-scope array so the parser stays reentrant and self-contained. */
struct frame {
  nd_block  *node;
  struct vec kids;
};

struct builder {
  struct nd_arena *a;
  const struct nd_source *src;

  nd_block    *root;
  struct frame stack[MAX_BLOCK_DEPTH];
  int          sp;
  /* Blocks md4c opened that we declined to push because the stack was full.
   * Their closes must be declined too, or push and pop go out of balance. */
  int          overflow;

  struct span_frame spans[MAX_SPAN_DEPTH];
  int       spsp;
  /* Spans declined because the stack was full. leave_span already guards
   * spsp <= 0 so this was never memory-unsafe, but without the counter a
   * close pops the wrong frame and a link nested deeply enough simply
   * vanishes instead of being clickable. */
  int       span_overflow;

  nd_block *leaf;       /* where text() appends; NULL inside containers */
  nd_block *img_alt;    /* image currently collecting its alt text, if any */
  char      alt_buf[512];
  size_t    alt_len;
  uint32_t  first_break;
  bool      leaf_implicit; /* leaf was synthesised for bare text, see below */
  struct vec text;      /* char   — current inline text */
  struct vec runs;      /* nd_run — current inline runs */

  bool in_thead;
};

/* ---- tree plumbing -------------------------------------------------------- */

static nd_block *node_new(struct builder *b, nd_block_kind kind) {
  nd_block *n = nd_arena_calloc(b->a, sizeof *n);
  n->kind = kind;
  return n;
}

/* Children accumulate in a scratch vector per open block, then get copied into
 * the arena at their final size when the block closes. */
static void adopt(struct builder *b, nd_block *n) {
  if (b->sp <= 0) return;
  struct frame *parent = &b->stack[b->sp - 1];
  n->parent = parent->node;
  nd_block **slot = vec_push(&parent->kids);
  *slot = n;
}

static void push(struct builder *b, nd_block *n) {
  if (b->sp >= MAX_BLOCK_DEPTH) {
    /* Silently dropping the push while still honouring the matching pop would
     * walk sp down past zero, and leave_block would then read stack[-1] and
     * write through the garbage pointer it found there. Count it instead. */
    b->overflow++;
    return;
  }
  adopt(b, n);
  b->stack[b->sp].node = n;
  vec_init(&b->stack[b->sp].kids, sizeof(nd_block *));
  b->sp++;
}

static nd_block *pop(struct builder *b) {
  if (b->sp <= 0) return NULL;
  b->sp--;
  struct frame *f = &b->stack[b->sp];
  if (f->kids.len) {
    f->node->nkids = (uint32_t)f->kids.len;
    f->node->kids =
        nd_arena_memdup(b->a, f->kids.p, f->kids.len * sizeof(nd_block *));
  }
  free(f->kids.p);
  memset(&f->kids, 0, sizeof f->kids);
  return f->node;
}

/* ---- inline accumulation --------------------------------------------------- */

static void inl_begin(struct builder *b) {
  b->text.len = 0;
  b->runs.len = 0;
  b->spsp = 0;
  b->span_overflow = 0;
  b->first_break = 0;
}

static void inl_put(struct builder *b, const char *s, size_t n) {
  for (size_t i = 0; i < n; i++) {
    char *c = vec_push(&b->text);
    *c = s[i];
  }
}

/* Runs are emitted on leave_span, so inner spans are emitted BEFORE outer ones.
 * PangoAttrList resolves same-type overlaps in favour of later insertions, so
 * building in emission order would let an outer link's colour override an inner
 * code span's. Sorting by nesting depth ascending fixes it — outermost first. */
static int run_cmp(const void *pa, const void *pb) {
  const nd_run *x = pa, *y = pb;
  if (x->depth != y->depth) return x->depth < y->depth ? -1 : 1;
  if (x->start != y->start) return x->start < y->start ? -1 : 1;
  return 0;
}

static void inl_commit(struct builder *b, nd_inline *out) {
  out->first_break = b->first_break;
  out->len = (uint32_t)b->text.len;
  out->text = nd_arena_strndup(b->a, b->text.p ? b->text.p : "", b->text.len);

  if (b->runs.len) {
    qsort(b->runs.p, b->runs.len, sizeof(nd_run), run_cmp);
    out->nruns = (uint32_t)b->runs.len;
    out->runs = nd_arena_memdup(b->a, b->runs.p, b->runs.len * sizeof(nd_run));
  } else {
    out->runs = NULL;
    out->nruns = 0;
  }
  b->text.len = 0;
  b->runs.len = 0;
}

/* ---- md4c attribute flattening --------------------------------------------- */

/* Mirrors md4c's own render_attribute() loop; the termination condition is
 * theirs, not invented here. */
static char *attr_dup(struct builder *b, const MD_ATTRIBUTE *attr) {
  if (!attr || !attr->text || attr->size == 0) return NULL;

  struct vec tmp;
  vec_init(&tmp, 1);
  for (int i = 0; attr->substr_offsets[i] < attr->size; i++) {
    MD_TEXTTYPE type = attr->substr_types[i];
    MD_OFFSET off = attr->substr_offsets[i];
    MD_SIZE sz = attr->substr_offsets[i + 1] - off;
    const char *txt = attr->text + off;

    if (type == MD_TEXT_ENTITY) {
      char decoded[8];
      int n = nd_entity_decode(txt, sz, decoded);
      if (n > 0) {
        for (int k = 0; k < n; k++) { char *c = vec_push(&tmp); *c = decoded[k]; }
        continue;
      }
    }
    for (MD_SIZE k = 0; k < sz; k++) { char *c = vec_push(&tmp); *c = txt[k]; }
  }

  char *s = nd_arena_strndup(b->a, tmp.p ? tmp.p : "", tmp.len);
  free(tmp.p);
  return s;
}

/* ---- callbacks -------------------------------------------------------------- */

/* In a tight list md4c emits an item's text directly into MD_BLOCK_LI with no
 * enclosing MD_BLOCK_P, so there is no block to attach it to. Rather than
 * special-casing list items, any bare text gets an implicit paragraph. It is
 * committed as soon as a real block opens or the container closes. */
static void implicit_close(struct builder *b) {
  if (!b->leaf_implicit) return;
  inl_commit(b, &b->leaf->inl);
  b->leaf = NULL;
  b->leaf_implicit = false;
}

/* Synthesise that paragraph. This must happen on the first INLINE EVENT, not
 * on the first text: inl_begin() resets the span stack, so a span opened while
 * the leaf was still missing -- `- **bold lead-in** rest`, or an item that
 * starts with a link or a code span -- had its frame wiped out from under it
 * and vanished on leave_span, which found an empty stack and returned. The
 * markers were consumed either way, so the emphasis silently disappeared while
 * the text stayed. Every bullet in a tight list beginning with bold, and every
 * item that is a bare link, was affected. */
static void ensure_leaf(struct builder *b) {
  if (b->leaf || b->sp <= 0) return;
  nd_block *para = node_new(b, ND_PARA);
  adopt(b, para);
  b->leaf = para;
  b->leaf_implicit = true;
  inl_begin(b);
}

static int enter_block(MD_BLOCKTYPE type, void *detail, void *ud) {
  struct builder *b = ud;
  implicit_close(b);

  switch (type) {
    case MD_BLOCK_DOC:
      push(b, b->root);
      break;

    case MD_BLOCK_P: {
      nd_block *n = node_new(b, ND_PARA);
      push(b, n);
      /* Use the node we created, not the stack top: a declined push leaves a
       * different block on top and text would be attributed to it. */
      b->leaf = n;
      inl_begin(b);
      break;
    }

    case MD_BLOCK_H: {
      MD_BLOCK_H_DETAIL *d = detail;
      nd_block *n = node_new(b, ND_HEADING);
      n->level = (uint8_t)d->level;
      push(b, n);
      b->leaf = n;
      inl_begin(b);
      break;
    }

    case MD_BLOCK_CODE: {
      MD_BLOCK_CODE_DETAIL *d = detail;
      nd_block *n = node_new(b, ND_CODE);
      n->code_lang = attr_dup(b, &d->lang);
      /* A mermaid fence is still a fence — same text, same offsets — and only
       * differs in what layout does with it. If the diagram cannot be built it
       * falls back to being drawn as exactly this. */
      if (nd_mermaid_is_fence(n->code_lang)) n->kind = ND_MERMAID;
      push(b, n);
      b->leaf = n;
      inl_begin(b);
      break;
    }

    case MD_BLOCK_QUOTE:
      push(b, node_new(b, ND_QUOTE));
      break;

    case MD_BLOCK_UL: {
      MD_BLOCK_UL_DETAIL *d = detail;
      nd_block *n = node_new(b, ND_LIST);
      n->list_ordered = 0;
      n->list_tight = d->is_tight ? 1 : 0;
      push(b, n);
      break;
    }

    case MD_BLOCK_OL: {
      MD_BLOCK_OL_DETAIL *d = detail;
      nd_block *n = node_new(b, ND_LIST);
      n->list_ordered = 1;
      n->list_tight = d->is_tight ? 1 : 0;
      n->list_start = d->start;
      push(b, n);
      break;
    }

    case MD_BLOCK_LI: {
      MD_BLOCK_LI_DETAIL *d = detail;
      nd_block *n = node_new(b, ND_ITEM);
      n->item_is_task = d->is_task ? 1 : 0;
      if (d->is_task) {
        n->item_checked = (d->task_mark == 'x' || d->task_mark == 'X');
        /* md4c saw a preprocessed buffer: both the frontmatter split and any
         * embed rewrites moved bytes, so go through the offset map. */
        n->item_mark_offset =
            (uint32_t)nd_source_orig_offset(b->src, d->task_mark_offset);
      }
      push(b, n);
      break;
    }

    case MD_BLOCK_HR:
      push(b, node_new(b, ND_HR));
      break;

    case MD_BLOCK_TABLE:
      push(b, node_new(b, ND_TABLE));
      break;

    /* THEAD/TBODY carry no geometry; collapse them and tag the rows instead. */
    case MD_BLOCK_THEAD: b->in_thead = true;  break;
    case MD_BLOCK_TBODY: b->in_thead = false; break;

    case MD_BLOCK_TR: {
      nd_block *n = node_new(b, ND_ROW);
      n->row_is_header = b->in_thead;
      push(b, n);
      break;
    }

    case MD_BLOCK_TH:
    case MD_BLOCK_TD: {
      MD_BLOCK_TD_DETAIL *d = detail;
      nd_block *n = node_new(b, ND_CELL);
      n->cell_align = (uint8_t)d->align;
      push(b, n);
      b->leaf = n;
      inl_begin(b);
      break;
    }

    case MD_BLOCK_HTML:
      push(b, node_new(b, ND_PARA)); /* unreachable with MD_FLAG_NOHTML */
      break;
  }
  return 0;
}

static int leave_block(MD_BLOCKTYPE type, void *detail, void *ud) {
  (void)detail;
  struct builder *b = ud;

  if (type == MD_BLOCK_THEAD || type == MD_BLOCK_TBODY) return 0;

  implicit_close(b);

  /* This block was never pushed, so there is nothing to close. */
  if (b->overflow > 0) { b->overflow--; return 0; }
  if (b->sp <= 0) return 0;

  nd_block *n = b->stack[b->sp - 1].node;

  switch (type) {
    case MD_BLOCK_P:
    case MD_BLOCK_H:
    case MD_BLOCK_TH:
    case MD_BLOCK_TD:
      inl_commit(b, &n->inl);
      b->leaf = NULL;
      break;

    case MD_BLOCK_CODE:
      n->code_len = (uint32_t)b->text.len;
      n->code_text = nd_arena_strndup(b->a, b->text.p ? b->text.p : "", b->text.len);
      b->text.len = 0;
      b->runs.len = 0;
      b->leaf = NULL;
      break;

    default:
      break;
  }

  pop(b);
  return 0;
}

static int enter_span(MD_SPANTYPE type, void *detail, void *ud) {
  struct builder *b = ud;
  /* Before the frame is pushed, so inl_begin()'s reset cannot discard it.
   * MD_SPAN_IMG is excluded: it builds its own block and suppresses its text,
   * so forcing a leaf would leave an empty paragraph beside the image of an
   * item that is nothing but an image, and that renders as a blank gap. */
  if (type != MD_SPAN_IMG) ensure_leaf(b);
  if (b->spsp >= MAX_SPAN_DEPTH) { b->span_overflow++; return 0; }

  struct span_frame *f = &b->spans[b->spsp];
  f->flags = 0;
  f->href = NULL;
  f->start = (uint32_t)b->text.len;
  f->depth = (uint16_t)b->spsp;
  f->suppress_text = false;

  switch (type) {
    case MD_SPAN_EM:     f->flags = ND_RUN_EM;     break;
    case MD_SPAN_STRONG: f->flags = ND_RUN_STRONG; break;
    case MD_SPAN_CODE:   f->flags = ND_RUN_CODE;   break;
    case MD_SPAN_DEL:    f->flags = ND_RUN_DEL;    break;
    case MD_SPAN_U:      f->flags = ND_RUN_U;      break;

    case MD_SPAN_A: {
      MD_SPAN_A_DETAIL *d = detail;
      f->flags = ND_RUN_LINK;
      f->href = attr_dup(b, &d->href);
      break;
    }
    case MD_SPAN_WIKILINK: {
      MD_SPAN_WIKILINK_DETAIL *d = detail;
      f->flags = ND_RUN_WIKILINK;
      f->href = attr_dup(b, &d->target);
      break;
    }
    case MD_SPAN_IMG: {
      /* Images become their own block when they are a paragraph's sole
       * content; the promotion happens after parsing. */
      MD_SPAN_IMG_DETAIL *d = detail;
      nd_block *img = node_new(b, ND_IMAGE);
      img->img_src = attr_dup(b, &d->src);
      /* The preprocessor puts an Obsidian `|400` size hint in the title. */
      img->img_size = attr_dup(b, &d->title);
      adopt(b, img);
      f->flags = 0;
      /* Alt text belongs to the image as its caption; letting it flow into the
       * paragraph would read as stray words. */
      f->suppress_text = true;
      b->img_alt = img;
      b->alt_len = 0;
      break;
    }
    case MD_SPAN_LATEXMATH:
      f->flags = ND_RUN_MATH;
      break;
    case MD_SPAN_LATEXMATH_DISPLAY:
      f->flags = ND_RUN_MATH | ND_RUN_MATH_DISPLAY;
      break;
  }

  b->spsp++;
  return 0;
}

static int leave_span(MD_SPANTYPE type, void *detail, void *ud) {
  (void)type; (void)detail;
  struct builder *b = ud;
  if (b->span_overflow > 0) { b->span_overflow--; return 0; }
  if (b->spsp <= 0) return 0;

  struct span_frame *f = &b->spans[--b->spsp];
  if (f->suppress_text) {
    if (b->img_alt && b->alt_len) {
      b->alt_buf[b->alt_len] = '\0';
      b->img_alt->img_caption.text =
          nd_arena_strndup(b->a, b->alt_buf, b->alt_len);
      b->img_alt->img_caption.len = (uint32_t)b->alt_len;
    }
    b->img_alt = NULL;
    b->alt_len = 0;
  }
  if (f->flags == 0) return 0;

  nd_run *r = vec_push(&b->runs);
  r->start = f->start;
  r->end   = (uint32_t)b->text.len;
  r->flags = f->flags;
  r->depth = f->depth;
  r->href  = f->href;
  return 0;
}

static int text_cb(MD_TEXTTYPE type, const MD_CHAR *text, MD_SIZE size, void *ud) {
  struct builder *b = ud;

  for (int i = 0; i < b->spsp; i++) {
    if (b->spans[i].suppress_text) {
      if (b->img_alt && type != MD_TEXT_SOFTBR && type != MD_TEXT_BR &&
          b->alt_len + size < sizeof b->alt_buf - 1) {
        memcpy(b->alt_buf + b->alt_len, text, size);
        b->alt_len += size;
      }
      return 0;
    }
  }

  ensure_leaf(b);
  if (!b->leaf) return 0;

  switch (type) {
    case MD_TEXT_NULLCHAR:
      inl_put(b, "\xEF\xBF\xBD", 3); /* U+FFFD */
      break;

    case MD_TEXT_BR:
      /* Pango treats a newline inside one layout as a hard break, which is
       * exactly the semantics we want. */
      inl_put(b, "\n", 1);
      break;

    case MD_TEXT_SOFTBR:
      if (b->first_break == 0) b->first_break = (uint32_t)b->text.len;
      inl_put(b, " ", 1);
      break;

    case MD_TEXT_ENTITY: {
      char decoded[8];
      int n = nd_entity_decode(text, size, decoded);
      if (n > 0) inl_put(b, decoded, (size_t)n);
      else       inl_put(b, text, size); /* unknown: pass through verbatim */
      break;
    }

    default:
      inl_put(b, text, size);
      break;
  }
  return 0;
}

/* ---- driver ----------------------------------------------------------------- */

nd_block *nd_parse(struct nd_arena *a, const struct nd_source *src) {
  struct builder b;
  memset(&b, 0, sizeof b);
  b.a = a;
  b.src = src;
  b.root = nd_arena_calloc(a, sizeof *b.root);
  b.root->kind = ND_DOC_ROOT;

  vec_init(&b.text, 1);
  vec_init(&b.runs, sizeof(nd_run));

  MD_PARSER parser = {
    .abi_version = 0,
    /* UNDERLINE is deliberately absent: it steals `_` from emphasis, and
     * Obsidian keeps `_` italic. HARD_SOFT_BREAKS likewise — Obsidian's
     * default turns soft breaks into spaces. */
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

  int rc = md_parse(src->body, (MD_SIZE)src->body_len, &parser, &b);

  free(b.text.p);
  free(b.runs.p);
  /* Any frame still open means md4c bailed mid-document; release its scratch. */
  for (int i = 0; i < b.sp; i++) free(b.stack[i].kids.p);

  return rc == 0 ? b.root : NULL;
}
