#include "st_comptime.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

ST_ct_val_t ST_ct_nil(void) { return (ST_ct_val_t){.kind = ST_CT_NIL}; }
ST_ct_val_t ST_ct_bool(b8 v) { return (ST_ct_val_t){.kind = ST_CT_BOOL, .b = v}; }
ST_ct_val_t ST_ct_int(i64 v) { return (ST_ct_val_t){.kind = ST_CT_INT, .i = v}; }
ST_ct_val_t ST_ct_float(f64 v) { return (ST_ct_val_t){.kind = ST_CT_FLOAT, .f = v}; }
ST_ct_val_t ST_ct_str(const char *data, u32 len) {
    return (ST_ct_val_t){.kind = ST_CT_STRING, .str = {.data = data, .len = len}};
}
ST_ct_val_t ST_ct_ptr(void *p) { return (ST_ct_val_t){.kind = ST_CT_PTR, .ptr = p}; }

b8 ST_ct_truthy(ST_ct_val_t v) {
    switch (v.kind) {
        case ST_CT_NIL: return 0;
        case ST_CT_BOOL: return v.b;
        case ST_CT_INT: return v.i != 0;
        case ST_CT_FLOAT: return v.f != 0.0;
        case ST_CT_PTR: return v.ptr != NULL;
        default: return 1;
    }
}

void ST_ct_val_print(ST_ct_val_t v) {
    switch (v.kind) {
        case ST_CT_NIL: printf("nil"); break;
        case ST_CT_BOOL: printf(v.b ? "true" : "false"); break;
        case ST_CT_INT: printf("%lld", (long long)v.i); break;
        case ST_CT_FLOAT: printf("%g", v.f); break;
        case ST_CT_STRING: printf("%.*s", (int)v.str.len, v.str.data); break;
        case ST_CT_PTR: printf("<ptr %p>", v.ptr); break;
        case ST_CT_NATIVE: printf("<native %p/%u>", v.native.fn, v.native.n_args); break;
        case ST_CT_STRUCT: printf("<struct %u field%s>", v.st.n_fields, v.st.n_fields == 1 ? "" : "s"); break;
    }
}

void ST_ct_chunk_init(ST_arena_t *arena, ST_ct_chunk_t *c) {
    memset(c, 0, sizeof(*c));
    c->arena = arena;
}

// Arena-backed grow-by-copy, same shape as ST_da_append_arena elsewhere in
// the compiler, but written out per-array here since a chunk has three
// differently-typed arrays growing off of one shared 'count' cadence
// (code/lines grow in lockstep per byte written; consts grows per constant).
#define ST_CT_ARENA_GROW(arena, arr, cap, cnt, elem_ty)                                          \
    do {                                                                                         \
        if ((cnt) + 1 > (cap)) {                                                                 \
            u32 new_cap = (cap) < 8 ? 8 : (cap) * 2;                                             \
            elem_ty *new_arr = ST_arena_push((arena), (u64)new_cap * sizeof(elem_ty));           \
            if (cnt)                                                                             \
                memcpy(new_arr, (arr), (u64)(cnt) * sizeof(elem_ty));                            \
            (arr) = new_arr;                                                                     \
            (cap) = new_cap;                                                                     \
        }                                                                                        \
    } while (0)

void ST_ct_chunk_mark_file(ST_ct_chunk_t *c, ST_string_t file) {
    if (c->n_file_ranges && ST_string_eq(c->file_ranges[c->n_file_ranges - 1].file, file))
        return;
    ST_CT_ARENA_GROW(c->arena, c->file_ranges, c->cap_file_ranges, c->n_file_ranges,
                     ST_ct_file_range_t);
    c->file_ranges[c->n_file_ranges].start_ip = c->count;
    c->file_ranges[c->n_file_ranges].file = file;
    c->n_file_ranges++;
}

ST_string_t ST_ct_chunk_file_at(ST_ct_chunk_t *c, u32 ip) {
    ST_string_t best = (ST_string_t){0};
    ST_forrange(0, c->n_file_ranges) {
        if (c->file_ranges[i].start_ip > ip)
            break;
        best = c->file_ranges[i].file;
    }
    return best;
}

static void ST_ct_write_byte(ST_ct_chunk_t *c, u8 b, u32 line) {
    // code and lines must grow together (one entry per byte), so they're
    // grown against the same capacity counter deliberately here
    if (c->count + 1 > c->capacity) {
        u32 new_cap = c->capacity < 8 ? 8 : c->capacity * 2;
        u8 *new_code = ST_arena_push(c->arena, (u64)new_cap * sizeof(u8));
        u32 *new_lines = ST_arena_push(c->arena, (u64)new_cap * sizeof(u32));
        if (c->count) {
            memcpy(new_code, c->code, c->count * sizeof(u8));
            memcpy(new_lines, c->lines, c->count * sizeof(u32));
        }
        c->code = new_code;
        c->lines = new_lines;
        c->capacity = new_cap;
    }
    c->code[c->count] = b;
    c->lines[c->count] = line;
    c->count++;
}

static void ST_ct_write_u32(ST_ct_chunk_t *c, u32 v, u32 line) {
    ST_ct_write_byte(c, (u8)(v & 0xFF), line);
    ST_ct_write_byte(c, (u8)((v >> 8) & 0xFF), line);
    ST_ct_write_byte(c, (u8)((v >> 16) & 0xFF), line);
    ST_ct_write_byte(c, (u8)((v >> 24) & 0xFF), line);
}

static void ST_ct_patch_u32(ST_ct_chunk_t *c, u32 offset, u32 v) {
    c->code[offset + 0] = (u8)(v & 0xFF);
    c->code[offset + 1] = (u8)((v >> 8) & 0xFF);
    c->code[offset + 2] = (u8)((v >> 16) & 0xFF);
    c->code[offset + 3] = (u8)((v >> 24) & 0xFF);
}

