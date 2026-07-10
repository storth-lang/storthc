#ifndef LEXER_H
#define LEXER_H

#include "../utils/st_string.h"
#include "../utils/st_helper.h"
#include "../utils/st_types.h"
#include "../utils/st_arena.h"

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
    b8 ok;
} ST_tokens_t;

typedef struct
{
    ST_arena_t *arena;
    ST_tokens_t tokens;
    ST_string_t src;
    ST_string_t file;
    u32 pos, line, col;
    b8 failed;
} ST_lexer_t;

ST_string_t ST_token_kind_to_string(ST_token_kind_t kind);
void ST_token_error(ST_string_t src);

ST_token_t ST_lexer_advance(ST_lexer_t *l);
ST_token_t ST_lexer_peek(ST_lexer_t *l);

b8 ST_iswhitespace(char c);
b8 ST_is_primitive(ST_string_t s);
b8 ST_is_keyword(ST_string_t s);
b8 ST_is_symbol(ST_string_t s);

ST_tokens_t ST_lex(ST_arena_t *arena, ST_string_t src, ST_string_t file);
ST_string_t ST_lexer_error(ST_string_t src, u32 col, u32 pos);
void ST_colorize_source_line(FILE *out, ST_string_t text, const char *code_col, const char *com_col);
void ST_dump_token(ST_tokens_t tokens);

#endif
