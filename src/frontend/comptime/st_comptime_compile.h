#ifndef ST_COMPTIME_COMPILE_H
#define ST_COMPTIME_COMPILE_H

#include "../st_ast.h"
#include "../st_semantic.h"
#include "../st_types.h"
#include "st_comptime.h"

typedef struct {
    ST_string_t name;
    u32 slot;
    ST_string_t struct_type;
    b8 is_buffer;
    b8 has_elem_info; // true when this buffer-backed local is a fixed-size array of a
                     // recognized primitive int type, so 'name[i]'/'&name[i]' can be
                     // compiled via ST_OP_PTR_ADD/ST_OP_PTR_LOAD
    u32 elem_size;
    b8 elem_is_signed;
    b8 has_known_len; // true for a fixed-size array local ('name: [N]T;'): N is a
                     // compile-time constant, known from the declaration itself
    u32 known_len;
    i32 len_slot;    // >=0: this local is a slice-shaped parameter whose length lives
                     // in ANOTHER local at this VM slot (a hidden companion declared
                     // right after it
} ST_ct_local_t;

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
    ST_sema_t *sema;
    ST_string_t src, file;

    ST_ct_fn_entry_t fns[ST_CT_MAX_PROG_FNS];
    u32 n_fns;

    ST_decl_t *worklist[ST_CT_MAX_PROG_FNS];
    u32 n_worklist;

    ST_ct_pending_call_t pending[ST_CT_MAX_PENDING_CALLS];
    u32 n_pending;

    b8 native_so_built;
    b8 native_so_ok;
    char native_so_path[512];

    b8 failed;
    u32 err_line, err_col;
    ST_string_t err_file; // which file (line, col) is actually in
    char err_msg[256];
} ST_ct_prog_ctx_t;

typedef struct {
    u32 body_start_locals; // cc->n_locals when the loop body starts;
    u32 body_start_defer_scopes; // cc->n_defer_scopes when the loop body starts; 'break'
                                // and 'continue' flush every defer scope at or above this
                                // depth (see ST_ct_emit_defers_from) before jumping
    u32 continue_patches[32];   // 'continue' jumps (forward) here, patched once known
    u32 n_continue_patches;
    u32 break_patches[32];      // offsets of forward JMPs waiting for the loop's exit point
    u32 n_break_patches;
} ST_ct_loop_ctx_t;

typedef struct {
    ST_stmt_t *items[16];
    u32 count;
} ST_ct_defer_scope_t;

typedef struct {
    ST_arena_t *arena;
    ST_ct_chunk_t *chunk;

    ST_ct_local_t locals[64];
    u32 n_locals;

    ST_ct_loop_ctx_t loop_stack[16];
    u32 n_loops;

    ST_ct_defer_scope_t defer_scopes[32];
    u32 n_defer_scopes;

    ST_ct_prog_ctx_t *prog_ctx;

    ST_string_t cur_file;
    b8 failed;
    u32 err_line, err_col;
    ST_string_t err_file;
    char err_msg[256];
} ST_ct_compiler_t;

void ST_ct_compiler_init(ST_ct_compiler_t *cc, ST_arena_t *arena, ST_ct_chunk_t *chunk);
void ST_ct_compile_expr_return(ST_ct_compiler_t *cc, ST_expr_t *e);
void ST_ct_compile_block(ST_ct_compiler_t *cc, ST_stmts_t *body);
b8 ST_ct_compile_program(ST_arena_t *arena, ST_ct_chunk_t *chunk, ST_program_t *prog,
                         ST_sema_t *sema, ST_string_t src, ST_string_t file,
                         ST_decl_t *entry_decl, u32 *out_entry_ip, u32 *err_line, u32 *err_col,
                         ST_string_t *err_file, char *err_msg, u32 err_msg_cap);

b8 ST_ct_decl_needs_native(ST_decl_t *d);

#endif
