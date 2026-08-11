#ifndef ST_COMPTIME_COMPILE_H
#define ST_COMPTIME_COMPILE_H

// ---------------------------------------------------------------------------
// AST -> bytecode. Compiles the subset of ST_expr_t / ST_stmt_t that's
// actually reachable inside a #comptime-evaluated expression or statement
// into an ST_ct_chunk_t, then st_semantic.c runs it through ST_ct_run.
//
// Deliberately NOT a general compiler for the whole language: it only
// handles what #if / #case conditions and #comptime blocks need --
// literals, idents bound to *comptime* locals (a separate, tiny symbol
// table from the real ST_sema_t scope chain -- see ST_ct_compiler_t below),
// binary/unary ops, '.len' / indexing on strings, if/while control flow,
// and '#comp_error(...)' calls. Anything else it can't fold (a call to a
// runtime function, a load through a runtime pointer, etc.) is reported as
// "not comptime-evaluable" at the offending expression's line/col -- it is
// not this compiler's job to guess.
// ---------------------------------------------------------------------------

#include "../st_ast.h"
#include "../st_types.h"
#include "st_comptime.h"

typedef struct {
    ST_string_t name;
    u32 slot;
} ST_ct_local_t;

typedef struct {
    ST_arena_t *arena;
    ST_ct_chunk_t *chunk;

    ST_ct_local_t locals[64];
    u32 n_locals;

    // Set when something in the tree can't be compile-time evaluated (a
    // call to a non-comptime function, a runtime variable reference, a
    // pointer deref, etc). The caller (st_semantic.c) should report this
    // as an ordinary diagnostic at (err_line, err_col) rather than run
    // the chunk -- it will be incomplete.
    b8 failed;
    u32 err_line, err_col;
    char err_msg[256];
} ST_ct_compiler_t;

void ST_ct_compiler_init(ST_ct_compiler_t *cc, ST_arena_t *arena, ST_ct_chunk_t *chunk);

// Compiles 'e' to push exactly one value, followed by ST_OP_RETURN. Used for
// '#if cond', '#comptime some_expr', and switch-case scrutinees/labels.
void ST_ct_compile_expr_return(ST_ct_compiler_t *cc, ST_expr_t *e);

// Compiles a statement list (a '#comptime { ... }' block body) in place,
// ending in ST_OP_HALT (or ST_OP_RETURN if the block ends in a 'return'-like
// expression statement -- see st_comptime_compile.c for exactly which
// statement forms are supported).
void ST_ct_compile_block(ST_ct_compiler_t *cc, ST_stmts_t *body);

#endif
