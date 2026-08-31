#include "st_comptime_compile.h"

#include "st_comptime_native.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static b8 ST_ct_expr_has_asm(ST_expr_t *e) {
    if (!e)
        return 0;
    switch (e->kind) {
        case ST_EX_ASM:
            return 1;
        case ST_EX_UNARY:
            return ST_ct_expr_has_asm(e->unary.operand);
        case ST_EX_BINARY:
            return ST_ct_expr_has_asm(e->bin.l) || ST_ct_expr_has_asm(e->bin.r);
        case ST_EX_CALL: {
            if (ST_ct_expr_has_asm(e->call.callee))
                return 1;
            ST_forrange(0, e->call.args.count)
                if (ST_ct_expr_has_asm(e->call.args.items[i].value)) return 1;
            return 0;
        }
        case ST_EX_FIELD:
            return ST_ct_expr_has_asm(e->field.base);
        case ST_EX_INDEX:
            return ST_ct_expr_has_asm(e->index.base) || ST_ct_expr_has_asm(e->index.index);
        case ST_EX_CAST:
            return ST_ct_expr_has_asm(e->cast.operand);
        case ST_EX_STRUCT_LIT: {
            ST_forrange(0, e->struct_lit.inits.count)
                if (ST_ct_expr_has_asm(e->struct_lit.inits.items[i].value)) return 1;
            return 0;
        }
        case ST_EX_SIZEOF:
        case ST_EX_TYPEOF:
        case ST_EX_KIND:
        case ST_EX_CSTR:
        case ST_EX_FIELDS:
            return ST_ct_expr_has_asm(e->tyop.operand);
        case ST_EX_COMP_ERROR: {
            ST_forrange(0, e->comp_error.args.count)
                if (ST_ct_expr_has_asm(e->comp_error.args.items[i])) return 1;
            return 0;
        }
        case ST_EX_STR_FROM_RAW:
            return ST_ct_expr_has_asm(e->str_from_raw.ptr) || ST_ct_expr_has_asm(e->str_from_raw.len);
        default:
            return 0;
    }
}

static b8 ST_ct_stmts_have_asm(ST_stmts_t *body);

static b8 ST_ct_stmt_has_asm(ST_stmt_t *s) {
    if (!s)
        return 0;
    switch (s->kind) {
        case ST_ST_ASM:
            return 1;
        case ST_ST_EXPR:
            return ST_ct_expr_has_asm(s->expr);
        case ST_ST_DECL:
            return ST_ct_expr_has_asm(s->decl.init);
        case ST_ST_ASSIGN:
            return ST_ct_expr_has_asm(s->assign.lhs) || ST_ct_expr_has_asm(s->assign.rhs);
        case ST_ST_MULTI_BIND: {
            ST_forrange(0, s->multi.values.count)
                if (ST_ct_expr_has_asm(s->multi.values.items[i])) return 1;
            return 0;
        }
        case ST_ST_IF:
            return ST_ct_expr_has_asm(s->if_.cond) || ST_ct_stmts_have_asm(&s->if_.then_body) ||
                   ST_ct_stmt_has_asm(s->if_.else_stmt);
        case ST_ST_SWITCH: {
            if (ST_ct_expr_has_asm(s->switch_.cond))
                return 1;
            ST_forrange(0, s->switch_.cases.count) {
                ST_case_t *c = &s->switch_.cases.items[i];
                b8 case_has_asm = 0;
                ST_forrange(0, c->values.count)
                    if (ST_ct_expr_has_asm(c->values.items[i])) case_has_asm = 1;
                if (case_has_asm || ST_ct_stmts_have_asm(&c->body))
                    return 1;
            }
            return 0;
        }
        case ST_ST_WHILE:
            return ST_ct_expr_has_asm(s->while_.cond) || ST_ct_stmts_have_asm(&s->while_.body);
        case ST_ST_FOR_RANGE:
            return ST_ct_expr_has_asm(s->for_range.lo) || ST_ct_expr_has_asm(s->for_range.hi) ||
                   ST_ct_stmts_have_asm(&s->for_range.body);
        case ST_ST_FOR_ARRAY:
            return ST_ct_expr_has_asm(s->for_array.target) ||
                   ST_ct_stmts_have_asm(&s->for_array.body);
        case ST_ST_RETURN: {
            ST_forrange(0, s->ret.values.count)
                if (ST_ct_expr_has_asm(s->ret.values.items[i])) return 1;
            return 0;
        }
        case ST_ST_BLOCK:
            return ST_ct_stmts_have_asm(&s->block);
        case ST_ST_DEFER:
            return ST_ct_stmt_has_asm(s->defer_stmt);
        default:
            return 0;
    }
}

static b8 ST_ct_stmts_have_asm(ST_stmts_t *body) {
    ST_forrange(0, body->count)
        if (ST_ct_stmt_has_asm(body->items[i])) return 1;
    return 0;
}

b8 ST_ct_decl_needs_native(ST_decl_t *d) {
    if (!d || d->kind != ST_DE_FN)
        return 0;
    return ST_ct_stmts_have_asm(&d->fn.body);
}

void ST_ct_compiler_init(ST_ct_compiler_t *cc, ST_arena_t *arena, ST_ct_chunk_t *chunk) {
    memset(cc, 0, sizeof(*cc));
    cc->arena = arena;
    cc->chunk = chunk;
}

typedef enum {
    ST_CT_LVAL_SCALAR,
    ST_CT_LVAL_PTR,
    ST_CT_LVAL_STRING,
    ST_CT_LVAL_STRUCT,
    ST_CT_LVAL_ARRAY,
} ST_ct_lval_kind_t;

typedef struct {
    ST_ct_lval_kind_t kind;
    ST_decl_t *struct_decl;   // valid when kind == ST_CT_LVAL_STRUCT
    ST_tyexpr_t *te;         // element/field type; valid for SCALAR, ARRAY (element te), PTR
    u32 elem_size;           // valid for ARRAY when te == NULL (a plain int-array local),
                             // and for SCALAR when te == NULL (that array's element width)
    b8 elem_is_signed;
} ST_ct_lval_ty_t;

static b8 ST_ct_prog_want_call(ST_ct_prog_ctx_t *pctx, ST_string_t callee_name, u32 patch_off,
                               u32 line, u32 col);
static ST_decl_t *ST_ct_prog_find_extern(ST_ct_prog_ctx_t *pctx, ST_string_t name);
static ST_decl_t *ST_ct_prog_find_struct(ST_ct_prog_ctx_t *pctx, ST_string_t name);
static ST_decl_t *ST_ct_prog_find_type_alias(ST_ct_prog_ctx_t *pctx, ST_string_t name);
static ST_decl_t *ST_ct_prog_find_enum(ST_ct_prog_ctx_t *pctx, ST_string_t name);
static ST_decl_t *ST_ct_prog_find_const(ST_ct_prog_ctx_t *pctx, ST_string_t name);
static ST_decl_t *ST_ct_prog_find_decl(ST_ct_prog_ctx_t *pctx, ST_string_t name, b8 *not_callable);
static i32 ST_ct_struct_field_index(ST_decl_t *struct_decl, ST_string_t field_name);
static u32 ST_ct_field_byte_size(ST_tyexpr_t *te);
static b8 ST_ct_tyexpr_byte_size(ST_ct_compiler_t *cc, ST_tyexpr_t *te, u32 *out_size);
static b8 ST_ct_tyexpr_int_info(ST_tyexpr_t *te, u32 *width, b8 *is_signed);
static b8 ST_ct_tyexpr_is_int_slice(ST_tyexpr_t *te, u32 *width, b8 *is_signed);
static b8 ST_ct_ty_int_info(ST_ty_t *t, u32 *width, b8 *is_signed);
static ST_string_t ST_ct_expr_struct_type(ST_ct_compiler_t *cc, ST_expr_t *e);
static ST_decl_t *ST_ct_compile_plain_call(ST_ct_compiler_t *cc, ST_expr_t *call_expr);
static b8 ST_ct_struct_total_size(ST_ct_compiler_t *cc, ST_decl_t *sd, u32 *out_size);
static b8 ST_ct_struct_field_offset(ST_ct_compiler_t *cc, ST_decl_t *sd, ST_string_t field_name,
                                    u32 *out_offset, ST_tyexpr_t **out_te);
static b8 ST_ct_lval_addr(ST_ct_compiler_t *cc, ST_expr_t *e, ST_ct_lval_ty_t *out_ty);
static b8 ST_ct_lval_classify_te(ST_ct_compiler_t *cc, ST_tyexpr_t *te, ST_ct_lval_ty_t *out_ty);
static void ST_ct_emit_lval_load(ST_ct_compiler_t *cc, ST_ct_lval_ty_t *ty, u32 line);
static b8 ST_ct_emit_lval_store(ST_ct_compiler_t *cc, ST_ct_lval_ty_t *ty, u32 line);
static b8 ST_ct_compile_struct_rvalue(ST_ct_compiler_t *cc, ST_expr_t *e, ST_string_t struct_type);
static ST_decl_t *ST_ct_compile_native_call(ST_ct_compiler_t *cc, ST_expr_t *call_expr,
                                            ST_decl_t *callee_decl);
static b8 ST_ct_compile_syscall_asm(ST_ct_compiler_t *cc, ST_token_t *tokens, u32 n_tokens,
                                    u32 line, u32 col);

static void ST_ct_cfail(ST_ct_compiler_t *cc, u32 line, u32 col, const char *fmt, ...) {
    if (cc->failed)
        return;
    cc->failed = 1;
    cc->err_line = line;
    cc->err_col = col;
    cc->err_file = cc->cur_file;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cc->err_msg, sizeof(cc->err_msg), fmt, ap);
    va_end(ap);
}

static i32 ST_ct_find_local(ST_ct_compiler_t *cc, ST_string_t name) {
    // last-declared-wins, same as normal lexical shadowing
    for (i32 k = (i32)cc->n_locals - 1; k >= 0; k--)
        if (ST_string_eq(cc->locals[k].name, name))
            return (i32)cc->locals[k].slot;
    return -1;
}

static u32 ST_ct_declare_local(ST_ct_compiler_t *cc, ST_string_t name) {
    // Slot == stack position at declaration time. This only stays correct
    // because every path that declares a local pushes exactly one value
    // for it (see ST_ct_compile_expr below) and scope exit unwinds exactly
    // as many POPs as locals it introduced (see ST_ct_compile_scoped)
    if (cc->n_locals >= (u32)(sizeof(cc->locals) / sizeof(cc->locals[0]))) {
        ST_ct_cfail(cc, 0, 0, "comptime: too many locals in one #comptime scope (max %zu)",
                    sizeof(cc->locals) / sizeof(cc->locals[0]));
        return 0;
    }
    u32 slot = cc->n_locals; // stack currently holds exactly n_locals live comptime locals
    cc->locals[cc->n_locals].name = name;
    cc->locals[cc->n_locals].slot = slot;
    cc->locals[cc->n_locals].struct_type = (ST_string_t){0};
    cc->locals[cc->n_locals].is_buffer = 0;
    cc->locals[cc->n_locals].has_elem_info = 0;
    cc->locals[cc->n_locals].elem_size = 0;
    cc->locals[cc->n_locals].elem_is_signed = 0;
    cc->locals[cc->n_locals].has_known_len = 0;
    cc->locals[cc->n_locals].known_len = 0;
    cc->locals[cc->n_locals].len_slot = -1;
    cc->n_locals++;
    return slot;
}

static void ST_ct_local_set_struct_type(ST_ct_compiler_t *cc, ST_string_t struct_type) {
    if (cc->n_locals > 0)
        cc->locals[cc->n_locals - 1].struct_type = struct_type;
}

static void ST_ct_local_set_is_buffer(ST_ct_compiler_t *cc) {
    if (cc->n_locals > 0)
        cc->locals[cc->n_locals - 1].is_buffer = 1;
}

static void ST_ct_local_set_elem_info(ST_ct_compiler_t *cc, u32 size, b8 is_signed) {
    if (cc->n_locals > 0) {
        cc->locals[cc->n_locals - 1].has_elem_info = 1;
        cc->locals[cc->n_locals - 1].elem_size = size;
        cc->locals[cc->n_locals - 1].elem_is_signed = is_signed;
    }
}

static void ST_ct_local_set_known_len(ST_ct_compiler_t *cc, u32 len) {
    if (cc->n_locals > 0) {
        cc->locals[cc->n_locals - 1].has_known_len = 1;
        cc->locals[cc->n_locals - 1].known_len = len;
    }
}

// Returns NULL if 'name' isn't a known local at all (distinct from "not
// buffer-backed"/"no elem info"), so callers can tell "not a local" apart
// from "a local with nothing useful to report" without a second lookup.
static ST_ct_local_t *ST_ct_find_local_info(ST_ct_compiler_t *cc, ST_string_t name) {
    for (i32 k = (i32)cc->n_locals - 1; k >= 0; k--)
        if (ST_string_eq(cc->locals[k].name, name))
            return &cc->locals[k];
    return NULL;
}

static ST_string_t ST_ct_local_struct_type(ST_ct_compiler_t *cc, ST_string_t name) {
    for (i32 k = (i32)cc->n_locals - 1; k >= 0; k--)
        if (ST_string_eq(cc->locals[k].name, name))
            return cc->locals[k].struct_type;
    return (ST_string_t){0};
}

// -1 = syscall number (rax), 0..5 = arg0..arg5, -2 = unrecognized. Both the
// 64-bit and (for i32-typed params, see the register-width fix from
// earlier) 32-bit register names are accepted, since either can appear
// depending on the parameter's declared width.
static i32 ST_ct_syscall_reg_slot(ST_string_t reg) {
    if (ST_string_eq_cstr(reg, "rax") || ST_string_eq_cstr(reg, "eax")) return -1;
    if (ST_string_eq_cstr(reg, "rdi") || ST_string_eq_cstr(reg, "edi")) return 0;
    if (ST_string_eq_cstr(reg, "rsi") || ST_string_eq_cstr(reg, "esi")) return 1;
    if (ST_string_eq_cstr(reg, "rdx") || ST_string_eq_cstr(reg, "edx")) return 2;
    if (ST_string_eq_cstr(reg, "r10") || ST_string_eq_cstr(reg, "r10d")) return 3;
    if (ST_string_eq_cstr(reg, "r8") || ST_string_eq_cstr(reg, "r8d")) return 4;
    if (ST_string_eq_cstr(reg, "r9") || ST_string_eq_cstr(reg, "r9d")) return 5;
    return -2;
}

