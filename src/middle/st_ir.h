#ifndef ST_IR_H
#define ST_IR_H

#include "../frontend/st_types.h"
#include "../utils/st_arena.h"
#include "../utils/st_helper.h"
#include "../utils/st_ht.h"
#include "../utils/st_string.h"

typedef struct ST_ir_fn_t ST_ir_fn_t;
typedef struct ST_ir_module_t ST_ir_module_t;
typedef struct ST_ir_block_t ST_ir_block_t;
typedef struct ST_ir_inst_t ST_ir_inst_t;

// @note: ST_ir_blocks_t is a dynamic array to ST_ir_inst_t.
typedef struct {
    ST_ir_inst_t **items;
    u32 count;
    u32 capacity;
} ST_ir_insts_t;

// @note: ST_ir_blocks_t is a dynamic array to ST_ir_block_t.
typedef struct {
    ST_ir_block_t **items;
    u32 count;
    u32 capacity;
} ST_ir_blocks_t;

// @note This is all the IR operations we have supported right now.
typedef enum {
    ST_IR_CONST_INT,
    ST_IR_CONST_FLOAT,
    ST_IR_CONST_STRING,

    ST_IR_ADD,
    ST_IR_SUB,
    ST_IR_MUL,
    ST_IR_SDIV,
    ST_IR_UDIV,
    ST_IR_SREM,
    ST_IR_UREM,
    ST_IR_FADD,
    ST_IR_FSUB,
    ST_IR_FMUL,
    ST_IR_FDIV,
    ST_IR_NEG,
    ST_IR_FNEG,
    ST_IR_AND,
    ST_IR_OR,
    ST_IR_XOR,
    ST_IR_SHL,
    ST_IR_LSHR,
    ST_IR_ASHR,
    ST_IR_NOT,

    ST_IR_ICMP_EQ,
    ST_IR_ICMP_NE,
    ST_IR_ICMP_SLT,
    ST_IR_ICMP_SLE,
    ST_IR_ICMP_SGT,
    ST_IR_ICMP_SGE,
    ST_IR_ICMP_ULT,
    ST_IR_ICMP_ULE,
    ST_IR_ICMP_UGT,
    ST_IR_ICMP_UGE,
    ST_IR_FCMP_EQ,
    ST_IR_FCMP_NE,
    ST_IR_FCMP_LT,
    ST_IR_FCMP_LE,
    ST_IR_FCMP_GT,
    ST_IR_FCMP_GE,

    ST_IR_CAST,
    ST_IR_PARAM,
    ST_IR_CALL,
    ST_IR_EXTRACT_OP,
    ST_IR_CALL_INDIRECT,
    ST_IR_PHI,

    ST_IR_ALLOCA,
    ST_IR_LOAD,
    ST_IR_STORE,
    ST_IR_ADDR,
    ST_IR_GLOBAL_ADDR,
    ST_IR_INLINE_ASM,

    ST_IR_COUNT,
} ST_ir_op_t;

typedef enum {
    ST_IR_TERM_NONE,
    ST_IR_TERM_RET,
    ST_IR_TERM_BR,
    ST_IR_TERM_COND_BR,
    ST_IR_TERM_UNREACHABLE,
} ST_ir_term_kind_t;

typedef struct {
    ST_ir_term_kind_t kind;
    ST_ir_insts_t rets;
    ST_ir_inst_t *cond;
    ST_ir_block_t *t_block, *f_block;
    u32 line, col;
} ST_ir_term_t;

struct ST_ir_inst_t {
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
        u32 str_index;
        struct {
            ST_ir_inst_t *l, *r;
        } bin;
        struct {
            ST_ir_inst_t *v;
        } unary;
        struct {
            ST_ir_inst_t *v;
        } cast;
        struct {
            u32 index;
            ST_string_t name;
        } params;
        struct {
            ST_string_t callee_name;
            ST_ir_fn_t *callee;
            ST_ir_insts_t args;
            u32 ret_buf_offset;
        } call;
        struct {
            ST_ir_inst_t *callee_ptr;
            ST_ir_insts_t args;
        } call_ind;
        struct {
            ST_ir_insts_t values;
            ST_ir_blocks_t preds;
        } phi;
        struct {
            ST_ir_inst_t *agg;
            u32 index;
        } extract;
        struct {
            u32 size, align;
            u32 frame_off;
        } alloca_;
        struct {
            ST_ir_inst_t *addr;
        } load;
        struct {
            ST_ir_inst_t *addr;
            ST_ir_inst_t *v;
        } store;
        struct {
            ST_ir_inst_t *base;
            ST_ir_inst_t *index;
            u32 scale;
            i32 offset;
        } addr;
        ST_string_t global_name;
        struct {
            ST_string_t tmpl;
            ST_ir_insts_t refs;
        } inline_asm;
    };
};

