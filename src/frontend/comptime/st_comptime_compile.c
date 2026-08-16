#include "st_comptime_compile.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

void ST_ct_compiler_init(ST_ct_compiler_t *cc, ST_arena_t *arena, ST_ct_chunk_t *chunk) {
    memset(cc, 0, sizeof(*cc));
    cc->arena = arena;
    cc->chunk = chunk;
}

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
    cc->n_locals++;
    return slot;
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
        case ST_EX_NULL:
            ST_ct_emit_op(cc->chunk, ST_OP_NIL, e->line);
            return;

        case ST_EX_IDENT: {
            i32 slot = ST_ct_find_local(cc, e->name);
            if (slot < 0) {
                ST_ct_cfail(cc, e->line, e->col,
                            "'" ST_sv_fmt "' isn't a compile-time value here "
                            ". Only locals declared inside this #comptime scope are",
                            ST_sv_args(e->name));
                return;
            }
            ST_ct_emit_op_u32(cc->chunk, ST_OP_GET_LOCAL, (u32)slot, e->line);
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
            ST_ct_compile_expr(cc, e->bin.l);
            ST_ct_compile_expr(cc, e->bin.r);
            ST_string_t op = e->bin.op;
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
                            "(no short-circuit &&/|| yet. See st_comptime_compile.c)",
                            ST_sv_args(op));
                return;
            }
            ST_ct_emit_op(cc->chunk, o, e->line);
            return;
        }

        case ST_EX_FIELD: {
            if (!ST_string_eq_cstr(e->field.name, "len")) {
                ST_ct_cfail(cc, e->line, e->col,
                            "comptime: '.'" ST_sv_fmt "' isn't comptime-evaluable "
                            "(only '.len' on a string is, right now)",
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

        case ST_EX_CALL:
            ST_ct_cfail(cc, e->line, e->col,
                        "comptime: function calls aren't comptime-evaluable yet");
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
        ST_ct_emit_op(cc->chunk, ST_OP_RETURN, e ? e->line : 0);
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
            ST_ct_compile_expr(cc, s->decl.init); // leaves the local's value on the stack...
            ST_ct_declare_local(cc, s->decl.name); // ...which becomes its permanent slot
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
            u32 exit_jump = ST_ct_emit_jump(cc->chunk, ST_OP_JMP_IF_FALSE, s->line);
            ST_ct_compile_scoped(cc, &s->while_.body);
            ST_ct_emit_loop(cc->chunk, loop_start, s->line);
            ST_ct_patch_jump(cc->chunk, exit_jump);
            return;
        }

        case ST_ST_BLOCK:
            ST_ct_compile_scoped(cc, &s->block);
            return;

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