static b8 ST_ct_compile_syscall_asm(ST_ct_compiler_t *cc, ST_token_t *tokens, u32 n_tokens,
                                    u32 line, u32 col) {
    b8 have_slot[7] = {0};   // 0 = syscall number, 1..6 = arg0..arg5
    b8 is_const[7] = {0};
    i64 const_val[7] = {0};
    ST_string_t ident_val[7] = {0};

    u32 i = 0;
    while (i < n_tokens) {
        ST_token_t *t = &tokens[i];
        if (t->kind == ST_TIDENT && ST_string_eq_cstr(t->text, "syscall")) {
            i++;
            break;
        }
        if (t->kind != ST_TIDENT || !ST_string_eq_cstr(t->text, "mov")) {
            ST_ct_cfail(cc, line, col,
                        "comptime: only 'mov reg, value' lines and a trailing 'syscall' are "
                        "supported in a #comptime asm block");
            return 0;
        }
        i++;
        if (i >= n_tokens || tokens[i].kind != ST_TIDENT) {
            ST_ct_cfail(cc, line, col, "comptime: expected a register after 'mov'");
            return 0;
        }
        i32 which = ST_ct_syscall_reg_slot(tokens[i].text);
        if (which == -2) {
            ST_ct_cfail(cc, line, col,
                        "comptime: unsupported register '" ST_sv_fmt "' in a #comptime asm "
                        "block",
                        ST_sv_args(tokens[i].text));
            return 0;
        }
        i++;
        if (i >= n_tokens || tokens[i].kind != ST_TSYMBOL ||
            !ST_string_eq_cstr(tokens[i].text, ",")) {
            ST_ct_cfail(cc, line, col, "comptime: expected ',' after register");
            return 0;
        }
        i++;
        if (i >= n_tokens) {
            ST_ct_cfail(cc, line, col, "comptime: expected a value after ','");
            return 0;
        }
        u32 slot = (u32)(which + 1);
        if (tokens[i].kind == ST_TINT) {
            const_val[slot] = tokens[i].val.i;
            is_const[slot] = 1;
            have_slot[slot] = 1;
            i++;
        } else if (tokens[i].kind == ST_TIDENT) {
            ident_val[slot] = tokens[i].text;
            is_const[slot] = 0;
            have_slot[slot] = 1;
            i++;
        } else {
            ST_ct_cfail(cc, line, col,
                        "comptime: a #comptime asm block's mov operand must be an integer or a "
                        "local name");
            return 0;
        }
    }

    if (!have_slot[0]) {
        ST_ct_cfail(cc, line, col, "comptime: a #comptime asm block needs 'mov rax, <syscall "
                    "number>'");
        return 0;
    }

    for (u32 k = 0; k < 7; k++) {
        if (!have_slot[k]) {
            ST_ct_emit_const(cc->chunk, ST_ct_int(0), line);
            continue;
        }
        if (is_const[k]) {
            ST_ct_emit_const(cc->chunk, ST_ct_int(const_val[k]), line);
            continue;
        }
        i32 local_slot = ST_ct_find_local(cc, ident_val[k]);
        if (local_slot < 0) {
            ST_ct_cfail(cc, line, col,
                        "comptime: '" ST_sv_fmt "' isn't a known parameter or local in this "
                        "#comptime asm block",
                        ST_sv_args(ident_val[k]));
            return 0;
        }
        ST_ct_emit_op_u32(cc->chunk, ST_OP_GET_LOCAL, (u32)local_slot, line);
    }
    ST_ct_emit_op(cc->chunk, ST_OP_SYSCALL, line);
    return 1;
}