static u32 ST_ct_read_u32(ST_ct_chunk_t *c, u32 offset) {
    return (u32)c->code[offset] | ((u32)c->code[offset + 1] << 8) |
           ((u32)c->code[offset + 2] << 16) | ((u32)c->code[offset + 3] << 24);
}

u32 ST_ct_emit_op(ST_ct_chunk_t *c, ST_ct_op_t op, u32 line) {
    u32 off = c->count;
    ST_ct_write_byte(c, (u8)op, line);
    return off;
}

u32 ST_ct_emit_op_u32(ST_ct_chunk_t *c, ST_ct_op_t op, u32 operand, u32 line) {
    u32 off = c->count;
    ST_ct_write_byte(c, (u8)op, line);
    ST_ct_write_u32(c, operand, line);
    return off;
}

u32 ST_ct_emit_const(ST_ct_chunk_t *c, ST_ct_val_t v, u32 line) {
    ST_CT_ARENA_GROW(c->arena, c->consts, c->cap_consts, c->n_consts, ST_ct_val_t);
    u32 idx = c->n_consts++;
    c->consts[idx] = v;
    return ST_ct_emit_op_u32(c, ST_OP_CONST, idx, line);
}

u32 ST_ct_emit_jump(ST_ct_chunk_t *c, ST_ct_op_t jump_op, u32 line) {
    ST_ct_write_byte(c, (u8)jump_op, line);
    u32 operand_off = c->count;
    ST_ct_write_u32(c, 0, line); // placeholder, patched later
    return operand_off;
}

void ST_ct_patch_jump(ST_ct_chunk_t *c, u32 operand_offset) {
    ST_ct_patch_jump_to(c, operand_offset, c->count);
}

void ST_ct_patch_jump_to(ST_ct_chunk_t *c, u32 operand_offset, u32 target_ip) {
    i64 target = (i64)target_ip - (i64)(operand_offset + 4);
    ST_ct_patch_u32(c, operand_offset, (u32)(i32)target);
}

void ST_ct_emit_loop(ST_ct_chunk_t *c, u32 loop_start, u32 line) {
    ST_ct_write_byte(c, (u8)ST_OP_LOOP, line);
    u32 operand_off = c->count;
    i64 target = (i64)loop_start - (i64)(operand_off + 4);
    ST_ct_write_u32(c, (u32)(i32)target, line);
}

u32 ST_ct_emit_call(ST_ct_chunk_t *c, u32 n_args, u32 line) {
    ST_ct_write_byte(c, (u8)ST_OP_CALL, line);
    u32 entry_ip_off = c->count;
    ST_ct_write_u32(c, 0, line); // placeholder, patched via ST_ct_patch_call
    ST_ct_write_u32(c, n_args, line);
    return entry_ip_off;
}

void ST_ct_patch_call(ST_ct_chunk_t *c, u32 entry_ip_operand_offset, u32 entry_ip) {
    ST_ct_patch_u32(c, entry_ip_operand_offset, entry_ip);
}

void ST_ct_emit_pack_struct(ST_ct_chunk_t *c, const u32 *field_sizes, u32 n_fields, u32 line) {
    ST_ct_write_byte(c, (u8)ST_OP_PACK_STRUCT, line);
    ST_ct_write_u32(c, n_fields, line);
    ST_forrange(0, n_fields) ST_ct_write_u32(c, field_sizes[i], line);
}

void *ST_ct_lib_load(const char *path) {
    return dlopen(path, RTLD_NOW | RTLD_GLOBAL);
}

ST_ct_native_t ST_ct_lib_bind(void *handle, const char *sym, u32 n_args) {
    ST_ct_native_t n = {0};
    if (!handle)
        return n;
    n.fn = dlsym(handle, sym);
    n.n_args = n_args;
    return n;
}

i64 ST_ct_call_native(ST_ct_native_t fn, i64 *args, u32 n_args) {
    if (!fn.fn)
        return 0;
    i64 a0 = n_args > 0 ? args[0] : 0;
    i64 a1 = n_args > 1 ? args[1] : 0;
    i64 a2 = n_args > 2 ? args[2] : 0;
    i64 a3 = n_args > 3 ? args[3] : 0;
    i64 a4 = n_args > 4 ? args[4] : 0;
    i64 a5 = n_args > 5 ? args[5] : 0;

#if defined(__x86_64__)
    register i64 rdi_ __asm__("rdi") = a0;
    register i64 rsi_ __asm__("rsi") = a1;
    register i64 rdx_ __asm__("rdx") = a2;
    register i64 rcx_ __asm__("rcx") = a3;
    register i64 r8_ __asm__("r8") = a4;
    register i64 r9_ __asm__("r9") = a5;
    register i64 rax_ __asm__("rax");
    void *target = fn.fn;
    __asm__ volatile("call *%1"
                     : "=r"(rax_)
                     : "r"(target), "r"(rdi_), "r"(rsi_), "r"(rdx_), "r"(rcx_), "r"(r8_), "r"(r9_)
                     : "memory", "cc", "r10", "r11");
    return rax_;
#else
    typedef i64 (*fn6)(i64, i64, i64, i64, i64, i64);
    return ((fn6)fn.fn)(a0, a1, a2, a3, a4, a5);
#endif
}

static i64 ST_ct_do_syscall(i64 nr, i64 a0, i64 a1, i64 a2, i64 a3, i64 a4, i64 a5) {
#if defined(__x86_64__)
    i64 ret;
    register i64 r10_ __asm__("r10") = a3;
    register i64 r8_ __asm__("r8") = a4;
    register i64 r9_ __asm__("r9") = a5;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(nr), "D"(a0), "S"(a1), "d"(a2), "r"(r10_), "r"(r8_), "r"(r9_)
                     : "rcx", "r11", "memory");
    return ret;
