/* highlight.c — one generic lexer core driven by per-language tables.
 *
 * Writing a scanner per language is how these turn into a thousand lines of
 * near-duplicate code. Instead the core handles the shapes every C-family
 * language shares — comments, strings, numbers, identifiers, operators — and a
 * table says which delimiters and keywords a given language uses. The genuine
 * irregularities are single flag-guarded branches.
 */

#include "doc/highlight.h"

#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

#include "ui/theme.h"

const uint32_t nd_tok_colors[TK_COUNT] = {
  [TK_TEXT]        = CTP_TEXT,
  [TK_KEYWORD]     = CTP_MAUVE,
  [TK_TYPE]        = CTP_YELLOW,
  [TK_FUNCTION]    = CTP_BLUE,
  [TK_STRING]      = CTP_GREEN,
  [TK_STRING_ESC]  = CTP_PINK,
  [TK_NUMBER]      = CTP_PEACH,
  [TK_COMMENT]     = CTP_OVERLAY0,
  [TK_DOC_COMMENT] = CTP_OVERLAY1,
  [TK_OPERATOR]    = CTP_SKY,
  [TK_PUNCT]       = CTP_OVERLAY2,
  [TK_CONSTANT]    = CTP_PEACH,
  [TK_BUILTIN]     = CTP_RED,
  [TK_PROPERTY]    = CTP_LAVENDER,
  [TK_KEY]         = CTP_BLUE,
  [TK_PREPROC]     = CTP_PINK,
  [TK_ATTRIBUTE]   = CTP_YELLOW,
};

const uint8_t nd_tok_italic[TK_COUNT] = {
  [TK_COMMENT] = 1, [TK_DOC_COMMENT] = 1,
};

#define LANG_PREPROC      (1u << 0) /* c: '#' directive at line start */
#define LANG_SIGIL_DOLLAR (1u << 1) /* bash: $var */
#define LANG_TRIPLE_QUOTE (1u << 2) /* python */
#define LANG_CASE_INSENS  (1u << 3) /* sql */
#define LANG_KEY_COLON    (1u << 4) /* json/yaml: ident-or-string before ':' */
#define LANG_UPPER_IS_TYPE (1u << 5)
#define LANG_DECORATOR_AT (1u << 6)
#define LANG_HASH_COMMENT (1u << 7) /* '#' begins a comment, not a directive */

typedef struct {
  const char *const *aliases;
  const char *line_comment;
  const char *block_open, *block_close;
  const char *quotes;
  char        escape_char;
  const char *const *keywords;
  const char *const *types;
  const char *const *constants;
  uint32_t    flags;
} nd_lang;

/* NULL-terminated, and kept sorted only for readability: lookup is linear over
 * a handful of entries and never shows up in a profile. */
#define L(...) (const char *const[]){__VA_ARGS__, NULL}

static const char *const c_aliases[]   = {"c", "h", "cpp", "c++", "cc", "hpp", NULL};
static const char *const py_aliases[]  = {"python", "py", NULL};
static const char *const js_aliases[]  = {"js", "javascript", "ts", "typescript",
                                          "jsx", "tsx", "mjs", NULL};
static const char *const rs_aliases[]  = {"rust", "rs", NULL};
static const char *const go_aliases[]  = {"go", "golang", NULL};
static const char *const sh_aliases[]  = {"sh", "bash", "zsh", "shell", "console", NULL};
static const char *const json_aliases[]= {"json", "jsonc", NULL};
static const char *const yaml_aliases[]= {"yaml", "yml", NULL};
static const char *const sql_aliases[] = {"sql", NULL};
static const char *const lua_aliases[] = {"lua", NULL};