// Expressions
static void ST_ct_compile_expr(ST_ct_compiler_t *cc, ST_expr_t *e) {
    if (cc->failed || !e)
        return;

    switch (e->kind) {
        case ST_EX_INT:
        case ST_EX_CHAR:  // char/bool literals reuse 'ival'
        case ST_EX_BOOL:  // separate field for them in ST_expr_t's union.
            ST_ct_emit_const(cc->chunk, ST_ct_int(e->ival), e->line);
            return;
        case ST_EX_FLOAT:
            ST_ct_emit_const(cc->chunk, ST_ct_float(e->fval), e->line);
            return;
        case ST_EX_STR:
            ST_ct_emit_const(cc->chunk, ST_ct_str((const char *)e->sval.data, e->sval.len), e->line);
            return;
        case ST_EX_CSTR:
            ST_ct_compile_expr(cc, e->tyop.operand);
            if (cc->failed)
                return;
            ST_ct_emit_op(cc->chunk, ST_OP_NATIVE_ARG, e->line);
            return;
        case ST_EX_NULL:
            ST_ct_emit_op(cc->chunk, ST_OP_NIL, e->line);
            return;

        case ST_EX_STRUCT_LIT: {
            if (!cc->prog_ctx) {
                ST_ct_cfail(cc, e->line, e->col,
                            "comptime: struct literals aren't comptime-evaluable in this "
                            "context");
                return;
            }
            if (e->struct_lit.type_name.len == 0) {
                u32 n_elems = e->struct_lit.inits.count;
                if (n_elems > 64) {
                    ST_ct_cfail(cc, e->line, e->col,
                                "comptime: array literal has too many elements for a "
                                "#comptime literal (max 64)");
                    return;
                }
                ST_forrange(0, n_elems) {
                    ST_ct_compile_expr(cc, e->struct_lit.inits.items[i].value);
                    if (cc->failed)
                        return;
                }
                ST_ct_emit_op_u32(cc->chunk, ST_OP_MAKE_STRUCT, n_elems, e->line);
                return;
            }
            ST_ct_compile_struct_rvalue(cc, e, e->struct_lit.type_name);
            return;
        }

        case ST_EX_IDENT: {
            i32 slot = ST_ct_find_local(cc, e->name);
            if (slot >= 0) {
                ST_ct_emit_op_u32(cc->chunk, ST_OP_GET_LOCAL, (u32)slot, e->line);
                // A zero-initialized scalar local's own VM value is a real pointer
                // to its storage (see ST_OP_ALLOC_ZEROED), not the value itself
                ST_ct_local_t *lo = ST_ct_find_local_info(cc, e->name);
                u32 ew;
                b8 es;
                if (lo && lo->is_buffer && ST_ct_ty_int_info(e->ty, &ew, &es)) {
                    u32 operand = (0 & 0xF) | ((ew & 0xFF) << 4) | ((es & 1) << 12);
                    ST_ct_emit_op_u32(cc->chunk, ST_OP_PTR_LOAD, operand, e->line);
                }
                return;
            }
            if (cc->prog_ctx) {
                ST_decl_t *cd = ST_ct_prog_find_const(cc->prog_ctx, e->name);
                if (cd && cd->const_.is_comptime) {
                    // Inline substitution: a 'NAME :: #comptime value;' const has no
                    // runtime storage of its own here, it's just recompiled at every
                    // reference, the same as if 'value' had been written in place of
                    // the name. This also transparently handles a const referencing
                    // another '#comptime' const, since the recursive
                    // ST_ct_compile_expr call hits this same fallback again.
                    ST_ct_compile_expr(cc, cd->const_.value);
                    return;
                }
                if (cd) {
                    ST_ct_cfail(cc, e->line, e->col,
                                "'" ST_sv_fmt "' isn't usable in a #comptime scope add "
                                "'#comptime' after '::' to opt it in ('" ST_sv_fmt " :: "
                                "#comptime ...;')",
                                ST_sv_args(e->name), ST_sv_args(e->name));
                    return;
                }
            }
            ST_ct_cfail(cc, e->line, e->col,
                        "'" ST_sv_fmt "' isn't a compile-time value here "
                        ". Only locals declared inside this #comptime scope, and "
                        "'NAME :: #comptime value' consts, are",
                        ST_sv_args(e->name));
            return;
        }

        case ST_EX_UNARY: {
            if (ST_string_eq_cstr(e->unary.op, "&")) {
                ST_ct_lval_ty_t ty;
                ST_ct_lval_addr(cc, e->unary.operand, &ty);
                return;
            }
            if (ST_string_eq_cstr(e->unary.op, "*")) {
                ST_ct_compile_expr(cc, e->unary.operand);
                if (cc->failed)
                    return;
                if (e->ty && e->ty->kind == ST_TY_STRING) {
                    ST_ct_emit_op(cc->chunk, ST_OP_PTR_LOAD_STR, e->line);
                    return;
                }
                if (e->ty && e->ty->kind == ST_TY_PTR) {
                    ST_ct_emit_op_u32(cc->chunk, ST_OP_PTR_LOAD, 3, e->line);
                    return;
                }
                u32 width;
                b8 is_signed;
                if (e->ty && ST_ct_ty_int_info(e->ty, &width, &is_signed)) {
                    u32 operand = (0 & 0xF) | ((width & 0xFF) << 4) | ((is_signed & 1) << 12);
                    ST_ct_emit_op_u32(cc->chunk, ST_OP_PTR_LOAD, operand, e->line);
                    return;
                }
                ST_ct_cfail(cc, e->line, e->col,
                            "comptime: dereferencing this pointer type isn't supported yet");
                return;
            }
            ST_ct_compile_expr(cc, e->unary.operand);
            if (ST_string_eq_cstr(e->unary.op, "-"))
                ST_ct_emit_op(cc->chunk, ST_OP_NEG, e->line);
            else if (ST_string_eq_cstr(e->unary.op, "!"))
                ST_ct_emit_op(cc->chunk, ST_OP_NOT, e->line);
            else
                ST_ct_cfail(cc, e->line, e->col,
                            "comptime: unary '" ST_sv_fmt "' isn't supported in a #comptime context",
                            ST_sv_args(e->unary.op));
            return;
        }

        case ST_EX_BINARY: {
            ST_string_t op = e->bin.op;

            if (ST_string_eq_cstr(op, "&&")) {
                ST_ct_compile_expr(cc, e->bin.l);
                if (cc->failed) return;
                u32 false_jump = ST_ct_emit_jump(cc->chunk, ST_OP_JMP_IF_FALSE, e->line);
                ST_ct_compile_expr(cc, e->bin.r);
                if (cc->failed) return;
                u32 end_jump = ST_ct_emit_jump(cc->chunk, ST_OP_JMP, e->line);
                ST_ct_patch_jump(cc->chunk, false_jump);
                ST_ct_emit_op(cc->chunk, ST_OP_FALSE, e->line);
                ST_ct_patch_jump(cc->chunk, end_jump);
                return;
            }
            if (ST_string_eq_cstr(op, "||")) {
                ST_ct_compile_expr(cc, e->bin.l);
                if (cc->failed) return;
                u32 false_jump = ST_ct_emit_jump(cc->chunk, ST_OP_JMP_IF_FALSE, e->line);
                u32 true_jump = ST_ct_emit_jump(cc->chunk, ST_OP_JMP, e->line);
                ST_ct_patch_jump(cc->chunk, false_jump);
                ST_ct_compile_expr(cc, e->bin.r);
                if (cc->failed) return;
                u32 end_jump = ST_ct_emit_jump(cc->chunk, ST_OP_JMP, e->line);
                ST_ct_patch_jump(cc->chunk, true_jump);
                ST_ct_emit_op(cc->chunk, ST_OP_TRUE, e->line);
                ST_ct_patch_jump(cc->chunk, end_jump);
                return;
            }

            ST_ct_compile_expr(cc, e->bin.l);
            ST_ct_compile_expr(cc, e->bin.r);
            ST_ct_op_t o;
            if (ST_string_eq_cstr(op, "+")) o = ST_OP_ADD;
            else if (ST_string_eq_cstr(op, "-")) o = ST_OP_SUB;
            else if (ST_string_eq_cstr(op, "*")) o = ST_OP_MUL;
            else if (ST_string_eq_cstr(op, "/")) o = ST_OP_DIV;
            else if (ST_string_eq_cstr(op, "%")) o = ST_OP_MOD;
            else if (ST_string_eq_cstr(op, "==")) o = ST_OP_EQ;
            else if (ST_string_eq_cstr(op, "!=")) o = ST_OP_NEQ;
            else if (ST_string_eq_cstr(op, "<")) o = ST_OP_LT;
            else if (ST_string_eq_cstr(op, "<=")) o = ST_OP_LE;
            else if (ST_string_eq_cstr(op, ">")) o = ST_OP_GT;
            else if (ST_string_eq_cstr(op, ">=")) o = ST_OP_GE;
            else if (ST_string_eq_cstr(op, "&")) o = ST_OP_BAND;
            else if (ST_string_eq_cstr(op, "|")) o = ST_OP_BOR;
            else if (ST_string_eq_cstr(op, "^")) o = ST_OP_BXOR;
            else if (ST_string_eq_cstr(op, "<<")) o = ST_OP_SHL;
            else if (ST_string_eq_cstr(op, ">>")) o = ST_OP_SHR;
            else {
                ST_ct_cfail(cc, e->line, e->col,
                            "comptime: '" ST_sv_fmt "' isn't supported in a #comptime context "
                            "yet",
                            ST_sv_args(op));
                return;
            }
            ST_ct_emit_op(cc->chunk, o, e->line);
            return;
        }

        case ST_EX_FIELD: {
            // 'EnumType.Variant' is a symbolic lookup against the enum decl, not a
            // value at all.
            if (e->field.base->kind == ST_EX_IDENT && cc->prog_ctx) {
                ST_decl_t *ed = ST_ct_prog_find_enum(cc->prog_ctx, e->field.base->name);
                if (ed) {
                    ST_variant_spec_t *variant = NULL;
                    ST_forrange(0, ed->enum_.variants.count)
                        if (ST_string_eq(ed->enum_.variants.items[i].name, e->field.name)) {
                            variant = &ed->enum_.variants.items[i];
                            break;
                        }
                    if (!variant) {
                        ST_ct_cfail(cc, e->line, e->col,
                                    "comptime: '" ST_sv_fmt "' has no variant '" ST_sv_fmt "'",
                                    ST_sv_args(e->field.base->name), ST_sv_args(e->field.name));
                        return;
                    }
                    if (!variant->has_computed) {
                        ST_ct_cfail(cc, e->line, e->col,
                                    "comptime: '" ST_sv_fmt "." ST_sv_fmt "' isn't resolved to "
                                    "a value yet",
                                    ST_sv_args(e->field.base->name), ST_sv_args(e->field.name));
                        return;
                    }
                    ST_ct_emit_const(cc->chunk, ST_ct_int(variant->computed), e->line);
                    return;
                }
            }

            // A dyn_array's fields ('items'/'count'/'capacity') always live at fixed
            // byte offsets in real memory.
            ST_ty_t *bt = e->field.base->ty;
            ST_ty_t *dat = NULL;
            if (bt && bt->kind == ST_TY_DYN_ARRAY)
                dat = bt;
            else if (bt && bt->kind == ST_TY_PTR && bt->inner && bt->inner->kind == ST_TY_DYN_ARRAY)
                dat = bt->inner;
            if (dat) {
                u32 offset, tag = 0;
                if (ST_string_eq_cstr(e->field.name, "items")) { offset = 0; tag = 3; }
                else if (ST_string_eq_cstr(e->field.name, "count")) offset = 8;
                else if (ST_string_eq_cstr(e->field.name, "capacity")) offset = 16;
                else {
                    ST_ct_cfail(cc, e->line, e->col,
                                "comptime: a dyn_array has no field '" ST_sv_fmt "'",
                                ST_sv_args(e->field.name));
                    return;
                }
                ST_ct_compile_expr(cc, e->field.base);
                if (cc->failed)
                    return;
                ST_ct_emit_const(cc->chunk, ST_ct_int(offset), e->line);
                ST_ct_emit_op_u32(cc->chunk, ST_OP_PTR_ADD, 1, e->line);
                u32 operand = (tag & 0xF) | ((8 & 0xFF) << 4) | ((1 & 1) << 12);
                ST_ct_emit_op_u32(cc->chunk, ST_OP_PTR_LOAD, operand, e->line);
                return;
            }

            // A struct-typed base (by static type, or by the local-tracked struct
            // name) resolves its field via the general addressing helper, which
            // also handles arbitrary nesting through further structs/arrays.
            b8 looks_like_struct_field = (bt && bt->kind == ST_TY_STRUCT && bt->decl) ||
                                         (cc->prog_ctx && ST_ct_expr_struct_type(cc, e->field.base).len);
            if (looks_like_struct_field) {
                ST_ct_lval_ty_t fty;
                if (!ST_ct_lval_addr(cc, e, &fty))
                    return;
                if (fty.kind == ST_CT_LVAL_STRUCT || fty.kind == ST_CT_LVAL_ARRAY)
                    return;
                ST_ct_emit_lval_load(cc, &fty, e->line);
                return;
            }

            if (ST_string_eq_cstr(e->field.name, "ptr") && e->field.base->ty && e->field.base->ty->kind == ST_TY_STRING) {
		ST_ct_compile_expr(cc, e->field.base);
		if (cc->failed) return;
		ST_ct_emit_op(cc->chunk, ST_OP_STR_PTR, e->line);
                return;
            }


            if (!ST_string_eq_cstr(e->field.name, "len")) {
                ST_ct_cfail(cc, e->line, e->col,
                            "comptime: '.'" ST_sv_fmt "' isn't comptime-evaluable "
                            "fields on a struct-typed "
                            "local/const, are right now)",
                            ST_sv_args(e->field.name));
                return;
            }
            if (e->field.base->kind == ST_EX_IDENT) {
                ST_ct_local_t *lo = ST_ct_find_local_info(cc, e->field.base->name);
                if (lo && lo->has_known_len) {
                    ST_ct_emit_const(cc->chunk, ST_ct_int(lo->known_len), e->line);
                    return;
                }
                if (lo && lo->len_slot >= 0) {
                    ST_ct_emit_op_u32(cc->chunk, ST_OP_GET_LOCAL, (u32)lo->len_slot, e->line);
                    return;
                }
            }
            ST_ct_compile_expr(cc, e->field.base);
            ST_ct_emit_op(cc->chunk, ST_OP_STR_LEN, e->line);
            return;
        }

        case ST_EX_INDEX: {
            ST_expr_t *base = e->index.base;

            if (base->ty && base->ty->kind == ST_TY_PTR) {
                u32 ew;
                b8 es;
                if (ST_ct_ty_int_info(base->ty->inner, &ew, &es)) {
                    ST_ct_compile_expr(cc, base);
                    ST_ct_compile_expr(cc, e->index.index);
                    ST_ct_emit_op_u32(cc->chunk, ST_OP_PTR_ADD, ew, e->line);
                    u32 operand = (0 & 0xF) | ((ew & 0xFF) << 4) | ((es & 1) << 12);
                    ST_ct_emit_op_u32(cc->chunk, ST_OP_PTR_LOAD, operand, e->line);
                    return;
                }
            }

            if (base->kind == ST_EX_IDENT) {
                ST_ct_local_t *lo = ST_ct_find_local_info(cc, base->name);
                if (lo && lo->is_buffer && lo->has_elem_info) {
                    ST_ct_compile_expr(cc, base);
                    ST_ct_compile_expr(cc, e->index.index);
                    ST_ct_emit_op_u32(cc->chunk, ST_OP_PTR_ADD, lo->elem_size, e->line);
                    u32 tag = 0; // int
                    u32 operand = (tag & 0xF) | ((lo->elem_size & 0xFF) << 4) |
                                 ((lo->elem_is_signed & 1) << 12);
                    ST_ct_emit_op_u32(cc->chunk, ST_OP_PTR_LOAD, operand, e->line);
                    return;
                }
            }

            if (base->kind == ST_EX_FIELD && ST_string_eq_cstr(base->field.name, "ptr") &&
                base->field.base->ty) {
                ST_ty_kind_t bk = base->field.base->ty->kind;
                if (bk == ST_TY_STRING) {
                    ST_ct_compile_expr(cc, base->field.base);
                    ST_ct_compile_expr(cc, e->index.index);
                    ST_ct_emit_op(cc->chunk, ST_OP_STR_INDEX, e->line);
                    return;
                }
                if (bk == ST_TY_SLICE || bk == ST_TY_ARRAY || bk == ST_TY_DYN_ARRAY) {
                    ST_ct_compile_expr(cc, base->field.base);
                    ST_ct_compile_expr(cc, e->index.index);
                    ST_ct_emit_op(cc->chunk, ST_OP_STRUCT_INDEX, e->line);
                    return;
                }
            }
            if (base->kind == ST_EX_FIELD) {
                ST_ty_t *fbt = base->field.base->ty;
                b8 base_looks_like_struct = (fbt && fbt->kind == ST_TY_STRUCT && fbt->decl) ||
                                            (cc->prog_ctx && ST_ct_expr_struct_type(cc, base->field.base).len);
                if (base_looks_like_struct) {
                    ST_ct_lval_ty_t ty;
                    if (!ST_ct_lval_addr(cc, e, &ty))
                        return;
                    if (ty.kind == ST_CT_LVAL_STRUCT || ty.kind == ST_CT_LVAL_ARRAY)
                        return;
                    ST_ct_emit_lval_load(cc, &ty, e->line);
                    return;
                }
            }
            if (base->ty) {
                ST_ty_kind_t bk = base->ty->kind;
                if (bk == ST_TY_ARRAY || bk == ST_TY_SLICE || bk == ST_TY_DYN_ARRAY ||
                    bk == ST_TY_STRUCT) {
                    ST_ct_compile_expr(cc, base);
                    ST_ct_compile_expr(cc, e->index.index);
                    ST_ct_emit_op(cc->chunk, ST_OP_STRUCT_INDEX, e->line);
                    return;
                }
            }
            ST_ct_compile_expr(cc, e->index.base);
            ST_ct_compile_expr(cc, e->index.index);
            ST_ct_emit_op(cc->chunk, ST_OP_STR_INDEX, e->line);
            return;
        }

        case ST_EX_CAST: {
            ST_ct_compile_expr(cc, e->cast.operand);
            if (cc->failed)
                return;
            ST_ty_t *t = e->ty;
            if (!t) {
                ST_ct_cfail(cc, e->line, e->col, "comptime: cast target type isn't known here");
                return;
            }
            u32 tag, width = 0, is_signed = 0;
            switch (t->kind) {
                case ST_TY_INT: tag = 0; width = t->width / 8; is_signed = t->is_signed; break;
                case ST_TY_UNTYPED_INT: tag = 0; width = 8; is_signed = 1; break;
                case ST_TY_CHAR: tag = 0; width = 1; is_signed = 0; break;
                case ST_TY_FLOAT: case ST_TY_UNTYPED_FLOAT: tag = 1; break;
                case ST_TY_BOOL: tag = 2; break;
                case ST_TY_PTR: tag = 3; break;
                default:
                    ST_ct_cfail(cc, e->line, e->col,
                                "comptime: casting to this type isn't supported in a "
                                "#comptime scope yet");
                    return;
            }
            u32 operand = (tag & 0xF) | ((width & 0xFF) << 4) | ((is_signed & 1) << 12);
            ST_ct_emit_op_u32(cc->chunk, ST_OP_CAST, operand, e->line);
            return;
        }

        case ST_EX_STR_FROM_RAW: {
            ST_ct_compile_expr(cc, e->str_from_raw.ptr);
            if (cc->failed)
                return;
            ST_ct_compile_expr(cc, e->str_from_raw.len);
            if (cc->failed)
                return;
            ST_ct_emit_op(cc->chunk, ST_OP_STR_FROM_RAW, e->line);
            return;
        }

        case ST_EX_KIND: {
            ST_ty_t *t = e->tyop.operand ? e->tyop.operand->ty : NULL;
            if (!t) {
                ST_ct_cfail(cc, e->line, e->col,
                            "comptime: kind() needs its operand's type to already be known here");
                return;
            }
            const char *name;
            if (t->kind == ST_TY_STRUCT)
                name = "struct"; // dispatchable category, not e.g. "Foo". See #fields() for the
                                 // per-field type, which is what actually varies per struct
            else if (t->kind == ST_TY_TAG_UNION)
                name = "tag_union";
            else if (t->kind == ST_TY_ARRAY)
                name = "array"; // fixed-size [N]T. #fields() gives indexed element access
            else if (t->kind == ST_TY_DYN_ARRAY)
                name = "dyn_array"; // [..]T for any T; distinct from "array" since it has
                                    // items/count/capacity fields, not #fields() elements
            else if (t->kind == ST_TY_SLICE)
                name = "slice";
            else if (t->kind == ST_TY_PTR)
                name = "ptr"; // *T for any T, including *void (what 'null' resolves to
                              // with no other context) and *fn(...)->... function pointers
            else if (t->kind == ST_TY_ENUM)
                name = "enum"; // dispatchable category, not e.g. "Error"; there's no name
                               // reverse-lookup yet, so print_value falls back to the
                               // underlying integer
            else
                name = ST_ty_cstr(cc->arena, t);
            ST_ct_emit_const(cc->chunk, ST_ct_str(name, (u32)strlen(name)), e->line);
            return;
        }

        case ST_EX_COMP_ERROR: {
            ST_forrange(0, e->comp_error.args.count)
                ST_ct_compile_expr(cc, e->comp_error.args.items[i]);
            ST_ct_emit_op_u32(cc->chunk, ST_OP_COMP_ERROR, e->comp_error.args.count, e->line);
            ST_ct_emit_op(cc->chunk, ST_OP_NIL, e->line);
            return;
        }

        case ST_EX_CALL: {
            if (!cc->prog_ctx) {
                ST_ct_cfail(cc, e->line, e->col,
                            "comptime: function calls aren't comptime-evaluable in this context");
                return;
            }
            if (e->call.callee->kind != ST_EX_IDENT) {
                ST_ct_cfail(cc, e->line, e->col,
                            "comptime: only a direct call to a named function is supported yet");
                return;
            }
            ST_string_t callee_name = e->call.callee->name;
            ST_forrange(0, e->call.args.count) {
                if (e->call.args.items[i].name.len) {
                    ST_ct_cfail(cc, e->line, e->col,
                                "comptime: named arguments aren't supported in a #comptime call "
                                "yet");
                    return;
                }
            }

            ST_decl_t *extern_decl = ST_ct_prog_find_extern(cc->prog_ctx, callee_name);
            if (extern_decl) {
                u32 n_named = extern_decl->extern_fn.sig.params.count;
                b8 is_variadic = extern_decl->extern_fn.sig.is_variadic;
                if (!is_variadic && e->call.args.count > n_named) {
                    ST_ct_cfail(cc, e->line, e->col,
                                "comptime: '" ST_sv_fmt "' expects at most %u argument%s, "
                                "got %u",
                                ST_sv_args(callee_name), n_named, n_named == 1 ? "" : "s",
                                e->call.args.count);
                    return;
                }
                u32 n_args = e->call.args.count > n_named ? e->call.args.count : n_named;
                if (n_args > 8) {
                    ST_ct_cfail(cc, e->line, e->col,
                                "comptime: '" ST_sv_fmt "' has too many arguments for a "
                                "#comptime native call (max 8)",
                                ST_sv_args(callee_name));
                    return;
                }
                ST_forrange(0, n_args) {
                    ST_expr_t *arg;
                    if (i < e->call.args.count) {
                        arg = e->call.args.items[i].value;
                    } else {
                        arg = extern_decl->extern_fn.sig.params.items[i].def;
                        if (!arg) {
                            ST_ct_cfail(cc, e->line, e->col,
                                        "comptime: '" ST_sv_fmt "' is missing argument '"
                                        ST_sv_fmt "' (it has no default value)",
                                        ST_sv_args(callee_name),
                                        ST_sv_args(extern_decl->extern_fn.sig.params.items[i].name));
                            return;
                        }
                    }
                    ST_string_t struct_type = ST_ct_expr_struct_type(cc, arg);
                    ST_ct_compile_expr(cc, arg);
                    if (cc->failed)
                        return;
                    if (struct_type.len) {
                        ST_decl_t *sd = ST_ct_prog_find_struct(cc->prog_ctx, struct_type);
                        if (!sd) {
                            ST_ct_cfail(cc, e->line, e->col,
                                        "comptime: no struct named '" ST_sv_fmt "' found",
                                        ST_sv_args(struct_type));
                            return;
                        }
                        u32 n_fields = sd->struct_.fields.count;
                        if (n_fields > 64) {
                            ST_ct_cfail(cc, e->line, e->col,
                                        "comptime: struct '" ST_sv_fmt "' has too many "
                                        "fields to pack for a native call (max 64)",
                                        ST_sv_args(struct_type));
                            return;
                        }
                        u32 sizes[64];
                        ST_forrange(0, n_fields)
                            sizes[i] = ST_ct_field_byte_size(sd->struct_.fields.items[i].te);
                        ST_ct_emit_pack_struct(cc->chunk, sizes, n_fields, e->line);
                    } else {
                        ST_ct_emit_op(cc->chunk, ST_OP_NATIVE_ARG, e->line);
                    }
                }
                ST_ct_emit_const(cc->chunk, ST_ct_str("", 0), e->line);
                ST_ct_emit_op(cc->chunk, ST_OP_LOAD_LIB, e->line);
                {
                    u8 *name_copy = ST_arena_push(cc->arena, callee_name.len);
                    memcpy(name_copy, callee_name.data, callee_name.len);
                    ST_ct_emit_const(cc->chunk, ST_ct_str((const char *)name_copy, callee_name.len),
                                     e->line);
                }
                ST_ct_emit_const(cc->chunk, ST_ct_int((i64)n_args), e->line);
                ST_ct_emit_op(cc->chunk, ST_OP_BIND_SYM, e->line);
                ST_ct_emit_op(cc->chunk, ST_OP_CALL_NATIVE, e->line);
                return;
            }

            ST_decl_t *callee_decl = ST_ct_compile_plain_call(cc, e);
            if (!callee_decl)
                return;
            if (!ST_ct_decl_needs_native(callee_decl) && callee_decl->fn.sig.rets.count != 1) {
                ST_ct_cfail(cc, e->line, e->col,
                            "comptime: '" ST_sv_fmt "' returns %u values, but is used here as "
                            "a single value (bind it with 'a, b := ...' instead)",
                            ST_sv_args(callee_decl->name), callee_decl->fn.sig.rets.count);
                return;
            }
            return;
        }

        case ST_EX_ASM:
            ST_ct_compile_syscall_asm(cc, e->asm_.tokens, e->asm_.n_tokens, e->line, e->col);
            return;

        default:
            ST_ct_cfail(cc, e->line, e->col,
                        "comptime: this expression form isn't comptime-evaluable yet");
            return;
    }
}

void ST_ct_compile_expr_return(ST_ct_compiler_t *cc, ST_expr_t *e) {
    ST_ct_compile_expr(cc, e);
    if (!cc->failed)
        ST_ct_emit_op_u32(cc->chunk, ST_OP_RETURN, 1, e ? e->line : 0);
}