#else
    (void)nr; (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    return -1;
#endif
}

void ST_ct_vm_init(ST_ct_vm_t *vm) {
    memset(vm, 0, sizeof(*vm));
}

static void ST_ct_push(ST_ct_vm_t *vm, ST_ct_val_t v) {
    if (vm->sp >= ST_CT_STACK_MAX) {
        vm->status = ST_CT_ERR_RUNTIME;
        snprintf(vm->err_msg, sizeof(vm->err_msg), "comptime VM stack overflow");
        return;
    }
    vm->stack[vm->sp++] = v;
}

static ST_ct_val_t ST_ct_pop(ST_ct_vm_t *vm) {
    if (vm->sp == 0) {
        vm->status = ST_CT_ERR_RUNTIME;
        snprintf(vm->err_msg, sizeof(vm->err_msg), "comptime VM stack underflow");
        return ST_ct_nil();
    }
    return vm->stack[--vm->sp];
}

static b8 ST_ct_fail(ST_ct_vm_t *vm, u32 line, const char *fmt, ...) {
    vm->status = ST_CT_ERR_RUNTIME;
    vm->err_line = line;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(vm->err_msg, sizeof(vm->err_msg), fmt, ap);
    va_end(ap);
    return 0;
}

static b8 ST_ct_as_f64(ST_ct_val_t v, f64 *out) {
    if (v.kind == ST_CT_INT) { *out = (f64)v.i; return 1; }
    if (v.kind == ST_CT_FLOAT) { *out = v.f; return 1; }
    return 0;
}

