#ifndef ST_COMPTIME_H
#define ST_COMPTIME_H

// st_comptime: a small stack-based bytecode VM that runs at *compile time*,
// now arena-backed throughout (chunks, constants, and everything the
// AST->bytecode compiler in st_comptime_compile.c allocates all come from
// the same ST_arena_t the rest of the frontend/middle passes already use.

#include "../../utils/st_arena.h"
#include "../../utils/st_helper.h"
#include "../../utils/st_string.h"

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
    ST_OP_DUP,
    ST_OP_NIP,

    ST_OP_ADD, ST_OP_SUB, ST_OP_MUL, ST_OP_DIV, ST_OP_MOD, ST_OP_NEG,
    ST_OP_EQ, ST_OP_NEQ, ST_OP_LT, ST_OP_LE, ST_OP_GT, ST_OP_GE,
    ST_OP_NOT,
    ST_OP_BAND, ST_OP_BOR, ST_OP_BXOR, ST_OP_SHL, ST_OP_SHR, // bitwise int ops:
                         // '&' '|' '^' '<<' '>>'. '>>' is arithmetic (sign-extending),
                         // matching plain i64 semantics
    ST_OP_STR_LEN,       // pops a string or a #comptime array/struct value -> push its length
    ST_OP_STR_INDEX,     // pop index, pop string -> push int (char code) backs 'fmt[i]'
    ST_OP_STR_PTR,       // pop a string -> push a ST_CSTR_PTR with no deep copy.
    ST_OP_STRUCT_INDEX,  // pop index, pop struct/array value -> push that field/element,
                         // runtime-indexed (unlike ST_OP_GET_FIELD's compile-time-constant index);
                         // backs 'arr.ptr[i]' for a #comptime array/slice literal

    ST_OP_CAST,          // operand: u32 encoding (tag | width<<4 | is_signed<<12); tag 0=int,
                         // 1=float, 2=bool, 3=ptr. pops one value, pushes the converted value

    ST_OP_GET_LOCAL,     // operand: u32 stack slot index (relative to frame base)
    ST_OP_SET_LOCAL,     // operand: u32 stack slot index

    ST_OP_JMP,           // operand: i32 relative offset from *end* of this instr
    ST_OP_JMP_IF_FALSE,  // pops cond; operand: i32 relative offset
    ST_OP_LOOP,          // operand: i32 relative offset (always taken, backward)

    ST_OP_LOAD_LIB,      // pop string path -> push ptr (dlopen handle); an empty string means
                         // dlopen(NULL)
    ST_OP_BIND_SYM,      // pop arity int, pop name string, pop handle ptr -> push native (or nil)
    ST_OP_BIND_DATA_SYM,
    ST_OP_CALL_NATIVE,   // pop native fn; pop n_args values (arg0 pushed first) -> push i64
                         // result. Args are used as-is except ST_CT_STRING, which is rejected
    ST_OP_NATIVE_ARG,    // pop one value; if it's ST_CT_STRING, push a ST_CT_PTR to a fresh
                         // nul-terminated copy (native/C-ABI calls need real C strings, not
                         // storth's fat {ptr,len} strings); anything else passes through as-is
    ST_OP_NATIVE_ARG_STRING, // pop a string -> push its raw data pointer (no copy, no
                             // nul-termination), then push its length as a separate int.
                             // Backs a 'string'-typed parameter of a REAL Storth function
                             // (asm-backed, called via ST_ct_compile_native_call)
    ST_OP_NATIVE_ARG_STRING_ARRAY, // pop a struct value holding an array of strings ->
                                  // build a real, contiguous {ptr,len}-per-element buffer
                                  // in the VM's own scratch memory (matching how a real
                                  // '[]string' is laid out in memory), then push that
                                  // buffer's pointer and the element count as two separate
                                  // args
    ST_OP_SYSCALL,       // pop 7 values (syscall number, then arg0..arg5, number pushed first)
                         // -> perform a real x86-64 Linux syscall directly, push the i64 result

    ST_OP_MAKE_STRUCT,   // operand: u32 n_fields
                         // -> push a ST_CT_STRUCT holding them in declaration order
    ST_OP_GET_FIELD,     // operand: u32 field_index
    ST_OP_SET_FIELD,     // operand: u32 field_index; pop new value, pop struct -> mutate
                         // struct.fields[field_index] in place (the fields array is its own
                         // fresh allocation per ST_OP_MAKE_STRUCT, so this is safe), push the
                         // new value back (same convention as ST_OP_SET_LOCAL). Backs
                         // 'some_struct_local.field = value;'
    ST_OP_PACK_STRUCT,   // operand: u32 n_fields, then n_fields more u32s (each field's byte
                         // width)
    ST_OP_ALLOC_ZEROED, // operand: u32 byte size -> zero a fresh block from the VM's own
                        // scratch memory and push a ST_CT_PTR to it. Backs a zero-initialized
                        // local ('name : T;' with no initializer) whose address can genuinely
                        // be taken and handed to a native call that writes through it (e.g.
                        // an output parameter like pipe(2)'s int[2])
    ST_OP_PTR_ADD,      // operand: u32 elem_size; pop index (int), pop base (ptr) -> push
                        // base + index*elem_size. Backs '&arr[i]' and reading 'arr[i]' (see
                        // ST_OP_PTR_LOAD) for a zero-initialized, buffer-backed local array
    ST_OP_PTR_LOAD,     // operand: u32 encoding (tag|width<<4|is_signed<<12), same encoding
                        // as ST_OP_CAST; pop a ptr -> read that many bytes from it, push the
                        // resulting int. Backs reading 'arr[i]' as a value (as opposed to
                        // '&arr[i]', which stops at ST_OP_PTR_ADD)
    ST_OP_PTR_STORE,    // operand: u32 byte width; pop value, pop ptr -> write the value's
                        // low 'width' bytes to *ptr, push the value back (same convention as
                        // ST_OP_SET_LOCAL/ST_OP_SET_FIELD). Backs 'arr[i] = value;' for a
                        // buffer-backed array (a zero-initialized local, or an array/slice
                        // parameter
    ST_OP_STR_FROM_RAW, // pop len (int), pop ptr -> push a ST_CT_STRING viewing that same
                        // memory (no copy). Backs the 'str_from_raw(ptr, len)' intrinsic
    ST_OP_PTR_LOAD_STR,
    ST_OP_PTR_STORE_STR,
    ST_OP_MEM_COPY,

    ST_OP_CALL,          // operand: u32 entry_ip, u32 n_args. Top n_args values become the
                         // callee's first locals; pushes a frame and jumps to entry_ip
    ST_OP_COMP_ERROR,    // pop u32 count values -> abort compilation, concatenated into one diagnostic
    ST_OP_RETURN,        // operand: u32 n_values.
    ST_OP_HALT,          // no caller frame: halt with ST_CT_NIL as the result; with a caller
                         // frame: same as 'return nil' (a void function falling off its end)

    ST_OP_COUNT,
} ST_ct_op_t;

