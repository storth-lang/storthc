#ifndef ST_PARSER_H
#define ST_PARSER_H

#include "st_lexer.h"
#include "st_ast.h"
#include "../utils/st_arena.h"

#define ST_PARSE_MAX_ERRORS 20

typedef struct
{
    ST_arena_t *arena;
    ST_token_t *tokens;
    u32 n_tokens;
    u32 pos;
    ST_string_t src;
    ST_string_t file;
    u32 n_errors;
    u32 no_struct_lit;
} ST_parser_t;

b8 ST_parse(ST_arena_t *arena, ST_tokens_t tokens, ST_string_t src,
             ST_string_t file, ST_program_t *out);

#endif
