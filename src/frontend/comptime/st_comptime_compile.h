#ifndef ST_COMPTIME_COMPILE_H
#define ST_COMPTIME_COMPILE_H

#include "../st_ast.h"
#include "../st_types.h"
#include "st_comptime.h"

typedef struct {
    ST_string_t name;
    u32 slot;
    ST_string_t struct_type; // name of the struct decl this local holds, if any (len==0
                             // otherwise); set from a struct-literal initializer, used to
                             // resolve '.field' access without needing ST_ty_t visibility
} ST_ct_local_t;

// Shared across the compilation of every function reachable from a
// #comptime entry point (main, or another #comptime function). Functions
// are discovered and compiled on demand as calls to them are encountered;
// forward references (a function calling one not compiled yet) are handled
// by recording a pending patch and filling in the real entry_ip once that
// function is eventually compiled -- same idea as ST_ct_patch_jump, just
// across function boundaries instead of within one.
typedef struct {
    ST_string_t name;
    u32 entry_ip;
    b8 queued;
    b8 compiled;
    ST_decl_t *decl;
} ST_ct_fn_entry_t;

typedef struct {
    u32 operand_offset;
    ST_string_t callee_name;
    u32 line, col;
} ST_ct_pending_call_t;

#define ST_CT_MAX_PROG_FNS 128
#define ST_CT_MAX_PENDING_CALLS 512

typedef struct {
    ST_arena_t *arena;
    ST_ct_chunk_t *chunk;
    ST_program_t *prog;

    ST_ct_fn_entry_t fns[ST_CT_MAX_PROG_FNS];
    u32 n_fns;

    ST_decl_t *worklist[ST_CT_MAX_PROG_FNS];
    u32 n_worklist;

    ST_ct_pending_call_t pending[ST_CT_MAX_PENDING_CALLS];
    u32 n_pending;

    b8 failed;
    u32 err_line, err_col;
    char err_msg[256];
} ST_ct_prog_ctx_t;

typedef struct {
    u32 body_start_locals; // cc->n_locals when the loop body starts; break/continue pop
                                // back down to this before jumping, so locals declared in a
                                // nested scope inside the loop body don't leave the VM stack
                                // unbalanced
    u32 continue_patches[32];   // 'continue' jumps (forward) here, patched once known
    u32 n_continue_patches;
    u32 break_patches[32];      // offsets of forward JMPs waiting for the loop's exit point
    u32 n_break_patches;
} ST_ct_loop_ctx_t;

typedef struct {
    ST_arena_t *arena;
    ST_ct_chunk_t *chunk;

    ST_ct_local_t locals[64];
    u32 n_locals;

    ST_ct_loop_ctx_t loop_stack[16]; // one entry per enclosing loop, innermost last
    u32 n_loops;

    // Non-NULL when this function is being compiled as part of a full
    // #comptime program (see ST_ct_compile_program below), which is what
    // makes calling other plain Storth functions possible. NULL for the
    // narrower standalone uses ('#if cond', '#comp_error', a bare
    // '#comptime { }' block with no calls) which still reject ST_EX_CALL.
    ST_ct_prog_ctx_t *prog_ctx;

    // Set when something in the tree can't be compile-time evaluated (a
    // call to something that isn't a plain comptime-eligible function, a
    // runtime variable reference, a pointer deref, etc). The caller
    // (st_semantic.c) should report this as an ordinary diagnostic at
    // (err_line, err_col) rather than run the chunk -- it will be
    // incomplete.
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

// Compiles 'entry_decl' (a '#comptime'-marked function, e.g. main) and
// every plain Storth function it transitively calls, into one shared
// chunk, resolving the whole call graph and backpatching forward
// references. On success, *out_entry_ip is where entry_decl's own bytecode
// begins (always 0, since it's compiled first) and the returned chunk can
// be run directly with ST_ct_run. On failure, returns 0 and fills
// *err_line/*err_col/err_msg (err_msg_cap bytes, always nul-terminated).
b8 ST_ct_compile_program(ST_arena_t *arena, ST_ct_chunk_t *chunk, ST_program_t *prog,
                         ST_decl_t *entry_decl, u32 *out_entry_ip, u32 *err_line, u32 *err_col,
                         char *err_msg, u32 err_msg_cap);

#endif