typedef struct {
    u32 start_ip;
    ST_string_t file;
} ST_ct_file_range_t;

typedef struct {
    ST_arena_t *arena; // everything below grows through this, never malloc/free

    u8 *code;
    u32 count, capacity;

    u32 *lines; // parallel to 'code', one entry per byte

    ST_ct_val_t *consts;
    u32 n_consts, cap_consts;

    // Which source file each stretch of 'code' actually came from lines[]
    // alone is ambiguous once a program's call graph spans multiple #import'd
    // files, since it's just a line number with no file attached. Appended to
    // (via ST_ct_chunk_mark_file) once per function as ST_ct_compile_program
    // compiles it, in increasing start_ip order, so a failure at some ip can
    // be mapped back to the right file with ST_ct_chunk_file_at.
    ST_ct_file_range_t *file_ranges;
    u32 n_file_ranges, cap_file_ranges;
} ST_ct_chunk_t;

void ST_ct_chunk_init(ST_arena_t *arena, ST_ct_chunk_t *c);

// Records that code from here on (starting at the chunk's current byte
// offset) came from 'file'. Cheap to call on every function compiled
void ST_ct_chunk_mark_file(ST_ct_chunk_t *c, ST_string_t file);

// Which file the instruction at byte offset 'ip' came from (the most
// recent ST_ct_chunk_mark_file call whose start_ip <= ip), or an empty
// string if nothing was ever marked.
ST_string_t ST_ct_chunk_file_at(ST_ct_chunk_t *c, u32 ip);

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
    u32 err_ip; // byte offset of the instruction that failed; look up
               // ST_ct_chunk_file_at(chunk, err_ip) for which file err_line
               // is actually in

    u8 mem[1 << 20]; // scratch memory for ST_OP_ALLOC_ZEROED (zero-init locals whose
                     // address gets taken); a real, stable, byte-addressable buffer,
                     // not a boxed ST_ct_val_t
    u32 mem_used;
} ST_ct_vm_t;

void ST_ct_vm_init(ST_ct_vm_t *vm);

ST_ct_status_t ST_ct_run(ST_ct_vm_t *vm, ST_ct_chunk_t *chunk, ST_ct_val_t *out);

// Native C calls.
void *ST_ct_lib_load(const char *path);
ST_ct_native_t ST_ct_lib_bind(void *handle, const char *sym, u32 n_args);
i64 ST_ct_call_native(ST_ct_native_t fn, i64 *args, u32 n_args);

#endif
