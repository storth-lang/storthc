#ifndef ST_COMPTIME_H
#define ST_COMPTIME_H

// ---------------------------------------------------------------------------
// st_comptime: a small stack-based bytecode VM that runs at *compile time*,
// now arena-backed throughout (chunks, constants, and everything the
// AST->bytecode compiler in st_comptime_compile.c allocates all come from
// the same ST_arena_t the rest of the frontend/middle passes already use --
// no malloc/free anywhere in the hot path).
//
// Growth strategy: arenas can't realloc, so a chunk's code/lines/consts
// arrays grow by pushing a *new* block and copying, exactly like
// ST_da_append_arena elsewhere in the compiler (see utils/st_arena.h). The
// one thing this means for callers: never hold a raw pointer into a
// growing array across an emit call. The assembler API already only hands
// back byte *offsets* (see ST_ct_emit_jump/ST_ct_patch_jump), which stay
// valid across grows -- so nothing in the public API changed shape from the
// malloc version, only what happens underneath it.
// ---------------------------------------------------------------------------

#include "../../utils/st_arena.h"
#include "../../utils/st_helper.h"

#include <stdarg.h>

// -----------------------------------------------------------------------
// Values
// -----------------------------------------------------------------------

typedef enum {
    ST_CT_NIL,
    ST_CT_BOOL,
    ST_CT_INT,
    ST_CT_FLOAT,
    ST_CT_STRING, // fat pointer -- always into arena-owned (or source-owned) memory
    ST_CT_PTR,    // raw pointer -- results of native calls, dlopen handles, etc.
    ST_CT_NATIVE, // a bound C function ready to be called
} ST_ct_val_kind_t;

typedef struct {
    const char *data;
    u32 len;
} ST_ct_str_t;

// A native function binding: 'fn' is the resolved symbol, 'n_args' is how
// many i64-sized (integer/pointer) arguments it expects -- a manual
// substitute for real FFI type info; the directive that creates the
// binding states the arity, e.g. '#comptime_load("libm.so.6", "sqrt", 1)'.
typedef struct {
    void *fn;
    u32 n_args;
} ST_ct_native_t;

typedef struct {
    ST_ct_val_kind_t kind;
    union {
        b8 b;
        i64 i;
        f64 f;
        ST_ct_str_t str;
        void *ptr;
        ST_ct_native_t native;
    };
} ST_ct_val_t;

ST_ct_val_t ST_ct_nil(void);
ST_ct_val_t ST_ct_bool(b8 v);
ST_ct_val_t ST_ct_int(i64 v);
ST_ct_val_t ST_ct_float(f64 v);
ST_ct_val_t ST_ct_str(const char *data, u32 len);
ST_ct_val_t ST_ct_ptr(void *p);

b8 ST_ct_truthy(ST_ct_val_t v);
void ST_ct_val_print(ST_ct_val_t v); // debug only

// -----------------------------------------------------------------------
// Chunk
// -----------------------------------------------------------------------

typedef enum {
    ST_OP_CONST,        // operand: u32 index into chunk->consts -> push
    ST_OP_NIL,
    ST_OP_TRUE,
    ST_OP_FALSE,
    ST_OP_POP,

    ST_OP_ADD, ST_OP_SUB, ST_OP_MUL, ST_OP_DIV, ST_OP_MOD, ST_OP_NEG,
    ST_OP_EQ, ST_OP_NEQ, ST_OP_LT, ST_OP_LE, ST_OP_GT, ST_OP_GE,
    ST_OP_NOT,

    ST_OP_STR_LEN,       // pop string -> push int (its length) -- backs 'fmt.len'
    ST_OP_STR_INDEX,     // pop index, pop string -> push int (char code) -- backs 'fmt[i]'

    ST_OP_GET_LOCAL,     // operand: u32 stack slot index (relative to frame base)
    ST_OP_SET_LOCAL,     // operand: u32 stack slot index

    ST_OP_JMP,           // operand: i32 relative offset from *end* of this instr
    ST_OP_JMP_IF_FALSE,  // pops cond; operand: i32 relative offset
    ST_OP_LOOP,          // operand: i32 relative offset (always taken, backward)

    ST_OP_LOAD_LIB,      // pop string path -> push ptr (dlopen handle) or nil
    ST_OP_BIND_SYM,      // pop arity int, pop name string, pop handle ptr -> push native (or nil)
    ST_OP_CALL_NATIVE,   // pop native fn; pop n_args ints (arg0 pushed first) -> push i64 result

    ST_OP_COMP_ERROR,    // pop u32 count values -> abort compilation, concatenated into one diagnostic
    ST_OP_RETURN,        // pop value, halt with that as the chunk's result
    ST_OP_HALT,          // halt with ST_CT_NIL as the result

    ST_OP_COUNT,
} ST_ct_op_t;

typedef struct {
    ST_arena_t *arena; // everything below grows through this, never malloc/free

    u8 *code;
    u32 count, capacity;

    u32 *lines; // parallel to 'code', one entry per byte

    ST_ct_val_t *consts;
    u32 n_consts, cap_consts;
} ST_ct_chunk_t;

void ST_ct_chunk_init(ST_arena_t *arena, ST_ct_chunk_t *c);
// no ST_ct_chunk_free -- arena-owned, freed (all at once) with the arena

u32 ST_ct_emit_op(ST_ct_chunk_t *c, ST_ct_op_t op, u32 line);
u32 ST_ct_emit_op_u32(ST_ct_chunk_t *c, ST_ct_op_t op, u32 operand, u32 line);
u32 ST_ct_emit_const(ST_ct_chunk_t *c, ST_ct_val_t v, u32 line);
u32 ST_ct_emit_jump(ST_ct_chunk_t *c, ST_ct_op_t jump_op, u32 line);
void ST_ct_patch_jump(ST_ct_chunk_t *c, u32 operand_offset);
void ST_ct_emit_loop(ST_ct_chunk_t *c, u32 loop_start, u32 line);

// -----------------------------------------------------------------------
// VM
// -----------------------------------------------------------------------

#define ST_CT_STACK_MAX 1024

typedef enum {
    ST_CT_OK,
    ST_CT_ERR_RUNTIME,  // type error, stack over/underflow, etc: a VM/compiler bug, not user error
    ST_CT_ERR_COMPTIME, // #comp_error was reached: a legitimate diagnostic for the user
} ST_ct_status_t;

typedef struct {
    ST_ct_val_t stack[ST_CT_STACK_MAX];
    u32 sp;

    ST_ct_status_t status;
    char err_msg[512];
    u32 err_line;
} ST_ct_vm_t;

void ST_ct_vm_init(ST_ct_vm_t *vm);

ST_ct_status_t ST_ct_run(ST_ct_vm_t *vm, ST_ct_chunk_t *chunk, ST_ct_val_t *out);

// -----------------------------------------------------------------------
// Native C calls (dlopen/dlsym + hand-rolled SysV x86-64 caller). These
// stay outside the arena -- dlopen handles and resolved symbols are
// process-lifetime OS resources, not compiler data, so malloc-shaped
// bookkeeping (or none at all, for dlopen/dlsym themselves) is correct here.
// -----------------------------------------------------------------------

void *ST_ct_lib_load(const char *path);
ST_ct_native_t ST_ct_lib_bind(void *handle, const char *sym, u32 n_args);
i64 ST_ct_call_native(ST_ct_native_t fn, i64 *args, u32 n_args);

#endif
