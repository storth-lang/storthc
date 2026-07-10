#ifndef ST_AST_H
#define ST_AST_H

#include "../utils/st_helper.h"
#include "../utils/st_string.h"
#include "../utils/st_arena.h"

typedef struct ST_ty_t ST_ty_t;

typedef struct ST_tyexpr_t ST_tyexpr_t;
typedef struct ST_expr_t   ST_expr_t;
typedef struct ST_stmt_t   ST_stmt_t;
typedef struct ST_decl_t   ST_decl_t;

typedef struct { ST_expr_t   **items; u32 count, capacity; } ST_exprs_t;
typedef struct { ST_tyexpr_t **items; u32 count, capacity; } ST_tyexprs_t;
typedef struct { ST_stmt_t   **items; u32 count, capacity; } ST_stmts_t;
typedef struct { ST_decl_t   **items; u32 count, capacity; } ST_decls_t;

typedef enum
{
    ST_TE_NAME,
    ST_TE_PTR,
    ST_TE_ARRAY,
} ST_tyexpr_kind_t;

struct ST_tyexpr_t
{
    ST_tyexpr_kind_t kind;
    u32 line, col;
    ST_string_t name;
    ST_tyexpr_t *inner;
    ST_expr_t *count_expr;
};

typedef enum
{
    ST_EX_INT,
    ST_EX_FLOAT,
    ST_EX_STR,
    ST_EX_CHAR,
    ST_EX_BOOL,
    ST_EX_NULL,
    ST_EX_IDENT,
    ST_EX_UNARY,
    ST_EX_BINARY,
    ST_EX_CALL,
    ST_EX_FIELD,
    ST_EX_INDEX,
    ST_EX_CAST,
    ST_EX_STRUCT_LIT,
    ST_EX_ARRAY_NEW,
    ST_EX_SIZEOF,
    ST_EX_TYPEOF,
    ST_EX_TYPEINFO,
    ST_EX_KIND,
    ST_EX_CSTR,
    ST_EX_COUNT,
} ST_expr_kind_t;

typedef struct
{
    ST_string_t name;
    ST_expr_t *value;
    u32 line, col;
} ST_field_init_t;

typedef struct { ST_field_init_t *items; u32 count, capacity; } ST_field_inits_t;

typedef struct
{
    ST_string_t name;
    ST_expr_t *value;
} ST_arg_t;

typedef struct { ST_arg_t *items; u32 count, capacity; } ST_args_t;

struct ST_expr_t
{
    ST_expr_kind_t kind;
    ST_ty_t *ty;
    u32 line, col;
    union {
        i64 ival;
        f64 fval;
        ST_string_t sval;
        ST_string_t name;
        struct {
            ST_string_t op;
            ST_expr_t *operand;
        } unary;
        struct {
            ST_string_t op;
            ST_expr_t *l, *r;
        } bin;
        struct {
            ST_expr_t *callee;
            ST_args_t args;
        } call;
        struct {
            ST_expr_t *base;
            ST_string_t name;
        } field;
        struct {
            ST_expr_t *base;
            ST_expr_t *index;
        } index;
        struct {
            ST_expr_t *operand;
            ST_tyexpr_t *to;
        } cast;
        struct {
            ST_string_t type_name;
            ST_field_inits_t inits;
        } struct_lit;
        struct {
            ST_tyexpr_t *te;
        } array_new;
        struct {
            ST_tyexpr_t *te;
            ST_expr_t *operand;
            b8 is_align;
        } tyop;
    };
};

typedef enum
{
    ST_ST_EXPR,
    ST_ST_DECL,
    ST_ST_ASSIGN,
    ST_ST_MULTI_BIND,
    ST_ST_IF,
    ST_ST_SWITCH,
    ST_ST_WHILE,
    ST_ST_FOR_RANGE,
    ST_ST_FOR_ARRAY,
    ST_ST_RETURN,
    ST_ST_BLOCK,
    ST_ST_DEFER,
    ST_ST_BREAK,
    ST_ST_CONTINUE,
    ST_ST_LABEL,
    ST_ST_GODOWN,
    ST_ST_COUNT,
} ST_stmt_kind_t;

typedef struct
{
    ST_exprs_t values;
    ST_stmts_t body;
    u32 line, col;
} ST_case_t;

typedef struct { ST_case_t *items; u32 count, capacity; } ST_cases_t;