static ST_decl_t *ST_ct_compile_native_call(ST_ct_compiler_t *cc, ST_expr_t *call_expr,
                                            ST_decl_t *callee_decl) {
    ST_string_t callee_name = callee_decl->name;
    if (!callee_decl->is_pub) {
        ST_ct_cfail(cc, call_expr->line, call_expr->col,
                    "comptime: '" ST_sv_fmt "' has to be declared 'pub' to be called from "
                    "#comptime (it's implemented in assembly)",
                    ST_sv_args(callee_name));
        return NULL;
    }

    if (!ST_ct_ensure_native_module(cc->prog_ctx, cc->err_msg, sizeof(cc->err_msg))) {
        cc->failed = 1;
        cc->err_line = call_expr->line;
        cc->err_col = call_expr->col;
        return NULL;
    }

    u32 n_params = callee_decl->fn.sig.params.count;
    if (call_expr->call.args.count > n_params) {
        ST_ct_cfail(cc, call_expr->line, call_expr->col,
                    "comptime: '" ST_sv_fmt "' expects at most %u argument%s, got %u",
                    ST_sv_args(callee_name), n_params, n_params == 1 ? "" : "s",
                    call_expr->call.args.count);
        return NULL;
    }

    u32 arity = 0;
    ST_forrange(0, n_params) {
        ST_expr_t *arg;
        if (i < call_expr->call.args.count) {
            arg = call_expr->call.args.items[i].value;
        } else {
            arg = callee_decl->fn.sig.params.items[i].def;
            if (!arg) {
                ST_ct_cfail(cc, call_expr->line, call_expr->col,
                            "comptime: '" ST_sv_fmt "' is missing argument '" ST_sv_fmt "' (it "
                            "has no default value)",
                            ST_sv_args(callee_name), ST_sv_args(callee_decl->fn.sig.params.items[i].name));
                return NULL;
            }
        }

        // A real (asm-backed) Storth function follows Storth's own ABI, not C's:
        // a 'string' is two eightbyte registers (ptr, len), and so is a
        // '[]string' (items pointer, count)
        ST_tyexpr_t *pte = callee_decl->fn.sig.params.items[i].te;
        if (pte && pte->kind == ST_TE_NAME && ST_string_eq_cstr(pte->name, "string")) {
            ST_ct_compile_expr(cc, arg);
            if (cc->failed)
                return NULL;
            ST_ct_emit_op(cc->chunk, ST_OP_NATIVE_ARG_STRING, call_expr->line);
            arity += 2;
            continue;
        }
        if (pte && pte->kind == ST_TE_ARRAY && pte->inner && pte->inner->kind == ST_TE_NAME &&
            ST_string_eq_cstr(pte->inner->name, "string")) {
            ST_ct_compile_expr(cc, arg);
            if (cc->failed)
                return NULL;
            ST_ct_emit_op(cc->chunk, ST_OP_NATIVE_ARG_STRING_ARRAY, call_expr->line);
            arity += 2;
            continue;
        }

        ST_string_t struct_type = ST_ct_expr_struct_type(cc, arg);
        ST_ct_compile_expr(cc, arg);
        if (cc->failed)
            return NULL;
        if (struct_type.len) {
            ST_decl_t *sd = ST_ct_prog_find_struct(cc->prog_ctx, struct_type);
            if (!sd) {
                ST_ct_cfail(cc, call_expr->line, call_expr->col,
                            "comptime: no struct named '" ST_sv_fmt "' found",
                            ST_sv_args(struct_type));
                return NULL;
            }
            u32 n_fields = sd->struct_.fields.count;
            if (n_fields > 64) {
                ST_ct_cfail(cc, call_expr->line, call_expr->col,
                            "comptime: struct '" ST_sv_fmt "' has too many fields to pack "
                            "for a native call (max 64)",
                            ST_sv_args(struct_type));
                return NULL;
            }
            u32 sizes[64];
            ST_forrange(0, n_fields)
                sizes[i] = ST_ct_field_byte_size(sd->struct_.fields.items[i].te);
            ST_ct_emit_pack_struct(cc->chunk, sizes, n_fields, call_expr->line);
        } else {
            ST_ct_emit_op(cc->chunk, ST_OP_NATIVE_ARG, call_expr->line);
        }
        arity += 1;
    }

    {
        u32 path_len = (u32)strlen(cc->prog_ctx->native_so_path);
        u8 *path_copy = ST_arena_push(cc->arena, path_len);
        memcpy(path_copy, cc->prog_ctx->native_so_path, path_len);
        ST_ct_emit_const(cc->chunk, ST_ct_str((const char *)path_copy, path_len), call_expr->line);
    }
    ST_ct_emit_op(cc->chunk, ST_OP_LOAD_LIB, call_expr->line);
    {
        u8 *name_copy = ST_arena_push(cc->arena, callee_name.len);
        memcpy(name_copy, callee_name.data, callee_name.len);
        ST_ct_emit_const(cc->chunk, ST_ct_str((const char *)name_copy, callee_name.len),
                         call_expr->line);
    }
    ST_ct_emit_const(cc->chunk, ST_ct_int((i64)arity), call_expr->line);
    ST_ct_emit_op(cc->chunk, ST_OP_BIND_SYM, call_expr->line);
    ST_ct_emit_op(cc->chunk, ST_OP_CALL_NATIVE, call_expr->line);
    return callee_decl;
}

// Compiles a call to a plain (non-extern) Storth function: resolves the
// callee, fills in any missing trailing arguments from their declared
// defaults, compiles the (now-complete) argument list, emits the CALL,
// and registers it with ST_ct_prog_want_call. Leaves exactly
// callee_decl->fn.sig.rets.count values on the stack once the callee
// returns. Returns the callee decl on success, NULL on failure (with
// cc->failed already set).
static b8 ST_ct_struct_total_size(ST_ct_compiler_t *cc, ST_decl_t *sd, u32 *out_size) {
    u32 total = 0;
    ST_forrange(0, sd->struct_.fields.count) {
        u32 fs;
        if (!ST_ct_tyexpr_byte_size(cc, sd->struct_.fields.items[i].te, &fs))
            return 0;
        total += fs;
    }
    *out_size = total;
    return 1;
}

static b8 ST_ct_struct_field_offset(ST_ct_compiler_t *cc, ST_decl_t *sd, ST_string_t field_name,
                                    u32 *out_offset, ST_tyexpr_t **out_te) {
    u32 offset = 0;
    ST_forrange(0, sd->struct_.fields.count) {
        ST_tyexpr_t *fte = sd->struct_.fields.items[i].te;
        if (ST_string_eq(sd->struct_.fields.items[i].name, field_name)) {
            *out_offset = offset;
            *out_te = fte;
            return 1;
        }
        u32 fs;
        if (!ST_ct_tyexpr_byte_size(cc, fte, &fs))
            return 0;
        offset += fs;
    }
    return 0;
}

static b8 ST_ct_lval_classify_te(ST_ct_compiler_t *cc, ST_tyexpr_t *te, ST_ct_lval_ty_t *out_ty) {
    out_ty->elem_size = 0;
    out_ty->elem_is_signed = 0;
    if (te->kind == ST_TE_PTR) {
        out_ty->kind = ST_CT_LVAL_PTR;
        out_ty->struct_decl = NULL;
        out_ty->te = te;
        return 1;
    }
    if (te->kind == ST_TE_ARRAY) {
        out_ty->kind = ST_CT_LVAL_ARRAY;
        out_ty->struct_decl = NULL;
        out_ty->te = te->inner;
        return 1;
    }
    if (te->kind == ST_TE_NAME) {
        if (ST_string_eq_cstr(te->name, "string")) {
            out_ty->kind = ST_CT_LVAL_STRING;
            out_ty->struct_decl = NULL;
            out_ty->te = te;
            return 1;
        }
        if (cc->prog_ctx) {
            ST_decl_t *sd = ST_ct_prog_find_struct(cc->prog_ctx, te->name);
            if (sd) {
                out_ty->kind = ST_CT_LVAL_STRUCT;
                out_ty->struct_decl = sd;
                out_ty->te = te;
                return 1;
            }
        }
    }
    out_ty->kind = ST_CT_LVAL_SCALAR;
    out_ty->struct_decl = NULL;
    out_ty->te = te;
    return 1;
}

static b8 ST_ct_lval_addr(ST_ct_compiler_t *cc, ST_expr_t *e, ST_ct_lval_ty_t *out_ty) {
    out_ty->elem_size = 0;
    out_ty->elem_is_signed = 0;
    if (e->kind == ST_EX_IDENT) {
        ST_ct_local_t *lo = ST_ct_find_local_info(cc, e->name);
        if (!lo || !lo->is_buffer) {
            ST_ct_cfail(cc, e->line, e->col,
                        "comptime: '" ST_sv_fmt "' isn't addressable here", ST_sv_args(e->name));
            return 0;
        }
        i32 slot = ST_ct_find_local(cc, e->name);
        ST_ct_emit_op_u32(cc->chunk, ST_OP_GET_LOCAL, (u32)slot, e->line);
        ST_decl_t *sd = NULL;
        if (lo->struct_type.len && cc->prog_ctx)
            sd = ST_ct_prog_find_struct(cc->prog_ctx, lo->struct_type);
        if (!sd && cc->prog_ctx && e->ty && e->ty->kind == ST_TY_STRUCT && e->ty->decl)
            sd = e->ty->decl;
        if (sd) {
            out_ty->kind = ST_CT_LVAL_STRUCT;
            out_ty->struct_decl = sd;
            out_ty->te = NULL;
            return 1;
        }
        if (lo->has_elem_info) {
            out_ty->kind = ST_CT_LVAL_ARRAY;
            out_ty->struct_decl = NULL;
            out_ty->te = NULL;
            out_ty->elem_size = lo->elem_size;
            out_ty->elem_is_signed = lo->elem_is_signed;
            return 1;
        }
        out_ty->kind = ST_CT_LVAL_SCALAR;
        out_ty->struct_decl = NULL;
        out_ty->te = NULL;
        return 1;
    }

    if (e->kind == ST_EX_FIELD) {
        ST_ct_lval_ty_t base_ty;
        if (!ST_ct_lval_addr(cc, e->field.base, &base_ty))
            return 0;
        if (base_ty.kind != ST_CT_LVAL_STRUCT) {
            ST_ct_cfail(cc, e->line, e->col,
                        "comptime: '." ST_sv_fmt "' needs a struct-typed base here",
                        ST_sv_args(e->field.name));
            return 0;
        }
        u32 offset;
        ST_tyexpr_t *fte;
        if (!ST_ct_struct_field_offset(cc, base_ty.struct_decl, e->field.name, &offset, &fte)) {
            ST_ct_cfail(cc, e->line, e->col,
                        "comptime: '" ST_sv_fmt "' has no field '" ST_sv_fmt "'",
                        ST_sv_args(base_ty.struct_decl->name), ST_sv_args(e->field.name));
            return 0;
        }
        ST_ct_emit_const(cc->chunk, ST_ct_int(offset), e->line);
        ST_ct_emit_op_u32(cc->chunk, ST_OP_PTR_ADD, 1, e->line);
        return ST_ct_lval_classify_te(cc, fte, out_ty);
    }

    if (e->kind == ST_EX_INDEX) {
        ST_ct_lval_ty_t base_ty;
        if (!ST_ct_lval_addr(cc, e->index.base, &base_ty))
            return 0;
        if (base_ty.kind != ST_CT_LVAL_ARRAY) {
            ST_ct_cfail(cc, e->line, e->col,
                        "comptime: indexing needs an array-typed base here");
            return 0;
        }
        u32 elem_size;
        if (base_ty.te) {
            if (!ST_ct_tyexpr_byte_size(cc, base_ty.te, &elem_size)) {
                ST_ct_cfail(cc, e->line, e->col, "comptime: can't size this array's elements");
                return 0;
            }
        } else {
            elem_size = base_ty.elem_size;
        }
        ST_ct_compile_expr(cc, e->index.index);
        if (cc->failed)
            return 0;
        ST_ct_emit_op_u32(cc->chunk, ST_OP_PTR_ADD, elem_size, e->line);
        if (base_ty.te)
            return ST_ct_lval_classify_te(cc, base_ty.te, out_ty);
        out_ty->kind = ST_CT_LVAL_SCALAR;
        out_ty->struct_decl = NULL;
        out_ty->te = NULL;
        out_ty->elem_size = base_ty.elem_size;
        out_ty->elem_is_signed = base_ty.elem_is_signed;
        return 1;
    }

    ST_ct_cfail(cc, e->line, e->col, "comptime: this expression isn't addressable here");
    return 0;
}

static void ST_ct_emit_lval_load(ST_ct_compiler_t *cc, ST_ct_lval_ty_t *ty, u32 line) {
    if (ty->kind == ST_CT_LVAL_STRING) {
        ST_ct_emit_op(cc->chunk, ST_OP_PTR_LOAD_STR, line);
        return;
    }
    if (ty->kind == ST_CT_LVAL_PTR) {
        ST_ct_emit_op_u32(cc->chunk, ST_OP_PTR_LOAD, 3, line);
        return;
    }
    u32 width;
    b8 is_signed;
    if (ty->te && ST_ct_tyexpr_int_info(ty->te, &width, &is_signed)) {
        u32 operand = (0 & 0xF) | ((width & 0xFF) << 4) | ((is_signed & 1) << 12);
        ST_ct_emit_op_u32(cc->chunk, ST_OP_PTR_LOAD, operand, line);
        return;
    }
    if (!ty->te && ty->kind == ST_CT_LVAL_SCALAR && ty->elem_size) {
        u32 operand = (0 & 0xF) | ((ty->elem_size & 0xFF) << 4) | ((ty->elem_is_signed & 1) << 12);
        ST_ct_emit_op_u32(cc->chunk, ST_OP_PTR_LOAD, operand, line);
        return;
    }
    ST_ct_cfail(cc, line, 0, "comptime: don't know how to read this value through a pointer");
}

static b8 ST_ct_emit_lval_store(ST_ct_compiler_t *cc, ST_ct_lval_ty_t *ty, u32 line) {
    if (ty->kind == ST_CT_LVAL_STRING) {
        ST_ct_emit_op(cc->chunk, ST_OP_PTR_STORE_STR, line);
        return 1;
    }
    if (ty->kind == ST_CT_LVAL_PTR) {
        ST_ct_emit_op_u32(cc->chunk, ST_OP_PTR_STORE, 8, line);
        return 1;
    }
    u32 width;
    b8 is_signed;
    if (ty->te && ST_ct_tyexpr_int_info(ty->te, &width, &is_signed)) {
        ST_ct_emit_op_u32(cc->chunk, ST_OP_PTR_STORE, width, line);
        return 1;
    }
    if (!ty->te && ty->kind == ST_CT_LVAL_SCALAR && ty->elem_size) {
        ST_ct_emit_op_u32(cc->chunk, ST_OP_PTR_STORE, ty->elem_size, line);
        return 1;
    }
    ST_ct_cfail(cc, line, 0, "comptime: don't know how to write this value through a pointer");
    return 0;
}

