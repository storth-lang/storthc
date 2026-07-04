#ifndef LEXER_H
#define LEXER_H

#include "st_string.h"

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
    ST_string_t file, text;
    u32 line, col;
} ST_token_t;

typedef struct
{
    ST_string_t src;
    u32 pos;
} ST_lexer_t;

ST_string_t ST_token_kind_to_string(ST_token_kind_t kind);
b32 ST_iswhitespace(ST_lexer_t *l);

#endif
