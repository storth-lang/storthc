#ifndef ST_COMPTIME_H
#define ST_COMPTIME_H

// st_comptime: a small stack-based bytecode VM that runs at *compile time*,
// now arena-backed throughout (chunks, constants, and everything the
// AST->bytecode compiler in st_comptime_compile.c allocates all come from
// the same ST_arena_t the rest of the frontend/middle passes already use.

#include "../../utils/st_arena.h"
#include "../../utils/st_helper.h"

#include <stdarg.h>

typedef enum {
    ST_CT_NIL,
    ST_CT_BOOL,
    ST_CT_INT,
    ST_CT_FLOAT,
    ST_CT_STRING,
    ST_CT_PTR,
    ST_CT_NATIVE,
    ST_CT_STRUCT,
} ST_ct_val_kind_t;

typedef struct {
    const char *data;
    u32 len;
} ST_ct_str_t;

typedef struct {
    void *fn;
    u32 n_args;
} ST_ct_native_t;

typedef struct ST_ct_val_s {
    ST_ct_val_kind_t kind;
    union {
        b8 b;
        i64 i;
        f64 f;
        ST_ct_str_t str;
        void *ptr;
        ST_ct_native_t native;
        struct {
            struct ST_ct_val_s *fields;
            u32 n_fields;
        } st;
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

typedef enum {
    ST_OP_CONST,
    ST_OP_NIL,
    ST_OP_TRUE,
    ST_OP_FALSE,
    ST_OP_POP,

    ST_OP_ADD, ST_OP_SUB, ST_OP_MUL, ST_OP_DIV, ST_OP_MOD, ST_OP_NEG,
    ST_OP_EQ, ST_OP_NEQ, ST_OP_LT, ST_OP_LE, ST_OP_GT, ST_OP_GE,
    ST_OP_NOT,

    ST_OP_STR_LEN,
    ST_OP_STR_INDEX,     // pop index, pop string -> push int (char code) backs 'fmt[i]'

    ST_OP_GET_LOCAL,     // operand: u32 stack slot index (relative to frame base)
    ST_OP_SET_LOCAL,     // operand: u32 stack slot index

    ST_OP_JMP,           // operand: i32 relative offset from *end* of this instr
    ST_OP_JMP_IF_FALSE,  // pops cond; operand: i32 relative offset
    ST_OP_LOOP,          // operand: i32 relative offset (always taken, backward)

    ST_OP_LOAD_LIB,      // pop string path -> push ptr (dlopen handle); an empty string means
                         // dlopen(NULL)
    ST_OP_BIND_SYM,      // pop arity int, pop name string, pop handle ptr -> push native (or nil)
    ST_OP_CALL_NATIVE,   // pop native fn; pop n_args values (arg0 pushed first) -> push i64
                         // result. Args are used as-is except ST_CT_STRING, which is rejected
    ST_OP_NATIVE_ARG,    // pop one value; if it's ST_CT_STRING, push a ST_CT_PTR to a fresh
                         // nul-terminated copy (native/C-ABI calls need real C strings, not
                         // storth's fat {ptr,len} strings); anything else passes through as-is
    ST_OP_SYSCALL,       // pop 7 values (syscall number, then arg0..arg5, number pushed first)
                         // -> perform a real x86-64 Linux syscall directly, push the i64 result

    ST_OP_MAKE_STRUCT,   // operand: u32 n_fields
                         // -> push a ST_CT_STRUCT holding them in declaration order
    ST_OP_GET_FIELD,     // operand: u32 field_index
    ST_OP_PACK_STRUCT,   // operand: u32 n_fields, then n_fields more u32s (each field's byte
                         // width)

    ST_OP_CALL,          // operand: u32 entry_ip, u32 n_args. Top n_args values become the
                         // callee's first locals; pushes a frame and jumps to entry_ip
    ST_OP_COMP_ERROR,    // pop u32 count values -> abort compilation, concatenated into one diagnostic
    ST_OP_RETURN,        // operand: u32 n_values.
    ST_OP_HALT,          // no caller frame: halt with ST_CT_NIL as the result; with a caller
                         // frame: same as 'return nil' (a void function falling off its end)

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

u32 ST_ct_emit_op(ST_ct_chunk_t *c, ST_ct_op_t op, u32 line);
u32 ST_ct_emit_op_u32(ST_ct_chunk_t *c, ST_ct_op_t op, u32 operand, u32 line);
u32 ST_ct_emit_const(ST_ct_chunk_t *c, ST_ct_val_t v, u32 line);
u32 ST_ct_emit_jump(ST_ct_chunk_t *c, ST_ct_op_t jump_op, u32 line);
void ST_ct_patch_jump(ST_ct_chunk_t *c, u32 operand_offset);
void ST_ct_patch_jump_to(ST_ct_chunk_t *c, u32 operand_offset, u32 target_ip);
void ST_ct_emit_loop(ST_ct_chunk_t *c, u32 loop_start, u32 line);

// Emits 'CALL <placeholder entry_ip>, n_args'. Returns the offset of the
// entry_ip operand, to be filled in later via ST_ct_patch_call once the
// callee's real entry point is known (functions are compiled on demand,
// so a call site is often emitted before its callee has been compiled).
u32 ST_ct_emit_call(ST_ct_chunk_t *c, u32 n_args, u32 line);
void ST_ct_patch_call(ST_ct_chunk_t *c, u32 entry_ip_operand_offset, u32 entry_ip);

void ST_ct_emit_pack_struct(ST_ct_chunk_t *c, const u32 *field_sizes, u32 n_fields, u32 line);

#define ST_CT_STACK_MAX 1024
#define ST_CT_FRAMES_MAX 256

typedef enum {
    ST_CT_OK,
    ST_CT_ERR_RUNTIME,  // type error, stack over/underflow, etc: a VM/compiler bug, not user error
    ST_CT_ERR_COMPTIME, // #comp_error was reached: a legitimate diagnostic for the user
} ST_ct_status_t;

typedef struct {
    u32 return_ip;
    u32 saved_base;
} ST_ct_frame_t;

typedef struct {
    ST_ct_val_t stack[ST_CT_STACK_MAX];
    u32 sp;
    u32 base; // current frame's locals start here (0 for the outermost/main frame)

    ST_ct_frame_t frames[ST_CT_FRAMES_MAX];
    u32 n_frames;

    ST_ct_status_t status;
    char err_msg[512];
    u32 err_line;
} ST_ct_vm_t;

void ST_ct_vm_init(ST_ct_vm_t *vm);

ST_ct_status_t ST_ct_run(ST_ct_vm_t *vm, ST_ct_chunk_t *chunk, ST_ct_val_t *out);

// Native C calls.
void *ST_ct_lib_load(const char *path);
ST_ct_native_t ST_ct_lib_bind(void *handle, const char *sym, u32 n_args);
i64 ST_ct_call_native(ST_ct_native_t fn, i64 *args, u32 n_args);

#endif