static b8 ST_ct_compile_struct_rvalue(ST_ct_compiler_t *cc, ST_expr_t *e, ST_string_t struct_type) {
    if (!cc->prog_ctx) {
        ST_ct_cfail(cc, e->line, e->col, "comptime: structs aren't usable in this context");
        return 0;
    }
    ST_decl_t *sd = ST_ct_prog_find_struct(cc->prog_ctx, struct_type);
    if (!sd) {
        ST_ct_cfail(cc, e->line, e->col, "comptime: no struct named '" ST_sv_fmt "' found",
                    ST_sv_args(struct_type));
        return 0;
    }
    u32 total_size;
    if (!ST_ct_struct_total_size(cc, sd, &total_size)) {
        ST_ct_cfail(cc, e->line, e->col,
                    "comptime: '" ST_sv_fmt "' has a field type this #comptime scope can't "
                    "size yet", ST_sv_args(struct_type));
        return 0;
    }

    if (e->kind == ST_EX_STRUCT_LIT) {
        u32 n_fields = sd->struct_.fields.count;
        if (n_fields > 64) {
            ST_ct_cfail(cc, e->line, e->col,
                        "comptime: struct '" ST_sv_fmt "' has too many fields (max 64)",
                        ST_sv_args(struct_type));
            return 0;
        }
        ST_expr_t *field_exprs[64] = {0};
        ST_forrange(0, e->struct_lit.inits.count) {
            ST_field_init_t *fi = &e->struct_lit.inits.items[i];
            i32 idx = fi->name.len ? ST_ct_struct_field_index(sd, fi->name) : (i32)i;
            if (idx < 0 || (u32)idx >= n_fields) {
                ST_ct_cfail(cc, fi->line, fi->col, "comptime: '" ST_sv_fmt "' has no such field",
                            ST_sv_args(struct_type));
                return 0;
            }
            field_exprs[idx] = fi->value;
        }

        ST_ct_emit_op_u32(cc->chunk, ST_OP_ALLOC_ZEROED, total_size, e->line);
        u32 dst_slot = ST_ct_declare_local(cc, ST_cstr_to_str("$struct_scratch"));

        u32 offset = 0;
        ST_forrange(0, n_fields) {
            ST_tyexpr_t *fte = sd->struct_.fields.items[i].te;
            u32 fsize;
            if (!ST_ct_tyexpr_byte_size(cc, fte, &fsize)) {
                ST_ct_cfail(cc, e->line, e->col,
                            "comptime: can't size field '" ST_sv_fmt "' of '" ST_sv_fmt "'",
                            ST_sv_args(sd->struct_.fields.items[i].name), ST_sv_args(struct_type));
                return 0;
            }
            if (!field_exprs[i]) {
                ST_ct_cfail(cc, e->line, e->col,
                            "comptime: struct literal for '" ST_sv_fmt "' is missing a field",
                            ST_sv_args(struct_type));
                return 0;
            }
            ST_ct_lval_ty_t fty;
            ST_ct_lval_classify_te(cc, fte, &fty);
            ST_ct_emit_op_u32(cc->chunk, ST_OP_GET_LOCAL, dst_slot, e->line);
            ST_ct_emit_const(cc->chunk, ST_ct_int(offset), e->line);
            ST_ct_emit_op_u32(cc->chunk, ST_OP_PTR_ADD, 1, e->line);
            if (fty.kind == ST_CT_LVAL_STRUCT) {
                if (!ST_ct_compile_struct_rvalue(cc, field_exprs[i], fte->name))
                    return 0;
                ST_ct_emit_op_u32(cc->chunk, ST_OP_MEM_COPY, fsize, e->line);
                ST_ct_emit_op(cc->chunk, ST_OP_POP, e->line);
            } else {
                ST_ct_compile_expr(cc, field_exprs[i]);
                if (cc->failed)
                    return 0;
                if (!ST_ct_emit_lval_store(cc, &fty, e->line))
                    return 0;
                ST_ct_emit_op(cc->chunk, ST_OP_POP, e->line);
            }
            offset += fsize;
        }
        ST_ct_emit_op_u32(cc->chunk, ST_OP_GET_LOCAL, dst_slot, e->line);
        return 1;
    }

    ST_ct_emit_op_u32(cc->chunk, ST_OP_ALLOC_ZEROED, total_size, e->line);
    u32 dst_slot = ST_ct_declare_local(cc, ST_cstr_to_str("$struct_scratch"));
    ST_ct_emit_op_u32(cc->chunk, ST_OP_GET_LOCAL, dst_slot, e->line);
    ST_ct_compile_expr(cc, e);
    if (cc->failed)
        return 0;
    ST_ct_emit_op_u32(cc->chunk, ST_OP_MEM_COPY, total_size, e->line);
    ST_ct_emit_op(cc->chunk, ST_OP_POP, e->line);
    ST_ct_emit_op_u32(cc->chunk, ST_OP_GET_LOCAL, dst_slot, e->line);
    return 1;
}

static ST_decl_t *ST_ct_compile_plain_call(ST_ct_compiler_t *cc, ST_expr_t *call_expr) {
    if (!cc->prog_ctx) {
        ST_ct_cfail(cc, call_expr->line, call_expr->col,
                    "comptime: function calls aren't comptime-evaluable in this context");
        return NULL;
    }
    if (call_expr->call.callee->kind != ST_EX_IDENT) {
        ST_ct_cfail(cc, call_expr->line, call_expr->col,
                    "comptime: only a direct call to a named function is supported yet");
        return NULL;
    }
    ST_string_t callee_name = call_expr->call.callee->name;
    ST_forrange(0, call_expr->call.args.count) {
        if (call_expr->call.args.items[i].name.len) {
            ST_ct_cfail(cc, call_expr->line, call_expr->col,
                        "comptime: named arguments aren't supported in a #comptime call yet");
            return NULL;
        }
    }

    b8 not_callable = 0;
    ST_decl_t *callee_decl = ST_ct_prog_find_decl(cc->prog_ctx, callee_name, &not_callable);
    if (!callee_decl) {
        if (not_callable)
            ST_ct_cfail(cc, call_expr->line, call_expr->col,
                        "comptime: '" ST_sv_fmt "' has generics, packs, or variadic args, "
                        "which aren't supported in a #comptime call yet",
                        ST_sv_args(callee_name));
        else
            ST_ct_cfail(cc, call_expr->line, call_expr->col,
                        "comptime: no plain function named '" ST_sv_fmt "' found to call",
                        ST_sv_args(callee_name));
        return NULL;
    }

    if (ST_ct_decl_needs_native(callee_decl))
        return ST_ct_compile_native_call(cc, call_expr, callee_decl);

    u32 n_params = callee_decl->fn.sig.params.count;
    if (call_expr->call.args.count > n_params) {
        ST_ct_cfail(cc, call_expr->line, call_expr->col,
                    "comptime: '" ST_sv_fmt "' expects at most %u argument%s, got %u",
                    ST_sv_args(callee_name), n_params, n_params == 1 ? "" : "s",
                    call_expr->call.args.count);
        return NULL;
    }

    if (n_params > 64) {
        ST_ct_cfail(cc, call_expr->line, call_expr->col,
                    "comptime: '" ST_sv_fmt "' has too many parameters for a #comptime call "
                    "(max 64)", ST_sv_args(callee_name));
        return NULL;
    }
    u32 arity = 0;
    ST_forrange(0, n_params) {
        ST_expr_t *arg;
        if (i < call_expr->call.args.count) {
            arg = call_expr->call.args.items[i].value;
        } else {
            arg = callee_decl->fn.sig.params.items[i].def;
            if (!arg) {
                ST_ct_cfail(cc, call_expr->line, call_expr->col,
                            "comptime: '" ST_sv_fmt "' is missing argument '" ST_sv_fmt "' (it "
                            "has no default value)",
                            ST_sv_args(callee_name), ST_sv_args(callee_decl->fn.sig.params.items[i].name));
                return NULL;
            }
        }
        ST_tyexpr_t *pte = callee_decl->fn.sig.params.items[i].te;
        ST_string_t param_struct_type = (ST_string_t){0};
        if (pte && pte->kind == ST_TE_NAME && ST_ct_prog_find_struct(cc->prog_ctx, pte->name))
            param_struct_type = pte->name;
        if (param_struct_type.len) {
            if (!ST_ct_compile_struct_rvalue(cc, arg, param_struct_type))
                return NULL;
        } else {
            ST_ct_compile_expr(cc, arg);
            if (cc->failed)
                return NULL;
        }
        arity++;

        // A slice-typed parameter needs the caller to also push its length,
        // right after the pointer
        u32 sw;
        b8 ssig;
        if (!param_struct_type.len && pte && ST_ct_tyexpr_is_int_slice(pte, &sw, &ssig)) {
            ST_ct_local_t *lo = arg->kind == ST_EX_IDENT ? ST_ct_find_local_info(cc, arg->name) : NULL;
            if (lo && lo->has_known_len) {
                ST_ct_emit_const(cc->chunk, ST_ct_int(lo->known_len), call_expr->line);
            } else if (lo && lo->len_slot >= 0) {
                ST_ct_emit_op_u32(cc->chunk, ST_OP_GET_LOCAL, (u32)lo->len_slot, call_expr->line);
            } else {
                ST_ct_cfail(cc, call_expr->line, call_expr->col,
                            "comptime: can't tell how long this slice argument to '" ST_sv_fmt
                            "' is pass a fixed-size array local or another slice "
                            "parameter directly",
                            ST_sv_args(callee_name));
                return NULL;
            }
            arity++;
        }
    }

    u32 patch_off = ST_ct_emit_call(cc->chunk, arity, call_expr->line);
    if (!ST_ct_prog_want_call(cc->prog_ctx, callee_name, patch_off, call_expr->line,
                              call_expr->col)) {
        cc->failed = 1;
        cc->err_line = cc->prog_ctx->err_line;
        cc->err_col = cc->prog_ctx->err_col;
        memcpy(cc->err_msg, cc->prog_ctx->err_msg, sizeof(cc->err_msg));
        return NULL;
    }

    return callee_decl;
}

// Statements
static void ST_ct_compile_stmt(ST_ct_compiler_t *cc, ST_stmt_t *s);

// Compiles a body as its own lexical scope: locals declared inside are
// unreachable (and their stack slots reclaimed) once the body ends, same
// discipline a real stack-slot compiler uses for block scope.
static void ST_ct_compile_scoped(ST_ct_compiler_t *cc, ST_stmts_t *body) {
    u32 saved = cc->n_locals;
    ST_forrange(0, body->count) {
        if (cc->failed) return;
        ST_ct_compile_stmt(cc, body->items[i]);
    }
    u32 introduced = cc->n_locals - saved;
    ST_forrange(0, introduced) ST_ct_emit_op(cc->chunk, ST_OP_POP, 0);
    cc->n_locals = saved;
}

