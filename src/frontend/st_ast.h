#ifndef ST_AST_H
#define ST_AST_H

#include "../utils/st_arena.h"
#include "../utils/st_helper.h"
#include "../utils/st_string.h"
#include "st_lexer.h"

typedef struct ST_ty_t ST_ty_t;

typedef struct ST_tyexpr_t ST_tyexpr_t;
typedef struct ST_expr_t ST_expr_t;
typedef struct ST_stmt_t ST_stmt_t;
typedef struct ST_decl_t ST_decl_t;

typedef struct {
    ST_expr_t **items;
    u32 count, capacity;
} ST_exprs_t;
typedef struct {
    ST_tyexpr_t **items;
    u32 count, capacity;
} ST_tyexprs_t;
typedef struct {
    ST_stmt_t **items;
    u32 count, capacity;
} ST_stmts_t;
typedef struct {
    ST_decl_t **items;
    u32 count, capacity;
} ST_decls_t;

typedef enum {
    ST_TE_NAME,
    ST_TE_PTR,
    ST_TE_ARRAY,
    ST_TE_FN,
    ST_TE_TYPEOF,
    ST_TE_GENERIC_INST,
} ST_tyexpr_kind_t;

struct ST_tyexpr_t {
    ST_tyexpr_kind_t kind;
    u32 line, col;
    ST_string_t name;
    ST_tyexpr_t *inner;
    ST_expr_t *count_expr;
    b8 is_dynamic;
    ST_tyexprs_t fn_params;
    ST_tyexprs_t fn_rets;
    b8 fn_is_variadic;
    b8 is_generic_param;
    ST_tyexprs_t generic_args;
    ST_expr_t *typeof_operand;
    ST_ty_t *resolved;
};

typedef enum {
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
    ST_EX_FIELDS,
    ST_EX_COMP_ERROR,
    ST_EX_ASM,
    ST_EX_STR_FROM_RAW,
    ST_EX_COUNT,
} ST_expr_kind_t;

typedef struct {
    ST_string_t name;
    ST_expr_t *value;
    u32 line, col;
} ST_field_init_t;

typedef struct {
    ST_field_init_t *items;
    u32 count, capacity;
} ST_field_inits_t;

typedef struct {
    ST_string_t name;
    ST_expr_t *value;
} ST_arg_t;

typedef struct {
    ST_arg_t *items;
    u32 count, capacity;
} ST_args_t;

struct ST_expr_t {
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
            ST_tyexprs_t generic_args;
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
        struct {
            ST_exprs_t args; // '#comp_error(a, b, c)' -- concatenated into one diagnostic when reached
        } comp_error;
        struct {
            ST_token_t *tokens;
            u32 n_tokens;
        } asm_; // '#asm { .. }' used as an expression, e.g. 'return #asm { .. };'
        struct {
            ST_expr_t *ptr;
            ST_expr_t *len;
        } str_from_raw; // 'str_from_raw(ptr, len)' -- builds a 'string' from a raw '*char' + length
    };
};

typedef enum {
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
    ST_ST_ASM,
    ST_ST_COUNT,
} ST_stmt_kind_t;

typedef struct {
    ST_exprs_t values;
    ST_stmts_t body;
    u32 line, col;
} ST_case_t;

typedef struct {
    ST_case_t *items;
    u32 count, capacity;
} ST_cases_t;

struct ST_stmt_t {
    ST_stmt_kind_t kind;
    u32 line, col;
    union {
        ST_expr_t *expr;
        struct {
            ST_string_t name;
            ST_tyexpr_t *te;
            ST_expr_t *init;
            b8 is_static;
            b8 is_const;
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
            b8 is_comptime; // leading token was '#if', not 'if'
        } if_;
        struct {
            ST_expr_t *cond;
            ST_cases_t cases;
            b8 is_comptime; // same '#if { case ... }' form, comptime-pruned
        } switch_;
        struct {
            ST_expr_t *cond;
            ST_stmts_t body;
        } while_;
        struct {
            ST_string_t iter;
            ST_expr_t *lo, *hi;
            ST_tyexpr_t *iter_te;
            b8 inclusive;
            b8 is_comptime; // '#for' -- unrolled at compile time, not a real loop
            ST_stmts_t body;
        } for_range;
        struct {
            ST_string_t iter;
            ST_string_t spec_iter; // optional second binding, len==0 if unused (see '#for' parsing)
            ST_expr_t *target;
            b8 is_comptime; // '#for ch[, spec]: string_expr' -- unrolled at compile time
            ST_stmts_t body;
        } for_array;
        struct {
            ST_exprs_t values;
        } ret;
        struct {
            ST_token_t *tokens;
            u32 n_tokens;
        } asm_;
        ST_stmts_t block;
        ST_stmt_t *defer_stmt;
        ST_string_t label;
    };
};

