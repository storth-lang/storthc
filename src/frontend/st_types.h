#ifndef ST_TYPES_H
#define ST_TYPES_H

#include "st_ast.h"
#include "../utils/st_types.h"
#include "../utils/st_ht.h"

typedef enum
{
    ST_TY_VOID,
    ST_TY_BOOL,
    ST_TY_CHAR,
    ST_TY_STRING,
    ST_TY_ANY,
    ST_TY_INT,
    ST_TY_FLOAT,
    ST_TY_UNTYPED_INT,
    ST_TY_UNTYPED_FLOAT,
    ST_TY_PTR,
    ST_TY_ARRAY,
    ST_TY_DYN_ARRAY,
    ST_TY_STRUCT,
    ST_TY_ENUM,
    ST_TY_TAG_UNION,
    ST_TY_FN,
    ST_TY_COUNT,
} ST_ty_kind_t;

typedef enum
{
    ST_TY_STATE_NONE,
    ST_TY_STATE_COMPUTING,
    ST_TY_STATE_DONE,
} ST_ty_state_t;

typedef struct
{
    ST_string_t name;
    ST_ty_t *ty;
    u32 offset;
} ST_ty_field_t;

typedef struct { ST_ty_field_t *items; u32 count, capacity; } ST_ty_fields_t;
typedef struct { ST_ty_t **items; u32 count, capacity; } ST_tys_t;

struct ST_ty_t
{
    ST_ty_kind_t kind;
    u32 align, size;
    u32 width;
    b8 is_signed;
    ST_ty_t *inner;
    u64 count;
    ST_decl_t *decl;
    ST_ty_fields_t fields;
    ST_tys_t params, rets;
    b8 is_variadic;
    ST_ty_state_t state;
};

typedef struct
{
    ST_arena_t *arena;
    ST_ty_t *prim[ST_TY_COUNT];
    ST_ty_t *untyped_int;
    ST_ty_t *untyped_float;
    ST_ty_t *null_ptr;
    ST_ht_t interned, decl_type;
} ST_ty_ctx_t;

void ST_ty_ctx_init(ST_ty_ctx_t *ctx, ST_arena_t *arena);

ST_ty_t *ST_ty_prim(ST_ty_ctx_t *ctx, ST_type_t t);
ST_ty_t *ST_ty_prim_named(ST_ty_ctx_t *ctx, ST_type_t t, ST_string_t name);
ST_ty_t *ST_ty_ptr(ST_ty_ctx_t *ctx, ST_ty_t *inner);
ST_ty_t *ST_ty_array(ST_ty_ctx_t *ctx, ST_ty_t *inner, u64 count);
ST_ty_t *ST_ty_dyn_array(ST_ty_ctx_t *ctx, ST_ty_t *inner);
ST_ty_t *ST_ty_fn_new(ST_ty_ctx_t *ctx);
ST_ty_t *ST_ty_for_decls(ST_ty_ctx_t *ctx, ST_decl_t *d);

b8 ST_ty_equal(ST_ty_t *a, ST_ty_t *b);
b8 ST_ty_is_int(ST_ty_t *a);
b8 ST_ty_is_float(ST_ty_t *a);
b8 ST_ty_is_numeric(ST_ty_t *a);
b8 ST_ty_is_untyped(ST_ty_t *a);

const char *ST_ty_cstr(ST_arena_t *a, ST_ty_t *t);

#endif

