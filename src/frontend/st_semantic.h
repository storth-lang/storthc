#ifndef ST_SEMANTIC_H
#define ST_SEMANTIC_H

#include "./st_ast.h"
#include "./st_types.h"
#include "../utils/st_ht.h"
#include "../utils/st_diagnostic.h"

#define ST_SEMA_MAX_ERRORS 20

typedef enum
{
    ST_SYM_VAR,
    ST_SYM_FN,
    ST_SYM_TYPE,
    ST_SYM_CONST,
    ST_SYM_EXTERN_VAR,
} ST_sym_kind_t;

typedef struct
{
    ST_sym_kind_t kind;
    ST_string_t name;
    ST_decl_t *decl;
    ST_ty_t *t;
    u32 line, col;
} ST_sym_t;

typedef struct ST_scope_t ST_scope_t;

struct ST_scope_t
{
    ST_ht_t table;
    ST_scope_t *parent;
};

typedef struct
{
    ST_arena_t *arena;
    ST_diag_t diag;
    ST_ht_t globals;
    ST_scope_t *scope;
    ST_ht_t *labels;
    ST_ty_ctx_t tys;
    ST_tys_t *cur_rets;
} ST_sema_t;

b8 ST_sema_run(ST_arena_t *arena, ST_program_t *prog, ST_string_t src,
                ST_string_t file);

#endif