static void ST_ct_compile_stmt(ST_ct_compiler_t *cc, ST_stmt_t *s) {
    if (cc->failed || !s)
        return;

    switch (s->kind) {
        case ST_ST_DECL: {
            if (!s->decl.init) {
                u32 size;
                if (!s->decl.te || !ST_ct_tyexpr_byte_size(cc, s->decl.te, &size)) {
                    ST_ct_cfail(cc, s->line, s->col,
                                "comptime: '" ST_sv_fmt "' needs an initializer here its "
                                "type isn't one this #comptime scope knows how to "
                                "zero-initialize yet",
                                ST_sv_args(s->decl.name));
                    return;
                }
                ST_ct_emit_op_u32(cc->chunk, ST_OP_ALLOC_ZEROED, size, s->line);
                ST_ct_declare_local(cc, s->decl.name);
                ST_ct_local_set_is_buffer(cc);
                if (s->decl.te->kind == ST_TE_NAME && cc->prog_ctx) {
                    ST_decl_t *sd = ST_ct_prog_find_struct(cc->prog_ctx, s->decl.te->name);
                    if (sd)
                        ST_ct_local_set_struct_type(cc, s->decl.te->name);
                }
                if (s->decl.te->kind == ST_TE_ARRAY) {
                    u32 ew;
                    b8 es;
                    if (ST_ct_tyexpr_int_info(s->decl.te->inner, &ew, &es))
                        ST_ct_local_set_elem_info(cc, ew, es);
                    // A fixed-size array's own element count is a compile-time
                    // constant, known straight from its declaration
                    if (s->decl.te->count_expr && cc->prog_ctx && cc->prog_ctx->sema) {
                        i64 count;
                        if (ST_const_eval(cc->prog_ctx->sema, s->decl.te->count_expr, &count) &&
                            count >= 0)
                            ST_ct_local_set_known_len(cc, (u32)count);
                    }
                }
                return;
            }
            // Computed before declaring the new local, using ST_ct_expr_struct_type
            // rather than checking for a direct ST_EX_STRUCT_LIT only, so this also
            // covers 'd := c;' (copying an already struct-typed local) and
            // 'd := RED;' (copying a struct-typed global const), not just a struct
            // literal written inline. Falls back to the local's own declared type
            // name when the initializer is a bracket literal ('out: Process = {...};')
            ST_string_t struct_type = ST_ct_expr_struct_type(cc, s->decl.init);
            if (!struct_type.len && s->decl.init->kind == ST_EX_STRUCT_LIT &&
                s->decl.te && s->decl.te->kind == ST_TE_NAME)
                struct_type = s->decl.te->name;
            if (!struct_type.len && s->decl.te && s->decl.te->kind == ST_TE_NAME && cc->prog_ctx &&
                ST_ct_prog_find_struct(cc->prog_ctx, s->decl.te->name))
                struct_type = s->decl.te->name;
            if (struct_type.len) {
                if (!ST_ct_compile_struct_rvalue(cc, s->decl.init, struct_type))
                    return;
            } else {
                ST_ct_compile_expr(cc, s->decl.init); // leaves the local's value on the stack...
                if (cc->failed)
                    return;
            }
            ST_ct_declare_local(cc, s->decl.name); // ...which becomes its permanent slot
            if (struct_type.len) {
                ST_ct_local_set_struct_type(cc, struct_type);
                ST_ct_local_set_is_buffer(cc);
            }
            return;
        }

        case ST_ST_ASSIGN: {
            if (!ST_string_eq_cstr(s->assign.op, "=")) {
                ST_ct_cfail(cc, s->line, s->col,
                            "comptime: only plain '=' assignment is supported in a #comptime scope "
                            "yet (no '+=' etc)");
                return;
            }
            if (s->assign.lhs->kind == ST_EX_FIELD) {
                ST_ty_t *bt = s->assign.lhs->field.base->ty;
                ST_ty_t *dat = NULL;
                if (bt && bt->kind == ST_TY_DYN_ARRAY)
                    dat = bt;
                else if (bt && bt->kind == ST_TY_PTR && bt->inner &&
                         bt->inner->kind == ST_TY_DYN_ARRAY)
                    dat = bt->inner;
                if (dat) {
                    u32 offset;
                    if (ST_string_eq_cstr(s->assign.lhs->field.name, "items")) offset = 0;
                    else if (ST_string_eq_cstr(s->assign.lhs->field.name, "count")) offset = 8;
                    else if (ST_string_eq_cstr(s->assign.lhs->field.name, "capacity")) offset = 16;
                    else {
                        ST_ct_cfail(cc, s->line, s->col,
                                    "comptime: a dyn_array has no field '" ST_sv_fmt "'",
                                    ST_sv_args(s->assign.lhs->field.name));
                        return;
                    }
                    ST_ct_compile_expr(cc, s->assign.lhs->field.base);
                    if (cc->failed)
                        return;
                    ST_ct_emit_const(cc->chunk, ST_ct_int(offset), s->line);
                    ST_ct_emit_op_u32(cc->chunk, ST_OP_PTR_ADD, 1, s->line);
                    ST_ct_compile_expr(cc, s->assign.rhs);
                    if (cc->failed)
                        return;
                    ST_ct_emit_op_u32(cc->chunk, ST_OP_PTR_STORE, 8, s->line);
                    ST_ct_emit_op(cc->chunk, ST_OP_POP, s->line);
                    return;
                }
            }
            if (s->assign.lhs->kind == ST_EX_FIELD) {
                ST_ty_t *fbt = s->assign.lhs->field.base->ty;
                b8 looks_like_struct_field = (fbt && fbt->kind == ST_TY_STRUCT && fbt->decl) ||
                                             (cc->prog_ctx && ST_ct_expr_struct_type(cc, s->assign.lhs->field.base).len);
                if (looks_like_struct_field) {
                    ST_ct_lval_ty_t ty;
                    if (!ST_ct_lval_addr(cc, s->assign.lhs, &ty))
                        return;
                    if (ty.kind == ST_CT_LVAL_STRUCT) {
                        u32 total_size;
                        if (!ST_ct_struct_total_size(cc, ty.struct_decl, &total_size)) {
                            ST_ct_cfail(cc, s->line, s->col,
                                        "comptime: '" ST_sv_fmt "' has a field type this "
                                        "#comptime scope can't size yet",
                                        ST_sv_args(ty.struct_decl->name));
                            return;
                        }
                        if (!ST_ct_compile_struct_rvalue(cc, s->assign.rhs, ty.struct_decl->name))
                            return;
                        ST_ct_emit_op_u32(cc->chunk, ST_OP_MEM_COPY, total_size, s->line);
                        ST_ct_emit_op(cc->chunk, ST_OP_POP, s->line);
                        return;
                    }
                    ST_ct_compile_expr(cc, s->assign.rhs);
                    if (cc->failed)
                        return;
                    if (!ST_ct_emit_lval_store(cc, &ty, s->line))
                        return;
                    ST_ct_emit_op(cc->chunk, ST_OP_POP, s->line);
                    return;
                }
            }
            if (s->assign.lhs->kind == ST_EX_UNARY && ST_string_eq_cstr(s->assign.lhs->unary.op, "*")) {
                ST_ct_compile_expr(cc, s->assign.lhs->unary.operand);
                if (cc->failed)
                    return;
                ST_ct_compile_expr(cc, s->assign.rhs);
                if (cc->failed)
                    return;
                ST_ty_t *pt = s->assign.lhs->ty;
                if (pt && pt->kind == ST_TY_STRING) {
                    ST_ct_emit_op(cc->chunk, ST_OP_PTR_STORE_STR, s->line);
                    ST_ct_emit_op(cc->chunk, ST_OP_POP, s->line);
                    return;
                }
                if (pt && pt->kind == ST_TY_PTR) {
                    ST_ct_emit_op_u32(cc->chunk, ST_OP_PTR_STORE, 8, s->line);
                    ST_ct_emit_op(cc->chunk, ST_OP_POP, s->line);
                    return;
                }
                u32 width;
                b8 is_signed;
                if (pt && ST_ct_ty_int_info(pt, &width, &is_signed)) {
                    ST_ct_emit_op_u32(cc->chunk, ST_OP_PTR_STORE, width, s->line);
                    ST_ct_emit_op(cc->chunk, ST_OP_POP, s->line);
                    return;
                }
                ST_ct_cfail(cc, s->line, s->col,
                            "comptime: assigning through this pointer type isn't supported yet");
                return;
            }
            if (s->assign.lhs->kind == ST_EX_INDEX) {
                ST_expr_t *ibase = s->assign.lhs->index.base;
                u32 ew = 0;
                b8 es = 0;
                b8 have_elem = 0;
                if (ibase->ty && ibase->ty->kind == ST_TY_PTR &&
                    ST_ct_ty_int_info(ibase->ty->inner, &ew, &es))
                    have_elem = 1;
                else if (ibase->kind == ST_EX_IDENT) {
                    ST_ct_local_t *lo = ST_ct_find_local_info(cc, ibase->name);
                    if (lo && lo->is_buffer && lo->has_elem_info) {
                        ew = lo->elem_size;
                        es = lo->elem_is_signed;
                        have_elem = 1;
                    }
                }
                if (have_elem) {
                    ST_ct_compile_expr(cc, ibase);
                    ST_ct_compile_expr(cc, s->assign.lhs->index.index);
                    ST_ct_emit_op_u32(cc->chunk, ST_OP_PTR_ADD, ew, s->line);
                    ST_ct_compile_expr(cc, s->assign.rhs);
                    if (cc->failed)
                        return;
                    ST_ct_emit_op_u32(cc->chunk, ST_OP_PTR_STORE, ew, s->line);
                    ST_ct_emit_op(cc->chunk, ST_OP_POP, s->line);
                    return;
                }
            }
            if (s->assign.lhs->kind == ST_EX_INDEX && s->assign.lhs->index.base->kind == ST_EX_FIELD) {
                ST_expr_t *fb = s->assign.lhs->index.base;
                ST_ty_t *fbt2 = fb->field.base->ty;
                b8 looks_like_struct_field = (fbt2 && fbt2->kind == ST_TY_STRUCT && fbt2->decl) ||
                                             (cc->prog_ctx && ST_ct_expr_struct_type(cc, fb->field.base).len);
                if (looks_like_struct_field) {
                    ST_ct_lval_ty_t ty;
                    if (!ST_ct_lval_addr(cc, s->assign.lhs, &ty))
                        return;
                    ST_ct_compile_expr(cc, s->assign.rhs);
                    if (cc->failed)
                        return;
                    if (!ST_ct_emit_lval_store(cc, &ty, s->line))
                        return;
                    ST_ct_emit_op(cc->chunk, ST_OP_POP, s->line);
                    return;
                }
            }
            if (s->assign.lhs->kind == ST_EX_IDENT) {
                ST_ct_local_t *lo = ST_ct_find_local_info(cc, s->assign.lhs->name);
                if (lo && lo->struct_type.len && cc->prog_ctx) {
                    ST_decl_t *sd = ST_ct_prog_find_struct(cc->prog_ctx, lo->struct_type);
                    if (sd) {
                        u32 total_size;
                        if (!ST_ct_struct_total_size(cc, sd, &total_size)) {
                            ST_ct_cfail(cc, s->line, s->col,
                                        "comptime: '" ST_sv_fmt "' has a field type this "
                                        "#comptime scope can't size yet",
                                        ST_sv_args(lo->struct_type));
                            return;
                        }
                        i32 dslot = ST_ct_find_local(cc, s->assign.lhs->name);
                        ST_ct_emit_op_u32(cc->chunk, ST_OP_GET_LOCAL, (u32)dslot, s->line);
                        if (!ST_ct_compile_struct_rvalue(cc, s->assign.rhs, lo->struct_type))
                            return;
                        ST_ct_emit_op_u32(cc->chunk, ST_OP_MEM_COPY, total_size, s->line);
                        ST_ct_emit_op(cc->chunk, ST_OP_POP, s->line);
                        return;
                    }
                }
            }
            if (s->assign.lhs->kind != ST_EX_IDENT) {
                ST_ct_cfail(cc, s->line, s->col,
                            "comptime: can only assign to a plain local name, a field of a "
                            "struct-typed local, or an element of a buffer-backed array "
                            "local, in a #comptime scope");
                return;
            }
            i32 slot = ST_ct_find_local(cc, s->assign.lhs->name);
            if (slot < 0) {
                ST_ct_cfail(cc, s->line, s->col,
                            "'" ST_sv_fmt "' isn't a local declared in this #comptime scope",
                            ST_sv_args(s->assign.lhs->name));
                return;
            }
            ST_ct_compile_expr(cc, s->assign.rhs);
            ST_ct_emit_op_u32(cc->chunk, ST_OP_SET_LOCAL, (u32)slot, s->line);
            ST_ct_emit_op(cc->chunk, ST_OP_POP, s->line); // discard SET_LOCAL's leftover value
            return;
        }

        case ST_ST_EXPR: {
            if (s->expr && s->expr->kind == ST_EX_COMP_ERROR) {
                ST_forrange(0, s->expr->comp_error.args.count)
                    ST_ct_compile_expr(cc, s->expr->comp_error.args.items[i]);
                ST_ct_emit_op_u32(cc->chunk, ST_OP_COMP_ERROR, s->expr->comp_error.args.count, s->line);
                return; // COMP_ERROR halts the VM; nothing to pop
            }
            if (s->expr && s->expr->kind == ST_EX_CALL && cc->prog_ctx &&
                s->expr->call.callee->kind == ST_EX_IDENT &&
                !ST_ct_prog_find_extern(cc->prog_ctx, s->expr->call.callee->name)) {
                ST_decl_t *callee_decl = ST_ct_compile_plain_call(cc, s->expr);
                if (!callee_decl)
                    return;
                // A native call always leaves exactly 1 value (CALL_NATIVE's own
                // convention). A plain VM call declared void (rets.count == 0)
                // STILL leaves exactly 1 value too: ST_OP_HALT (used both when a
                // function falls off the end of its body and for a bare
                // 'return;') unconditionally pushes one nil, regardless of the
                // callee's declared return count
                u32 n_pop = ST_ct_decl_needs_native(callee_decl) ? 1
                          : callee_decl->fn.sig.rets.count == 0 ? 1
                          : callee_decl->fn.sig.rets.count;
                ST_forrange(0, n_pop) ST_ct_emit_op(cc->chunk, ST_OP_POP, s->line);
                return;
            }
            ST_ct_compile_expr(cc, s->expr);
            ST_ct_emit_op(cc->chunk, ST_OP_POP, s->line); // statement result is discarded
            return;
        }

        case ST_ST_IF: {
            ST_ct_compile_expr(cc, s->if_.cond);
            u32 else_jump = ST_ct_emit_jump(cc->chunk, ST_OP_JMP_IF_FALSE, s->line);
            ST_ct_compile_scoped(cc, &s->if_.then_body);
            if (s->if_.else_stmt) {
                u32 end_jump = ST_ct_emit_jump(cc->chunk, ST_OP_JMP, s->line);
                ST_ct_patch_jump(cc->chunk, else_jump);
                ST_ct_compile_stmt(cc, s->if_.else_stmt); // 'else { }' is itself an ST_ST_BLOCK
                ST_ct_patch_jump(cc->chunk, end_jump);
            } else {
                ST_ct_patch_jump(cc->chunk, else_jump);
            }
            return;
        }

        case ST_ST_WHILE: {
            u32 loop_start = cc->chunk->count;
            ST_ct_compile_expr(cc, s->while_.cond);
            if (cc->failed)
                return;
            u32 exit_jump = ST_ct_emit_jump(cc->chunk, ST_OP_JMP_IF_FALSE, s->line);

            if (cc->n_loops >= ST_array_len(cc->loop_stack)) {
                ST_ct_cfail(cc, s->line, s->col,
                            "comptime: loops nested too deeply in a #comptime scope");
                return;
            }
            ST_ct_loop_ctx_t *loop = &cc->loop_stack[cc->n_loops++];
            loop->n_break_patches = 0;
            loop->n_continue_patches = 0;
            loop->body_start_locals = cc->n_locals;

            ST_ct_compile_scoped(cc, &s->while_.body);

            if (!cc->failed) {
                ST_forrange(0, loop->n_continue_patches)
                    ST_ct_patch_jump_to(cc->chunk, loop->continue_patches[i], loop_start);
                ST_ct_emit_loop(cc->chunk, loop_start, s->line);
                ST_ct_patch_jump(cc->chunk, exit_jump);
                ST_forrange(0, loop->n_break_patches)
                    ST_ct_patch_jump(cc->chunk, loop->break_patches[i]);
            }
            cc->n_loops--;
            return;
        }

        case ST_ST_FOR_RANGE: {
            if (s->for_range.is_comptime) {
                ST_ct_cfail(cc, s->line, s->col,
                            "comptime: '#for' unrolling isn't supported in a #comptime scope "
                            "yet");
                return;
            }
            // Desugars to: i := lo; while i < hi (or <=, for '..=') { body; i = i + 1; }
            u32 saved_locals = cc->n_locals;
            ST_ct_compile_expr(cc, s->for_range.lo);
            if (cc->failed)
                return;
            u32 iter_slot = ST_ct_declare_local(cc, s->for_range.iter);

            u32 loop_start = cc->chunk->count;
            ST_ct_emit_op_u32(cc->chunk, ST_OP_GET_LOCAL, iter_slot, s->line);
            ST_ct_compile_expr(cc, s->for_range.hi);
            if (cc->failed)
                return;
            ST_ct_emit_op(cc->chunk, s->for_range.inclusive ? ST_OP_LE : ST_OP_LT, s->line);
            u32 exit_jump = ST_ct_emit_jump(cc->chunk, ST_OP_JMP_IF_FALSE, s->line);

            if (cc->n_loops >= ST_array_len(cc->loop_stack)) {
                ST_ct_cfail(cc, s->line, s->col,
                            "comptime: loops nested too deeply in a #comptime scope");
                return;
            }
            ST_ct_loop_ctx_t *loop = &cc->loop_stack[cc->n_loops++];
            loop->n_break_patches = 0;
            loop->n_continue_patches = 0;
            loop->body_start_locals = cc->n_locals;

            ST_ct_compile_scoped(cc, &s->for_range.body);

            if (!cc->failed) {
                // 'continue' has to land here, at the increment, not back at
                // loop_start directly.
                u32 continue_point = cc->chunk->count;
                ST_forrange(0, loop->n_continue_patches)
                    ST_ct_patch_jump_to(cc->chunk, loop->continue_patches[i], continue_point);

                ST_ct_emit_op_u32(cc->chunk, ST_OP_GET_LOCAL, iter_slot, s->line);
                ST_ct_emit_const(cc->chunk, ST_ct_int(1), s->line);
                ST_ct_emit_op(cc->chunk, ST_OP_ADD, s->line);
                ST_ct_emit_op_u32(cc->chunk, ST_OP_SET_LOCAL, iter_slot, s->line);
                ST_ct_emit_op(cc->chunk, ST_OP_POP, s->line); // discard SET_LOCAL's leftover value

                ST_ct_emit_loop(cc->chunk, loop_start, s->line);
                ST_ct_patch_jump(cc->chunk, exit_jump);
                ST_forrange(0, loop->n_break_patches)
                    ST_ct_patch_jump(cc->chunk, loop->break_patches[i]);

                ST_ct_emit_op(cc->chunk, ST_OP_POP, s->line); // drop the iterator local
            }
            cc->n_loops--;
            cc->n_locals = saved_locals;
            return;
        }

        case ST_ST_FOR_ARRAY: {
            if (s->for_array.is_comptime) {
                ST_ct_cfail(cc, s->line, s->col,
                            "comptime: '#for' unrolling isn't supported in a #comptime scope "
                            "yet");
                return;
            }
            // Desugars to:
            //   __arr := target; __idx := 0;
            //   while __idx < __arr.len {
            //       iter := __arr[__idx]; [spec_iter := __idx;]
            //       body
            //       __idx = __idx + 1;
            //   }
            // reusing STR_LEN (already dispatches on string vs. struct-shaped)
            // for the length check, and dispatching STR_INDEX vs. STRUCT_INDEX
            // for the element fetch based on the target's own static type.
            u32 saved_locals = cc->n_locals;

            ST_ct_compile_expr(cc, s->for_array.target);
            if (cc->failed)
                return;
            u32 arr_slot = ST_ct_declare_local(cc, ST_cstr_to_str("$for_arr"));

            ST_ct_emit_const(cc->chunk, ST_ct_int(0), s->line);
            u32 idx_slot = ST_ct_declare_local(cc, ST_cstr_to_str("$for_idx"));

            ST_ct_emit_op(cc->chunk, ST_OP_NIL, s->line);
            u32 iter_slot = ST_ct_declare_local(cc, s->for_array.iter);

            b8 has_spec = s->for_array.spec_iter.len > 0;
            u32 spec_slot = 0;
            if (has_spec) {
                ST_ct_emit_op(cc->chunk, ST_OP_NIL, s->line);
                spec_slot = ST_ct_declare_local(cc, s->for_array.spec_iter);
            }

            u32 loop_start = cc->chunk->count;
            ST_ct_emit_op_u32(cc->chunk, ST_OP_GET_LOCAL, idx_slot, s->line);
            ST_ct_emit_op_u32(cc->chunk, ST_OP_GET_LOCAL, arr_slot, s->line);
            ST_ct_emit_op(cc->chunk, ST_OP_STR_LEN, s->line);
            ST_ct_emit_op(cc->chunk, ST_OP_LT, s->line);
            u32 exit_jump = ST_ct_emit_jump(cc->chunk, ST_OP_JMP_IF_FALSE, s->line);

            if (cc->n_loops >= ST_array_len(cc->loop_stack)) {
                ST_ct_cfail(cc, s->line, s->col,
                            "comptime: loops nested too deeply in a #comptime scope");
                return;
            }
            ST_ct_loop_ctx_t *loop = &cc->loop_stack[cc->n_loops++];
            loop->n_break_patches = 0;
            loop->n_continue_patches = 0;

            ST_ct_emit_op_u32(cc->chunk, ST_OP_GET_LOCAL, arr_slot, s->line);
            ST_ct_emit_op_u32(cc->chunk, ST_OP_GET_LOCAL, idx_slot, s->line);
            // A string iterates byte-by-byte via ST_OP_STR_INDEX (it's never a
            // boxed ST_CT_STRUCT); anything else (array/slice/dyn_array/struct)
            // uses ST_OP_STRUCT_INDEX, same dispatch ST_EX_INDEX already does.
            b8 is_string_target = s->for_array.target->ty &&
                                  s->for_array.target->ty->kind == ST_TY_STRING;
            ST_ct_emit_op(cc->chunk, is_string_target ? ST_OP_STR_INDEX : ST_OP_STRUCT_INDEX,
                         s->line);
            ST_ct_emit_op_u32(cc->chunk, ST_OP_SET_LOCAL, iter_slot, s->line);
            ST_ct_emit_op(cc->chunk, ST_OP_POP, s->line);

            if (has_spec) {
                ST_ct_emit_op_u32(cc->chunk, ST_OP_GET_LOCAL, idx_slot, s->line);
                ST_ct_emit_op_u32(cc->chunk, ST_OP_SET_LOCAL, spec_slot, s->line);
                ST_ct_emit_op(cc->chunk, ST_OP_POP, s->line);
            }

            loop->body_start_locals = cc->n_locals;
            ST_ct_compile_scoped(cc, &s->for_array.body);

            if (!cc->failed) {
                u32 continue_point = cc->chunk->count;
                ST_forrange(0, loop->n_continue_patches)
                    ST_ct_patch_jump_to(cc->chunk, loop->continue_patches[i], continue_point);

                ST_ct_emit_op_u32(cc->chunk, ST_OP_GET_LOCAL, idx_slot, s->line);
                ST_ct_emit_const(cc->chunk, ST_ct_int(1), s->line);
                ST_ct_emit_op(cc->chunk, ST_OP_ADD, s->line);
                ST_ct_emit_op_u32(cc->chunk, ST_OP_SET_LOCAL, idx_slot, s->line);
                ST_ct_emit_op(cc->chunk, ST_OP_POP, s->line);

                ST_ct_emit_loop(cc->chunk, loop_start, s->line);
                ST_ct_patch_jump(cc->chunk, exit_jump);
                ST_forrange(0, loop->n_break_patches)
                    ST_ct_patch_jump(cc->chunk, loop->break_patches[i]);

                ST_ct_emit_op(cc->chunk, ST_OP_POP, s->line); // drop iter
                if (has_spec)
                    ST_ct_emit_op(cc->chunk, ST_OP_POP, s->line); // drop spec_iter
                ST_ct_emit_op(cc->chunk, ST_OP_POP, s->line); // drop $for_idx
                ST_ct_emit_op(cc->chunk, ST_OP_POP, s->line); // drop $for_arr
            }
            cc->n_loops--;
            cc->n_locals = saved_locals;
            return;
        }

        case ST_ST_BLOCK:
            ST_ct_compile_scoped(cc, &s->block);
            return;

        case ST_ST_RETURN: {
            if (s->ret.values.count == 0) {
                ST_ct_emit_op(cc->chunk, ST_OP_HALT, s->line);
                return;
            }
            if (s->ret.values.count > 8) {
                ST_ct_cfail(cc, s->line, s->col,
                            "comptime: too many return values in a #comptime scope (max 8)");
                return;
            }
            ST_forrange(0, s->ret.values.count) {
                ST_expr_t *rv = s->ret.values.items[i];
                ST_string_t struct_type = (ST_string_t){0};
                if (rv->ty && rv->ty->kind == ST_TY_STRUCT && rv->ty->decl)
                    struct_type = rv->ty->decl->name;
                else if (cc->prog_ctx)
                    struct_type = ST_ct_expr_struct_type(cc, rv);
                if (struct_type.len) {
                    if (!ST_ct_compile_struct_rvalue(cc, rv, struct_type))
                        return;
                } else {
                    ST_ct_compile_expr(cc, rv);
                    if (cc->failed)
                        return;
                }
            }
            ST_ct_emit_op_u32(cc->chunk, ST_OP_RETURN, s->ret.values.count, s->line);
            return;
        }

        case ST_ST_MULTI_BIND: {
            if (!s->multi.declare) {
                ST_ct_cfail(cc, s->line, s->col,
                            "comptime: multi-assign to existing locals isn't supported in a "
                            "#comptime scope yet (only 'a, b := f()')");
                return;
            }
            if (s->multi.values.count != 1 || s->multi.values.items[0]->kind != ST_EX_CALL) {
                ST_ct_cfail(cc, s->line, s->col,
                            "comptime: a #comptime multi-bind must come from a single "
                            "function call ('a, b := f()')");
                return;
            }
            ST_decl_t *callee_decl = ST_ct_compile_plain_call(cc, s->multi.values.items[0]);
            if (!callee_decl)
                return;
            if (ST_ct_decl_needs_native(callee_decl)) {
                ST_ct_cfail(cc, s->line, s->col,
                            "comptime: '" ST_sv_fmt "' is implemented in assembly and only "
                            "supports a single return value, can't multi-bind it",
                            ST_sv_args(callee_decl->name));
                return;
            }
            u32 n_rets = callee_decl->fn.sig.rets.count;
            if (n_rets != s->multi.n_names) {
                ST_ct_cfail(cc, s->line, s->col,
                            "comptime: call returns %u value%s, but %u name%s bound", n_rets,
                            n_rets == 1 ? "" : "s", s->multi.n_names,
                            s->multi.n_names == 1 ? " is" : "s are");
                return;
            }
            // The call already left n_rets values on the stack in order;
            // declaring a local per name just records "the value already
            // sitting here now has this name"
            ST_forrange(0, s->multi.n_names) {
                ST_ct_declare_local(cc, s->multi.names[i]);
                if (i < callee_decl->fn.sig.rets.count) {
                    ST_tyexpr_t *rte = callee_decl->fn.sig.rets.items[i];
                    if (rte && rte->kind == ST_TE_NAME && cc->prog_ctx &&
                        ST_ct_prog_find_struct(cc->prog_ctx, rte->name)) {
                        ST_ct_local_set_struct_type(cc, rte->name);
                        ST_ct_local_set_is_buffer(cc);
                    }
                }
            }
            return;
        }

        case ST_ST_ASM: {
            if (!ST_ct_compile_syscall_asm(cc, s->asm_.tokens, s->asm_.n_tokens, s->line, s->col))
                return;
            ST_ct_emit_op(cc->chunk, ST_OP_POP, s->line); // statement result is discarded
            return;
        }

        case ST_ST_BREAK: {
            if (cc->n_loops == 0) {
                ST_ct_cfail(cc, s->line, s->col, "comptime: 'break' outside of a loop");
                return;
            }
            ST_ct_loop_ctx_t *loop = &cc->loop_stack[cc->n_loops - 1];
            if (loop->n_break_patches >= ST_array_len(loop->break_patches)) {
                ST_ct_cfail(cc, s->line, s->col, "comptime: too many 'break's in one loop");
                return;
            }
            ST_forrange(loop->body_start_locals, cc->n_locals)
                ST_ct_emit_op(cc->chunk, ST_OP_POP, s->line);
            loop->break_patches[loop->n_break_patches++] =
                ST_ct_emit_jump(cc->chunk, ST_OP_JMP, s->line);
            return;
        }

        case ST_ST_CONTINUE: {
            if (cc->n_loops == 0) {
                ST_ct_cfail(cc, s->line, s->col, "comptime: 'continue' outside of a loop");
                return;
            }
            ST_ct_loop_ctx_t *loop = &cc->loop_stack[cc->n_loops - 1];
            if (loop->n_continue_patches >= ST_array_len(loop->continue_patches)) {
                ST_ct_cfail(cc, s->line, s->col, "comptime: too many 'continue's in one loop");
                return;
            }
            ST_forrange(loop->body_start_locals, cc->n_locals)
                ST_ct_emit_op(cc->chunk, ST_OP_POP, s->line);
            loop->continue_patches[loop->n_continue_patches++] =
                ST_ct_emit_jump(cc->chunk, ST_OP_JMP, s->line);
            return;
        }

	case ST_ST_SWITCH: {
	    u32 saved_locals = cc->n_locals;

	    ST_ct_compile_expr(cc, s->switch_.cond);
	    if (cc->failed) return;
	    u32 cond_slot = ST_ct_declare_local(cc, ST_cstr_to_str("$switch_cond"));

	    u32 n_cases = s->switch_.cases.count;
	    u32 *end_jumps = n_cases ? ST_arena_push(cc->arena, sizeof(u32) * n_cases) : NULL;
	    u32 n_end_jumps = 0;
	    ST_case_t *default_case = NULL;

	    for (u32 i = 0; i < n_cases; i++) {
		ST_case_t *c = &s->switch_.cases.items[i];
		if (c->values.count == 0) {
		    default_case = c;
		    continue;
		}

		u32 *to_body_jumps = ST_arena_push(cc->arena, sizeof(u32) * c->values.count);
		u32 n_to_body = 0;
		for (u32 k = 0; k < c->values.count; k++) {
		    ST_ct_emit_op_u32(cc->chunk, ST_OP_GET_LOCAL, cond_slot, c->line);
		    ST_ct_compile_expr(cc, c->values.items[k]);
		    if (cc->failed)
                    return;
		    ST_ct_emit_op(cc->chunk, ST_OP_EQ, c->line);
		    u32 fail_jump = ST_ct_emit_jump(cc->chunk, ST_OP_JMP_IF_FALSE, c->line);
		    to_body_jumps[n_to_body++] = ST_ct_emit_jump(cc->chunk, ST_OP_JMP, c->line);
		    ST_ct_patch_jump(cc->chunk, fail_jump);
		}

		u32 skip_body_jump = ST_ct_emit_jump(cc->chunk, ST_OP_JMP, c->line);
		for (u32 k = 0; k < n_to_body; k++)
		ST_ct_patch_jump(cc->chunk, to_body_jumps[k]);

		ST_ct_compile_scoped(cc, &c->body);
		if (cc->failed)
		return;
		end_jumps[n_end_jumps++] = ST_ct_emit_jump(cc->chunk, ST_OP_JMP, c->line);

		ST_ct_patch_jump(cc->chunk, skip_body_jump);
	    }

	    if (default_case) {
		ST_ct_compile_scoped(cc, &default_case->body);
		if (cc->failed)
		return;
	    }

	    for (u32 i = 0; i < n_end_jumps; i++)
            ST_ct_patch_jump(cc->chunk, end_jumps[i]);

	    ST_ct_emit_op(cc->chunk, ST_OP_POP, s->line);
	    cc->n_locals = saved_locals;
	    return;
	}

        default: {
            static const char *kind_names[] = {
                "expr", "decl", "assign", "multi_bind", "if", "while", "for_range",
                "for_array", "return", "block", "defer", "break", "continue", "label",
                "godown", "asm", "comptime_block", "pack_expand",
            };
            const char *nm = (s->kind >= 0 &&
                              (u32)s->kind < sizeof(kind_names) / sizeof(kind_names[0]))
                                  ? kind_names[s->kind]
                                  : "?";
            ST_ct_cfail(cc, s->line, s->col,
                        "comptime: this statement form ('%s', kind %d) isn't supported in a "
                        "#comptime scope yet",
                        nm, (int)s->kind);
            return;
        }
    }
}

