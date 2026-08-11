#ifndef ST_SEMANTIC_H
#define ST_SEMANTIC_H

#include "../utils/st_diagnostic.h"
#include "../utils/st_ht.h"
#include "./st_ast.h"
#include "./st_types.h"

#define ST_SEMA_MAX_ERRORS 20

typedef enum {
    ST_SYM_VAR,
    ST_SYM_FN,
    ST_SYM_TYPE,
    ST_SYM_CONST,
    ST_SYM_EXTERN_VAR,
    ST_SYM_GLOBAL,
    ST_SYM_MODULE,
} ST_sym_kind_t;

typedef struct {
    ST_sym_kind_t kind;
    ST_string_t name;
    ST_decl_t *decl;
    ST_ty_t *t;
    u32 line, col;
    ST_ht_t *generic_bindings;
    ST_string_t template_name;
    b8 is_const; // for ST_SYM_VAR: declared with '::' -- cannot be reassigned
} ST_sym_t;

typedef struct ST_scope_t ST_scope_t;

struct ST_scope_t {
    ST_ht_t table;
    ST_scope_t *parent;
};

typedef struct {
    ST_arena_t *arena;
    ST_diag_t diag;
    ST_ht_t globals;
    ST_scope_t *scope;
    ST_ht_t *labels;
    ST_ty_ctx_t tys;
    ST_tys_t *cur_rets;
    ST_program_t *prog;
    ST_ht_t templates;
    ST_ht_t instantiations;
    ST_ht_t fn_instantiations;
    ST_ht_t inst_info;
    ST_ht_t *generic_bindings;

    b8 stamp_tyexprs;
    u32 n_fn_instances;

    b8 has_pack;
    ST_string_t cur_pack_name;
    u32 cur_pack_count;

    b8 has_bound_str;
    ST_string_t cur_bound_str_param;
    ST_string_t cur_bound_str_value;
} ST_sema_t;

b8 ST_sema_run(ST_arena_t *arena, ST_program_t *prog, ST_string_t src, ST_string_t file,
               ST_sema_t *out);

// @note: ST_const_eval evaluates 'e' as a compile-time integer constant
// (int/char/bool literals, named '::' constants, enum/enum_flag variants,
// unary/binary integer ops, and sizeof/alignof). Returns 0 if 'e' cannot be
// folded to a constant at compile time.
b8 ST_const_eval(ST_sema_t *se, ST_expr_t *e, i64 *out);

#endif
