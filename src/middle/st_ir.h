#ifndef ST_IR_H
#define ST_IR_H

#include "../utils/st_helper.h"
#include "../utils/st_arena.h"
#include "../utils/st_string.h"
#include "../utils/st_ht.h"
#include "../frontend/st_types.h"

typedef struct ST_ir_fn_t ST_ir_fn_t;
typedef struct ST_ir_module_t ST_ir_module_t;
typedef struct ST_ir_block_t ST_ir_block_t;
typedef struct ST_ir_inst_t ST_ir_inst_t;

typedef struct
{
    ST_ir_inst_t **items;
    u32 count;
    u32 capacity;
} ST_ir_insts_t;

typedef struct
{
    ST_ir_block_t **items;
    u32 count;
    u32 capacity;
} ST_ir_blocks_t;

typedef enum
{
    ST_IR_CONST_INT,
    ST_IR_CONST_FLOAT,
 
    ST_IR_ADD, ST_IR_SUB, ST_IR_MUL,
    ST_IR_SDIV, ST_IR_UDIV, ST_IR_SREM, ST_IR_UREM,
    ST_IR_FADD, ST_IR_FSUB, ST_IR_FMUL, ST_IR_FDIV,
    ST_IR_NEG, ST_IR_FNEG,
    ST_IR_AND, ST_IR_OR, ST_IR_XOR, ST_IR_SHL, ST_IR_LSHR, ST_IR_ASHR, ST_IR_NOT,
 
    ST_IR_ICMP_EQ, ST_IR_ICMP_NE,
    ST_IR_ICMP_SLT, ST_IR_ICMP_SLE, ST_IR_ICMP_SGT, ST_IR_ICMP_SGE,
    ST_IR_ICMP_ULT, ST_IR_ICMP_ULE, ST_IR_ICMP_UGT, ST_IR_ICMP_UGE,
    ST_IR_FCMP_EQ, ST_IR_FCMP_NE,
    ST_IR_FCMP_LT, ST_IR_FCMP_LE, ST_IR_FCMP_GT, ST_IR_FCMP_GE,
 
    ST_IR_CAST,
    ST_IR_PARAM,
    ST_IR_CALL,
    ST_IR_CALL_INDIRECT,
    ST_IR_PHI,
 
    ST_IR_COUNT,
} ST_ir_op_t;

typedef enum
{
    ST_IR_TERM_NONE,
    ST_IR_TERM_RET,
    ST_IR_TERM_BR,
    ST_IR_TERM_COND_BR,
    ST_IR_TERM_UNREACHABLE,
} ST_ir_term_kind_t;

typedef struct
{
    ST_ir_term_kind_t kind;
    ST_ir_insts_t rets;
    ST_ir_inst_t *cond;
    ST_ir_block_t *t_block, *f_block;
    u32 line, col;
} ST_ir_term_t;

struct ST_ir_inst_t
{
    ST_ir_op_t kind;
    ST_ty_t *ty;
    u32 id, line, col;
    ST_ir_block_t *block;
    ST_ir_inst_t *prev, *next;
    ST_ir_inst_t *repl;
    b8 removed;

    union {
        i64 const_int;
        f64 const_float;
        struct { ST_ir_inst_t *l, *r; } bin;
        struct { ST_ir_inst_t *v; } unary;
        struct { ST_ir_inst_t *v; } cast;
        struct { u32 index; ST_string_t name; } params;
        struct {
            ST_string_t callee_name;
            ST_ir_fn_t *callee;
            ST_ir_insts_t args;
        } call;
        struct {
            ST_ir_inst_t *callee_ptr;
            ST_ir_insts_t args;
        } call_ind;
        struct {
            ST_ir_insts_t values;
            ST_ir_blocks_t preds;
        } phi;
    };

};

typedef struct { void *var; ST_ir_inst_t *phi; } ST_ir_pending_phi_t;
typedef struct { ST_ir_pending_phi_t *items; u32 count, capacity; } ST_ir_pending_phis_t;

struct ST_ir_block_t
{
    u32 id;
    ST_string_t name;
    ST_ir_fn_t *fn;