static const nd_lang langs[] = {
  { c_aliases, "//", "/*", "*/", "\"'", '\\',
    L("auto","break","case","const","continue","default","do","else","enum",
      "extern","for","goto","if","inline","register","restrict","return",
      "sizeof","static","struct","switch","typedef","union","volatile","while",
      "class","namespace","template","public","private","protected","virtual",
      "new","delete","using","try","catch","throw"),
    L("bool","char","double","float","int","long","short","signed","unsigned",
      "void","size_t","ssize_t","uint8_t","uint16_t","uint32_t","uint64_t",
      "int8_t","int16_t","int32_t","int64_t","FILE","va_list"),
    L("NULL","true","false","EOF"),
    LANG_PREPROC | LANG_UPPER_IS_TYPE },

  { py_aliases, "#", NULL, NULL, "\"'", '\\',
    L("and","as","assert","async","await","break","class","continue","def",
      "del","elif","else","except","finally","for","from","global","if",
      "import","in","is","lambda","nonlocal","not","or","pass","raise",
      "return","try","while","with","yield","match","case"),
    L("int","str","float","bool","list","dict","set","tuple","bytes","object"),
    L("None","True","False","self","cls"),
    LANG_TRIPLE_QUOTE | LANG_HASH_COMMENT | LANG_DECORATOR_AT | LANG_UPPER_IS_TYPE },

  { js_aliases, "//", "/*", "*/", "\"'`", '\\',
    L("async","await","break","case","catch","class","const","continue",
      "debugger","default","delete","do","else","export","extends","finally",
      "for","function","if","import","in","instanceof","let","new","of",
      "return","static","super","switch","this","throw","try","typeof","var",
      "void","while","yield","interface","type","enum","implements","public",
      "private","readonly","as","from","satisfies"),
    L("string","number","boolean","object","symbol","bigint","any","unknown",
      "never","Array","Promise","Map","Set","Record"),
    L("null","undefined","true","false","NaN","Infinity"),
    LANG_UPPER_IS_TYPE | LANG_DECORATOR_AT },

  { rs_aliases, "//", "/*", "*/", "\"", '\\',
    L("as","async","await","break","const","continue","crate","dyn","else",
      "enum","extern","fn","for","if","impl","in","let","loop","match","mod",
      "move","mut","pub","ref","return","self","static","struct","super",
      "trait","type","unsafe","use","where","while"),
    L("bool","char","f32","f64","i8","i16","i32","i64","i128","isize","str",
      "u8","u16","u32","u64","u128","usize","String","Vec","Option","Result",
      "Box","Rc","Arc","HashMap"),
    L("None","Some","Ok","Err","true","false"),
    LANG_UPPER_IS_TYPE },

  { go_aliases, "//", "/*", "*/", "\"`'", '\\',
    L("break","case","chan","const","continue","default","defer","else",
      "fallthrough","for","func","go","goto","if","import","interface","map",
      "package","range","return","select","struct","switch","type","var"),
    L("bool","byte","complex64","complex128","error","float32","float64",
      "int","int8","int16","int32","int64","rune","string","uint","uint8",
      "uint16","uint32","uint64","uintptr","any"),
    L("nil","true","false","iota"),
    LANG_UPPER_IS_TYPE },

  { sh_aliases, "#", NULL, NULL, "\"'", '\\',
    L("if","then","else","elif","fi","case","esac","for","while","until","do",
      "done","function","in","select","time","return","break","continue",
      "local","export","readonly","declare","source","alias","set","unset",
      "shift","trap","eval","exec"),
    NULL,
    L("true","false"),
    LANG_HASH_COMMENT | LANG_SIGIL_DOLLAR },

  { json_aliases, "//", "/*", "*/", "\"", '\\',
    NULL, NULL, L("true","false","null"), LANG_KEY_COLON },

  { yaml_aliases, "#", NULL, NULL, "\"'", '\\',
    NULL, NULL, L("true","false","null","yes","no","on","off"),
    LANG_HASH_COMMENT | LANG_KEY_COLON },

  { sql_aliases, "--", "/*", "*/", "'\"", '\\',
    L("select","from","where","insert","into","values","update","set","delete",
      "create","table","drop","alter","add","index","join","left","right",
      "inner","outer","on","group","by","order","having","limit","offset",
      "union","all","distinct","as","and","or","not","in","exists","between",
      "like","case","when","then","else","end","primary","key","foreign",
      "references","constraint","default","returning","with"),
    L("integer","int","bigint","smallint","text","varchar","char","boolean",
      "date","timestamp","numeric","decimal","real","json","jsonb","uuid"),
    L("null","true","false"),
    LANG_CASE_INSENS },

  { lua_aliases, "--", "--[[", "]]", "\"'", '\\',
    L("and","break","do","else","elseif","end","false","for","function","goto",
      "if","in","local","nil","not","or","repeat","return","then","true",
      "until","while"),
    NULL, L("nil","true","false","self","_G"), LANG_HASH_COMMENT },
};

static const nd_lang *find_lang(const char *name) {
  if (!name || !*name) return NULL;
  for (unsigned i = 0; i < sizeof langs / sizeof langs[0]; i++)
    for (const char *const *a = langs[i].aliases; *a; a++)
      if (strcasecmp(*a, name) == 0) return &langs[i];
  return NULL;
}

static bool in_list(const char *const *list, const char *s, uint32_t n,
                    bool fold) {
  if (!list) return false;
  for (const char *const *p = list; *p; p++) {
    if (strlen(*p) != n) continue;
    if (fold ? strncasecmp(*p, s, n) == 0 : strncmp(*p, s, n) == 0) return true;
  }
  return false;
}

