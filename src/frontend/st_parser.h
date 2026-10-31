#ifndef ST_PARSER_H
#define ST_PARSER_H

#include "../utils/st_arena.h"
#include "../utils/st_srcmap.h"
#include "st_ast.h"
#include "st_lexer.h"

#define ST_PARSE_MAX_ERRORS 20

typedef struct {
    ST_decl_t *fn;
    ST_string_t type_name;
    ST_strings_t generics;
    b8 explicit_generics;
    ST_token_t *head;
} ST_impl_method_t;

typedef struct {
    ST_impl_method_t *items;
    u32 count, capacity;
} ST_impl_methods_t;

typedef struct {
    ST_arena_t *arena;
    ST_token_t *tokens;
    u32 n_tokens;
    u32 pos;
    ST_string_t src;
    ST_string_t file;
    u32 n_errors;
    u32 no_struct_lit;
    ST_srcmap_t *srcs;
    u32 suppress_errors; // Issue fix: https://github.com/storth-lang/storthc/issues/40
    ST_decls_t nested_fns;
    ST_impl_methods_t impl_methods;
    b8 impl_consumed;
    ST_strings_t fn_name_stack;
    u32 next_block_id;
    u32 next_trait_impl_id;
    b8 in_comptime_scope;
    u32 expr_depth;
} ST_parser_t;

b8 ST_parse(ST_arena_t *arena, ST_tokens_t tokens, ST_string_t src, ST_string_t file,
            ST_srcmap_t *srcs, ST_program_t *out);

#endif