ST_ct_status_t ST_ct_run(ST_ct_vm_t *vm, ST_ct_chunk_t *chunk, ST_ct_val_t *out) {
    u32 ip = 0;
    vm->status = ST_CT_OK;

    while (ip < chunk->count) {
        u32 line = chunk->lines[ip];
        vm->err_ip = ip; // set unconditionally each instruction, so it's already
                        // correct if this one is the one that fails
        ST_ct_op_t op = (ST_ct_op_t)chunk->code[ip++];

        switch (op) {
            case ST_OP_CONST: {
                u32 idx = ST_ct_read_u32(chunk, ip); ip += 4;
                ST_ct_push(vm, chunk->consts[idx]);
                break;
            }
            case ST_OP_NIL: ST_ct_push(vm, ST_ct_nil()); break;
            case ST_OP_TRUE: ST_ct_push(vm, ST_ct_bool(1)); break;
            case ST_OP_FALSE: ST_ct_push(vm, ST_ct_bool(0)); break;
            case ST_OP_POP: ST_ct_pop(vm); break;
            case ST_OP_DUP: {
                if (vm->sp == 0) { ST_ct_fail(vm, line, "comptime: stack underflow on dup"); break; }
                ST_ct_push(vm, vm->stack[vm->sp - 1]);
                break;
            }
            case ST_OP_NIP: {
                ST_ct_val_t top = ST_ct_pop(vm);
                ST_ct_pop(vm);
                ST_ct_push(vm, top);
                break;
            }

            case ST_OP_ADD: case ST_OP_SUB: case ST_OP_MUL:
            case ST_OP_DIV: case ST_OP_MOD: {
                ST_ct_val_t b = ST_ct_pop(vm), a = ST_ct_pop(vm);
                if ((a.kind == ST_CT_PTR || b.kind == ST_CT_PTR) &&
                    (op == ST_OP_ADD || op == ST_OP_SUB)) {
                    // Raw pointer arithmetic (byte offsets), as used directly in
                    // low-level code like an allocator's 'base + HDR'/'ptr - n' --
                    // distinct from ST_OP_PTR_ADD, which is element-size-scaled
                    // and used internally for array indexing.
                    if (a.kind == ST_CT_PTR && b.kind == ST_CT_PTR) {
                        if (op == ST_OP_SUB) {
                            ST_ct_push(vm, ST_ct_int((i64)((u8 *)a.ptr - (u8 *)b.ptr)));
                            break;
                        }
                        ST_ct_fail(vm, line, "comptime: can't add two pointers");
                        break;
                    }
                    ST_ct_val_t p = a.kind == ST_CT_PTR ? a : b;
                    ST_ct_val_t n = a.kind == ST_CT_PTR ? b : a;
                    if (n.kind != ST_CT_INT) {
                        ST_ct_fail(vm, line, "comptime: pointer arithmetic needs an integer offset");
                        break;
                    }
                    if (op == ST_OP_SUB && a.kind != ST_CT_PTR) {
                        ST_ct_fail(vm, line, "comptime: can't subtract a pointer from an integer");
                        break;
                    }
                    i64 off = op == ST_OP_SUB ? -n.i : n.i;
                    ST_ct_push(vm, ST_ct_ptr((u8 *)p.ptr + off));
                    break;
                }
                if (a.kind == ST_CT_INT && b.kind == ST_CT_INT) {
                    i64 r = 0;
                    if (op == ST_OP_ADD) r = a.i + b.i;
                    else if (op == ST_OP_SUB) r = a.i - b.i;
                    else if (op == ST_OP_MUL) r = a.i * b.i;
                    else if (op == ST_OP_DIV) {
                        if (b.i == 0) { ST_ct_fail(vm, line, "comptime: division by zero"); break; }
                        r = a.i / b.i;
                    } else if (op == ST_OP_MOD) {
                        if (b.i == 0) { ST_ct_fail(vm, line, "comptime: modulo by zero"); break; }
                        r = a.i % b.i;
                    }
                    ST_ct_push(vm, ST_ct_int(r));
                } else {
                    f64 fa, fb;
                    if (!ST_ct_as_f64(a, &fa) || !ST_ct_as_f64(b, &fb)) {
                        ST_ct_fail(vm, line, "comptime: arithmetic on non-numeric value");
                        break;
                    }
                    f64 r = 0;
                    if (op == ST_OP_ADD) r = fa + fb;
                    else if (op == ST_OP_SUB) r = fa - fb;
                    else if (op == ST_OP_MUL) r = fa * fb;
                    else if (op == ST_OP_DIV) r = fa / fb;
                    else if (op == ST_OP_MOD) { ST_ct_fail(vm, line, "comptime: '%%' needs integers"); break; }
                    ST_ct_push(vm, ST_ct_float(r));
                }
                break;
            }
            case ST_OP_NEG: {
                ST_ct_val_t a = ST_ct_pop(vm);
                if (a.kind == ST_CT_INT) ST_ct_push(vm, ST_ct_int(-a.i));
                else if (a.kind == ST_CT_FLOAT) ST_ct_push(vm, ST_ct_float(-a.f));
                else ST_ct_fail(vm, line, "comptime: '-' needs a numeric operand");
                break;
            }
            case ST_OP_NOT: {
                ST_ct_val_t a = ST_ct_pop(vm);
                ST_ct_push(vm, ST_ct_bool(!ST_ct_truthy(a)));
                break;
            }

            case ST_OP_BAND: case ST_OP_BOR: case ST_OP_BXOR:
            case ST_OP_SHL: case ST_OP_SHR: {
                ST_ct_val_t b = ST_ct_pop(vm), a = ST_ct_pop(vm);
                if (a.kind != ST_CT_INT || b.kind != ST_CT_INT) {
                    ST_ct_fail(vm, line, "comptime: bitwise operators need integers");
                    break;
                }
                i64 r = 0;
                if (op == ST_OP_BAND) r = a.i & b.i;
                else if (op == ST_OP_BOR) r = a.i | b.i;
                else if (op == ST_OP_BXOR) r = a.i ^ b.i;
                else if (op == ST_OP_SHL) r = a.i << b.i;
                else if (op == ST_OP_SHR) r = a.i >> b.i;
                ST_ct_push(vm, ST_ct_int(r));
                break;
            }

            case ST_OP_EQ: case ST_OP_NEQ: case ST_OP_LT:
            case ST_OP_LE: case ST_OP_GT: case ST_OP_GE: {
                ST_ct_val_t b = ST_ct_pop(vm), a = ST_ct_pop(vm);
                b8 r = 0;
                if (op == ST_OP_EQ || op == ST_OP_NEQ) {
                    b8 eq;
                    if (a.kind == ST_CT_NIL && b.kind == ST_CT_PTR) eq = (b.ptr == NULL);
                    else if (b.kind == ST_CT_NIL && a.kind == ST_CT_PTR) eq = (a.ptr == NULL);
                    else if (a.kind != b.kind) eq = 0;
                    else switch (a.kind) {
                        case ST_CT_NIL: eq = 1; break;
                        case ST_CT_BOOL: eq = a.b == b.b; break;
                        case ST_CT_INT: eq = a.i == b.i; break;
                        case ST_CT_FLOAT: eq = a.f == b.f; break;
                        case ST_CT_STRING:
                            eq = a.str.len == b.str.len &&
                                 memcmp(a.str.data, b.str.data, a.str.len) == 0;
                            break;
                        case ST_CT_PTR: eq = a.ptr == b.ptr; break;
                        default: eq = 0; break;
                    }
                    r = (op == ST_OP_EQ) ? eq : !eq;
                } else {
                    f64 fa, fb;
                    if (!ST_ct_as_f64(a, &fa) || !ST_ct_as_f64(b, &fb)) {
                        ST_ct_fail(vm, line, "comptime: comparison needs numeric operands");
                        break;
                    }
                    if (op == ST_OP_LT) r = fa < fb;
                    else if (op == ST_OP_LE) r = fa <= fb;
                    else if (op == ST_OP_GT) r = fa > fb;
                    else r = fa >= fb;
                }
                ST_ct_push(vm, ST_ct_bool(r));
                break;
            }

            case ST_OP_STR_LEN: {
                ST_ct_val_t s = ST_ct_pop(vm);
                if (s.kind == ST_CT_STRING) { ST_ct_push(vm, ST_ct_int(s.str.len)); break; }
                if (s.kind == ST_CT_STRUCT) { ST_ct_push(vm, ST_ct_int(s.st.n_fields)); break; }
                ST_ct_fail(vm, line, "comptime: '.len' needs a string or array value");
                break;
            }

            case ST_OP_STR_INDEX: {
                ST_ct_val_t idx = ST_ct_pop(vm), s = ST_ct_pop(vm);
                if (s.kind != ST_CT_STRING || idx.kind != ST_CT_INT) {
                    ST_ct_fail(vm, line, "comptime: string index needs (string, int)");
                    break;
                }
                if (idx.i < 0 || (u32)idx.i >= s.str.len) {
                    ST_ct_fail(vm, line, "comptime: string index %lld out of bounds (len %u)",
                               (long long)idx.i, s.str.len);
                    break;
                }
                ST_ct_push(vm, ST_ct_int((u8)s.str.data[idx.i]));
                break;
            }

	    case ST_OP_STR_PTR: {
		ST_ct_val_t s = ST_ct_pop(vm);
		if (s.kind != ST_CT_STRING) {
		    ST_ct_fail(vm, line, "comptime: '.ptr' needs a string");
                    break;
		}
                ST_ct_push(vm, ST_ct_ptr((void *)s.str.data));
		break;
	    }

            case ST_OP_STRUCT_INDEX: {
                ST_ct_val_t idx = ST_ct_pop(vm), s = ST_ct_pop(vm);
                if (s.kind != ST_CT_STRUCT || idx.kind != ST_CT_INT) {
                    ST_ct_fail(vm, line, "comptime: array index needs (array, int)");
                    break;
                }
                if (idx.i < 0 || (u32)idx.i >= s.st.n_fields) {
                    ST_ct_fail(vm, line, "comptime: array index %lld out of bounds (len %u)",
                               (long long)idx.i, s.st.n_fields);
                    break;
                }
                ST_ct_push(vm, s.st.fields[idx.i]);
                break;
            }

            case ST_OP_CAST: {
                u32 operand = ST_ct_read_u32(chunk, ip); ip += 4;
                u32 tag = operand & 0xF;
                u32 width = (operand >> 4) & 0xFF;
                b8 is_signed = (operand >> 12) & 1;
                ST_ct_val_t v = ST_ct_pop(vm);
                if (tag == 0) {
                    i64 iv;
                    if (v.kind == ST_CT_INT) iv = v.i;
                    else if (v.kind == ST_CT_FLOAT) iv = (i64)v.f;
                    else if (v.kind == ST_CT_BOOL) iv = v.b ? 1 : 0;
                    else if (v.kind == ST_CT_PTR) iv = (i64)(intptr_t)v.ptr;
                    else { ST_ct_fail(vm, line, "comptime: can't cast this value to an integer"); break; }
                    if (width > 0 && width < 8) {
                        u64 mask = ((u64)1 << (width * 8)) - 1;
                        u64 uv = (u64)iv & mask;
                        if (is_signed) {
                            u64 sign_bit = (u64)1 << (width * 8 - 1);
                            if (uv & sign_bit)
                                uv |= ~mask;
                        }
                        iv = (i64)uv;
                    }
                    ST_ct_push(vm, ST_ct_int(iv));
                } else if (tag == 1) {
                    f64 fv;
                    if (v.kind == ST_CT_INT) fv = (f64)v.i;
                    else if (v.kind == ST_CT_FLOAT) fv = v.f;
                    else { ST_ct_fail(vm, line, "comptime: can't cast this value to a float"); break; }
                    ST_ct_push(vm, ST_ct_float(fv));
                } else if (tag == 2) {
                    ST_ct_push(vm, ST_ct_bool(ST_ct_truthy(v)));
                } else if (tag == 3) {
                    if (v.kind == ST_CT_PTR) { ST_ct_push(vm, v); break; }
                    if (v.kind == ST_CT_INT) { ST_ct_push(vm, ST_ct_ptr((void *)(intptr_t)v.i)); break; }
                    ST_ct_fail(vm, line, "comptime: can't cast this value to a pointer");
                } else {
                    ST_ct_fail(vm, line, "comptime: unsupported cast");
                }
                break;
            }

            case ST_OP_GET_LOCAL: {
                u32 slot = ST_ct_read_u32(chunk, ip); ip += 4;
                u32 abs = vm->base + slot;
                if (abs >= vm->sp) { ST_ct_fail(vm, line, "comptime: bad local slot"); break; }
                ST_ct_push(vm, vm->stack[abs]);
                break;
            }
            case ST_OP_SET_LOCAL: {
                u32 slot = ST_ct_read_u32(chunk, ip); ip += 4;
                u32 abs = vm->base + slot;
                if (abs >= vm->sp) { ST_ct_fail(vm, line, "comptime: bad local slot"); break; }
                vm->stack[abs] = vm->stack[vm->sp - 1];
                break;
            }

            case ST_OP_JMP: {
                i32 off = (i32)ST_ct_read_u32(chunk, ip);
                ip = (u32)((i64)ip + 4 + off);
                break;
            }
            case ST_OP_JMP_IF_FALSE: {
                i32 off = (i32)ST_ct_read_u32(chunk, ip);
                ST_ct_val_t cond = ST_ct_pop(vm);
                u32 next_ip = ip + 4;
                ip = ST_ct_truthy(cond) ? next_ip : (u32)((i64)next_ip + off);
                break;
            }
            case ST_OP_LOOP: {
                i32 off = (i32)ST_ct_read_u32(chunk, ip);
                ip = (u32)((i64)ip + 4 + off);
                break;
            }

            case ST_OP_LOAD_LIB: {
                ST_ct_val_t path = ST_ct_pop(vm);
                if (path.kind != ST_CT_STRING) { ST_ct_fail(vm, line, "comptime: lib path must be a string"); break; }
                void *h;
                if (path.str.len == 0) {
                    h = ST_ct_lib_load(NULL);
                } else {
                    char buf[512];
                    u32 n = path.str.len < sizeof(buf) - 1 ? path.str.len : (u32)sizeof(buf) - 1;
                    memcpy(buf, path.str.data, n);
                    buf[n] = 0;
                    h = ST_ct_lib_load(buf);
                }
                ST_ct_push(vm, h ? ST_ct_ptr(h) : ST_ct_nil());
                break;
            }
            case ST_OP_BIND_SYM: {
                ST_ct_val_t arity = ST_ct_pop(vm), name = ST_ct_pop(vm), handle = ST_ct_pop(vm);
                if (handle.kind != ST_CT_PTR || name.kind != ST_CT_STRING || arity.kind != ST_CT_INT) {
                    ST_ct_fail(vm, line, "comptime: bad #comptime_load arguments");
                    break;
                }
                char buf[256];
                u32 n = name.str.len < sizeof(buf) - 1 ? name.str.len : (u32)sizeof(buf) - 1;
                memcpy(buf, name.str.data, n);
                buf[n] = 0;
                ST_ct_native_t nat = ST_ct_lib_bind(handle.ptr, buf, (u32)arity.i);
                if (!nat.fn) {
                    ST_ct_push(vm, ST_ct_nil());
                    break;
                }
                ST_ct_push(vm, (ST_ct_val_t){.kind = ST_CT_NATIVE, .native = nat});
                break;
            }
            case ST_OP_BIND_DATA_SYM: {
                ST_ct_val_t name = ST_ct_pop(vm), handle = ST_ct_pop(vm);
                if (handle.kind != ST_CT_PTR || name.kind != ST_CT_STRING) {
                    ST_ct_fail(vm, line, "comptime: bad extern-variable symbol lookup arguments");
                    break;
                }
                char buf[256];
                u32 n = name.str.len < sizeof(buf) - 1 ? name.str.len : (u32)sizeof(buf) - 1;
                memcpy(buf, name.str.data, n);
                buf[n] = 0;
                void *addr = dlsym(handle.ptr, buf);
                ST_ct_push(vm, addr ? ST_ct_ptr(addr) : ST_ct_nil());
                break;
            }
            case ST_OP_CALL_NATIVE: {
                ST_ct_val_t fnv = ST_ct_pop(vm);
                if (fnv.kind != ST_CT_NATIVE) { ST_ct_fail(vm, line, "comptime: call target isn't native"); break; }
                i64 args[8] = {0};
                u32 n = fnv.native.n_args > 8 ? 8 : fnv.native.n_args;
                for (i64 k = (i64)n - 1; k >= 0; k--) {
                    ST_ct_val_t a = ST_ct_pop(vm);
                    args[k] = a.kind == ST_CT_INT ? a.i : (i64)(intptr_t)a.ptr;
                }
                i64 r = ST_ct_call_native(fnv.native, args, n);
                ST_ct_push(vm, ST_ct_int(r));
                break;
            }

            case ST_OP_NATIVE_ARG: {
                ST_ct_val_t v = ST_ct_pop(vm);
                if (v.kind == ST_CT_STRING) {
                    char *buf = malloc((size_t)v.str.len + 1);
                    if (!buf) { ST_ct_fail(vm, line, "comptime: out of memory"); break; }
                    memcpy(buf, v.str.data, v.str.len);
                    buf[v.str.len] = 0;
                    ST_ct_push(vm, ST_ct_ptr(buf));
                } else if (v.kind == ST_CT_STRUCT) {
                    ST_ct_fail(vm, line,
                              "comptime: passing a struct by value to an extern function "
                              "isn't supported yet");
                } else {
                    ST_ct_push(vm, v);
                }
                break;
            }

            case ST_OP_NATIVE_ARG_STRING: {
                ST_ct_val_t v = ST_ct_pop(vm);
                if (v.kind != ST_CT_STRING) {
                    ST_ct_fail(vm, line, "comptime: expected a string here");
                    break;
                }
                ST_ct_push(vm, ST_ct_ptr((void *)v.str.data));
                ST_ct_push(vm, ST_ct_int((i64)v.str.len));
                break;
            }

            case ST_OP_NATIVE_ARG_STRING_ARRAY: {
                ST_ct_val_t v = ST_ct_pop(vm);
                if (v.kind != ST_CT_STRUCT) {
                    ST_ct_fail(vm, line, "comptime: expected an array of strings here");
                    break;
                }
                u32 n = v.st.n_fields;
                u32 size = n * 16;
                if ((u64)vm->mem_used + size > sizeof(vm->mem)) {
                    ST_ct_fail(vm, line, "comptime: out of comptime scratch memory");
                    break;
                }
                u8 *buf = vm->mem + vm->mem_used;
                vm->mem_used += size;
                b8 bad = 0;
                for (u32 k = 0; k < n; k++) {
                    ST_ct_val_t sv = v.st.fields[k];
                    if (sv.kind != ST_CT_STRING) {
                        bad = 1;
                        break;
                    }
                    const void *ptr = sv.str.data;
                    i64 len = (i64)sv.str.len;
                    memcpy(buf + (u64)k * 16, &ptr, 8);
                    memcpy(buf + (u64)k * 16 + 8, &len, 8);
                }
                if (bad) {
                    ST_ct_fail(vm, line, "comptime: array element isn't a string");
                    break;
                }
                ST_ct_push(vm, ST_ct_ptr(buf));
                ST_ct_push(vm, ST_ct_int((i64)n));
                break;
            }

            case ST_OP_SYSCALL: {
                i64 vals[7];
                for (i64 k = 6; k >= 0; k--) {
                    ST_ct_val_t v = ST_ct_pop(vm);
                    vals[k] = v.kind == ST_CT_INT ? v.i : (i64)(intptr_t)v.ptr;
                }
                i64 r = ST_ct_do_syscall(vals[0], vals[1], vals[2], vals[3], vals[4], vals[5],
                                         vals[6]);
                ST_ct_push(vm, ST_ct_int(r));
                break;
            }

            case ST_OP_MAKE_STRUCT: {
                u32 n = ST_ct_read_u32(chunk, ip); ip += 4;
                ST_ct_val_t *fields = n ? malloc(sizeof(ST_ct_val_t) * n) : NULL;
                if (n && !fields) { ST_ct_fail(vm, line, "comptime: out of memory"); break; }
                for (i64 k = (i64)n - 1; k >= 0; k--)
                    fields[k] = ST_ct_pop(vm);
                ST_ct_val_t v;
                v.kind = ST_CT_STRUCT;
                v.st.fields = fields;
                v.st.n_fields = n;
                ST_ct_push(vm, v);
                break;
            }

            case ST_OP_GET_FIELD: {
                u32 idx = ST_ct_read_u32(chunk, ip); ip += 4;
                ST_ct_val_t s = ST_ct_pop(vm);
                if (s.kind != ST_CT_STRUCT) {
                    ST_ct_fail(vm, line, "comptime: field access on a non-struct value");
                    break;
                }
                if (idx >= s.st.n_fields) {
                    ST_ct_fail(vm, line, "comptime: struct field index out of range");
                    break;
                }
                ST_ct_push(vm, s.st.fields[idx]);
                break;
            }

            case ST_OP_SET_FIELD: {
                u32 idx = ST_ct_read_u32(chunk, ip); ip += 4;
                ST_ct_val_t v = ST_ct_pop(vm);
                ST_ct_val_t s = ST_ct_pop(vm);
                if (s.kind != ST_CT_STRUCT) {
                    ST_ct_fail(vm, line, "comptime: field assignment on a non-struct value");
                    break;
                }
                if (idx >= s.st.n_fields) {
                    ST_ct_fail(vm, line, "comptime: struct field index out of range");
                    break;
                }
                s.st.fields[idx] = v;
                ST_ct_push(vm, v);
                break;
            }

            case ST_OP_PACK_STRUCT: {
                u32 n = ST_ct_read_u32(chunk, ip); ip += 4;
                u32 sizes[64];
                for (u32 k = 0; k < n; k++) {
                    u32 sz = ST_ct_read_u32(chunk, ip); ip += 4;
                    if (k < 64) sizes[k] = sz;
                }
                ST_ct_val_t s = ST_ct_pop(vm);
                if (s.kind != ST_CT_STRUCT) {
                    ST_ct_fail(vm, line, "comptime: expected a struct to pack for a native call");
                    break;
                }
                u32 nn = n > 64 ? 64 : n;
                u32 total = 0;
                for (u32 k = 0; k < nn; k++) total += sizes[k];
                if (total > 8) {
                    ST_ct_fail(vm, line,
                              "comptime: structs bigger than 8 bytes can't be passed by "
                              "value to an extern function yet");
                    break;
                }
                u64 packed = 0;
                u32 offset = 0;
                for (u32 k = 0; k < nn && k < s.st.n_fields; k++) {
                    u64 raw = 0;
                    ST_ct_val_t fv = s.st.fields[k];
                    if (fv.kind == ST_CT_INT) raw = (u64)fv.i;
                    else if (fv.kind == ST_CT_BOOL) raw = fv.b ? 1u : 0u;
                    else if (fv.kind == ST_CT_PTR) raw = (u64)(uintptr_t)fv.ptr;
                    u64 mask = sizes[k] >= 8 ? ~(u64)0 : (((u64)1 << (sizes[k] * 8)) - 1);
                    packed |= (raw & mask) << (offset * 8);
                    offset += sizes[k];
                }
                ST_ct_push(vm, ST_ct_int((i64)packed));
                break;
            }

            case ST_OP_ALLOC_ZEROED: {
                u32 size = ST_ct_read_u32(chunk, ip); ip += 4;
                if ((u64)vm->mem_used + size > sizeof(vm->mem)) {
                    ST_ct_fail(vm, line,
                              "comptime: out of comptime scratch memory (used for "
                              "zero-initialized locals)");
                    break;
                }
                void *p = vm->mem + vm->mem_used;
                memset(p, 0, size);
                vm->mem_used += size;
                ST_ct_push(vm, ST_ct_ptr(p));
                break;
            }

            case ST_OP_PTR_ADD: {
                u32 elem_size = ST_ct_read_u32(chunk, ip); ip += 4;
                ST_ct_val_t idx = ST_ct_pop(vm), base = ST_ct_pop(vm);
                if (base.kind != ST_CT_PTR || idx.kind != ST_CT_INT) {
                    ST_ct_fail(vm, line, "comptime: array indexing needs (pointer, int)");
                    break;
                }
                ST_ct_push(vm, ST_ct_ptr((u8 *)base.ptr + idx.i * (i64)elem_size));
                break;
            }

            case ST_OP_PTR_LOAD: {
                u32 operand = ST_ct_read_u32(chunk, ip); ip += 4;
                u32 tag = operand & 0xF;
                u32 width = (operand >> 4) & 0xFF;
                b8 is_signed = (operand >> 12) & 1;
                ST_ct_val_t p = ST_ct_pop(vm);
                if (p.kind != ST_CT_PTR) {
                    ST_ct_fail(vm, line, "comptime: can't load through a non-pointer value");
                    break;
                }
                if (tag == 3) {
                    void *raw_ptr;
                    memcpy(&raw_ptr, p.ptr, sizeof(void *));
                    ST_ct_push(vm, ST_ct_ptr(raw_ptr));
                    break;
                }
                u64 raw = 0;
                memcpy(&raw, p.ptr, width > 8 ? 8 : width);
                i64 iv;
                if (width > 0 && width < 8) {
                    u64 mask = ((u64)1 << (width * 8)) - 1;
                    raw &= mask;
                    if (is_signed) {
                        u64 sign_bit = (u64)1 << (width * 8 - 1);
                        if (raw & sign_bit)
                            raw |= ~mask;
                    }
                }
                iv = (i64)raw;
                ST_ct_push(vm, ST_ct_int(iv));
                break;
            }

            case ST_OP_PTR_STORE: {
                u32 width = ST_ct_read_u32(chunk, ip); ip += 4;
                ST_ct_val_t v = ST_ct_pop(vm), p = ST_ct_pop(vm);
                if (p.kind != ST_CT_PTR) {
                    ST_ct_fail(vm, line, "comptime: can't store through a non-pointer value");
                    break;
                }
                if (v.kind == ST_CT_PTR) {
                    memcpy(p.ptr, &v.ptr, sizeof(void *));
                    ST_ct_push(vm, v);
                    break;
                }
                i64 iv = 0;
                if (v.kind == ST_CT_INT) iv = v.i;
                else if (v.kind == ST_CT_BOOL) iv = v.b ? 1 : 0;
                memcpy(p.ptr, &iv, width > 8 ? 8 : width);
                ST_ct_push(vm, v);
                break;
            }

            case ST_OP_PTR_LOAD_STR: {
                ST_ct_val_t p = ST_ct_pop(vm);
                if (p.kind != ST_CT_PTR) {
                    ST_ct_fail(vm, line, "comptime: can't load through a non-pointer value");
                    break;
                }
                const char *data;
                u64 len;
                memcpy(&data, p.ptr, sizeof(data));
                memcpy(&len, (u8 *)p.ptr + sizeof(data), sizeof(len));
                ST_ct_push(vm, ST_ct_str(data, (u32)len));
                break;
            }

            case ST_OP_PTR_STORE_STR: {
                ST_ct_val_t v = ST_ct_pop(vm), p = ST_ct_pop(vm);
                if (p.kind != ST_CT_PTR) {
                    ST_ct_fail(vm, line, "comptime: can't store through a non-pointer value");
                    break;
                }
                if (v.kind != ST_CT_STRING) {
                    ST_ct_fail(vm, line, "comptime: expected a string value to store here");
                    break;
                }
                u64 len = v.str.len;
                memcpy(p.ptr, &v.str.data, sizeof(v.str.data));
                memcpy((u8 *)p.ptr + sizeof(v.str.data), &len, sizeof(len));
                ST_ct_push(vm, v);
                break;
            }

            case ST_OP_MEM_COPY: {
                u32 size = ST_ct_read_u32(chunk, ip); ip += 4;
                ST_ct_val_t src = ST_ct_pop(vm), dst = ST_ct_pop(vm);
                if (dst.kind != ST_CT_PTR || src.kind != ST_CT_PTR) {
                    ST_ct_fail(vm, line, "comptime: mem copy needs (pointer, pointer)");
                    break;
                }
                memcpy(dst.ptr, src.ptr, size);
                ST_ct_push(vm, dst);
                break;
            }

            case ST_OP_STR_FROM_RAW: {
                ST_ct_val_t len = ST_ct_pop(vm), p = ST_ct_pop(vm);
                if (p.kind != ST_CT_PTR || len.kind != ST_CT_INT) {
                    ST_ct_fail(vm, line, "comptime: str_from_raw needs (pointer, int)");
                    break;
                }
                if (len.i < 0) {
                    ST_ct_fail(vm, line, "comptime: str_from_raw got a negative length");
                    break;
                }
                ST_ct_push(vm, ST_ct_str((const char *)p.ptr, (u32)len.i));
                break;
            }

            case ST_OP_COMP_ERROR: {
                u32 nparts = ST_ct_read_u32(chunk, ip); ip += 4;
                char msg[512] = {0};
                u32 used = 0;
                ST_ct_val_t parts[16];
                for (u32 k = 0; k < nparts && k < 16; k++)
                    parts[nparts - 1 - k] = ST_ct_pop(vm);
                for (u32 k = 0; k < nparts && k < 16; k++) {
                    char piece[128];
                    int n;
                    switch (parts[k].kind) {
                        case ST_CT_STRING:
                            n = snprintf(piece, sizeof(piece), "%.*s", (int)parts[k].str.len, parts[k].str.data);
                            break;
                        case ST_CT_INT: n = snprintf(piece, sizeof(piece), "%lld", (long long)parts[k].i); break;
                        case ST_CT_FLOAT: n = snprintf(piece, sizeof(piece), "%g", parts[k].f); break;
                        default: n = snprintf(piece, sizeof(piece), "<val>"); break;
                    }
                    if (n > 0 && used + (u32)n < sizeof(msg)) {
                        memcpy(msg + used, piece, (u32)n);
                        used += (u32)n;
                    }
                }
                vm->status = ST_CT_ERR_COMPTIME;
                vm->err_line = line;
                memcpy(vm->err_msg, msg, used < sizeof(vm->err_msg) ? used : sizeof(vm->err_msg) - 1);
                return vm->status;
            }

            case ST_OP_CALL: {
                u32 entry_ip = ST_ct_read_u32(chunk, ip); ip += 4;
                u32 n_args = ST_ct_read_u32(chunk, ip); ip += 4;
                if (n_args > vm->sp) { ST_ct_fail(vm, line, "comptime: bad call argument count"); break; }
                if (vm->n_frames >= ST_CT_FRAMES_MAX) {
                    ST_ct_fail(vm, line, "comptime: call stack too deep (max %u; likely runaway "
                                        "recursion)", (u32)ST_CT_FRAMES_MAX);
                    break;
                }
                vm->frames[vm->n_frames].return_ip = ip;
                vm->frames[vm->n_frames].saved_base = vm->base;
                vm->n_frames++;
                vm->base = vm->sp - n_args;
                ip = entry_ip;
                break;
            }

            case ST_OP_RETURN: {
                u32 n = ST_ct_read_u32(chunk, ip); ip += 4;
                if (n > 8) { ST_ct_fail(vm, line, "comptime: too many return values"); break; }
                ST_ct_val_t vals[8];
                for (i64 k = (i64)n - 1; k >= 0; k--)
                    vals[k] = ST_ct_pop(vm);
                if (vm->n_frames == 0) {
                    if (out) *out = n > 0 ? vals[0] : ST_ct_nil();
                    return vm->status == ST_CT_OK ? ST_CT_OK : vm->status;
                }
                vm->n_frames--;
                vm->sp = vm->base;
                vm->base = vm->frames[vm->n_frames].saved_base;
                ip = vm->frames[vm->n_frames].return_ip;
                ST_forrange(0, n) ST_ct_push(vm, vals[i]);
                break;
            }

            case ST_OP_HALT:
                if (vm->n_frames == 0) {
                    if (out) *out = ST_ct_nil();
                    return vm->status == ST_CT_OK ? ST_CT_OK : vm->status;
                }
                vm->n_frames--;
                vm->sp = vm->base;
                vm->base = vm->frames[vm->n_frames].saved_base;
                ip = vm->frames[vm->n_frames].return_ip;
                ST_ct_push(vm, ST_ct_nil());
                break;

            default:
                ST_ct_fail(vm, line, "comptime: unknown opcode %d", (int)op);
                break;
        }

        if (vm->status != ST_CT_OK)
            return vm->status;
    }

    if (out) *out = ST_ct_nil();
    return ST_CT_OK;
}
