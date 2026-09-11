/* highlight.h — fenced code syntax highlighting.
 *
 * Only six tree-sitter grammars are packaged on this system (c, lua, markdown,
 * query, vim, vimdoc) and none ship highlight queries, so tree-sitter would
 * cover almost nothing people actually put in fenced blocks. v1 is a
 * hand-written lexer.
 *
 * The TOKEN KIND ENUM is the interface, not this lexer. A tree-sitter backend
 * later maps capture names onto the same kinds and touches nothing else.
 */

#ifndef NEMDOWN_HIGHLIGHT_H
#define NEMDOWN_HIGHLIGHT_H

#include <stdint.h>

#include "doc/arena.h"

typedef enum {
  TK_TEXT = 0, TK_KEYWORD, TK_TYPE, TK_FUNCTION, TK_STRING, TK_STRING_ESC,
  TK_NUMBER, TK_COMMENT, TK_DOC_COMMENT, TK_OPERATOR, TK_PUNCT,
  TK_CONSTANT, TK_BUILTIN, TK_PROPERTY, TK_KEY, TK_PREPROC, TK_ATTRIBUTE,
  TK_COUNT
} nd_tok;

typedef struct {
  uint32_t start, end;
  uint16_t kind;
} nd_tokspan;

extern const uint32_t nd_tok_colors[TK_COUNT];
extern const uint8_t  nd_tok_italic[TK_COUNT];

/* Returns 0 and leaves *out NULL when the language is unknown. */
uint32_t nd_highlight(struct nd_arena *a, const char *lang, const char *src,
                      uint32_t len, nd_tokspan **out);

#endif /* NEMDOWN_HIGHLIGHT_H */