void ST_ct_compile_block(ST_ct_compiler_t *cc, ST_stmts_t *body) {
    ST_ct_compile_scoped(cc, body);
    if (!cc->failed)
        ST_ct_emit_op(cc->chunk, ST_OP_HALT, 0);
}

static ST_ct_fn_entry_t *ST_ct_prog_find_fn(ST_ct_prog_ctx_t *pctx, ST_string_t name) {
    ST_forrange(0, pctx->n_fns)
        if (ST_string_eq(pctx->fns[i].name, name))
            return &pctx->fns[i];
    return NULL;
}

static ST_decl_t *ST_ct_prog_find_decl(ST_ct_prog_ctx_t *pctx, ST_string_t name, b8 *not_callable) {
    *not_callable = 0;
    ST_forrange(0, pctx->prog->decls.count) {
        ST_decl_t *d = pctx->prog->decls.items[i];
        if (!d || d->kind != ST_DE_FN || !ST_string_eq(d->name, name))
            continue;
        if (d->fn.sig.generics.count || d->fn.sig.has_any_pack || d->fn.sig.has_generic_pack ||
            d->fn.sig.is_variadic) {
            *not_callable = 1;
            return NULL;
        }
        return d;
    }
    return NULL;
}

static ST_decl_t *ST_ct_prog_find_extern(ST_ct_prog_ctx_t *pctx, ST_string_t name) {
    ST_forrange(0, pctx->prog->decls.count) {
        ST_decl_t *d = pctx->prog->decls.items[i];
        if (d && d->kind == ST_DE_EXTERN_FN && ST_string_eq(d->name, name))
            return d;
    }
    return NULL;
}

static ST_decl_t *ST_ct_prog_find_struct(ST_ct_prog_ctx_t *pctx, ST_string_t name) {
    ST_forrange(0, pctx->prog->decls.count) {
        ST_decl_t *d = pctx->prog->decls.items[i];
        if (d && d->kind == ST_DE_STRUCT && ST_string_eq(d->name, name))
            return d;
    }
    return NULL;
}

static ST_decl_t *ST_ct_prog_find_type_alias(ST_ct_prog_ctx_t *pctx, ST_string_t name) {
    ST_forrange(0, pctx->prog->decls.count) {
        ST_decl_t *d = pctx->prog->decls.items[i];
        if (d && d->kind == ST_DE_TYPE_ALIAS && ST_string_eq(d->name, name))
            return d;
    }
    return NULL;
}

static ST_decl_t *ST_ct_prog_find_enum(ST_ct_prog_ctx_t *pctx, ST_string_t name) {
    ST_forrange(0, pctx->prog->decls.count) {
        ST_decl_t *d = pctx->prog->decls.items[i];
        if (d && d->kind == ST_DE_ENUM && ST_string_eq(d->name, name))
            return d;
    }
    return NULL;
}

static ST_decl_t *ST_ct_prog_find_const(ST_ct_prog_ctx_t *pctx, ST_string_t name) {
    ST_forrange(0, pctx->prog->decls.count) {
        ST_decl_t *d = pctx->prog->decls.items[i];
        if (d && d->kind == ST_DE_CONST && ST_string_eq(d->name, name))
            return d;
    }
    return NULL;
}

static i32 ST_ct_struct_field_index(ST_decl_t *struct_decl, ST_string_t field_name) {
    ST_forrange(0, struct_decl->struct_.fields.count)
        if (ST_string_eq(struct_decl->struct_.fields.items[i].name, field_name))
            return (i32)i;
    return -1;
}