    ST_ir_inst_t *first, *last;
    ST_ir_blocks_t preds;
    ST_ir_term_t term;

    b8 seald, filled;
    ST_ht_t var_defs;
    ST_ir_pending_phis_t incomplete_phis;
};

struct ST_ir_fn_t
{
    ST_arena_t *arena;
    ST_string_t name;
    ST_ty_t *ty;

    ST_ir_blocks_t blocks;
    ST_ir_block_t *entry;

    u32 next_value_id;
    u32 next_block_id;

    b8 is_extern;
    b8 is_pub;
    b8 is_variadic;
};


typedef struct { ST_ir_fn_t **items; u32 count, capacity; } ST_ir_fns_t;
 
struct ST_ir_module_t
{
    ST_arena_t *arena;
    ST_string_t name;
    ST_ir_fns_t fns;
};


void ST_ir_module_init(ST_arena_t *arena, ST_string_t name, ST_ir_module_t *out);
ST_ir_fn_t *ST_ir_fn_new(ST_ir_module_t *m, ST_string_t name, ST_ty_t *fn_ty);
ST_ir_fn_t *ST_ir_module_find_fn(ST_ir_module_t *m, ST_string_t name);
ST_ir_block_t  *ST_ir_block_new(ST_ir_fn_t *fn, const char *label_hint);
 
void ST_ir_block_seal(ST_ir_block_t *b);
void ST_ir_add_edge(ST_ir_block_t *from, ST_ir_block_t *to);
b8 ST_ir_block_is_terminated(ST_ir_block_t *b);


void ST_ir_write_var(ST_ir_block_t *b, void *var, ST_ir_inst_t *val);
ST_ir_inst_t *ST_ir_read_var(ST_ir_block_t *b, void *var, ST_ty_t *ty);

ST_ir_inst_t *ST_ir_const_int(ST_ir_block_t *b, ST_ty_t *ty, i64 v);
ST_ir_inst_t *ST_ir_const_float(ST_ir_block_t *b, ST_ty_t *ty, f64 v);
ST_ir_inst_t *ST_ir_binop(ST_ir_block_t *b, ST_ir_op_t op, ST_ty_t *ty, ST_ir_inst_t *l, ST_ir_inst_t *r, u32 line, u32 col);
ST_ir_inst_t *ST_ir_unop(ST_ir_block_t *b, ST_ir_op_t op, ST_ty_t *ty, ST_ir_inst_t *v, u32 line, u32 col);
ST_ir_inst_t *ST_ir_cast(ST_ir_block_t *b, ST_ty_t *to_ty, ST_ir_inst_t *v, u32 line, u32 col);
ST_ir_inst_t *ST_ir_param(ST_ir_block_t *b, ST_ty_t *ty, u32 index, ST_string_t name);
ST_ir_inst_t *ST_ir_call(ST_ir_block_t *b, ST_ty_t *ret_ty, ST_string_t callee_name,
                          ST_ir_fn_t *callee, ST_ir_inst_t **args, u32 n_args, u32 line, u32 col);
ST_ir_inst_t *ST_ir_call_indirect(ST_ir_block_t *b, ST_ty_t *ret_ty, ST_ir_inst_t *callee_ptr,
                                   ST_ir_inst_t **args, u32 n_args, u32 line, u32 col);

void ST_ir_term_ret(ST_ir_block_t *b, ST_ir_inst_t **vals, u32 n_vals, u32 line, u32 col);
void ST_ir_term_br(ST_ir_block_t *b, ST_ir_block_t *target, u32 line, u32 col);
void ST_ir_term_condbr(ST_ir_block_t *b, ST_ir_inst_t *cond, ST_ir_block_t *t, ST_ir_block_t *f, u32 line, u32 col);
void ST_ir_term_unreachable(ST_ir_block_t *b, u32 line, u32 col);
 
void ST_ir_inst_remove(ST_ir_inst_t *inst);

void ST_ir_dump_module(FILE *out, ST_ir_module_t *m);
void ST_ir_dump_fn(FILE *out, ST_ir_fn_t *fn);
 
#endif
