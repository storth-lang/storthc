#include "st_comptime_compile.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

void ST_ct_compiler_init(ST_ct_compiler_t *cc, ST_arena_t *arena, ST_ct_chunk_t *chunk) {
    memset(cc, 0, sizeof(*cc));
    cc->arena = arena;
    cc->chunk = chunk;
}

static b8 ST_ct_prog_want_call(ST_ct_prog_ctx_t *pctx, ST_string_t callee_name, u32 patch_off,
                               u32 line, u32 col);
static ST_decl_t *ST_ct_prog_find_extern(ST_ct_prog_ctx_t *pctx, ST_string_t name);
static ST_decl_t *ST_ct_prog_find_struct(ST_ct_prog_ctx_t *pctx, ST_string_t name);
static ST_decl_t *ST_ct_prog_find_const(ST_ct_prog_ctx_t *pctx, ST_string_t name);
static ST_decl_t *ST_ct_prog_find_decl(ST_ct_prog_ctx_t *pctx, ST_string_t name, b8 *not_callable);
static i32 ST_ct_struct_field_index(ST_decl_t *struct_decl, ST_string_t field_name);
static u32 ST_ct_field_byte_size(ST_tyexpr_t *te);
static ST_string_t ST_ct_expr_struct_type(ST_ct_compiler_t *cc, ST_expr_t *e);
static ST_decl_t *ST_ct_compile_plain_call(ST_ct_compiler_t *cc, ST_expr_t *call_expr);
static b8 ST_ct_compile_syscall_asm(ST_ct_compiler_t *cc, ST_token_t *tokens, u32 n_tokens,
                                    u32 line, u32 col);

static void ST_ct_cfail(ST_ct_compiler_t *cc, u32 line, u32 col, const char *fmt, ...) {
    if (cc->failed)
        return;
    cc->failed = 1;
    cc->err_line = line;
    cc->err_col = col;
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
    cc->n_locals++;
    return slot;
}

