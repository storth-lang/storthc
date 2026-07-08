#ifndef LEXER_H
#define LEXER_H

#include "st_string.h"
#include "st_types.h"
#include "st_arena.h"

typedef enum
{
    ST_TINT,
    ST_TFLOAT,
    ST_TSTRING,
    ST_TCHAR,
    ST_TIDENT,
    ST_TTYPE,
    ST_TKEYWORD,
    ST_TSYMBOL,
    ST_TDOCCOMENT,
    ST_TCOUNT,
} ST_token_kind_t;

typedef struct
{
    ST_token_kind_t kind;
    ST_string_t file, text, str;
    union {
        i64 i;
        f64 f;
        char c;
    } val;
    u32 line, col;
} ST_token_t;

typedef struct
{

    ST_token_t *items;
    u32 count, capacity;
    b32 ok;
} ST_tokens_t;

typedef struct
{
    ST_arena_t *arena;
    ST_tokens_t tokens;
    ST_string_t src;
    ST_string_t file;
    u32 pos, line, col;
    b32 failed;
} ST_lexer_t;

#define ST_KEYWORD_LIST                    \
    ST_KEYWORD(ST_kfn, "fn")               \
    ST_KEYWORD(ST_kstruct, "struct")       \
    ST_KEYWORD(ST_kenum, "enum")           \
    ST_KEYWORD(ST_kextern, "extern")       \
    ST_KEYWORD(ST_kpub, "pub")             \
    ST_KEYWORD(ST_knull, "null")           \
    ST_KEYWORD(ST_kif, "if")               \
    ST_KEYWORD(ST_kelse, "else")           \
    ST_KEYWORD(ST_kwhile, "while")         \
    ST_KEYWORD(ST_kfor, "for")             \
    ST_KEYWORD(ST_kcast, "cast")           \
    ST_KEYWORD(ST_ktype_of, "type_of")     \
    ST_KEYWORD(ST_ktype_info, "type_info") \
    ST_KEYWORD(ST_ksizeof, "sizeof")       \
    ST_KEYWORD(ST_kand, "and")             \
    ST_KEYWORD(ST_kor, "or")               \
    ST_KEYWORD(ST_knot, "not")             \
    ST_KEYWORD(ST_kdefault, "default")     \
    ST_KEYWORD(ST_kenum_flag, "enum_flag") \
    ST_KEYWORD(ST_ktag, "tag_union")       \
    ST_KEYWORD(ST_kcase, "case")           \
    ST_KEYWORD(ST_kreturn, "return")       \
    ST_KEYWORD(ST_kusing, "using")         \

typedef enum {
#define ST_KEYWORD(e, s) e,
    ST_KEYWORD_LIST
#undef ST_KEYWORD
    ST_KEYWORD_COUNT,
} ST_keyword_t;

extern const char *ST_keyword_names[ST_KEYWORD_COUNT];

#define ST_TYPE_LIST          \
    ST_TYPE(ST_ti8, "i8")     \
    ST_TYPE(ST_ti16, "i16")   \
    ST_TYPE(ST_ti32, "i32")   \
    ST_TYPE(ST_ti64, "i64")   \
    ST_TYPE(ST_tu8, "u8")     \
    ST_TYPE(ST_tu16, "u16")   \
    ST_TYPE(ST_tu32, "u32")   \
    ST_TYPE(ST_tu64, "u64")   \
    ST_TYPE(ST_tf32, "f32")   \
    ST_TYPE(ST_tf64, "f64")   \
    ST_TYPE(ST_tf128, "f128") \
    ST_TYPE(ST_tchar, "char") \
    ST_TYPE(ST_tvoid, "void") \
    ST_TYPE(ST_tany, "any")   \
    ST_TYPE(ST_tbool, "bool") \
    ST_TYPE(ST_tstring, "string") \

typedef enum {
#define ST_TYPE(e, s) e,
    ST_TYPE_LIST
#undef ST_TYPE
    ST_TYPE_COUNT,
} ST_type_t;

extern const char *ST_type_names[ST_TYPE_COUNT];

ST_string_t ST_token_kind_to_string(ST_token_kind_t kind);
void ST_token_error(ST_string_t src);

ST_token_t ST_lexer_advance(ST_lexer_t *l);
ST_token_t ST_lexer_peek(ST_lexer_t *l);

b32 ST_iswhitespace(char c);
b32 ST_is_primitive(ST_string_t s);
b32 ST_is_keyword(ST_string_t s);
b32 ST_is_symbol(ST_string_t s);

ST_tokens_t ST_lex(ST_arena_t *arena, ST_string_t src, ST_string_t file);
ST_string_t ST_lexer_error(ST_string_t src, u32 col, u32 pos);
void ST_colorize_source_line(FILE *out, ST_string_t text, const char *code_col, const char *com_col);
void ST_dump_token(ST_tokens_t tokens);

#endif