typedef struct {
    void *var;
    ST_ir_inst_t *phi;
} ST_ir_pending_phi_t;
typedef struct {
    ST_ir_pending_phi_t *items;
    u32 count, capacity;
} ST_ir_pending_phis_t;

struct ST_ir_block_t {
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

struct ST_ir_fn_t {
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

// @note; ST_ir_fns_t is dynamic array of ST_ir_fn_t.
typedef struct {
    ST_ir_fn_t **items;
    u32 count, capacity;
} ST_ir_fns_t;

// @note; ST_ir_strs_t is dynamic array of ST_string_t.
typedef struct {
    ST_string_t *items;
    u32 count, capacity;
} ST_ir_strs_t;

// @note: a single top-level 'global'/'global const' variable: reserves
// storage in .bss (zero/no initializer) or .data (constant scalar
// initializer) under 'name', addressed the same way as a function (see
// ST_ir_global_addr).
typedef struct {
    ST_string_t name;
    ST_ty_t *ty;
    b8 has_init;
    b8 init_is_float;
    i64 init_int;
    f64 init_float;
    b8 is_pub;
} ST_ir_global_var_t;

typedef struct {
    ST_ir_global_var_t *items;
    u32 count, capacity;
} ST_ir_global_vars_t;

// @note: ST_ir_module_t contains all of the function and strings in the
// intermediate represntation.
struct ST_ir_module_t {
    ST_arena_t *arena;
    ST_string_t name;
    ST_ir_fns_t fns;
    ST_ir_strs_t strs;
    ST_ir_global_vars_t globals;
};

// @note: ST_ir_module_init is module initalization for the SSA IR it takes an arena and
// module name and module out.
void ST_ir_module_init(ST_arena_t *arena, ST_string_t name, ST_ir_module_t *out);

// @note: ST_ir_fn_new will create a new function in the module and associate it with
// the type of the function. It takes the name of the function and the return type of
// the function. It will return as a function structure.
ST_ir_fn_t *ST_ir_fn_new(ST_ir_module_t *m, ST_string_t name, ST_ty_t *fn_ty);

// @note: ST_ir_module_find_fn will go though the function list in the module and checks if that
// function exists or not.
ST_ir_fn_t *ST_ir_module_find_fn(ST_ir_module_t *m, ST_string_t name);

// @note: ST_ir_module_add_global registers a top-level 'global'/'global
// const' variable with the module so the backend can reserve storage for it.
// Pass has_init=0 for a zero-initialized (.bss) global; otherwise fill in
// exactly one of init_int/init_float depending on init_is_float.
void ST_ir_module_add_global(ST_ir_module_t *m, ST_string_t name, ST_ty_t *ty, b8 is_pub,
                             b8 has_init, b8 init_is_float, i64 init_int, f64 init_float);

// @note: ST_ir_module_find_global looks up a registered global variable by name.
ST_ir_global_var_t *ST_ir_module_find_global(ST_ir_module_t *m, ST_string_t name);

// @note: ST_ir_block_new is for creating a new block inside the function with a label.
ST_ir_block_t *ST_ir_block_new(ST_ir_fn_t *fn, const char *label_hint);

// @note: ST_ir_block_seal is to seal the block as the block does not have any exit showing
// the end of the block.
void ST_ir_block_seal(ST_ir_block_t *b);

// @note: ST_ir_add_edge will add a new 'edge' in the IR that goes from one
// block to another as it accept the source 'from' and the destination 'to'
void ST_ir_add_edge(ST_ir_block_t *from, ST_ir_block_t *to);

// @note: ST_ir_block_is_terminated will check if out block is terminated or not.
b8 ST_ir_block_is_terminated(ST_ir_block_t *b);

// @note: ST_ir_write_var will create a new variable at the block with the
// instance value.
void ST_ir_write_var(ST_ir_block_t *b, void *var, ST_ir_inst_t *val);

// @note: ST_ir_read_var will read a variable at that block with its type.
ST_ir_inst_t *ST_ir_read_var(ST_ir_block_t *b, void *var, ST_ty_t *ty);

// @note: ST_ir_const_int will create an interger value at that block with that
// integer type as we have different integer types as i32, u32, u64, and so on.
ST_ir_inst_t *ST_ir_const_int(ST_ir_block_t *b, ST_ty_t *ty, i64 v);

// @note: ST_ir_const_float will create a float value at that block with that
// float type as we have different float types aka f32 and f64.
ST_ir_inst_t *ST_ir_const_float(ST_ir_block_t *b, ST_ty_t *ty, f64 v);

// @note: ST_ir_module_intern_str this will just intern a string byte so we can
// pass it to the module. This will return the bytes of the strings that got interened.
u32 ST_ir_module_intern_str(ST_ir_module_t *m, ST_string_t bytes);

// @note: ST_ir_const_str is going to create a constant string in that module
// with its own index as our implementation of strings are sized.
ST_ir_inst_t *ST_ir_const_str(ST_ir_block_t *b, ST_ty_t *ty, u32 index);

// @note: ST_ir_binop will create a new binary operation between the left hand
// side 'l' instance and the right hand side 'r' instance. It accepts where the
// operation was created 'line' and also the 'colomn'.
ST_ir_inst_t *ST_ir_binop(ST_ir_block_t *b, ST_ir_op_t op, ST_ty_t *ty, ST_ir_inst_t *l,
                          ST_ir_inst_t *r, u32 line, u32 col);

// @note: ST_ir_unary will create a new unary operation on a value instance and
// It accepts where the operation was created 'line' and also the 'colomn'.
ST_ir_inst_t *ST_ir_unop(ST_ir_block_t *b, ST_ir_op_t op, ST_ty_t *ty, ST_ir_inst_t *v, u32 line,
                         u32 col);

// @note: ST_ir_cast is used to accept a value 'v' to be then chaned into desired
// type while accept the line and colomun locations.
ST_ir_inst_t *ST_ir_cast(ST_ir_block_t *b, ST_ty_t *to_ty, ST_ir_inst_t *v, u32 line, u32 col);

// @note: ST_ir_param this is for parameters that are loaded for example in
// functions or strcture that have parameters will have their own block index as
// we can have multiple parameter and the name of parameter.
ST_ir_inst_t *ST_ir_param(ST_ir_block_t *b, ST_ty_t *ty, u32 index, ST_string_t name);

// @note: ST_ir_call is to denote if a function has been called. It accepts the
// block, who the caller is and how many arguments have been passed as well as the return type of
// the function as well as the location from which it is called.
ST_ir_inst_t *ST_ir_call(ST_ir_block_t *b, ST_ty_t *ret_ty, ST_string_t callee_name,
                         ST_ir_fn_t *callee, ST_ir_inst_t **args, u32 n_args, u32 line, u32 col);

// @note: ST_ir_extract is to extract the return type of an instance. This might
// be depending on the context might be function return or structure
// parameter. This will just be used to extract values from an instance. As the
// signature implies it expects line and column location and the index of the
// item we are trying to extract from.
ST_ir_inst_t *ST_ir_extract(ST_ir_block_t *b, ST_ty_t *ret_ty, ST_ir_inst_t *agg, u32 index,
                            u32 line, u32 col);

// @note: ST_ir_call_indirect is for indirect call of that happens due to
// pointer aka function pointer as it accept the arugments, the number of the
// arguments and the calle as it is a pointer.
ST_ir_inst_t *ST_ir_call_indirect(ST_ir_block_t *b, ST_ty_t *ret_ty, ST_ir_inst_t *callee_ptr,
                                  ST_ir_inst_t **args, u32 n_args, u32 line, u32 col);

// @note: ST_ir_alloca is for allocating some object in the ir. If we did 'x := 0' we
// did allocate some memory for x we will use this function to allocate such addressses.
ST_ir_inst_t *ST_ir_alloca(ST_ir_fn_t *fn, ST_ty_ctx_t *ctx, ST_ty_t *p, u32 line, u32 col);

// @note: ST_ir_load will load an address from that block if we obviously now
// tried to access 'x := 0' with 'p := &x' we effectly will use ir_load to get
// the address of x.
ST_ir_inst_t *ST_ir_load(ST_ir_block_t *b, ST_ty_t *ty, ST_ir_inst_t *addr, u32 line, u32 col);

// @note: ST_ir_store will store a value on the address specified.
ST_ir_inst_t *ST_ir_store(ST_ir_block_t *b, ST_ty_t *ty, ST_ir_inst_t *addr, ST_ir_inst_t *v,
                          u32 line, u32 col);

// @note: ST_ir_addr will allocate a new address for the pointer by checking the
// base and the index as well as the offset as not all type have the same offset.
ST_ir_inst_t *ST_ir_addr(ST_ir_block_t *b, ST_ty_t *ptr_ty, ST_ir_inst_t *base, ST_ir_inst_t *index,
                         u32 scale, i32 offset, u32 line, u32 col);

// @note: st_ir_global_addr will allocate a new address just like ST_ir_add the
// only difference being here is that in the global address you can put
// functions so that function pointers can load them from the global address not
// the regular address space.
ST_ir_inst_t *ST_ir_global_addr(ST_ir_block_t *b, ST_ty_t *ptr_ty, ST_string_t name, u32 line,
                                u32 col);

// @note: ST_ir_inline_asm emits a raw inline-assembly instruction. 'tmpl' is
// the already-expanded template text (see the union comment in this header
// for the placeholder format) and 'refs' are the ST_IR_ALLOCA instructions
// that the template's placeholders index into, in placeholder order. 'ty' is
// NULL for a statement-form asm block with no result value, or the expected
// type when the block is used as an expression (e.g. 'return #asm { .. };')
// -- the backend spills whatever's in rax/xmm0 to this instruction's slot
// whenever 'ty' is non-void, so a non-NULL 'ty' here is what makes the
// asm block's result usable as a value.
ST_ir_inst_t *ST_ir_inline_asm(ST_ir_block_t *b, ST_string_t tmpl, ST_ir_inst_t **refs, u32 n_refs,
                               ST_ty_t *ty, u32 line, u32 col);

// @note: ST_ir_term_ret will make a block return values as the values and their
// count is passed in the function parameter.
void ST_ir_term_ret(ST_ir_block_t *b, ST_ir_inst_t **vals, u32 n_vals, u32 line, u32 col);

// @note: ST_ir_term_ret will make a block break to a target block.
void ST_ir_term_br(ST_ir_block_t *b, ST_ir_block_t *target, u32 line, u32 col);

// @note: ST_ir_term_ret will make a signal a conditional break used in if,
// while, else. So then we will accept what is the new target we want to do our
// conditional break 'to' and also accept where we are breaking 'from'.
void ST_ir_term_condbr(ST_ir_block_t *b, ST_ir_inst_t *cond, ST_ir_block_t *t, ST_ir_block_t *f,
                       u32 line, u32 col);

// @note: ST_ir_term_unreachable this is to denote an unreachable block later
// can be used by the control flow analysis for warning the user we have
// unreachable dead code.
void ST_ir_term_unreachable(ST_ir_block_t *b, u32 line, u32 col);

// @note: ST_ir_inst_remove this used to remove an instance we created.
void ST_ir_inst_remove(ST_ir_inst_t *inst);

// @DEBUG: The functions below are for debugging in real world once the compiler works there is
// not a use case for these functions. Unless some one obviously tries to study
// what I did in my IR.

// @note: ST_ir_dump_module will dump the entire module to the output stream set
// by default it is set to stdout if passed NULL.
void ST_ir_dump_module(FILE *out, ST_ir_module_t *m);

// @note: ST_ir_dump_fn will dump the entire function to the output stream set
// by default it is set to stdout if passed NULL.
void ST_ir_dump_fn(FILE *out, ST_ir_fn_t *fn);

#endif