static void ST_ct_local_set_struct_type(ST_ct_compiler_t *cc, ST_string_t struct_type) {
    if (cc->n_locals > 0)
        cc->locals[cc->n_locals - 1].struct_type = struct_type;
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
            ST_decl_t *sd = ST_ct_prog_find_struct(cc->prog_ctx, e->struct_lit.type_name);
            if (!sd) {
                ST_ct_cfail(cc, e->line, e->col,
                            "comptime: no struct named '" ST_sv_fmt "' found",
                            ST_sv_args(e->struct_lit.type_name));
                return;
            }
            u32 n_fields = sd->struct_.fields.count;
            if (n_fields > 64) {
                ST_ct_cfail(cc, e->line, e->col,
                            "comptime: struct '" ST_sv_fmt "' has too many fields for a "
                            "#comptime literal (max 64)",
                            ST_sv_args(e->struct_lit.type_name));
                return;
            }
            ST_expr_t *field_exprs[64] = {0};
            ST_forrange(0, e->struct_lit.inits.count) {
                ST_field_init_t *fi = &e->struct_lit.inits.items[i];
                i32 idx = fi->name.len ? ST_ct_struct_field_index(sd, fi->name) : (i32)i;
                if (idx < 0 || (u32)idx >= n_fields) {
                    ST_ct_cfail(cc, fi->line, fi->col,
                                "comptime: '" ST_sv_fmt "' has no such field",
                                ST_sv_args(e->struct_lit.type_name));
                    return;
                }
                field_exprs[idx] = fi->value;
            }
            ST_forrange(0, n_fields) {
                if (!field_exprs[i]) {
                    ST_ct_cfail(cc, e->line, e->col,
                                "comptime: struct literal for '" ST_sv_fmt "' is missing a "
                                "field",
                                ST_sv_args(e->struct_lit.type_name));
                    return;
                }
                ST_ct_compile_expr(cc, field_exprs[i]);
                if (cc->failed)
                    return;
            }
            ST_ct_emit_op_u32(cc->chunk, ST_OP_MAKE_STRUCT, n_fields, e->line);
            return;
        }

        case ST_EX_IDENT: {
            i32 slot = ST_ct_find_local(cc, e->name);
            if (slot >= 0) {
                ST_ct_emit_op_u32(cc->chunk, ST_OP_GET_LOCAL, (u32)slot, e->line);
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
                                "'" ST_sv_fmt "' isn't usable in a #comptime scope -- add "
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
            // A struct-typed base's field access is resolved purely from
            // ST_ct_expr_struct_type (which covers locals, consts, and inline
            // literals)
            if (cc->prog_ctx) {
                ST_string_t struct_type = ST_ct_expr_struct_type(cc, e->field.base);
                if (struct_type.len) {
                    ST_decl_t *sd = ST_ct_prog_find_struct(cc->prog_ctx, struct_type);
                    if (!sd) {
                        ST_ct_cfail(cc, e->line, e->col,
                                    "comptime: no struct named '" ST_sv_fmt "' found",
                                    ST_sv_args(struct_type));
                        return;
                    }
                    i32 fidx = ST_ct_struct_field_index(sd, e->field.name);
                    if (fidx < 0) {
                        ST_ct_cfail(cc, e->line, e->col,
                                    "comptime: '" ST_sv_fmt "' has no field '" ST_sv_fmt "'",
                                    ST_sv_args(struct_type), ST_sv_args(e->field.name));
                        return;
                    }
                    ST_ct_compile_expr(cc, e->field.base);
                    if (cc->failed)
                        return;
                    ST_ct_emit_op_u32(cc->chunk, ST_OP_GET_FIELD, (u32)fidx, e->line);
                    return;
                }
            }

            if (!ST_string_eq_cstr(e->field.name, "len")) {
                ST_ct_cfail(cc, e->line, e->col,
                            "comptime: '.'" ST_sv_fmt "' isn't comptime-evaluable "
                            "(only '.len' on a string, and fields on a struct-typed "
                            "local/const, are right now)",
                            ST_sv_args(e->field.name));
                return;
            }
            ST_ct_compile_expr(cc, e->field.base);
            ST_ct_emit_op(cc->chunk, ST_OP_STR_LEN, e->line);
            return;
        }

        case ST_EX_INDEX:
            ST_ct_compile_expr(cc, e->index.base);
            ST_ct_compile_expr(cc, e->index.index);
            ST_ct_emit_op(cc->chunk, ST_OP_STR_INDEX, e->line);
            return;

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
                name = "slice"; // []T for any T; distinct from "dyn_array" since it has
                                // ptr/len fields (no capacity), not items/count/capacity
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
                u32 n_params = extern_decl->extern_fn.sig.params.count;
                if (e->call.args.count > n_params) {
                    ST_ct_cfail(cc, e->line, e->col,
                                "comptime: '" ST_sv_fmt "' expects at most %u argument%s, "
                                "got %u",
                                ST_sv_args(callee_name), n_params, n_params == 1 ? "" : "s",
                                e->call.args.count);
                    return;
                }
                ST_forrange(0, n_params) {
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
                ST_ct_emit_const(cc->chunk, ST_ct_int((i64)n_params), e->line);
                ST_ct_emit_op(cc->chunk, ST_OP_BIND_SYM, e->line);
                ST_ct_emit_op(cc->chunk, ST_OP_CALL_NATIVE, e->line);
                return;
            }

            ST_decl_t *callee_decl = ST_ct_compile_plain_call(cc, e);
            if (!callee_decl)
                return;
            if (callee_decl->fn.sig.rets.count != 1) {
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

// Compiles a call to a plain (non-extern) Storth function: resolves the
// callee, fills in any missing trailing arguments from their declared
// defaults, compiles the (now-complete) argument list, emits the CALL,
// and registers it with ST_ct_prog_want_call. Leaves exactly
// callee_decl->fn.sig.rets.count values on the stack once the callee
// returns. Returns the callee decl on success, NULL on failure (with
// cc->failed already set).
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

    u32 n_params = callee_decl->fn.sig.params.count;
    if (call_expr->call.args.count > n_params) {
        ST_ct_cfail(cc, call_expr->line, call_expr->col,
                    "comptime: '" ST_sv_fmt "' expects at most %u argument%s, got %u",
                    ST_sv_args(callee_name), n_params, n_params == 1 ? "" : "s",
                    call_expr->call.args.count);
        return NULL;
    }

    ST_forrange(0, call_expr->call.args.count) {
        ST_ct_compile_expr(cc, call_expr->call.args.items[i].value);
        if (cc->failed)
            return NULL;
    }
    for (u32 k = call_expr->call.args.count; k < n_params; k++) {
        ST_expr_t *def = callee_decl->fn.sig.params.items[k].def;
        if (!def) {
            ST_ct_cfail(cc, call_expr->line, call_expr->col,
                        "comptime: '" ST_sv_fmt "' is missing argument '" ST_sv_fmt "' (it "
                        "has no default value)",
                        ST_sv_args(callee_name), ST_sv_args(callee_decl->fn.sig.params.items[k].name));
            return NULL;
        }
        ST_ct_compile_expr(cc, def);
        if (cc->failed)
            return NULL;
    }

    u32 patch_off = ST_ct_emit_call(cc->chunk, n_params, call_expr->line);
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
                ST_ct_cfail(cc, s->line, s->col,
                            "comptime: '" ST_sv_fmt "' needs an initializer in a #comptime scope "
                            "(no zero-init locals yet)",
                            ST_sv_args(s->decl.name));
                return;
            }
            // Computed before declaring the new local, using ST_ct_expr_struct_type
            // rather than checking for a direct ST_EX_STRUCT_LIT only, so this also
            // covers 'd := c;' (copying an already struct-typed local) and
            // 'd := RED;' (copying a struct-typed global const), not just a struct
            // literal written inline.
            ST_string_t struct_type = ST_ct_expr_struct_type(cc, s->decl.init);
            ST_ct_compile_expr(cc, s->decl.init); // leaves the local's value on the stack...
            if (cc->failed)
                return;
            ST_ct_declare_local(cc, s->decl.name); // ...which becomes its permanent slot
            if (struct_type.len)
                ST_ct_local_set_struct_type(cc, struct_type);
            return;
        }

        case ST_ST_ASSIGN: {
            if (s->assign.lhs->kind != ST_EX_IDENT) {
                ST_ct_cfail(cc, s->line, s->col,
                            "comptime: can only assign to a plain local name in a #comptime scope");
                return;
            }
            if (!ST_string_eq_cstr(s->assign.op, "=")) {
                ST_ct_cfail(cc, s->line, s->col,
                            "comptime: only plain '=' assignment is supported in a #comptime scope "
                            "yet (no '+=' etc)");
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
                ST_ct_compile_expr(cc, s->ret.values.items[i]);
                if (cc->failed)
                    return;
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
            // sitting here now has this name" -- nothing more to push.
            ST_forrange(0, s->multi.n_names) ST_ct_declare_local(cc, s->multi.names[i]);
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

        default:
            ST_ct_cfail(cc, s->line, s->col,
                        "comptime: this statement form isn't supported in a #comptime scope yet");
            return;
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
                         ST_decl_t *entry_decl, u32 *out_entry_ip, u32 *err_line, u32 *err_col,
                         char *err_msg, u32 err_msg_cap) {
    ST_ct_prog_ctx_t pctx = {0};
    pctx.arena = arena;
    pctx.chunk = chunk;
    pctx.prog = prog;

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

        ST_ct_compiler_t cc;
        ST_ct_compiler_init(&cc, arena, chunk);
        cc.prog_ctx = &pctx;

        if (d != entry_decl)
            ST_forrange(0, d->fn.sig.params.count)
                ST_ct_declare_local(&cc, d->fn.sig.params.items[i].name);

        ST_ct_compile_block(&cc, &d->fn.body);

        if (cc.failed) {
            pctx.failed = 1;
            pctx.err_line = cc.err_line;
            pctx.err_col = cc.err_col;
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