struct ST_stmt_t
{
    ST_stmt_kind_t kind;
    u32 line, col;
    union {
        ST_expr_t *expr;
        struct {
            ST_string_t name;
            ST_tyexpr_t *te;
            ST_expr_t *init;
            b8 is_static;
        } decl;
        struct {
            ST_expr_t *lhs;
            ST_string_t op;
            ST_expr_t *rhs;
        } assign;
        struct {
            ST_string_t *names;
            u32 n_names;
            ST_exprs_t values;
            b8 declare;
        } multi;
        struct {
            ST_expr_t *cond;
            ST_stmts_t then_body;
            ST_stmt_t *else_stmt;
        } if_;
        struct {
            ST_expr_t *cond;
            ST_cases_t cases;
        } switch_;
        struct {
            ST_expr_t *cond;
            ST_stmts_t body;
        } while_;
        struct {
            ST_string_t iter;
            ST_expr_t *lo, *hi;
            b8 inclusive;
            ST_stmts_t body;
        } for_range;
        struct {
            ST_string_t iter;
            ST_expr_t *target;
            ST_stmts_t body;
        } for_array;
        struct {
            ST_exprs_t values;
        } ret;
        ST_stmts_t block;
        ST_stmt_t *defer_stmt;
        ST_string_t label;
    };
};

typedef enum
{
    ST_DE_STRUCT,
    ST_DE_ENUM,
    ST_DE_TAG_UNION,
    ST_DE_CONST,
    ST_DE_EXTERN_FN,
    ST_DE_EXTERN_VAR,
    ST_DE_FN,
    ST_DE_COUNT,
} ST_decl_kind_t;

typedef enum
{
    ST_PACK_DEFAULT,
    ST_PACK_C,
    ST_PACK_PACKED,
} ST_packing_t;

typedef struct
{
    ST_string_t name;
    ST_tyexpr_t *te;
    ST_decl_t *anon;
    u32 line, col;
} ST_field_spec_t;

typedef struct { ST_field_spec_t *items; u32 count, capacity; } ST_field_specs_t;

typedef struct
{
    ST_string_t name;
    ST_expr_t *value;
    ST_tyexpr_t *payload;
    u32 line, col;
} ST_variant_spec_t;

typedef struct { ST_variant_spec_t *items; u32 count, capacity; } ST_variant_specs_t;

typedef struct
{
    ST_string_t name;
    ST_tyexpr_t *te;
    ST_expr_t *def;
    u32 line, col;
} ST_param_t;

typedef struct { ST_param_t *items; u32 count, capacity; } ST_params_t;

typedef struct
{
    ST_params_t params;
    ST_tyexprs_t rets;
    b8 has_ret_ann;
    b8 is_variadic;
} ST_fn_sig_t;

struct ST_decl_t
{
    ST_decl_kind_t kind;
    ST_string_t name;
    b8 is_pub;
    u32 line, col;
    union {
        struct {
            ST_packing_t packing;
            ST_field_specs_t fields;
        } struct_;
        struct {
            b8 is_flag;
            ST_variant_specs_t variants;
        } enum_;
        struct {
            ST_variant_specs_t variants;
        } tag_union;
        struct {
            ST_expr_t *value;
        } const_;
        struct {
            ST_fn_sig_t sig;
        } extern_fn;
        struct {
            ST_tyexpr_t *te;
        } extern_var;
        struct {
            ST_fn_sig_t sig;
            ST_stmts_t body;
            b8 is_prototype;
        } fn;
    };
};

typedef struct
{
    ST_decls_t decls;
    ST_string_t file;
} ST_program_t;

ST_expr_t   *ST_expr_new(ST_arena_t *a, ST_expr_kind_t kind, u32 line, u32 col);
ST_stmt_t   *ST_stmt_new(ST_arena_t *a, ST_stmt_kind_t kind, u32 line, u32 col);
ST_decl_t   *ST_decl_new(ST_arena_t *a, ST_decl_kind_t kind, u32 line, u32 col);
ST_tyexpr_t *ST_tyexpr_new(ST_arena_t *a, ST_tyexpr_kind_t kind, u32 line, u32 col);

void ST_dump_program(FILE *out, ST_program_t *prog);
void ST_dump_decl(FILE *out, ST_decl_t *d, u32 depth);
void ST_dump_stmt(FILE *out, ST_stmt_t *s, u32 depth);
void ST_dump_expr(FILE *out, ST_expr_t *e, u32 depth);
void ST_dump_tyexpr(FILE *out, ST_tyexpr_t *te);

#endif