static bool ident_start(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
static bool ident_cont(char c) {
  return ident_start(c) || (c >= '0' && c <= '9');
}
static bool digit(char c) { return c >= '0' && c <= '9'; }

struct spans {
  nd_tokspan *p;
  uint32_t len, cap;
};

static void emit(struct spans *sp, uint32_t start, uint32_t end, nd_tok kind) {
  if (start >= end || kind == TK_TEXT) return;
  if (sp->len == sp->cap) {
    sp->cap = sp->cap ? sp->cap * 2 : 128;
    sp->p = realloc(sp->p, sp->cap * sizeof *sp->p);
  }
  sp->p[sp->len].start = start;
  sp->p[sp->len].end = end;
  sp->p[sp->len].kind = (uint16_t)kind;
  sp->len++;
}

uint32_t nd_highlight(struct nd_arena *a, const char *lang, const char *src,
                      uint32_t len, nd_tokspan **out) {
  *out = NULL;
  const nd_lang *L = find_lang(lang);
  if (!L) return 0;

  struct spans sp = {0};
  bool fold = (L->flags & LANG_CASE_INSENS) != 0;
  uint32_t i = 0;
  bool line_start = true;

  while (i < len) {
    char c = src[i];

    if (c == '\n') { line_start = true; i++; continue; }
    if (c == ' ' || c == '\t') { i++; continue; }

    /* Preprocessor directives, before '#' can be read as a comment. */
    if ((L->flags & LANG_PREPROC) && line_start && c == '#') {
      uint32_t j = i;
      while (j < len && src[j] != '\n') j++;
      emit(&sp, i, j, TK_PREPROC);
      i = j;
      continue;
    }

    /* Block comment. */
    if (L->block_open && strncmp(src + i, L->block_open, strlen(L->block_open)) == 0) {
      uint32_t j = i + (uint32_t)strlen(L->block_open);
      while (j < len && strncmp(src + j, L->block_close, strlen(L->block_close)) != 0) j++;
      if (j < len) j += (uint32_t)strlen(L->block_close);
      emit(&sp, i, j, TK_COMMENT);
      i = j;
      line_start = false;
      continue;
    }

    /* Line comment. */
    if (L->line_comment && strncmp(src + i, L->line_comment, strlen(L->line_comment)) == 0) {
      uint32_t j = i;
      while (j < len && src[j] != '\n') j++;
      emit(&sp, i, j, TK_COMMENT);
      i = j;
      continue;
    }

    /* Strings, including Python's triple quotes. */
    if (L->quotes && strchr(L->quotes, c)) {
      uint32_t j = i + 1;
      bool triple = (L->flags & LANG_TRIPLE_QUOTE) && i + 2 < len &&
                    src[i + 1] == c && src[i + 2] == c;
      if (triple) {
        j = i + 3;
        while (j + 2 < len &&
               !(src[j] == c && src[j + 1] == c && src[j + 2] == c))
          j++;
        j = (j + 2 < len) ? j + 3 : len;
      } else {
        while (j < len && src[j] != c && src[j] != '\n') {
          if (L->escape_char && src[j] == L->escape_char && j + 1 < len) j += 2;
          else j++;
        }
        if (j < len && src[j] == c) j++;
      }
      emit(&sp, i, j, TK_STRING);
      i = j;
      line_start = false;
      continue;
    }

    /* Shell/perl-style sigils. */
    if ((L->flags & LANG_SIGIL_DOLLAR) && c == '$') {
      uint32_t j = i + 1;
      if (j < len && src[j] == '{') { while (j < len && src[j] != '}') j++; j++; }
      else while (j < len && ident_cont(src[j])) j++;
      emit(&sp, i, j, TK_PROPERTY);
      i = j;
      line_start = false;
      continue;
    }

    if ((L->flags & LANG_DECORATOR_AT) && c == '@' && i + 1 < len &&
        ident_start(src[i + 1])) {
      uint32_t j = i + 1;
      while (j < len && ident_cont(src[j])) j++;
      emit(&sp, i, j, TK_ATTRIBUTE);
      i = j;
      line_start = false;
      continue;
    }

    if (digit(c)) {
      uint32_t j = i;
      while (j < len && (ident_cont(src[j]) || src[j] == '.')) j++;
      emit(&sp, i, j, TK_NUMBER);
      i = j;
      line_start = false;
      continue;
    }

    if (ident_start(c)) {
      uint32_t j = i;
      while (j < len && ident_cont(src[j])) j++;
      uint32_t n = j - i;

      nd_tok kind = TK_TEXT;
      if (in_list(L->keywords, src + i, n, fold))       kind = TK_KEYWORD;
      else if (in_list(L->types, src + i, n, fold))     kind = TK_TYPE;
      else if (in_list(L->constants, src + i, n, fold)) kind = TK_CONSTANT;
      else {
        uint32_t k = j;
        while (k < len && (src[k] == ' ' || src[k] == '\t')) k++;
        if (k < len && src[k] == '(')                        kind = TK_FUNCTION;
        else if ((L->flags & LANG_KEY_COLON) && k < len && src[k] == ':')
          kind = TK_KEY;
        else if ((L->flags & LANG_UPPER_IS_TYPE) && src[i] >= 'A' && src[i] <= 'Z')
          kind = TK_TYPE;
      }
      emit(&sp, i, j, kind);
      i = j;
      line_start = false;
      continue;
    }

    if (strchr("+-*/%=<>!&|^~?", c)) {
      uint32_t j = i;
      while (j < len && strchr("+-*/%=<>!&|^~?", src[j])) j++;
      emit(&sp, i, j, TK_OPERATOR);
      i = j;
      line_start = false;
      continue;
    }

    if (strchr("()[]{},;:.", c)) {
      emit(&sp, i, i + 1, TK_PUNCT);
      i++;
      line_start = false;
      continue;
    }

    i++;
    line_start = false;
  }

  if (sp.len) *out = nd_arena_memdup(a, sp.p, sp.len * sizeof *sp.p);
  uint32_t n = sp.len;
  free(sp.p);
  return n;
}