static u32 ST_ct_field_byte_size(ST_tyexpr_t *te) {
    if (!te || te->kind != ST_TE_NAME)
        return 8;
    ST_string_t n = te->name;
    if (ST_string_eq_cstr(n, "u8") || ST_string_eq_cstr(n, "i8") ||
        ST_string_eq_cstr(n, "char") || ST_string_eq_cstr(n, "bool"))
        return 1;
    if (ST_string_eq_cstr(n, "u16") || ST_string_eq_cstr(n, "i16"))
        return 2;
    if (ST_string_eq_cstr(n, "u32") || ST_string_eq_cstr(n, "i32") ||
        ST_string_eq_cstr(n, "f32"))
        return 4;
    return 8;
}

// Unlike ST_ct_field_byte_size (which only needs a rough width for packing
// a value into an extern-call register and defaults to 8 for anything it
// doesn't recognize), this has to get the SIZE RIGHT: it decides how many
// bytes ST_OP_ALLOC_ZEROED reserves for a zero-initialized local, and a
// native call may write through that memory (e.g. pipe(2) filling in an
// 'out_pipe : [2]i32;'). Returns 0 (failure) rather than guessing when it
// doesn't know a type, since a guessed-too-small buffer is a real,
// silent-corruption bug, not just a cosmetic one.
static b8 ST_ct_tyexpr_byte_size(ST_ct_compiler_t *cc, ST_tyexpr_t *te, u32 *out_size) {
    if (!te)
        return 0;
    switch (te->kind) {
        case ST_TE_PTR:
            *out_size = 8;
            return 1;
        case ST_TE_ARRAY: {
            if (te->is_dynamic) {
                // A dyn_array is always {items: ptr, count: i64, capacity: i64}
                // regardless of element type
                *out_size = 24;
                return 1;
            }
            if (!te->count_expr || !cc->prog_ctx || !cc->prog_ctx->sema) {
                // No count_expr and not dynamic: a slice ('[]T'. {ptr, len}).
                if (!te->count_expr && !te->is_dynamic) {
                    *out_size = 16;
                    return 1;
                }
                return 0;
            }
            i64 count;
            if (!ST_const_eval(cc->prog_ctx->sema, te->count_expr, &count) || count < 0)
                return 0;
            u32 elem_size;
            if (!ST_ct_tyexpr_byte_size(cc, te->inner, &elem_size))
                return 0;
            *out_size = (u32)count * elem_size;
            return 1;
        }
        case ST_TE_NAME: {
            static const struct { const char *name; u32 size; } prims[] = {
                {"i8", 1}, {"u8", 1}, {"bool", 1}, {"char", 1},
                {"i16", 2}, {"u16", 2},
                {"i32", 4}, {"u32", 4}, {"f32", 4},
                {"i64", 8}, {"u64", 8}, {"f64", 8},
                {"f128", 16}, {"string", 16}, {"any", 16},
            };
            ST_forrange(0, sizeof(prims) / sizeof(prims[0]))
                if (ST_string_eq_cstr(te->name, prims[i].name)) {
                    *out_size = prims[i].size;
                    return 1;
                }
            if (!cc->prog_ctx)
                return 0;
            ST_decl_t *sd = ST_ct_prog_find_struct(cc->prog_ctx, te->name);
            if (sd) {
                u32 total = 0;
                ST_forrange(0, sd->struct_.fields.count) {
                    u32 fs;
                    if (!ST_ct_tyexpr_byte_size(cc, sd->struct_.fields.items[i].te, &fs))
                        return 0;
                    total += fs;
                }
                *out_size = total;
                return 1;
            }
            ST_decl_t *ad = ST_ct_prog_find_type_alias(cc->prog_ctx, te->name);
            if (ad)
                return ST_ct_tyexpr_byte_size(cc, ad->type_alias.te, out_size);
            return 0;
        }
        default:
            return 0;
    }
}

// Like ST_ct_tyexpr_byte_size but also reports signedness, for a recognized
// primitive int type only used to fill in a buffer-backed array local's
// elem_size/elem_is_signed so 'arr[i]'/'&arr[i]' can be compiled.
static b8 ST_ct_tyexpr_int_info(ST_tyexpr_t *te, u32 *width, b8 *is_signed) {
    if (!te || te->kind != ST_TE_NAME)
        return 0;
    static const struct { const char *name; u32 size; b8 is_signed; } ints[] = {
        {"i8", 1, 1}, {"u8", 1, 0}, {"char", 1, 0}, {"bool", 1, 0},
        {"i16", 2, 1}, {"u16", 2, 0},
        {"i32", 4, 1}, {"u32", 4, 0},
        {"i64", 8, 1}, {"u64", 8, 0},
    };
    ST_forrange(0, sizeof(ints) / sizeof(ints[0]))
        if (ST_string_eq_cstr(te->name, ints[i].name)) {
            *width = ints[i].size;
            *is_signed = ints[i].is_signed;
            return 1;
        }
    return 0;
}

// A genuine slice ('[]T', as opposed to a fixed-size array '[N]T' or a
// dyn_array '[..]T') of a recognized primitive int element. Its VM
// representation is a bare ST_CT_PTR with NO length attached (unlike a
// zero-initialized fixed array, whose length is a compile-time constant, or
// a dyn_array, which carries its own length in real memory).
static b8 ST_ct_tyexpr_is_int_slice(ST_tyexpr_t *te, u32 *width, b8 *is_signed) {
    if (!te || te->kind != ST_TE_ARRAY || te->is_dynamic || te->count_expr)
        return 0;
    return ST_ct_tyexpr_int_info(te->inner, width, is_signed);
}

// Same idea as ST_ct_tyexpr_int_info but works from a real, resolved ST_ty_t
// (post-generic-instantiation, e.g. a bound '$T') instead of a raw type
// expression used for indexing through an actual pointer value ('base:
// *T'), where the representation is unambiguously ST_CT_PTR (unlike an
// array/slice type, which could be either a real pointer or a boxed
// ST_CT_STRUCT depending on how that particular value was built).
static b8 ST_ct_ty_int_info(ST_ty_t *t, u32 *width, b8 *is_signed) {
    if (!t)
        return 0;
    switch (t->kind) {
        case ST_TY_INT: *width = t->width / 8; *is_signed = t->is_signed; return 1;
        case ST_TY_UNTYPED_INT: *width = 8; *is_signed = 1; return 1;
        case ST_TY_CHAR: *width = 1; *is_signed = 0; return 1;
        case ST_TY_BOOL: *width = 1; *is_signed = 0; return 1;
        default: return 0;
    }
}

// The base of a field-access or a call argument is struct-typed in three
// shapes this file can see all the way through: a local declared straight
// from a struct literal (tracked via ST_ct_local_struct_type), a global
// 'NAME :: Color {...}' const whose value is a struct literal, or a
// struct literal used directly inline.
static ST_string_t ST_ct_expr_struct_type(ST_ct_compiler_t *cc, ST_expr_t *e) {
    if (e->kind == ST_EX_IDENT) {
        ST_string_t local_type = ST_ct_local_struct_type(cc, e->name);
        if (local_type.len)
            return local_type;
        if (cc->prog_ctx) {
            ST_decl_t *cd = ST_ct_prog_find_const(cc->prog_ctx, e->name);
            if (cd && cd->const_.is_comptime && cd->const_.value->kind == ST_EX_STRUCT_LIT)
                return cd->const_.value->struct_lit.type_name;
        }
        return (ST_string_t){0};
    }
    if (e->kind == ST_EX_STRUCT_LIT)
        return e->struct_lit.type_name;
    return (ST_string_t){0};
}

static void ST_ct_prog_fail(ST_ct_prog_ctx_t *pctx, u32 line, u32 col, const char *fmt, ...) {
    if (pctx->failed)
        return;
    pctx->failed = 1;
    pctx->err_line = line;
    pctx->err_col = col;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(pctx->err_msg, sizeof(pctx->err_msg), fmt, ap);
    va_end(ap);
}

static b8 ST_ct_prog_want_call(ST_ct_prog_ctx_t *pctx, ST_string_t callee_name, u32 patch_off,
                               u32 line, u32 col) {
    if (pctx->failed)
        return 0;

    ST_ct_fn_entry_t *entry = ST_ct_prog_find_fn(pctx, callee_name);
    if (!entry) {
        b8 not_callable = 0;
        ST_decl_t *d = ST_ct_prog_find_decl(pctx, callee_name, &not_callable);
        if (!d) {
            if (not_callable)
                ST_ct_prog_fail(pctx, line, col,
                                "comptime: '" ST_sv_fmt "' has generics, packs, or variadic "
                                "args, which aren't supported in a #comptime call yet",
                                ST_sv_args(callee_name));
            else
                ST_ct_prog_fail(pctx, line, col,
                                "comptime: no plain function named '" ST_sv_fmt
                                "' found to call",
                                ST_sv_args(callee_name));
            return 0;
        }
        if (pctx->n_fns >= ST_CT_MAX_PROG_FNS) {
            ST_ct_prog_fail(pctx, line, col,
                            "comptime: too many functions reachable from this #comptime entry "
                            "point (max %u)",
                            (u32)ST_CT_MAX_PROG_FNS);
            return 0;
        }
        entry = &pctx->fns[pctx->n_fns++];
        entry->name = callee_name;
        entry->decl = d;
        entry->queued = 0;
        entry->compiled = 0;
        entry->entry_ip = 0;
    }
    if (!entry->queued) {
        if (pctx->n_worklist >= ST_CT_MAX_PROG_FNS) {
            ST_ct_prog_fail(pctx, line, col, "comptime: too many pending functions to compile");
            return 0;
        }
        pctx->worklist[pctx->n_worklist++] = entry->decl;
        entry->queued = 1;
    }

    if (pctx->n_pending >= ST_CT_MAX_PENDING_CALLS) {
        ST_ct_prog_fail(pctx, line, col, "comptime: too many pending calls to patch");
        return 0;
    }
    ST_ct_pending_call_t *p = &pctx->pending[pctx->n_pending++];
    p->operand_offset = patch_off;
    p->callee_name = callee_name;
    p->line = line;
    p->col = col;
    return 1;
}

b8 ST_ct_compile_program(ST_arena_t *arena, ST_ct_chunk_t *chunk, ST_program_t *prog,
                         ST_sema_t *sema, ST_string_t src, ST_string_t file,
                         ST_decl_t *entry_decl, u32 *out_entry_ip, u32 *err_line, u32 *err_col,
                         ST_string_t *err_file, char *err_msg, u32 err_msg_cap) {
    ST_ct_prog_ctx_t pctx = {0};
    pctx.arena = arena;
    pctx.chunk = chunk;
    pctx.prog = prog;
    pctx.sema = sema;
    pctx.src = src;
    pctx.file = file;
    pctx.err_file = file;

    pctx.fns[0].name = entry_decl->name;
    pctx.fns[0].decl = entry_decl;
    pctx.fns[0].queued = 1;
    pctx.n_fns = 1;
    pctx.worklist[0] = entry_decl;
    pctx.n_worklist = 1;

    u32 head = 0;
    while (head < pctx.n_worklist && !pctx.failed) {
        ST_decl_t *d = pctx.worklist[head++];
        ST_ct_fn_entry_t *entry = ST_ct_prog_find_fn(&pctx, d->name);
        if (entry->compiled)
            continue;

        if (d == entry_decl && d->fn.sig.params.count > 0) {
            ST_ct_prog_fail(&pctx, d->line, d->col,
                            "comptime: a '#comptime' entry point (like main) cannot take "
                            "parameters");
            break;
        }

        entry->entry_ip = chunk->count;
        entry->compiled = 1;
        ST_ct_chunk_mark_file(chunk, d->file);

        ST_ct_compiler_t cc;
        ST_ct_compiler_init(&cc, arena, chunk);
        cc.prog_ctx = &pctx;
        cc.cur_file = d->file;

        if (d != entry_decl)
            ST_forrange(0, d->fn.sig.params.count) {
                ST_param_t *p = &d->fn.sig.params.items[i];
                ST_ct_declare_local(&cc, p->name);
                if (p->te && p->te->kind == ST_TE_NAME) {
                    ST_decl_t *psd = ST_ct_prog_find_struct(&pctx, p->te->name);
                    if (psd) {
                        ST_ct_local_set_is_buffer(&cc);
                        ST_ct_local_set_struct_type(&cc, p->te->name);
                    }
                }
                // A slice/array/dyn_array-typed parameter of a recognized primitive
                // int element is assumed to arrive as a real pointer (the caller is
                // the one that actually allocated the backing storage, e.g. a
                // zero-initialized local passed by a caller), so 'param[i]' and
                // 'param[i] = value;' both need to know its element width/signedness
                // the same way a zero-initialized local does.
                if (p->te && p->te->kind == ST_TE_ARRAY) {
                    u32 ew;
                    b8 es;
                    if (ST_ct_tyexpr_int_info(p->te->inner, &ew, &es)) {
                        ST_ct_local_set_is_buffer(&cc);
                        ST_ct_local_set_elem_info(&cc, ew, es);
                    }
                }
                // A genuine slice ('[]T', not '[N]T' or '[..]T') carries no length
                // in its own bare-pointer VM value at all.
                u32 sw;
                b8 ssig;
                if (p->te && ST_ct_tyexpr_is_int_slice(p->te, &sw, &ssig)) {
                    u32 len_slot = ST_ct_declare_local(&cc, ST_cstr_to_str("$slice_len"));
                    ST_ct_local_t *param_local = ST_ct_find_local_info(&cc, p->name);
                    if (param_local)
                        param_local->len_slot = (i32)len_slot;
                } else if (p->te && p->te->kind == ST_TE_ARRAY && p->te->count_expr) {
                    // A fixed-size array parameter's length, unlike a slice's, is
                    // right there in its own declared type.
                    i64 count;
                    if (ST_const_eval(pctx.sema, p->te->count_expr, &count) && count >= 0)
                        ST_ct_local_set_known_len(&cc, (u32)count);
                }
            }

        ST_ct_compile_block(&cc, &d->fn.body);

        if (cc.failed) {
            pctx.failed = 1;
            pctx.err_line = cc.err_line;
            pctx.err_col = cc.err_col;
            pctx.err_file = cc.err_file.len ? cc.err_file : d->file;
            memcpy(pctx.err_msg, cc.err_msg, sizeof(pctx.err_msg));
        }
    }

    if (!pctx.failed) {
        ST_forrange(0, pctx.n_pending) {
            ST_ct_pending_call_t *p = &pctx.pending[i];
            ST_ct_fn_entry_t *entry = ST_ct_prog_find_fn(&pctx, p->callee_name);
            if (!entry || !entry->compiled) {
                ST_ct_prog_fail(&pctx, p->line, p->col,
                                "internal: comptime call to '" ST_sv_fmt
                                "' never got compiled",
                                ST_sv_args(p->callee_name));
                break;
            }
            ST_ct_patch_call(chunk, p->operand_offset, entry->entry_ip);
        }
    }

    if (pctx.failed) {
        if (err_line)
            *err_line = pctx.err_line;
        if (err_col)
            *err_col = pctx.err_col;
        if (err_file)
            *err_file = pctx.err_file;
        if (err_msg && err_msg_cap) {
            u32 n = (u32)strlen(pctx.err_msg);
            if (n >= err_msg_cap)
                n = err_msg_cap - 1;
            memcpy(err_msg, pctx.err_msg, n);
            err_msg[n] = 0;
        }
        return 0;
    }

    if (out_entry_ip)
        *out_entry_ip = pctx.fns[0].entry_ip;
    return 1;
}