typedef enum {
    ST_DE_STRUCT,
    ST_DE_ENUM,
    ST_DE_TAG_UNION,
    ST_DE_CONST,
    ST_DE_EXTERN_FN,
    ST_DE_EXTERN_VAR,
    ST_DE_GLOBAL,
    ST_DE_FN,
    ST_DE_IMPORT,
    ST_DE_COUNT,
} ST_decl_kind_t;

typedef enum {
    ST_PACK_DEFAULT,
    ST_PACK_C,
    ST_PACK_PACKED,
} ST_packing_t;

typedef struct {
    ST_string_t name;
    ST_tyexpr_t *te;
    ST_decl_t *anon;
    u32 line, col;
} ST_field_spec_t;

typedef struct {
    ST_field_spec_t *items;
    u32 count, capacity;
} ST_field_specs_t;

typedef struct {
    ST_string_t name;
    ST_expr_t *value; // explicit '= expr' initializer, if any (parsed, unevaluated)
    ST_tyexpr_t *payload;
    u32 line, col;
    i64 computed;    // resolved constant value, filled in by semantic analysis
    b8 has_computed; // whether 'computed' has been filled in yet
} ST_variant_spec_t;

typedef struct {
    ST_variant_spec_t *items;
    u32 count, capacity;
} ST_variant_specs_t;

typedef struct {
    ST_string_t name;
    ST_tyexpr_t *te;
    ST_expr_t *def;
    u32 line, col;
    b8 is_pack; // 'name: any...' -- collects all trailing call args into a real array
} ST_param_t;

typedef struct {
    ST_param_t *items;
    u32 count, capacity;
} ST_params_t;

typedef struct {
    ST_params_t params;
    ST_tyexprs_t rets;
    b8 has_ret_ann;
    b8 is_variadic;   // raw C-ABI '...' (extern only)
    b8 has_any_pack;  // trailing 'name: any...' (non-extern; collected into an array)
    b8 has_generic_pack; // trailing 'name: $T...' (comptime; one synthetic param per call-site arg)
    ST_strings_t generics;
} ST_fn_sig_t;

struct ST_decl_t {
    ST_decl_kind_t kind;
    ST_string_t name;
    ST_string_t display_name;
    b8 is_pub;
    u32 line, col;
    union {
        struct {
            ST_packing_t packing;
            ST_field_specs_t fields;
            ST_strings_t generics;
        } struct_;
        struct {
            b8 is_flag;
            ST_tyexpr_t *ty;
            ST_variant_specs_t variants;
        } enum_;
        struct {
            ST_variant_specs_t variants;
        } tag_union;
        struct {
            ST_tyexpr_t *te; // optional explicit type: 'NAME : type : expr;' (NULL if 'NAME :: expr;')
            ST_expr_t *value;
        } const_;
        struct {
            ST_fn_sig_t sig;
        } extern_fn;
        struct {
            ST_tyexpr_t *te;
        } extern_var;
        struct {
            ST_tyexpr_t *te;
            ST_expr_t *init;
        } global_;
        struct {
            ST_fn_sig_t sig;
            ST_stmts_t body;
            b8 is_prototype;
            b8 had_pack;         // true if this decl came from a '$T...' pack instantiation
            ST_string_t pack_name; // the pack param's original declared name, e.g. 'args'
            u32 pack_count;       // how many real args landed in the pack at this call site
            b8 has_bound_str;      // a plain 'string' param whose value was compile-time-known
            ST_string_t bound_str_param; // that param's declared name, e.g. 'fmt'
            ST_string_t bound_str_value; // the literal value bound at this call site
        } fn;
        struct {
            ST_string_t module_name; // directory name under modules/
            ST_string_t alias;       // namespace bound to (defaults to module_name)
        } import_;
    };
};

typedef struct {
    ST_decls_t decls;
    ST_string_t file;
} ST_program_t;

ST_expr_t *ST_expr_new(ST_arena_t *a, ST_expr_kind_t kind, u32 line, u32 col);
ST_stmt_t *ST_stmt_new(ST_arena_t *a, ST_stmt_kind_t kind, u32 line, u32 col);
ST_decl_t *ST_decl_new(ST_arena_t *a, ST_decl_kind_t kind, u32 line, u32 col);
ST_tyexpr_t *ST_tyexpr_new(ST_arena_t *a, ST_tyexpr_kind_t kind, u32 line, u32 col);

void ST_dump_program(FILE *out, ST_program_t *prog);
void ST_dump_decl(FILE *out, ST_decl_t *d, u32 depth);
void ST_dump_stmt(FILE *out, ST_stmt_t *s, u32 depth);
void ST_dump_expr(FILE *out, ST_expr_t *e, u32 depth);
void ST_dump_tyexpr(FILE *out, ST_tyexpr_t *te);

#endif
