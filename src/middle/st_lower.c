#include "st_lower.h"

typedef struct ST_lower_loop_t ST_lower_loop_t;
typedef struct ST_lower_defers_t ST_lower_defers_t;
struct ST_lower_loop_t {
    ST_ir_block_t *br_target;
    ST_ir_block_t *con_target;
    u32 defer_count;
    ST_lower_loop_t *parent;
};

struct ST_lower_defers_t {
    ST_stmt_t **items;
    u32 count, capacity;
};

typedef struct {
    ST_arena_t *arena;
    ST_ir_module_t *module;
    ST_sema_t *sema;
    ST_program_t *prog;
    ST_diag_t diag;

    ST_ir_fn_t *fn;
    ST_ir_block_t *cur;
    ST_ht_t scope;
    ST_ht_t labels;
    ST_ht_t addr_taken;
    ST_lower_loop_t *loop;
    ST_lower_defers_t defers;
    ST_ir_blocks_t label_blocks;
} ST_lower_ctx_t;

typedef enum {
    ST_BIND_SSA,
    ST_BIND_ADDR,
} ST_bind_kind_t;

typedef struct {
    ST_bind_kind_t kind;
    void *key;
    ST_ir_inst_t *slot;
    ST_ty_t *ty;
} ST_lower_bind_t;

static ST_ir_inst_t *ST_lower_expr(ST_lower_ctx_t *c, ST_expr_t *e);
static void ST_lower_stmt(ST_lower_ctx_t *c, ST_stmt_t *s);
static ST_ir_inst_t *ST_lower_asm_tokens(ST_lower_ctx_t *c, ST_token_t *tokens, u32 n_tokens,
                                         ST_ty_t *ty, u32 line, u32 col);
static ST_ty_t *ST_lower_tyexpr(ST_lower_ctx_t *c, ST_tyexpr_t *te);
static ST_ir_inst_t *ST_lower_lvalue_addr(ST_lower_ctx_t *c, ST_expr_t *e);
static ST_ir_inst_t *ST_lower_call(ST_lower_ctx_t *c, ST_expr_t *e);
static ST_ir_inst_t *ST_lower_short_and(ST_lower_ctx_t *c, ST_expr_t *e);
static ST_ir_inst_t *ST_lower_short_or(ST_lower_ctx_t *c, ST_expr_t *e);
static void ST_lower_struct_zero(ST_lower_ctx_t *c, ST_ir_inst_t *base, i32 off, ST_ty_t *st,
                                 u32 line, u32 col);
static void ST_lower_struct_copy(ST_lower_ctx_t *c, ST_ir_inst_t *dst, i32 doff, ST_ir_inst_t *src,
                                 i32 soff, ST_ty_t *st, u32 line, u32 col);
static void ST_lower_struct_lit_into(ST_lower_ctx_t *c, ST_ir_inst_t *base, i32 off, ST_ty_t *st,
                                     ST_expr_t *e);
static void ST_lower_array_copy(ST_lower_ctx_t *c, ST_ir_inst_t *dst, i32 doff, ST_ir_inst_t *src,
                                i32 soff, ST_ty_t *sty, u32 line, u32 col);
static void ST_lower_array_lit_into(ST_lower_ctx_t *c, ST_ir_inst_t *base, i32 off, ST_ty_t *sty,
                                    ST_expr_t *lit);
static void ST_lower_scan_stmt(ST_lower_ctx_t *c, ST_stmt_t *s);
static ST_ir_inst_t *ST_lower_struct_addr(ST_lower_ctx_t *c, ST_expr_t *e, ST_ty_t *st);

static u32 ST_lower_eight_bytes_count(ST_ty_t *st);
static ST_ty_t *ST_lower_eight_byte_ty(ST_lower_ctx_t *c, ST_ty_t *st, u32 eb);
static void ST_lower_push_string_arg(ST_lower_ctx_t *c, ST_ir_inst_t **out, u32 *count,
                                     ST_expr_t *e);

static ST_ir_block_t *ST_lower_label_block(ST_lower_ctx_t *c, ST_string_t name) {
    ST_ht_generic_t key = {.tag = name.data, .size = name.len};
    ST_ir_block_t *b = ST_ht_get(&c->labels, key).tag;
    if (b)
        return b;
    b = ST_ir_block_new(c->fn, "label");
    ST_ht_generic_t *hk = ST_arena_push(c->arena, sizeof(*hk));
    hk->tag = name.data;
    hk->size = name.len;
    ST_ht_set(&c->labels, hk, (ST_ht_generic_t){.tag = b, .size = 0});
    ST_da_append_arena(c->arena, &c->label_blocks, b);
    return b;
}

static void ST_lower_scope_bind(ST_lower_ctx_t *c, ST_string_t name, ST_lower_bind_t *bind) {
    ST_ht_generic_t *hk = ST_arena_push(c->arena, sizeof(*hk));
    hk->tag = name.data;
    hk->size = name.len;
    ST_ht_set(&c->scope, hk, (ST_ht_generic_t){.tag = bind, .size = 0});
}

static ST_lower_bind_t *ST_lower_scope_find(ST_lower_ctx_t *c, ST_string_t name) {
    ST_ht_generic_t val =
        ST_ht_get(&c->scope, (ST_ht_generic_t){.tag = name.data, .size = name.len});
    return (ST_lower_bind_t *)val.tag;
}

static ST_lower_bind_t *ST_lower_bind_ssa(ST_lower_ctx_t *c, ST_string_t name, void *key,
                                          ST_ty_t *ty) {
    ST_lower_bind_t *b = ST_arena_push_zeroed(c->arena, sizeof(*b));
    b->kind = ST_BIND_SSA;
    b->key = key;
    b->ty = ty;
    ST_lower_scope_bind(c, name, b);
    return b;
}

static ST_lower_bind_t *ST_lower_bind_addr(ST_lower_ctx_t *c, ST_string_t name, ST_ir_inst_t *slot,
                                           ST_ty_t *ty) {
    ST_lower_bind_t *b = ST_arena_push_zeroed(c->arena, sizeof(*b));
    b->kind = ST_BIND_ADDR;
    b->slot = slot;
    b->ty = ty;
    ST_lower_scope_bind(c, name, b);
    return b;
}

static void ST_lower_mark_addr_taken(ST_lower_ctx_t *c, ST_string_t name) {
    ST_ht_generic_t *hk = ST_arena_push(c->arena, sizeof(*hk));
    hk->tag = name.data;
    hk->size = name.len;
    ST_ht_set(&c->addr_taken, hk, (ST_ht_generic_t){.tag = (void *)1, .size = 0});
}

static b8 ST_lower_is_addr_taken(ST_lower_ctx_t *c, ST_string_t name) {
    ST_ht_generic_t val =
        ST_ht_get(&c->addr_taken, (ST_ht_generic_t){.tag = name.data, .size = name.len});
    return val.tag != NULL;
}

static void ST_lower_run_defers(ST_lower_ctx_t *c, u32 defermark) {
    for (u32 i = c->defers.count; i > defermark; i--)
        ST_lower_stmt(c, c->defers.items[i - 1]);
    c->defers.count = defermark;
}

static void ST_lower_scan_expr(ST_lower_ctx_t *c, ST_expr_t *e) {
    if (!e)
        return;
    switch (e->kind) {
        case ST_EX_UNARY:
            if (ST_string_eq_cstr(e->unary.op, "&") && e->unary.operand &&
                e->unary.operand->kind == ST_EX_IDENT)
                ST_lower_mark_addr_taken(c, e->unary.operand->name);
            ST_lower_scan_expr(c, e->unary.operand);
            break;
        case ST_EX_BINARY:
            ST_lower_scan_expr(c, e->bin.l);
            ST_lower_scan_expr(c, e->bin.r);
            break;
        case ST_EX_CALL:
            ST_lower_scan_expr(c, e->call.callee);
            ST_forrange(0, e->call.args.count) ST_lower_scan_expr(c, e->call.args.items[i].value);
            break;
        case ST_EX_FIELD:
            ST_lower_scan_expr(c, e->field.base);
            break;
        case ST_EX_INDEX:
            ST_lower_scan_expr(c, e->index.base);
            ST_lower_scan_expr(c, e->index.index);
            break;
        case ST_EX_CAST:
            ST_lower_scan_expr(c, e->cast.operand);
            break;
        case ST_EX_STRUCT_LIT:
            ST_forrange(0, e->struct_lit.inits.count)
                ST_lower_scan_expr(c, e->struct_lit.inits.items[i].value);
            break;
        case ST_EX_TYPEOF:
        case ST_EX_TYPEINFO:
        case ST_EX_KIND:
        case ST_EX_CSTR:
            ST_lower_scan_expr(c, e->tyop.operand);
            break;
        default:
            break;
    }
}

static void ST_lower_scan_stmt(ST_lower_ctx_t *c, ST_stmt_t *s) {
    if (!s)
        return;
    switch (s->kind) {
        case ST_ST_DEFER:
            ST_lower_scan_stmt(c, s->defer_stmt);
            break;
        case ST_ST_EXPR:
            ST_lower_scan_expr(c, s->expr);
            break;
        case ST_ST_DECL:
            ST_lower_scan_expr(c, s->decl.init);
            break;
        case ST_ST_ASSIGN:
            ST_lower_scan_expr(c, s->assign.lhs);
            ST_lower_scan_expr(c, s->assign.rhs);
            break;
        case ST_ST_MULTI_BIND:
            ST_forrange(0, s->multi.values.count) ST_lower_scan_expr(c, s->multi.values.items[i]);
            break;
        case ST_ST_IF:
            ST_lower_scan_expr(c, s->if_.cond);
            ST_forrange(0, s->if_.then_body.count) ST_lower_scan_stmt(c, s->if_.then_body.items[i]);
            ST_lower_scan_stmt(c, s->if_.else_stmt);
            break;
        case ST_ST_SWITCH:
            ST_lower_scan_expr(c, s->switch_.cond);
            ST_forrange(0, s->switch_.cases.count) {
                ST_case_t *cs = &s->switch_.cases.items[i];
                for (u32 k = 0; k < cs->values.count; k++)
                    ST_lower_scan_expr(c, cs->values.items[k]);
                for (u32 k = 0; k < cs->body.count; k++)
                    ST_lower_scan_stmt(c, cs->body.items[k]);
            }
            break;
        case ST_ST_WHILE:
            ST_lower_scan_expr(c, s->while_.cond);
            ST_forrange(0, s->while_.body.count) ST_lower_scan_stmt(c, s->while_.body.items[i]);
            break;
        case ST_ST_FOR_RANGE:
            ST_lower_scan_expr(c, s->for_range.lo);
            ST_lower_scan_expr(c, s->for_range.hi);
            ST_forrange(0, s->for_range.body.count)
                ST_lower_scan_stmt(c, s->for_range.body.items[i]);
            break;
        case ST_ST_FOR_ARRAY:
            ST_lower_scan_expr(c, s->for_array.target);
            ST_forrange(0, s->for_array.body.count)
                ST_lower_scan_stmt(c, s->for_array.body.items[i]);
            break;
        case ST_ST_RETURN:
            ST_forrange(0, s->ret.values.count) ST_lower_scan_expr(c, s->ret.values.items[i]);
            break;
        case ST_ST_BLOCK:
            ST_forrange(0, s->block.count) ST_lower_scan_stmt(c, s->block.items[i]);
            break;
        case ST_ST_ASM:
            // @note: we don't know which identifiers inside the asm block
            // are real locals vs. mnemonics/register names at this point
            // (that's resolved later against the scope), so conservatively
            // mark every bare identifier as address-taken. Marking a name
            // that never gets declared is harmless (the hashtable entry
            // just goes unused).
            ST_forrange(0, s->asm_.n_tokens) {
                ST_token_t *t = &s->asm_.tokens[i];
                if (t->kind == ST_TIDENT)
                    ST_lower_mark_addr_taken(c, t->text);
            }
            break;
        default:
            break;
    }
}

static void *ST_lower_prim_by_name(ST_ty_ctx_t *c, ST_string_t name) {
    ST_forrange(0, ST_TYPE_COUNT) {
        if (ST_string_eq_cstr(name, ST_type_names[i]))
            return ST_ty_prim(c, (ST_type_t)i);
    }
    return NULL;
}

static void *ST_lower_find_named_decl(ST_program_t *prog_name, ST_string_t name) {
    ST_forrange(0, prog_name->decls.count) {
        ST_decl_t *d = prog_name->decls.items[i];
        if ((d->kind == ST_DE_STRUCT || d->kind == ST_DE_ENUM || d->kind == ST_DE_TAG_UNION) &&
            ST_string_eq(d->name, name))
            return d;
    }
    return NULL;
}

static ST_ty_t *ST_lower_tyexpr(ST_lower_ctx_t *c, ST_tyexpr_t *te) {
    if (!te)
        return NULL;

    if (te->resolved)
        return te->resolved;

    switch (te->kind) {
        case ST_TE_FN: {
            ST_ty_t *t = ST_ty_fn_new(&c->sema->tys);
            t->is_variadic = te->fn_is_variadic;
            ST_forrange(0, te->fn_params.count) {
                ST_ty_t *pt = ST_lower_tyexpr(c, te->fn_params.items[i]);
                ST_da_append_arena(c->arena, &t->params, pt);
            }
            ST_forrange(0, te->fn_rets.count) {
                ST_ty_t *rt = ST_lower_tyexpr(c, te->fn_rets.items[i]);
                if (rt && rt->kind == ST_TY_VOID)
                    continue;
                ST_da_append_arena(c->arena, &t->rets, rt);
            }

            return t;
        }
        case ST_TE_GENERIC_INST: {
            ST_tys_t args = {0};
            ST_forrange(0, te->generic_args.count) {
                ST_ty_t *at = ST_lower_tyexpr(c, te->generic_args.items[i]);
                ST_da_append_arena(c->arena, &args, at);
            }

            ST_string_t mangled = ST_ty_mangle_instance_name(c->arena, te->name, &args);
            ST_decl_t *d = ST_lower_find_named_decl(c->prog, mangled);
            if (d)
                return ST_ty_for_decls(&c->sema->tys, d);

            ST_diag_error(&c->diag, te->line, te->col,
                          "internal: no monomorphized decl for '" ST_sv_fmt "(...)'",
                          ST_sv_args(te->name));
            return ST_ty_prim(&c->sema->tys, ST_ti32);
        }
        case ST_TE_TYPEOF:
            return te->typeof_operand->ty;
        case ST_TE_NAME: {
            ST_ty_t *prim = ST_lower_prim_by_name(&c->sema->tys, te->name);
            if (prim)
                return prim;
            ST_decl_t *d = ST_lower_find_named_decl(c->prog, te->name);
            if (d)
                return ST_ty_for_decls(&c->sema->tys, d);
            ST_diag_error(&c->diag, te->line, te->col,
                          "internal: unknown type name '" ST_sv_fmt "'", ST_sv_args(te->name));
            return ST_ty_prim(&c->sema->tys, ST_ti32);
        }
        case ST_TE_PTR:
            return ST_ty_ptr(&c->sema->tys, ST_lower_tyexpr(c, te->inner));
        case ST_TE_ARRAY: {
            ST_ty_t *inner = ST_lower_tyexpr(c, te->inner);
            if (!te->count_expr)
                return ST_ty_dyn_array(&c->sema->tys, inner);
            u64 count = te->count_expr->kind == ST_EX_INT ? (u64)te->count_expr->ival : 0;
            return ST_ty_array(&c->sema->tys, inner, count);
        }
    }
    return NULL;
}

static b8 ST_lower_ty_is_scalar(ST_ty_t *t) {
    if (!t)
        return 0;
    switch (t->kind) {
        case ST_TY_INT:
        case ST_TY_UNTYPED_INT:
        case ST_TY_FLOAT:
        case ST_TY_UNTYPED_FLOAT:
        case ST_TY_BOOL:
        case ST_TY_CHAR:
        case ST_TY_PTR:
        case ST_TY_ENUM:
        case ST_TY_FN:
            return 1;
        default:
            return 0;
    }
}

static b8 ST_lower_field_find(ST_ty_t *st, ST_string_t name, ST_ty_t **fty, u32 *off) {
    ST_forrange(0, st->fields.count) {
        if (ST_string_eq(st->fields.items[i].name, name)) {
            *fty = st->fields.items[i].ty;
            *off = st->fields.items[i].offset;
            return 1;
        }
    }
    return 0;
}

static ST_ir_inst_t *ST_lower_field_ptr(ST_lower_ctx_t *c, ST_ir_inst_t *base, i32 off,
                                        ST_ty_t *fty, u32 line, u32 col) {
    ST_ty_t *pty = ST_ty_ptr(&c->sema->tys, fty);
    return ST_ir_addr(c->cur, pty, base, NULL, 0, off, line, col);
}

static b8 ST_lower_is_union_construct(ST_expr_t *e) {
    return e->kind == ST_EX_STRUCT_LIT && e->ty && e->ty->kind == ST_TY_TAG_UNION;
}

static void ST_lower_union_construct_into(ST_lower_ctx_t *c, ST_ir_inst_t *base, i32 off,
                                          ST_ty_t *ut, ST_expr_t *lit) {
    ST_string_t vname = lit->struct_lit.inits.items[0].value->name;
    ST_decl_t *ud = ut->decl;
    i64 idx = -1;
    ST_ty_t *payload_ty = NULL;
    ST_forrange(0, ud->tag_union.variants.count) {
        if (!ST_string_eq(ud->tag_union.variants.items[i].name, vname))
            continue;
        idx = (i64)i;
        if (ud->tag_union.variants.items[i].payload)
            payload_ty = ST_lower_tyexpr(c, ud->tag_union.variants.items[i].payload);
        break;
    }

    ST_ty_t *tagty = c->sema->tys.prim[ST_ti64];
    ST_ir_inst_t *tagp = ST_lower_field_ptr(c, base, off, tagty, lit->line, lit->col);
    ST_ir_store(c->cur, tagty, tagp, ST_ir_const_int(c->cur, tagty, idx), lit->line, lit->col);

    if (lit->struct_lit.inits.count == 2) {
        ST_expr_t *pval = lit->struct_lit.inits.items[1].value;
        if (payload_ty && payload_ty->kind == ST_TY_STRUCT) {
            ST_ir_inst_t *pp = ST_lower_field_ptr(c, base, off + 8, payload_ty, lit->line, lit->col);
            ST_lower_struct_zero(c, pp, 0, payload_ty, lit->line, lit->col);
            if (pval->kind == ST_EX_STRUCT_LIT)
                ST_lower_struct_lit_into(c, pp, 0, payload_ty, pval);
            else {
                ST_ir_inst_t *src = ST_lower_lvalue_addr(c, pval);
                if (src)
                    ST_lower_struct_copy(c, pp, 0, src, 0, payload_ty, lit->line, lit->col);
            }
        } else if (payload_ty && ST_lower_ty_is_scalar(payload_ty)) {
            ST_ir_inst_t *pp = ST_lower_field_ptr(c, base, off + 8, payload_ty, lit->line, lit->col);
            ST_ir_inst_t *v = ST_lower_expr(c, pval);
            ST_ir_store(c->cur, payload_ty, pp, v, lit->line, lit->col);
        } else {
            ST_diag_error(&c->diag, lit->line, lit->col,
                          "internal: this tag_union payload type isn't lowered yet");
        }
    } else {
        // No payload: zero the payload region for a clean, deterministic value.
        u32 payload_bytes = ut->size > 8 ? ut->size - 8 : 0;
        i32 poff = off + 8;
        while (payload_bytes >= 8) {
            ST_ir_inst_t *pp = ST_lower_field_ptr(c, base, poff, tagty, lit->line, lit->col);
            ST_ir_store(c->cur, tagty, pp, ST_ir_const_int(c->cur, tagty, 0), lit->line, lit->col);
            poff += 8;
            payload_bytes -= 8;
        }
    }
}

// Allocates a fresh temp slot, constructs into it, and returns its address
// -- the generic fallback for a union-construct literal used as a plain
// expression value (matches ST_lower_struct_addr's role for struct literals).
static ST_ir_inst_t *ST_lower_union_construct_addr(ST_lower_ctx_t *c, ST_expr_t *e) {
    ST_ir_inst_t *slot = ST_ir_alloca(c->fn, &c->sema->tys, e->ty, e->line, e->col);
    ST_lower_union_construct_into(c, slot, 0, e->ty, e);
    return slot;
}

static ST_ir_inst_t *ST_lower_string_lit_addr(ST_lower_ctx_t *c, ST_expr_t *e) {
    u32 idx = ST_ir_module_intern_str(c->module, e->sval);
    ST_ty_t *pty = ST_ty_ptr(&c->sema->tys, c->sema->tys.prim[ST_tstring]);
    return ST_ir_const_str(c->cur, pty, idx);
}

static ST_ir_inst_t *ST_lower_str_from_raw_addr(ST_lower_ctx_t *c, ST_expr_t *e) {
    // 'str_from_raw(ptr, len)' -- assembles a real 'string' value (a fresh
    // local, ptr@0 / len@8, same layout ST_lower_string_copy already
    // assumes) from a raw '*char' and an integer length. This is the one
    // place a string's normally-read-only fields get written directly at
    // the IR level -- the source-level restriction against 'x.ptr = ..'
    // is a user-code invariant (see ST_check_assign), not a lowering one.
    ST_ir_inst_t *slot =
        ST_ir_alloca(c->fn, &c->sema->tys, c->sema->tys.prim[ST_tstring], e->line, e->col);

    ST_ty_t *pty = ST_ty_ptr(&c->sema->tys, c->sema->tys.prim[ST_tchar]);
    ST_ty_t *lty = c->sema->tys.prim[ST_ti64];

    ST_ir_inst_t *pv = ST_lower_expr(c, e->str_from_raw.ptr);
    ST_ir_inst_t *lv = ST_lower_expr(c, e->str_from_raw.len);
    if (lv && lv->ty && lv->ty != lty)
        lv = ST_ir_cast(c->cur, lty, lv, e->line, e->col);

    ST_ir_inst_t *pp = ST_lower_field_ptr(c, slot, 0, pty, e->line, e->col);
    ST_ir_store(c->cur, pty, pp, pv, e->line, e->col);

    ST_ir_inst_t *lp = ST_lower_field_ptr(c, slot, 8, lty, e->line, e->col);
    ST_ir_store(c->cur, lty, lp, lv, e->line, e->col);

    return slot;
}

static ST_ir_inst_t *ST_lower_string_addr(ST_lower_ctx_t *c, ST_expr_t *e) {
    if (e->kind == ST_EX_STR)
        return ST_lower_string_lit_addr(c, e);
    if (e->kind == ST_EX_STR_FROM_RAW)
        return ST_lower_str_from_raw_addr(c, e);
    if (e->kind == ST_EX_IDENT && !ST_lower_scope_find(c, e->name) &&
        !ST_ir_module_find_global(c->module, e->name)) {
        // A '::' string constant isn't stored anywhere; use its literal.
        ST_ht_generic_t key = {.tag = e->name.data, .size = e->name.len};
        ST_sym_t *sym = ST_ht_get(&c->sema->globals, key).tag;
        if (sym && sym->kind == ST_SYM_CONST && sym->decl && sym->decl->kind == ST_DE_CONST)
            return ST_lower_string_addr(c, sym->decl->const_.value);
    }
    return ST_lower_lvalue_addr(c, e);
}

static void ST_lower_string_copy(ST_lower_ctx_t *c, ST_ir_inst_t *dest, ST_ir_inst_t *src, u32 line,
                                 u32 col) {
    ST_ty_t *dty = ST_ty_ptr(&c->sema->tys, c->sema->tys.prim[ST_tchar]);
    ST_ty_t *lty = c->sema->tys.prim[ST_ti64];

    ST_ir_inst_t *p = ST_lower_field_ptr(c, src, 0, dty, line, col);
    ST_ir_inst_t *v = ST_ir_load(c->cur, dty, p, line, col);
    p = ST_lower_field_ptr(c, dest, 0, dty, line, col);
    ST_ir_store(c->cur, dty, p, v, line, col);

    p = ST_lower_field_ptr(c, src, 8, lty, line, col);
    v = ST_ir_load(c->cur, lty, p, line, col);
    p = ST_lower_field_ptr(c, dest, 8, lty, line, col);
    ST_ir_store(c->cur, lty, p, v, line, col);
}

static void ST_lower_array_zero(ST_lower_ctx_t *c, ST_ir_inst_t *base, i32 off, ST_ty_t *aty,
                                u32 line, u32 col) {
    ST_ty_t *ety = aty->inner;
    u32 esz = ety->size;
    ST_forrange(0, aty->count) {
        i32 eoff = off + (i32)(i * esz);
        if (ety->kind == ST_TY_STRUCT)
            ST_lower_struct_zero(c, base, eoff, ety, line, col);
        else if (ety->kind == ST_TY_STRING) {
            ST_ty_t *dty = ST_ty_ptr(&c->sema->tys, c->sema->tys.prim[ST_tchar]);
            ST_ty_t *lty = c->sema->tys.prim[ST_ti32];

            ST_ir_inst_t *fp = ST_lower_field_ptr(c, base, eoff, dty, line, col);
            ST_ir_store(c->cur, dty, fp, ST_ir_const_int(c->cur, dty, 0), line, col);
            fp = ST_lower_field_ptr(c, base, eoff + 8, lty, line, col);
            ST_ir_store(c->cur, lty, fp, ST_ir_const_int(c->cur, lty, 0), line, col);
        } else if (ety->kind == ST_TY_ARRAY)
            ST_lower_array_zero(c, base, eoff, ety, line, col);
        else if (ST_lower_ty_is_scalar(ety)) {
            ST_ir_inst_t *fp = ST_lower_field_ptr(c, base, eoff, ety, line, col);
            ST_ir_inst_t *z = ST_ty_is_float(ety) ? ST_ir_const_float(c->cur, ety, 0.0)
                                                  : ST_ir_const_int(c->cur, ety, 0);
            ST_ir_store(c->cur, ety, fp, z, line, col);
        } else {
            ST_diag_error(&c->diag, line, col, "internal: array element now lowered yet");
        }
    }
}

static void ST_lower_struct_zero(ST_lower_ctx_t *c, ST_ir_inst_t *base, i32 off, ST_ty_t *st,
                                 u32 line, u32 col) {
    ST_forrange(0, st->fields.count) {
        ST_ty_field_t *f = &st->fields.items[i];
        i32 foff = off + (i32)f->offset;
        if (f->ty->kind == ST_TY_STRUCT) {
            ST_lower_struct_zero(c, base, foff, f->ty, line, col);
        } else if (f->ty->kind == ST_TY_ARRAY) {
            ST_lower_array_zero(c, base, foff, f->ty, line, col);
        } else if (f->ty->kind == ST_TY_STRING) {
            ST_ty_t *dty = ST_ty_ptr(&c->sema->tys, c->sema->tys.prim[ST_tchar]);
            ST_ty_t *lty = c->sema->tys.prim[ST_ti32];

            ST_ir_inst_t *fp = ST_lower_field_ptr(c, base, foff, dty, line, col);
            ST_ir_store(c->cur, dty, fp, ST_ir_const_int(c->cur, dty, 0), line, col);

            fp = ST_lower_field_ptr(c, base, foff + 8, lty, line, col);
            ST_ir_store(c->cur, lty, fp, ST_ir_const_int(c->cur, lty, 0), line, col);
        } else if (ST_lower_ty_is_scalar(f->ty)) {
            ST_ir_inst_t *fp = ST_lower_field_ptr(c, base, foff, f->ty, line, col);
            ST_ir_inst_t *z = ST_ty_is_float(f->ty) ? ST_ir_const_float(c->cur, f->ty, 0.0)
                                                    : ST_ir_const_int(c->cur, f->ty, 0);
            ST_ir_store(c->cur, f->ty, fp, z, line, col);
        }

        else {
            ST_diag_error(&c->diag, line, col,
                          "internal: field '" ST_sv_fmt "' has a type that isn't "
                          "lowered yet (arrays in structs come later)",
                          ST_sv_args(f->name));
        }
    }
}

// Copies 'size' bytes as-is between two addresses, 8 bytes at a time (size
// is always a multiple of 8 for anything this gets called on: tag_unions
// are laid out {tag: i64, payload}, both 8-aligned). Used for tag_union
// values, which -- unlike structs -- have no per-field layout to walk field
// by field; the payload is opaque bytes as far as the copy is concerned.
static void ST_lower_raw_copy(ST_lower_ctx_t *c, ST_ir_inst_t *dst, i32 doff, ST_ir_inst_t *src,
                              i32 soff, u32 size, u32 line, u32 col) {
    ST_ty_t *chunk_ty = c->sema->tys.prim[ST_ti64];
    u32 off = 0;
    while (off + 8 <= size) {
        ST_ir_inst_t *sp = ST_lower_field_ptr(c, src, soff + (i32)off, chunk_ty, line, col);
        ST_ir_inst_t *v = ST_ir_load(c->cur, chunk_ty, sp, line, col);
        ST_ir_inst_t *dp = ST_lower_field_ptr(c, dst, doff + (i32)off, chunk_ty, line, col);
        ST_ir_store(c->cur, chunk_ty, dp, v, line, col);
        off += 8;
    }
}

static void ST_lower_struct_copy_direct(ST_lower_ctx_t *c, ST_ir_inst_t *dst, ST_ir_inst_t *src,
                                        ST_ty_t *st, u32 line, u32 col) {
    ST_forrange(0, st->fields.count) {
        ST_ty_field_t *f = &st->fields.items[i];
        ST_ty_t *fty = f->ty;
        i32 off = (i32)f->offset;
        if (f->ty->kind == ST_TY_STRUCT) {
            ST_ir_inst_t *dst_field = ST_lower_field_ptr(c, dst, off, fty, line, col);
            ST_ir_inst_t *src_field = ST_lower_field_ptr(c, src, off, fty, line, col);
            ST_lower_struct_copy_direct(c, dst_field, src_field, fty, line, col);
        } else if (f->ty->kind == ST_TY_ARRAY) {
            ST_lower_array_copy(c, dst, off, src, off, fty, line, col);
        } else if (f->ty->kind == ST_TY_STRING) {
            ST_ir_inst_t *sp = ST_lower_field_ptr(c, src, off, fty, line, col);
            ST_ir_inst_t *dp = ST_lower_field_ptr(c, dst, off, fty, line, col);
            ST_lower_string_copy(c, dp, sp, line, col);
        } else if (ST_lower_ty_is_scalar(f->ty)) {
            ST_ir_inst_t *sp = ST_lower_field_ptr(c, src, off, fty, line, col);
            ST_ir_inst_t *v = ST_ir_load(c->cur, fty, sp, line, col);
            ST_ir_inst_t *dp = ST_lower_field_ptr(c, dst, off, fty, line, col);
            ST_ir_store(c->cur, fty, dp, v, line, col);
        } else {
            ST_diag_error(&c->diag, line, col,
                          "internal: field '" ST_sv_fmt "' has a type that isn't "
                          "lowered yet.",
                          ST_sv_args(f->name));
        }
    }
}

static void ST_lower_struct_copy(ST_lower_ctx_t *c, ST_ir_inst_t *dst, i32 doff, ST_ir_inst_t *src,
                                 i32 soff, ST_ty_t *st, u32 line, u32 col) {
    ST_forrange(0, st->fields.count) {
        ST_ty_field_t *f = &st->fields.items[i];
        if (f->ty->kind == ST_TY_STRUCT) {
            ST_lower_struct_copy(c, dst, doff + (i32)f->offset, src, soff + (i32)f->offset, f->ty,
                                 line, col);
        } else if (f->ty->kind == ST_TY_ARRAY) {
            ST_lower_array_copy(c, dst, doff + (i32)f->offset, src, soff + (i32)f->offset, f->ty,
                                line, col);
        } else if (f->ty->kind == ST_TY_STRING) {
            ST_ir_inst_t *sp = ST_lower_field_ptr(c, src, soff + (i32)f->offset, f->ty, line, col);
            ST_ir_inst_t *dp = ST_lower_field_ptr(c, dst, doff + (i32)f->offset, f->ty, line, col);
            ST_lower_string_copy(c, dp, sp, line, col);
        } else if (ST_lower_ty_is_scalar(f->ty)) {
            ST_ir_inst_t *sp = ST_lower_field_ptr(c, src, soff + (i32)f->offset, f->ty, line, col);
            ST_ir_inst_t *v = ST_ir_load(c->cur, f->ty, sp, line, col);
            ST_ir_inst_t *dp = ST_lower_field_ptr(c, dst, doff + (i32)f->offset, f->ty, line, col);
            ST_ir_store(c->cur, f->ty, dp, v, line, col);
        } else {
            ST_diag_error(&c->diag, line, col,
                          "internal: field '" ST_sv_fmt "' has a type that isn't "
                          "lowered yet",
                          ST_sv_args(f->name));
        }
    }
}

static void ST_lower_struct_lit_into(ST_lower_ctx_t *c, ST_ir_inst_t *base, i32 off, ST_ty_t *st,
                                     ST_expr_t *lit) {
    ST_forrange(0, lit->struct_lit.inits.count) {
        ST_field_init_t *fi = &lit->struct_lit.inits.items[i];
        ST_ty_t *fty = NULL;
        u32 foff = 0;

        if (fi->name.len) {
            if (!ST_lower_field_find(st, fi->name, &fty, &foff))
                continue;
        } else {
            if (i >= st->fields.count)
                break;
            fty = st->fields.items[i].ty;
            foff = st->fields.items[i].offset;
        }

        if (fty->kind == ST_TY_STRUCT) {
            if (fi->value->kind == ST_EX_STRUCT_LIT)
                ST_lower_struct_lit_into(c, base, off + (i32)foff, fty, fi->value);
            else {
                ST_ir_inst_t *src = ST_lower_lvalue_addr(c, fi->value);
                if (src)
                    ST_lower_struct_copy(c, base, off + (i32)foff, src, 0, fty, fi->value->line,
                                         fi->value->col);
            }
        } else if (fty->kind == ST_TY_ARRAY) {
            if (fi->value->kind == ST_EX_STRUCT_LIT)
                ST_lower_array_lit_into(c, base, off + (i32)foff, fty, fi->value);
            else {
                ST_ir_inst_t *src = ST_lower_lvalue_addr(c, fi->value);
                if (src)
                    ST_lower_array_copy(c, base, off + (i32)foff, src, 0, fty, fi->value->line,
                                        fi->value->col);
            }
        } else if (fty->kind == ST_TY_STRING) {
            ST_ir_inst_t *src = ST_lower_string_addr(c, fi->value);
            ST_ir_inst_t *dp =
                ST_lower_field_ptr(c, base, off + (i32)foff, fty, fi->value->line, fi->value->col);
            ST_lower_string_copy(c, dp, src, fi->value->line, fi->value->col);
        }

        else if (ST_lower_ty_is_scalar(fty)) {
            ST_ir_inst_t *v = ST_lower_expr(c, fi->value);
            ST_ir_inst_t *fp =
                ST_lower_field_ptr(c, base, off + (i32)foff, fty, fi->value->line, fi->value->col);
            ST_ir_store(c->cur, fty, fp, v, fi->value->line, fi->value->col);
        } else {
            ST_diag_error(&c->diag, fi->value->line, fi->value->col,
                          "internal: this field type isn't lowered yet");
        }
    }
}

static ST_ir_inst_t *ST_lower_struct_addr(ST_lower_ctx_t *c, ST_expr_t *e, ST_ty_t *st) {
    if (ST_lower_is_union_construct(e))
        return ST_lower_union_construct_addr(c, e);
    if (e->kind == ST_EX_STRUCT_LIT) {
        ST_ir_inst_t *slot = ST_ir_alloca(c->fn, &c->sema->tys, st, e->line, e->col);
        ST_lower_struct_lit_into(c, slot, 0, st, e);
        return slot;
    }
    return ST_lower_lvalue_addr(c, e);
}

static void ST_lower_push_struct_arg(ST_lower_ctx_t *c, ST_ir_inst_t **out, u32 *count,
                                     ST_expr_t *e, ST_ty_t *st) {
    if (st->kind == ST_TY_STRING) {
        ST_lower_push_string_arg(c, out, count, e);
        return;
    }

    if (st->size > 16) {
        ST_ir_inst_t *addr = ST_lower_struct_addr(c, e, st);
        if (!addr)
            return;

        out[(*count)++] = addr;
        return;
    }
    ST_ir_inst_t *addr = ST_lower_struct_addr(c, e, st);
    if (!addr)
        return;

    u32 n_eb = ST_lower_eight_bytes_count(st);
    for (u32 k = 0; k < n_eb && k < 2; k++) {
        ST_ty_t *ebty = ST_lower_eight_byte_ty(c, st, k);
        ST_ir_inst_t *fp = ST_lower_field_ptr(c, addr, (i32)(k * 8), ebty, e->line, e->col);
        out[(*count)++] = ST_ir_load(c->cur, ebty, fp, e->line, e->col);
    }
}

static void ST_lower_push_string_arg(ST_lower_ctx_t *c, ST_ir_inst_t **out, u32 *count,
                                     ST_expr_t *e) {
    ST_ir_inst_t *addr = ST_lower_string_addr(c, e);
    if (!addr)
        return;

    ST_ty_t *ptr_ty = ST_ty_ptr(&c->sema->tys, c->sema->tys.prim[ST_tchar]);
    ST_ty_t *len_ty = c->sema->tys.prim[ST_ti64];

    ST_ir_inst_t *ptr_field = ST_lower_field_ptr(c, addr, 0, ptr_ty, e->line, e->col);
    out[(*count)++] = ST_ir_load(c->cur, ptr_ty, ptr_field, e->line, e->col);

    ST_ir_inst_t *len_field = ST_lower_field_ptr(c, addr, 8, len_ty, e->line, e->col);
    out[(*count)++] = ST_ir_load(c->cur, len_ty, len_field, e->line, e->col);
}

static ST_ir_inst_t *ST_lower_short_and(ST_lower_ctx_t *c, ST_expr_t *e) {
    ST_ir_inst_t *l = ST_lower_expr(c, e->bin.l);
    if (!l)
        return NULL;
    void *key = (void *)e;
    ST_ir_write_var(c->cur, key, ST_ir_const_int(c->cur, e->ty, 0));
    ST_ir_block_t *rhs_b = ST_ir_block_new(c->fn, "and_rhs");
    ST_ir_block_t *join_b = ST_ir_block_new(c->fn, "and_end");

    ST_ir_term_condbr(c->cur, l, rhs_b, join_b, e->line, e->col);
    ST_ir_block_seal(rhs_b);

    c->cur = rhs_b;
    ST_ir_inst_t *r = ST_lower_expr(c, e->bin.r);
    if (!r)
        r = ST_ir_const_int(c->cur, e->ty, 0);
    ST_ir_write_var(c->cur, key, r);
    if (!ST_ir_block_is_terminated(c->cur))
        ST_ir_term_br(c->cur, join_b, e->line, e->col);

    ST_ir_block_seal(join_b);
    c->cur = join_b;
    if (join_b->preds.count == 0) {
        ST_ir_term_unreachable(join_b, e->line, e->col);
        return ST_ir_const_int(c->cur, e->ty, 0);
    }
    return ST_ir_read_var(c->cur, key, e->ty);
}

static ST_ir_inst_t *ST_lower_short_or(ST_lower_ctx_t *c, ST_expr_t *e) {
    ST_ir_inst_t *l = ST_lower_expr(c, e->bin.l);
    if (!l)
        return NULL;
    void *key = (void *)e;
    ST_ir_write_var(c->cur, key, ST_ir_const_int(c->cur, e->ty, 1));
    ST_ir_block_t *rhs_b = ST_ir_block_new(c->fn, "or_rhs");
    ST_ir_block_t *join_b = ST_ir_block_new(c->fn, "or_end");

    ST_ir_term_condbr(c->cur, l, join_b, rhs_b, e->line, e->col);
    ST_ir_block_seal(rhs_b);

    c->cur = rhs_b;
    ST_ir_inst_t *r = ST_lower_expr(c, e->bin.r);
    if (!r)
        r = ST_ir_const_int(c->cur, e->ty, 0);
    ST_ir_write_var(c->cur, key, r);
    if (!ST_ir_block_is_terminated(c->cur))
        ST_ir_term_br(c->cur, join_b, e->line, e->col);

    ST_ir_block_seal(join_b);
    c->cur = join_b;
    if (join_b->preds.count == 0) {
        ST_ir_term_unreachable(join_b, e->line, e->col);
        return ST_ir_const_int(c->cur, e->ty, 0);
    }
    return ST_ir_read_var(c->cur, key, e->ty);
}

static ST_ir_inst_t *ST_lower_lvalue_addr(ST_lower_ctx_t *c, ST_expr_t *e) {
    switch (e->kind) {
        case ST_EX_IDENT: {
            ST_lower_bind_t *bind = ST_lower_scope_find(c, e->name);
            if (!bind) {
                ST_ir_global_var_t *g = ST_ir_module_find_global(c->module, e->name);
                if (g) {
                    ST_ty_t *ptr_ty = ST_ty_ptr(&c->sema->tys, g->ty);
                    return ST_ir_global_addr(c->cur, ptr_ty, e->name, e->line, e->col);
                }
                ST_diag_error(&c->diag, e->line, e->col,
                              "internal: '" ST_sv_fmt "' is not a known local",
                              ST_sv_args(e->name));
                return NULL;
            }
            if (bind->kind != ST_BIND_ADDR) {
                ST_diag_error(&c->diag, e->line, e->col,
                              "internal: '" ST_sv_fmt "' has no memory address "
                              "(it was kept in SSA form)",
                              ST_sv_args(e->name));
                return NULL;
            }
            return bind->slot;
        }

        case ST_EX_UNARY:
            if (ST_string_eq_cstr(e->unary.op, "*"))
                return ST_lower_expr(c, e->unary.operand);
            break;

        case ST_EX_CALL:
            // A call whose return type is an aggregate (struct/tag_union)
            // already yields a real address from ST_lower_call (it
            // materializes the callee's returned eightbyte(s) into a fresh
            // local -- see ST_lower_call). For a scalar-returning call
            // there's no address to take; ST_lower_call returns a plain
            // SSA value there and we fall through to the "can't take the
            // address" error below, same as before.
            if (e->ty && (e->ty->kind == ST_TY_STRUCT || e->ty->kind == ST_TY_TAG_UNION))
                return ST_lower_call(c, e);
            break;

        case ST_EX_FIELD: {
            ST_expr_t *b = e->field.base;
            ST_ty_t *bt = b->ty;
            ST_ir_inst_t *base;

            if (bt && bt->kind == ST_TY_PTR) {
                base = ST_lower_expr(c, b);
                bt = bt->inner;
            } else
                base = ST_lower_lvalue_addr(c, b);

            if (!base || !bt)
                return NULL;
            if (bt->kind == ST_TY_STRING || bt->kind == ST_TY_DYN_ARRAY) {
                if (ST_string_eq_cstr(e->field.name, "ptr")) {
                    ST_ty_t *elem =
                        bt->kind == ST_TY_STRING ? c->sema->tys.prim[ST_tchar] : bt->inner;
                    return ST_lower_field_ptr(c, base, 0, ST_ty_ptr(&c->sema->tys, elem), e->line,
                                              e->col);
                }
                if (ST_string_eq_cstr(e->field.name, "len"))
                    return ST_lower_field_ptr(c, base, 8, c->sema->tys.prim[ST_ti64], e->line,
                                              e->col);
            }

            if (bt->kind == ST_TY_TAG_UNION) {
                if (ST_string_eq_cstr(e->field.name, "kind"))
                    return ST_lower_field_ptr(c, base, 0, c->sema->tys.prim[ST_ti64], e->line,
                                              e->col);
                ST_variant_specs_t *vs = &bt->decl->tag_union.variants;
                ST_forrange(0, vs->count) if (ST_string_eq(vs->items[i].name, e->field.name)) {
                    ST_ty_t *pty = ST_lower_tyexpr(c, vs->items[i].payload);
                    return ST_lower_field_ptr(c, base, 8, pty, e->line, e->col);
                }
                ST_diag_error(&c->diag, e->line, e->col,
                              "internal: unknown tag_union variant '" ST_sv_fmt "'",
                              ST_sv_args(e->field.name));
                return NULL;
            }

            if (bt->kind != ST_TY_STRUCT) {
                ST_diag_error(&c->diag, e->line, e->col,
                              "internal: field access on this type isn't lowered yet ");
                return NULL;
            }
            ST_ty_t *fty = NULL;
            u32 foff = 0;
            if (!ST_lower_field_find(bt, e->field.name, &fty, &foff)) {
                ST_diag_error(&c->diag, e->line, e->col, "internal: unknown field '" ST_sv_fmt "'",
                              ST_sv_args(e->field.name));
                return NULL;
            }
            return ST_lower_field_ptr(c, base, (i32)foff, fty, e->line, e->col);
        }
        case ST_EX_INDEX: {
            ST_expr_t *b = e->index.base;
            ST_ty_t *bt = b->ty;
            ST_ir_inst_t *base;
            ST_ty_t *ety;
            if (bt && bt->kind == ST_TY_PTR) {
                base = ST_lower_expr(c, b);
                ety = bt->inner;
            } else if (bt && bt->kind == ST_TY_ARRAY) {
                base = ST_lower_lvalue_addr(c, b);
                ety = bt->inner;
            } else {
                ST_diag_error(&c->diag, e->line, e->col,
                              "internal: cannot take the address or non scalar type");
                return NULL;
            }

            if (!base)
                return NULL;
            ST_ir_inst_t *idx = ST_lower_expr(c, e->index.index);
            if (!idx)
                return NULL;

            ST_ty_t *pty = ST_ty_ptr(&c->sema->tys, ety);
            u32 scale = ety->size ? ety->size : 1;
            return ST_ir_addr(c->cur, pty, base, idx, scale, 0, e->line, e->col);
        } break;
        default:
            break;
    }

    ST_diag_error(&c->diag, e->line, e->col,
                  "internal: cannot take the address of this expression form yet");
    return NULL;
}

static ST_ir_op_t ST_lower_binop(ST_diag_t *diag, ST_expr_t *e, b8 is_f, b8 uns) {
    ST_string_t op = e->bin.op;
    if (ST_string_eq_cstr(op, "+"))
        return is_f ? ST_IR_FADD : ST_IR_ADD;
    if (ST_string_eq_cstr(op, "-"))
        return is_f ? ST_IR_FSUB : ST_IR_SUB;
    if (ST_string_eq_cstr(op, "*"))
        return is_f ? ST_IR_FMUL : ST_IR_MUL;
    if (ST_string_eq_cstr(op, "/"))
        return is_f ? ST_IR_FDIV : (uns ? ST_IR_UDIV : ST_IR_SDIV);
    if (ST_string_eq_cstr(op, "%"))
        return uns ? ST_IR_UREM : ST_IR_SREM;
    if (ST_string_eq_cstr(op, "&"))
        return ST_IR_AND;
    if (ST_string_eq_cstr(op, "|"))
        return ST_IR_OR;
    if (ST_string_eq_cstr(op, "^"))
        return ST_IR_XOR;
    if (ST_string_eq_cstr(op, "<<"))
        return ST_IR_SHL;
    if (ST_string_eq_cstr(op, ">>"))
        return uns ? ST_IR_LSHR : ST_IR_ASHR;
    if (ST_string_eq_cstr(op, "=="))
        return is_f ? ST_IR_FCMP_EQ : ST_IR_ICMP_EQ;
    if (ST_string_eq_cstr(op, "!="))
        return is_f ? ST_IR_FCMP_NE : ST_IR_ICMP_NE;
    if (ST_string_eq_cstr(op, "<"))
        return is_f ? ST_IR_FCMP_LT : (uns ? ST_IR_ICMP_ULT : ST_IR_ICMP_SLT);
    if (ST_string_eq_cstr(op, "<="))
        return is_f ? ST_IR_FCMP_LE : (uns ? ST_IR_ICMP_ULE : ST_IR_ICMP_SLE);
    if (ST_string_eq_cstr(op, ">"))
        return is_f ? ST_IR_FCMP_GT : (uns ? ST_IR_ICMP_UGT : ST_IR_ICMP_SGT);
    if (ST_string_eq_cstr(op, ">="))
        return is_f ? ST_IR_FCMP_GE : (uns ? ST_IR_ICMP_UGE : ST_IR_ICMP_SGE);
    ST_diag_error(diag, e->line, e->col, "internal: unsupported binary operator '" ST_sv_fmt "'",
                  ST_sv_args(op));
    return ST_IR_ADD;
}

static ST_ir_op_t ST_lower_compound_op(ST_diag_t *diag, ST_string_t op, u32 line, u32 col,
                                       b8 is_f) {
    if (ST_string_eq_cstr(op, "+="))
        return is_f ? ST_IR_FADD : ST_IR_ADD;
    if (ST_string_eq_cstr(op, "-="))
        return is_f ? ST_IR_FSUB : ST_IR_SUB;
    if (ST_string_eq_cstr(op, "*="))
        return is_f ? ST_IR_FMUL : ST_IR_MUL;
    if (ST_string_eq_cstr(op, "/="))
        return is_f ? ST_IR_FDIV : ST_IR_SDIV;
    if (ST_string_eq_cstr(op, "%="))
        return ST_IR_SREM;
    if (ST_string_eq_cstr(op, "&="))
        return ST_IR_AND;
    if (ST_string_eq_cstr(op, "|="))
        return ST_IR_OR;
    if (ST_string_eq_cstr(op, "^="))
        return ST_IR_XOR;
    ST_diag_error(diag, line, col, "internal: unsupported compound-assign operator '" ST_sv_fmt "'",
                  ST_sv_args(op));
    return ST_IR_ADD;
}

// Named '::' constants aren't stored anywhere (they're folded into their use
// sites), but '&N' needs a real address to hand back. The first time a
// constant's address is taken, this materializes a same-named, initialized
// global for it (reusing the exact same .data emission path as 'x := expr;'
// globals) so every subsequent '&N' in the module reuses the same storage.
static ST_ir_global_var_t *ST_lower_ensure_const_global(ST_lower_ctx_t *c, ST_sym_t *sym) {
    ST_ir_global_var_t *g = ST_ir_module_find_global(c->module, sym->name);
    if (g)
        return g;
    ST_ty_t *ty = sym->t;
    ST_expr_t *cv = sym->decl->const_.value;
    b8 is_float = ty && ST_ty_is_float(ty);
    i64 iv = 0;
    f64 fv = 0.0;
    if (is_float) {
        fv = cv->kind == ST_EX_FLOAT ? cv->fval : 0.0;
    } else if (!ST_const_eval(c->sema, cv, &iv)) {
        ST_diag_error(&c->diag, cv->line, cv->col,
                      "internal: cannot materialize storage for constant '" ST_sv_fmt
                      "' (its value isn't a compile-time integer/float constant)",
                      ST_sv_args(sym->name));
    }
    ST_ir_module_add_global(c->module, sym->name, ty, 0, 1, is_float, iv, fv);
    return ST_ir_module_find_global(c->module, sym->name);
}

static ST_ir_inst_t *ST_lower_addr_of(ST_lower_ctx_t *c, ST_expr_t *e, ST_ty_t *ptr_ty) {
    ST_expr_t *v = e->unary.operand;

    if (v->kind == ST_EX_UNARY && ST_string_eq_cstr(v->unary.op, "*"))
        return ST_lower_expr(c, v->unary.operand);

    if (v->kind == ST_EX_IDENT) {
        ST_lower_bind_t *bind = ST_lower_scope_find(c, v->name);
        if (!bind) {
            ST_ir_fn_t *fn = ST_ir_module_find_fn(c->module, v->name);
            if (fn)
                return ST_ir_global_addr(c->cur, ptr_ty, v->name, e->line, e->col);
            ST_ir_global_var_t *g = ST_ir_module_find_global(c->module, v->name);
            if (g)
                return ST_ir_global_addr(c->cur, ptr_ty, v->name, e->line, e->col);
            ST_ht_generic_t key = {.tag = v->name.data, .size = v->name.len};
            ST_sym_t *sym = ST_ht_get(&c->sema->globals, key).tag;
            if (sym && sym->kind == ST_SYM_CONST && sym->decl && sym->decl->kind == ST_DE_CONST) {
                ST_lower_ensure_const_global(c, sym);
                return ST_ir_global_addr(c->cur, ptr_ty, sym->name, e->line, e->col);
            }
            ST_diag_error(&c->diag, e->line, e->col,
                          "internal: cannot take the address of '" ST_sv_fmt "' "
                          "(not a known local)",
                          ST_sv_args(v->name));
            return ST_ir_const_int(c->cur, ptr_ty, 0);
        }
        if (bind->kind != ST_BIND_ADDR) {
            ST_diag_error(&c->diag, e->line, e->col,
                          "internal: '" ST_sv_fmt "' was not demoted to a stack slot "
                          "before its address was taken",
                          ST_sv_args(v->name));
            return ST_ir_const_int(c->cur, ptr_ty, 0);
        }
        return bind->slot;
    }

    if (v->kind == ST_EX_FIELD) {
        ST_ir_inst_t *a = ST_lower_lvalue_addr(c, v);
        return a ? a : ST_ir_const_int(c->cur, ptr_ty, 0);
    }

    ST_diag_error(&c->diag, e->line, e->col,
                  "internal: address-of this expression form isn't lowered yet "
                  "(array indexing comes with array lowering)");
    return ST_ir_const_int(c->cur, ptr_ty, 0);
}

static ST_fn_sig_t *ST_lower_find_sig(ST_lower_ctx_t *c, ST_string_t name) {
    ST_ht_generic_t key = {.tag = name.data, .size = name.len};
    ST_sym_t *sym = ST_ht_get(&c->sema->globals, key).tag;
    if (!sym || sym->kind != ST_SYM_FN || !sym->decl)
        return NULL;

    if (sym->decl->kind == ST_DE_FN) {
        return &sym->decl->fn.sig;
    }
    if (sym->decl->kind == ST_DE_EXTERN_FN)
        return &sym->decl->extern_fn.sig;
    return NULL;
}

static ST_ir_inst_t *ST_lower_call(ST_lower_ctx_t *c, ST_expr_t *e) {
    ST_fn_sig_t *sig = NULL;
    b8 direct =
        e->call.callee->kind == ST_EX_IDENT && !ST_lower_scope_find(c, e->call.callee->name);
    if (direct)
        sig = ST_lower_find_sig(c, e->call.callee->name);

    // @note: aggregate returns no longer need a hidden-pointer argument
    // threaded through here -- ST_lower_register_fn already expands a
    // single struct/tag_union return type into its real eightbyte-
    // classified slots in fn_ty->rets, so the existing rc==1/rc==2/rc>2
    // machinery in the CALL/RET codegen (untouched) handles placement,
    // including the hidden-pointer convention for rc>2, on its own. All
    // that's left for the caller to do is reassemble those register(s)
    // back into a real local after the call -- see below.
    ST_ir_inst_t **args;
    u32 n_args;

    u32 max_args = 0;

    if (sig && !sig->is_variadic) {
        u32 n_params = sig->params.count;
        max_args = n_params * 2;
        ST_expr_t **resolved =
            n_params ? ST_arena_push_zeroed(c->arena, sizeof(*resolved) * n_params) : NULL;

        b8 *filled = n_params ? ST_arena_push_zeroed(c->arena, sizeof(b8) * n_params) : NULL;

        u32 pos = 0;
        ST_forrange(0, e->call.args.count) {
            ST_arg_t *arg = &e->call.args.items[i];
            u32 idx = n_params;
            if (arg->name.len) {
                for (u32 k = 0; k < n_params; k++)
                    if (ST_string_eq(sig->params.items[k].name, arg->name)) {
                        idx = k;
                        break;
                    }
            } else
                idx = pos++;
            if (idx >= n_params)
                continue;
            resolved[idx] = arg->value;
            filled[idx] = 1;
        }

        ST_forrange(0, n_params) {
            if (filled[i])
                continue;
            ST_param_t *p = &sig->params.items[i];
            if (p->def)
                resolved[i] = p->def;
            else
                ST_diag_error(&c->diag, e->line, e->col,
                              "internal: missing argument '" ST_sv_fmt "' has no"
                              "default value",
                              ST_sv_args(p->name));
        }

        args = max_args ? ST_arena_push_zeroed(c->arena, sizeof(*args) * max_args) : NULL;
        n_args = 0;

        ST_forrange(0, n_params) {
            ST_expr_t *re = resolved[i];
            if (!re) {
                args[n_args++] = ST_ir_const_int(c->cur, c->sema->tys.prim[ST_ti32], 0);
                continue;
            }
            if (re->ty && (re->ty->kind == ST_TY_STRUCT || re->ty->kind == ST_TY_STRING ||
                          re->ty->kind == ST_TY_TAG_UNION || re->ty->kind == ST_TY_ARRAY))
                ST_lower_push_struct_arg(c, args, &n_args, re, re->ty);
            else
                args[n_args++] = ST_lower_expr(c, re);
        }
    } else {
        n_args = e->call.args.count * 2;
        args = n_args ? ST_arena_push(c->arena, sizeof(*args) * n_args) : NULL;
        u32 idx = 0;

        ST_forrange(0, e->call.args.count) {
            ST_expr_t *ae = e->call.args.items[i].value;
            if (ae->ty && (ae->ty->kind == ST_TY_STRUCT || ae->ty->kind == ST_TY_STRING ||
                          ae->ty->kind == ST_TY_TAG_UNION || ae->ty->kind == ST_TY_ARRAY))
                ST_lower_push_struct_arg(c, args, &idx, ae, ae->ty);
            else
                args[idx++] = ST_lower_expr(c, ae);
        }
        n_args = idx;
    }

    ST_ir_inst_t *result = NULL;
    if (direct) {
        ST_string_t name = e->call.callee->name;
        ST_ir_fn_t *target = ST_ir_module_find_fn(c->module, name);
        if (!target)
            ST_diag_error(&c->diag, e->line, e->col,
                          "internal: call to unknown function '" ST_sv_fmt "'", ST_sv_args(name));
        result = ST_ir_call(c->cur, e->ty, name, target, args, n_args, e->line, e->col);
    } else {
        ST_ir_inst_t *ptr = ST_lower_expr(c, e->call.callee);
        result = ST_ir_call_indirect(c->cur, e->ty, ptr, args, n_args, e->line, e->col);
    }

    // Aggregate result: 'result' is only the call's own generic SSA slot
    // (eightbyte 0), same as any other multi-value return -- see
    // ST_ST_MULTI_BIND's identical call_val/ST_ir_extract pattern. Copy
    // every eightbyte the callee actually sent back (1, 2, or however many
    // the hidden-pointer buffer holds for a large aggregate) into a real
    // local and hand back its address, so this is usable anywhere an
    // aggregate value/address is expected (decl init, struct copy,
    // ST_lower_lvalue_addr, ...).
    if (e->ty && (e->ty->kind == ST_TY_STRUCT || e->ty->kind == ST_TY_TAG_UNION) &&
        e->ty->size > 0) {
        ST_ir_inst_t *slot = ST_ir_alloca(c->fn, &c->sema->tys, e->ty, e->line, e->col);
        u32 n_eb = ST_lower_eight_bytes_count(e->ty);
        ST_forrange(0, n_eb) {
            ST_ty_t *ebty = ST_lower_eight_byte_ty(c, e->ty, i);
            ST_ir_inst_t *v = i == 0 ? result : ST_ir_extract(c->cur, ebty, result, i, e->line, e->col);
            ST_ir_inst_t *fp = ST_lower_field_ptr(c, slot, (i32)(i * 8), ebty, e->line, e->col);
            ST_ir_store(c->cur, ebty, fp, v, e->line, e->col);
        }
        return slot;
    }

    return result;
}

static ST_ir_inst_t *ST_lower_expr(ST_lower_ctx_t *c, ST_expr_t *e) {
    switch (e->kind) {
        case ST_EX_STRUCT_LIT:
            return ST_lower_struct_addr(c, e, e->ty);
        case ST_EX_INT:
            return ST_ir_const_int(c->cur, e->ty, e->ival);
        case ST_EX_BOOL:
        case ST_EX_CHAR:
        case ST_EX_NULL:
            return ST_ir_const_int(c->cur, e->ty, e->kind == ST_EX_NULL ? 0 : e->ival);
        case ST_EX_FLOAT:
            return ST_ir_const_float(c->cur, e->ty, e->fval);

        case ST_EX_ASM:
            return ST_lower_asm_tokens(c, e->asm_.tokens, e->asm_.n_tokens, e->ty, e->line, e->col);

        case ST_EX_IDENT: {
            ST_lower_bind_t *bind = ST_lower_scope_find(c, e->name);
            if (bind) {
                if (bind->kind == ST_BIND_SSA)
                    return ST_ir_read_var(c->cur, bind->key, e->ty);
                if (!ST_lower_ty_is_scalar(e->ty)) {
                    ST_diag_error(&c->diag, e->line, e->col,
                                  "internal: loading aggregate locals isn't lowered yet "
                                  "(lands with struct lowering)");
                    return ST_ir_const_int(c->cur, e->ty, 0);
                }
                return ST_ir_load(c->cur, e->ty, bind->slot, e->line, e->col);
            }
            ST_ir_global_var_t *g = ST_ir_module_find_global(c->module, e->name);
            if (g) {
                ST_ty_t *ptr_ty = ST_ty_ptr(&c->sema->tys, g->ty);
                ST_ir_inst_t *addr = ST_ir_global_addr(c->cur, ptr_ty, e->name, e->line, e->col);
                if (!ST_lower_ty_is_scalar(e->ty)) {
                    ST_diag_error(&c->diag, e->line, e->col,
                                  "internal: loading aggregate globals isn't lowered yet");
                    return ST_ir_const_int(c->cur, e->ty, 0);
                }
                return ST_ir_load(c->cur, e->ty, addr, e->line, e->col);
            }
            {
                // 'x :: expr;' / 'x : T : expr;' constants aren't stored
                // anywhere; using one as a value just lowers its defining
                // expression in place, coercing to the identifier's (possibly
                // annotated) type when that differs from the raw literal's.
                ST_ht_generic_t key = {.tag = e->name.data, .size = e->name.len};
                ST_sym_t *sym = ST_ht_get(&c->sema->globals, key).tag;
                if (sym && sym->kind == ST_SYM_CONST && sym->decl &&
                    sym->decl->kind == ST_DE_CONST) {
                    ST_expr_t *cv = sym->decl->const_.value;
                    ST_ir_inst_t *v = ST_lower_expr(c, cv);
                    if (e->ty && cv->ty && !ST_ty_equal(e->ty, cv->ty) &&
                        ST_lower_ty_is_scalar(e->ty))
                        return ST_ir_cast(c->cur, e->ty, v, e->line, e->col);
                    return v;
                }
            }
            ST_diag_error(&c->diag, e->line, e->col,
                          "internal: '" ST_sv_fmt "' used as a value is not supported yet "
                          "(only local vars/params and direct calls are lowered so far)",
                          ST_sv_args(e->name));
            return ST_ir_const_int(c->cur, e->ty, 0);
        }

        case ST_EX_UNARY: {
            ST_string_t op = e->unary.op;

            if (ST_string_eq_cstr(op, "&"))
                return ST_lower_addr_of(c, e, e->ty);

            if (ST_string_eq_cstr(op, "*")) {
                ST_ir_inst_t *p = ST_lower_expr(c, e->unary.operand);
                if (!ST_lower_ty_is_scalar(e->ty)) {
                    ST_diag_error(&c->diag, e->line, e->col,
                                  "internal: dereferencing to an aggregate isn't lowered yet "
                                  "(lands with struct lowering)");
                    return ST_ir_const_int(c->cur, e->ty, 0);
                }
                return ST_ir_load(c->cur, e->ty, p, e->line, e->col);
            }

            ST_ir_inst_t *v = ST_lower_expr(c, e->unary.operand);
            if (ST_string_eq_cstr(op, "-"))
                return ST_ir_unop(c->cur, ST_ty_is_float(e->ty) ? ST_IR_FNEG : ST_IR_NEG, e->ty, v,
                                  e->line, e->col);
            if (ST_string_eq_cstr(op, "~"))
                return ST_ir_unop(c->cur, ST_IR_NOT, e->ty, v, e->line, e->col);
            if (ST_string_eq_cstr(op, "!") || ST_string_eq_cstr(op, "not")) {
                ST_ir_inst_t *zero = ST_ir_const_int(c->cur, e->unary.operand->ty, 0);
                return ST_ir_binop(c->cur, ST_IR_ICMP_EQ, e->ty, v, zero, e->line, e->col);
            }
            ST_diag_error(&c->diag, e->line, e->col,
                          "internal: unsupported unary operator '" ST_sv_fmt "'", ST_sv_args(op));
            return v;
        }

        case ST_EX_BINARY: {
            if (ST_string_eq_cstr(e->bin.op, "&&"))
                return ST_lower_short_and(c, e);
            if (ST_string_eq_cstr(e->bin.op, "||"))
                return ST_lower_short_or(c, e);

            if (e->bin.l->ty && e->bin.l->ty->kind == ST_TY_STRING) {
                ST_ir_inst_t *args[2];
                args[0] = ST_lower_string_addr(c, e->bin.l);
                args[1] = ST_lower_string_addr(c, e->bin.r);

                ST_ir_inst_t *eq = ST_ir_call(c->cur, e->ty, ST_cstr_to_str("st_string_eq"), NULL,
                                              args, 2, e->line, e->col);
                if (ST_string_eq_cstr(e->bin.op, "=="))
                    return eq;
                return ST_ir_binop(c->cur, ST_IR_ICMP_EQ, e->ty, eq,
                                   ST_ir_const_int(c->cur, e->ty, 0), e->line, e->col);
            }

            ST_ir_inst_t *l = ST_lower_expr(c, e->bin.l);
            ST_ir_inst_t *r = ST_lower_expr(c, e->bin.r);

            b8 is_f = ST_ty_is_float(e->bin.l->ty) || ST_ty_is_float(e->bin.r->ty);
            b8 uns = e->bin.l->ty && !e->bin.l->ty->is_signed;
            ST_ir_op_t op = ST_lower_binop(&c->diag, e, is_f, uns);
            return ST_ir_binop(c->cur, op, e->ty, l, r, e->line, e->col);
        }

        case ST_EX_FIELD: {
            ST_expr_t *fb = e->field.base;
            {
                // Type.Variant (enum / enum_flag) folds to a constant; this
                // must be checked before any of the lvalue-address paths
                // below, which only know how to handle struct field access.
                i64 v;
                if (ST_const_eval(c->sema, e, &v))
                    return ST_ir_const_int(c->cur, e->ty, v);
            }
            ST_ty_t *ftb = fb->ty;
            if (ftb && ftb->kind == ST_TY_PTR)
                ftb = ftb->inner;
            if (ftb && ftb->kind == ST_TY_ARRAY) {
                if (ST_string_eq_cstr(e->field.name, "len"))
                    return ST_ir_const_int(c->cur, e->ty, (i64)ftb->count);
                if (ST_string_eq_cstr(e->field.name, "ptr")) {
                    b8 through_ptr = fb->ty && fb->ty->kind == ST_TY_PTR;
                    ST_ir_inst_t *base =
                        through_ptr ? ST_lower_expr(c, fb) : ST_lower_lvalue_addr(c, fb);
                    return base ? base : ST_ir_const_int(c->cur, e->ty, 0);
                }
            }
            ST_ir_inst_t *a = ST_lower_lvalue_addr(c, e);
            if (!a)
                return ST_ir_const_int(c->cur, e->ty, 0);
            if (!ST_lower_ty_is_scalar(e->ty)) {
                ST_diag_error(&c->diag, e->line, e->col,
                              "internal: using a whole struct field as a value is only "
                              "lowered in declarations and assignments so far");
                return ST_ir_const_int(c->cur, e->ty, 0);
            }
            return ST_ir_load(c->cur, e->ty, a, e->line, e->col);
        }

        case ST_EX_INDEX: {
            ST_ir_inst_t *a = ST_lower_lvalue_addr(c, e);
            if (!a)
                return ST_ir_const_int(c->cur, e->ty, 0);
            if (!ST_lower_ty_is_scalar(e->ty)) {
                ST_diag_error(&c->diag, e->line, e->col,
                              "internal: using a whole struct field as a value is only "
                              "lowered in declarations and assignments so far");
                return ST_ir_const_int(c->cur, e->ty, 0);
            }
            return ST_ir_load(c->cur, e->ty, a, e->line, e->col);
        }

        case ST_EX_STR:
            return ST_lower_string_lit_addr(c, e);

        case ST_EX_STR_FROM_RAW:
            return ST_lower_str_from_raw_addr(c, e);

        case ST_EX_CALL:
            if (ST_lower_is_union_construct(e))
                return ST_lower_union_construct_addr(c, e);
            return ST_lower_call(c, e);

        case ST_EX_CAST:
            return ST_ir_cast(c->cur, e->ty, ST_lower_expr(c, e->cast.operand), e->line, e->col);

        case ST_EX_CSTR: {
            ST_ir_inst_t *a = ST_lower_string_addr(c, e->tyop.operand);
            if (!a)
                return ST_ir_const_int(c->cur, e->ty, 0);
            ST_ir_inst_t *p = ST_lower_field_ptr(c, a, 0, e->ty, e->line, e->col);
            return ST_ir_load(c->cur, e->ty, p, e->line, e->col);
        } break;

        default:
            ST_diag_error(&c->diag, e->line, e->col,
                          "internal: this expression form isn't lowered yet");
            return ST_ir_const_int(c->cur, e->ty, 0);
    }
}

static void ST_lower_body(ST_lower_ctx_t *c, ST_stmts_t *body) {
    ST_forrange(0, body->count) ST_lower_stmt(c, body->items[i]);
}

static void ST_lower_store_assign(ST_lower_ctx_t *c, ST_stmt_t *s, ST_ty_t *ty,
                                  ST_ir_inst_t *addr) {
    ST_ir_inst_t *rhs = ST_lower_expr(c, s->assign.rhs);
    if (!ST_string_eq_cstr(s->assign.op, "=")) {
        ST_ir_inst_t *cur = ST_ir_load(c->cur, ty, addr, s->line, s->col);
        ST_ir_op_t op =
            ST_lower_compound_op(&c->diag, s->assign.op, s->line, s->col, ST_ty_is_float(ty));
        rhs = ST_ir_binop(c->cur, op, ty, cur, rhs, s->line, s->col);
    }
    ST_ir_store(c->cur, ty, addr, rhs, s->line, s->col);
}

static void ST_lower_multi_bind_one(ST_lower_ctx_t *c, ST_stmt_t *s, u32 i, ST_ir_inst_t *v,
                                    ST_ty_t *ty) {
    if (!v || !ty)
        return;
    if (s->multi.declare) {
        if (ST_lower_is_addr_taken(c, s->multi.names[i])) {
            ST_ir_inst_t *slot = ST_ir_alloca(c->fn, &c->sema->tys, ty, s->line, s->col);
            ST_ir_store(c->cur, ty, slot, v, s->line, s->col);
            ST_lower_bind_addr(c, s->multi.names[i], slot, ty);
        } else {
            void *key = (void *)&s->multi.names[i];
            ST_ir_write_var(c->cur, key, v);
            ST_lower_bind_ssa(c, s->multi.names[i], key, ty);
        }
        return;
    }

    ST_lower_bind_t *bind = ST_lower_scope_find(c, s->multi.names[i]);
    if (!bind) {
        ST_diag_error(&c->diag, s->line, s->col,
                      "internal: Unkown assignment target " ST_sv_fmt " is not a known local",
                      ST_sv_args(s->multi.names[i]));
        return;
    }

    if (bind->kind == ST_BIND_ADDR)
        ST_ir_store(c->cur, ty, bind->slot, v, s->line, s->col);
    else
        ST_ir_write_var(c->cur, bind->key, v);
}

static void ST_lower_array_copy(ST_lower_ctx_t *c, ST_ir_inst_t *dst, i32 doff, ST_ir_inst_t *src,
                                i32 soff, ST_ty_t *sty, u32 line, u32 col) {
    ST_ty_t *ety = sty->inner;
    u32 esz = ety->size;
    ST_forrange(0, sty->count) {
        i32 eoff = (i32)(i * esz);
        if (ety->kind == ST_TY_STRUCT)
            ST_lower_struct_copy(c, dst, doff + eoff, src, soff + eoff, ety, line, col);
        else if (ety->kind == ST_TY_STRING) {
            ST_ir_inst_t *sp = ST_lower_field_ptr(c, src, soff + eoff, ety, line, col);
            ST_ir_inst_t *dp = ST_lower_field_ptr(c, dst, doff + eoff, ety, line, col);
            ST_lower_string_copy(c, dp, sp, line, col);
        } else if (ety->kind == ST_TY_ARRAY) {
            ST_ir_inst_t *sp = ST_lower_field_ptr(c, src, soff + eoff, ety, line, col);
            ST_ir_inst_t *v = ST_ir_load(c->cur, ety, sp, line, col);
            ST_ir_inst_t *dp = ST_lower_field_ptr(c, dst, doff + eoff, ety, line, col);
            ST_ir_store(c->cur, ety, dp, v, line, col);
        } else if (ST_lower_ty_is_scalar(ety)) {
            ST_ir_inst_t *sp = ST_lower_field_ptr(c, src, soff + eoff, ety, line, col);
            ST_ir_inst_t *v = ST_ir_load(c->cur, ety, sp, line, col);
            ST_ir_inst_t *dp = ST_lower_field_ptr(c, dst, doff + eoff, ety, line, col);
            ST_ir_store(c->cur, ety, dp, v, line, col);
        } else {
            ST_diag_error(&c->diag, line, col, "internal: array element type is not lowered yet.");
        }
    }
}

static void ST_lower_start_dead_block(ST_lower_ctx_t *c, u32 line, u32 col) {
    ST_ir_block_t *dead = ST_ir_block_new(c->fn, "dead");
    ST_ir_block_seal(dead);
    ST_ir_term_unreachable(dead, line, col);
    c->cur = dead;
}

static void ST_lower_array_lit_into(ST_lower_ctx_t *c, ST_ir_inst_t *base, i32 off, ST_ty_t *sty,
                                    ST_expr_t *lit) {
    ST_ty_t *ety = sty->inner;
    u32 esz = ety->size;
    ST_forrange(0, lit->struct_lit.inits.count) {
        if (i >= sty->count)
            break;
        ST_field_init_t *fi = &lit->struct_lit.inits.items[i];
        i32 eoff = off + (i32)(i * esz);
        if (ety->kind == ST_TY_STRUCT) {
            if (fi->value->kind == ST_EX_STRUCT_LIT)
                ST_lower_struct_lit_into(c, base, eoff, ety, fi->value);
            else {
                ST_ir_inst_t *src = ST_lower_lvalue_addr(c, fi->value);
                if (src)
                    ST_lower_struct_copy(c, base, eoff, src, 0, ety, fi->value->line,
                                         fi->value->col);
            }
        } else if (ety->kind == ST_TY_STRING) {
            ST_ir_inst_t *src = ST_lower_string_addr(c, fi->value);
            ST_ir_inst_t *dp =
                ST_lower_field_ptr(c, base, eoff, ety, fi->value->line, fi->value->col);
            ST_lower_string_copy(c, dp, src, fi->value->line, fi->value->col);
        } else if (ety->kind == ST_TY_ARRAY) {
            if (fi->value->kind == ST_EX_STRUCT_LIT)
                ST_lower_array_lit_into(c, base, off, ety, fi->value);
            else {
                ST_ir_inst_t *src = ST_lower_lvalue_addr(c, fi->value);
                if (src)
                    ST_lower_array_copy(c, base, eoff, src, 0, ety, fi->value->line,
                                        fi->value->col);
            }
        } else if (ST_lower_ty_is_scalar(ety)) {
            ST_ir_inst_t *v = ST_lower_expr(c, fi->value);
            ST_ir_inst_t *fp =
                ST_lower_field_ptr(c, base, eoff, ety, fi->value->line, fi->value->col);
            ST_ir_store(c->cur, ety, fp, v, fi->value->line, fi->value->col);
        } else {
            ST_diag_error(&c->diag, fi->value->line, fi->value->col,
                          "internal: this array element type is not lowered yet.");
        }
    }
}

// @note: appends a decimal-encoded placeholder to sb: '\x01' for a bare
// variable reference (substituted with "[rbp-N]" at emission time) or
// '\x02' for an address-of reference (substituted with "rbp-N"), followed
// by the index into 'refs' and a '\x03' terminator.
static void ST_lower_asm_emit_placeholder(ST_sb_t *sb, b8 is_addr, u32 idx) {
    char buf[32];
    i32 n = snprintf(buf, sizeof(buf), "%c%u%c", is_addr ? 2 : 1, idx, 3);
    ST_forrange(0, (u32)n) ST_da_append(sb, buf[i]);
}

// @note: shared by the statement form (ST_lower_asm_stmt) and the
// expression form (ST_lower_expr's ST_EX_ASM case) -- builds the
// placeholder-substituted template text and ref list from a raw '#asm { .. }'
// token blob, then emits the ST_IR_INLINE_ASM instruction. 'ty' is NULL for
// the statement form (no result value, matches the previous behavior) or the
// expression's type when used as a value -- see the backend's generic
// "spill result to my slot" tail, which only fires when ty is non-void.
static ST_ir_inst_t *ST_lower_asm_tokens(ST_lower_ctx_t *c, ST_token_t *tokens, u32 n_tokens,
                                         ST_ty_t *ty, u32 line, u32 col) {
    ST_sb_t sb = {0};
    struct {
        ST_ir_inst_t **items;
        u32 count, capacity;
    } refs = {0};

    b8 have_line = 0;
    u32 last_line = 0;

    u32 i = 0;
    while (i < n_tokens) {
        ST_token_t *t = &tokens[i];

        b8 is_addr = 0;
        ST_lower_bind_t *bind = NULL;
        u32 advance = 1;

        if (t->kind == ST_TSYMBOL && ST_string_eq_cstr(t->text, "&") && i + 1 < n_tokens &&
            tokens[i + 1].kind == ST_TIDENT) {
            ST_lower_bind_t *b = ST_lower_scope_find(c, tokens[i + 1].text);
            if (b && b->kind == ST_BIND_ADDR) {
                is_addr = 1;
                bind = b;
                advance = 2;
            }
        }

        if (!bind && t->kind == ST_TIDENT) {
            ST_lower_bind_t *b = ST_lower_scope_find(c, t->text);
            if (b && b->kind == ST_BIND_ADDR)
                bind = b;
        }

        // preserve original line breaks (and re-indent) so the emitted asm
        // reads like the block the person actually wrote.
        if (!have_line || t->line != last_line) {
            if (have_line)
                ST_da_append(&sb, '\n');
            ST_append_to_builder(&sb, "    ");
            have_line = 1;
            last_line = t->line;
        } else {
            ST_da_append(&sb, ' ');
        }

        if (bind) {
            ST_lower_asm_emit_placeholder(&sb, is_addr, refs.count);
            ST_da_append_arena(c->arena, &refs, bind->slot);
            i += advance;
            continue;
        }

        for (u32 k = 0; k < t->text.len; k++)
            ST_da_append(&sb, t->text.data[k]);
        i += 1;
    }
    if (have_line)
        ST_da_append(&sb, '\n');

    ST_string_t tmpl = {0};
    tmpl.len = sb.count;
    if (sb.count) {
        tmpl.data = ST_arena_push(c->arena, sb.count);
        memcpy(tmpl.data, sb.items, sb.count);
    }
    if (sb.items)
        free(sb.items);

    return ST_ir_inline_asm(c->cur, tmpl, refs.items, refs.count, ty, line, col);
}

static void ST_lower_asm_stmt(ST_lower_ctx_t *c, ST_stmt_t *s) {
    ST_lower_asm_tokens(c, s->asm_.tokens, s->asm_.n_tokens, NULL, s->line, s->col);
}

static void ST_lower_stmt(ST_lower_ctx_t *c, ST_stmt_t *s) {
    switch (s->kind) {
        case ST_ST_EXPR:
            ST_lower_expr(c, s->expr);
            break;

        case ST_ST_DECL: {
            b8 taken = ST_lower_is_addr_taken(c, s->decl.name);

            ST_ty_t *ty = s->decl.te     ? ST_lower_tyexpr(c, s->decl.te)
                          : s->decl.init ? s->decl.init->ty
                                         : NULL;
            if (!ty) {
                ST_diag_error(&c->diag, s->line, s->col,
                              "internal: declaration has no resolvable type");
                break;
            }

            if (ty->kind == ST_TY_DYN_ARRAY && s->decl.init &&
                s->decl.init->kind == ST_EX_STRUCT_LIT && !s->decl.init->struct_lit.type_name.len) {
                u32 count = s->decl.init->struct_lit.inits.count;
                ty = ST_ty_array(&c->sema->tys, ty->inner, count);
            }

            if (ty->kind == ST_TY_STRUCT) {
                ST_ir_inst_t *slot = ST_ir_alloca(c->fn, &c->sema->tys, ty, s->line, s->col);
                ST_lower_struct_zero(c, slot, 0, ty, s->line, s->col);
                if (s->decl.init) {
                    if (s->decl.init->kind == ST_EX_STRUCT_LIT)
                        ST_lower_struct_lit_into(c, slot, 0, ty, s->decl.init);
                    else {
                        ST_ir_inst_t *src = ST_lower_lvalue_addr(c, s->decl.init);
                        if (src)
                            ST_lower_struct_copy(c, slot, 0, src, 0, ty, s->line, s->col);
                    }
                }
                ST_lower_bind_addr(c, s->decl.name, slot, ty);
                break;
            }

            if (ty->kind == ST_TY_TAG_UNION) {
                ST_ir_inst_t *slot = ST_ir_alloca(c->fn, &c->sema->tys, ty, s->line, s->col);
                if (s->decl.init) {
                    if (ST_lower_is_union_construct(s->decl.init))
                        ST_lower_union_construct_into(c, slot, 0, ty, s->decl.init);
                    else {
                        ST_ir_inst_t *src = ST_lower_lvalue_addr(c, s->decl.init);
                        if (src)
                            ST_lower_raw_copy(c, slot, 0, src, 0, ty->size, s->line, s->col);
                    }
                }
                ST_lower_bind_addr(c, s->decl.name, slot, ty);
                break;
            }

            if (ty->kind == ST_TY_STRING) {
                ST_ir_inst_t *slots = ST_ir_alloca(c->fn, &c->sema->tys, ty, s->line, s->col);
                if (s->decl.init) {
                    ST_ir_inst_t *src = ST_lower_string_addr(c, s->decl.init);
                    if (src)
                        ST_lower_string_copy(c, slots, src, s->line, s->col);
                } else {
                    ST_ty_t *dty = ST_ty_ptr(&c->sema->tys, c->sema->tys.prim[ST_tchar]);
                    ST_ty_t *lty = c->sema->tys.prim[ST_ti64];
                    ST_ir_inst_t *p = ST_lower_field_ptr(c, slots, 0, dty, s->line, s->col);
                    ST_ir_store(c->cur, dty, p, ST_ir_const_int(c->cur, dty, 0), s->line, s->col);
                    p = ST_lower_field_ptr(c, slots, 8, lty, s->line, s->col);
                    ST_ir_store(c->cur, lty, p, ST_ir_const_int(c->cur, lty, 0), s->line, s->col);
                }
                ST_lower_bind_addr(c, s->decl.name, slots, ty);
                break;
            }
            if (ty->kind == ST_TY_ARRAY || ty->kind == ST_TY_DYN_ARRAY) {
                ST_ir_inst_t *slot = ST_ir_alloca(c->fn, &c->sema->tys, ty, s->line, s->col);
                ST_lower_array_zero(c, slot, 0, ty, s->line, s->col);

                if (s->decl.init) {
                    if (s->decl.init->kind == ST_EX_STRUCT_LIT)
                        ST_lower_array_lit_into(c, slot, 0, ty, s->decl.init);
                    else {
                        ST_ir_inst_t *src = ST_lower_lvalue_addr(c, s->decl.init);
                        if (src)
                            ST_lower_array_copy(c, slot, 0, src, 0, ty, s->line, s->col);
                    }
                }
                ST_lower_bind_addr(c, s->decl.name, slot, ty);
                break;
            }

            if (!ST_lower_ty_is_scalar(ty)) {
                ST_diag_error(&c->diag, s->line, s->col,
                              "internal: this declaration type isn't lowered yet "
                              "(arrays come later)");
                break;
            }

            ST_ir_inst_t *init;
            if (s->decl.init)
                init = ST_lower_expr(c, s->decl.init);
            else if (ST_ty_is_float(ty))
                init = ST_ir_const_float(c->cur, ty, 0.0);
            else
                init = ST_ir_const_int(c->cur, ty, 0);

            if (init && init->ty && init->ty != ty) {
                b8 same_repr = init->ty->size == ty->size &&
                               ((ST_ty_is_int(init->ty) && ST_ty_is_int(ty)) ||
                                (ST_ty_is_float(init->ty) && ST_ty_is_float(ty)));
                if (same_repr)
                    init->ty = ty;
                else
                    init = ST_ir_cast(c->cur, ty, init, s->line, s->col);
            }

            if (taken) {
                ST_ir_inst_t *slot = ST_ir_alloca(c->fn, &c->sema->tys, ty, s->line, s->col);
                ST_ir_store(c->cur, ty, slot, init, s->line, s->col);
                ST_lower_bind_addr(c, s->decl.name, slot, ty);
            } else {
                ST_ir_write_var(c->cur, (void *)s, init);
                ST_lower_bind_ssa(c, s->decl.name, (void *)s, ty);
            }
            break;
        }

        case ST_ST_ASSIGN: {
            ST_expr_t *lhs = s->assign.lhs;

            if (lhs->ty && lhs->ty->kind == ST_TY_STRUCT) {
                if (!ST_string_eq_cstr(s->assign.op, "=")) {
                    ST_diag_error(&c->diag, s->line, s->col,
                                  "internal: compound assignment on struct values "
                                  "isn't a thing");
                    break;
                }
                ST_ir_inst_t *dst = ST_lower_lvalue_addr(c, lhs);
                if (!dst)
                    break;
                if (s->assign.rhs->kind == ST_EX_STRUCT_LIT) {
                    ST_lower_struct_zero(c, dst, 0, lhs->ty, s->line, s->col);
                    ST_lower_struct_lit_into(c, dst, 0, lhs->ty, s->assign.rhs);
                } else {
                    ST_ir_inst_t *src = ST_lower_lvalue_addr(c, s->assign.rhs);
                    if (src)
                        ST_lower_struct_copy(c, dst, 0, src, 0, lhs->ty, s->line, s->col);
                }
                break;
            }
            if (lhs->ty && lhs->ty->kind == ST_TY_TAG_UNION) {
                if (!ST_string_eq_cstr(s->assign.op, "=")) {
                    ST_diag_error(&c->diag, s->line, s->col,
                                  "internal: compound assignment on tag_union values "
                                  "isn't a thing");
                    break;
                }
                ST_ir_inst_t *dst = ST_lower_lvalue_addr(c, lhs);
                if (!dst)
                    break;
                if (ST_lower_is_union_construct(s->assign.rhs))
                    ST_lower_union_construct_into(c, dst, 0, lhs->ty, s->assign.rhs);
                else {
                    ST_ir_inst_t *src = ST_lower_lvalue_addr(c, s->assign.rhs);
                    if (src)
                        ST_lower_raw_copy(c, dst, 0, src, 0, lhs->ty->size, s->line, s->col);
                }
                break;
            }
            if (lhs->ty && lhs->ty->kind == ST_TY_STRING) {
                if (!ST_string_eq_cstr(s->assign.op, "=")) {
                    ST_diag_error(&c->diag, s->line, s->col,
                                  "internal: compound assignment on strings is not a thing");
                    break;
                }
                ST_ir_inst_t *dst = ST_lower_lvalue_addr(c, lhs);
                ST_ir_inst_t *src = dst ? ST_lower_string_addr(c, s->assign.rhs) : NULL;
                if (dst && src)
                    ST_lower_string_copy(c, dst, src, s->line, s->col);
                break;
            }

            if (lhs->kind == ST_EX_FIELD || lhs->kind == ST_EX_INDEX) {
                ST_ir_inst_t *addr = ST_lower_lvalue_addr(c, lhs);
                if (addr)
                    ST_lower_store_assign(c, s, lhs->ty, addr);
                break;
            }

            if (lhs->kind == ST_EX_UNARY && ST_string_eq_cstr(lhs->unary.op, "*")) {
                if (!ST_lower_ty_is_scalar(lhs->ty)) {
                    ST_diag_error(&c->diag, s->line, s->col,
                                  "internal: storing aggregates through pointers "
                                  "isn't lowered yet "
                                  "(lands with struct lowering)");
                    break;
                }
                ST_ir_inst_t *addr = ST_lower_expr(c, lhs->unary.operand);
                ST_lower_store_assign(c, s, lhs->ty, addr);
                break;
            }

            if (lhs->kind != ST_EX_IDENT) {
                ST_diag_error(&c->diag, s->line, s->col,
                              "internal: only assignment to locals and '*ptr' targets is "
                              "lowered right now (field/index targets land with structs)");
                break;
            }

            ST_lower_bind_t *bind = ST_lower_scope_find(c, lhs->name);
            if (!bind) {
                ST_ir_global_var_t *g = ST_ir_module_find_global(c->module, lhs->name);
                if (g) {
                    ST_ty_t *ptr_ty = ST_ty_ptr(&c->sema->tys, g->ty);
                    ST_ir_inst_t *addr =
                        ST_ir_global_addr(c->cur, ptr_ty, lhs->name, s->line, s->col);
                    ST_lower_store_assign(c, s, lhs->ty ? lhs->ty : g->ty, addr);
                    break;
                }
                ST_diag_error(&c->diag, s->line, s->col,
                              "internal: assignment target '" ST_sv_fmt "' is not a known local",
                              ST_sv_args(lhs->name));
                break;
            }

            if (bind->kind == ST_BIND_ADDR) {
                ST_lower_store_assign(c, s, lhs->ty ? lhs->ty : bind->ty, bind->slot);
                break;
            }

            ST_ir_inst_t *rhs = ST_lower_expr(c, s->assign.rhs);
            if (!ST_string_eq_cstr(s->assign.op, "=")) {
                ST_ir_inst_t *cur = ST_ir_read_var(c->cur, bind->key, lhs->ty);
                ST_ir_op_t op = ST_lower_compound_op(&c->diag, s->assign.op, s->line, s->col,
                                                     ST_ty_is_float(lhs->ty));
                rhs = ST_ir_binop(c->cur, op, lhs->ty, cur, rhs, s->line, s->col);
            }
            ST_ir_write_var(c->cur, bind->key, rhs);
            break;
        }

        case ST_ST_RETURN: {
            b8 agg_ret = s->ret.values.count == 1 && s->ret.values.items[0]->ty &&
                        (s->ret.values.items[0]->ty->kind == ST_TY_STRUCT ||
                         s->ret.values.items[0]->ty->kind == ST_TY_TAG_UNION);

            ST_ir_inst_t **vals;
            u32 n_vals;

            if (agg_ret) {
                // Split the single aggregate return value into its real
                // eightbyte-classified register slots -- same technique as
                // ST_lower_push_struct_arg for arguments -- so 'vals' lines
                // up 1:1 with the synthetic entries ST_lower_register_fn
                // already put in this function's fn_ty->rets, and the
                // existing rax/xmm0 (+rdx/xmm1, +hidden-pointer-buffer)
                // RET terminator codegen places them correctly with no
                // further changes needed there.
                ST_ty_t *st = s->ret.values.items[0]->ty;
                ST_ir_inst_t *addr = ST_lower_struct_addr(c, s->ret.values.items[0], st);
                u32 n_eb = ST_lower_eight_bytes_count(st);
                vals = n_eb ? ST_arena_push(c->arena, sizeof(*vals) * n_eb) : NULL;
                n_vals = n_eb;
                ST_forrange(0, n_eb) {
                    ST_ty_t *ebty = ST_lower_eight_byte_ty(c, st, i);
                    if (!addr) {
                        vals[i] = ST_ir_const_int(c->cur, ebty, 0);
                        continue;
                    }
                    ST_ir_inst_t *fp = ST_lower_field_ptr(c, addr, (i32)(i * 8), ebty, s->line, s->col);
                    vals[i] = ST_ir_load(c->cur, ebty, fp, s->line, s->col);
                }
            } else {
                vals = s->ret.values.count
                           ? ST_arena_push(c->arena, sizeof(*vals) * s->ret.values.count)
                           : NULL;
                ST_forrange(0, s->ret.values.count) vals[i] = ST_lower_expr(c, s->ret.values.items[i]);
                n_vals = s->ret.values.count;
            }

            ST_lower_run_defers(c, 0);
            ST_ir_term_ret(c->cur, vals, n_vals, s->line, s->col);
            ST_lower_start_dead_block(c, s->line, s->col);
            break;
        }

        case ST_ST_IF: {
            ST_ir_inst_t *cond = ST_lower_expr(c, s->if_.cond);
            if (!cond)
                break;

            ST_ir_block_t *then_b = ST_ir_block_new(c->fn, "if_then");
            ST_ir_block_t *else_b = s->if_.else_stmt ? ST_ir_block_new(c->fn, "if_else") : NULL;

            ST_ir_block_t *join = ST_ir_block_new(c->fn, "if_end");
            ST_ir_term_condbr(c->cur, cond, then_b, else_b ? else_b : join, s->line, s->col);

            u32 mark = c->defers.count;
            ST_ir_block_seal(then_b);
            c->cur = then_b;
            ST_lower_body(c, &s->if_.then_body);
            if (!ST_ir_block_is_terminated(c->cur)) {
                ST_lower_run_defers(c, mark);
                ST_ir_term_br(c->cur, join, s->line, s->col);
            }

            c->defers.count = mark;
            if (else_b) {
                ST_ir_block_seal(else_b);
                c->cur = else_b;
                ST_lower_stmt(c, s->if_.else_stmt);
                if (!ST_ir_block_is_terminated(c->cur)) {
                    ST_lower_run_defers(c, mark);
                    ST_ir_term_br(c->cur, join, s->line, s->col);
                }
                c->defers.count = mark;
            }
            ST_ir_block_seal(join);
            c->cur = join;
            if (join->preds.count == 0)
                ST_ir_term_unreachable(join, s->line, s->col);
        } break;

        case ST_ST_WHILE: {
            ST_ir_block_t *while_begin = ST_ir_block_new(c->fn, "while_begin");
            ST_ir_block_t *while_body = ST_ir_block_new(c->fn, "while_body");
            ST_ir_block_t *while_end = ST_ir_block_new(c->fn, "while_end");

            ST_ir_term_br(c->cur, while_begin, s->line, s->col);
            c->cur = while_begin;

            ST_ir_inst_t *cond = ST_lower_expr(c, s->while_.cond);
            if (!cond) {
                ST_ir_block_seal(while_begin);
                ST_ir_block_seal(while_end);
                c->cur = while_end;
                break;
            }
            ST_ir_term_condbr(c->cur, cond, while_body, while_end, s->line, s->col);
            ST_ir_block_seal(while_body);
            c->cur = while_body;

            u32 mark = c->defers.count;
            ST_lower_loop_t loop = {while_end, while_begin, mark, c->loop};

            c->loop = &loop;
            ST_lower_body(c, &s->while_.body);
            c->loop = loop.parent;

            if (!ST_ir_block_is_terminated(c->cur)) {
                ST_lower_run_defers(c, mark);
                ST_ir_term_br(c->cur, while_begin, s->line, s->col);
            }

            c->defers.count = mark;
            ST_ir_block_seal(while_begin);
            ST_ir_block_seal(while_end);
            c->cur = while_end;
        } break;

        case ST_ST_BREAK: {
            if (!c->loop) {
                ST_diag_error(&c->diag, s->line, s->col, "'break': used outside of a loop");
                break;
            }
            ST_lower_run_defers(c, c->loop->defer_count);
            ST_ir_term_br(c->cur, c->loop->br_target, s->line, s->col);
            ST_lower_start_dead_block(c, s->line, s->col);
        } break;

        case ST_ST_CONTINUE: {
            if (!c->loop) {
                ST_diag_error(&c->diag, s->line, s->col, "'continue': used outside of a loop");
                break;
            }
            ST_lower_run_defers(c, c->loop->defer_count);
            ST_ir_term_br(c->cur, c->loop->con_target, s->line, s->col);
            ST_lower_start_dead_block(c, s->line, s->col);
        } break;

        case ST_ST_BLOCK:
            ST_forrange(0, s->block.count) ST_lower_stmt(c, s->block.items[i]);
            break;

        case ST_ST_MULTI_BIND: {
            b8 call_from = (s->multi.n_names > 1 && s->multi.values.count == 1 &&
                            s->multi.values.items[0]->kind == ST_EX_CALL);
            if (call_from) {
                ST_expr_t *callee = s->multi.values.items[0];
                ST_ir_inst_t *call_val = ST_lower_expr(c, callee);
                ST_tys_t *ret_tys = NULL;
                if (callee->call.callee->kind == ST_EX_IDENT) {
                    ST_ir_fn_t *target = ST_ir_module_find_fn(c->module, callee->call.callee->name);
                    if (target)
                        ret_tys = &target->ty->rets;
                } else {
                    ST_diag_error(&c->diag, s->line, s->col,
                                  "internal: multi return func not implemented");
                    break;
                }
                if (!ret_tys || ret_tys->count != s->multi.n_names)
                    break;
                ST_forrange(0, s->multi.n_names) {
                    ST_ty_t *rt = ret_tys->items[i];
                    if (!ST_lower_ty_is_scalar(rt)) {
                        ST_diag_error(&c->diag, s->line, s->col,
                                      "internal: Only scalar value are supported right now");
                        continue;
                    }
                    ST_ir_inst_t *val =
                        i == 0 ? call_val : ST_ir_extract(c->cur, rt, call_val, i, s->line, s->col);

                    ST_lower_multi_bind_one(c, s, i, val, rt);
                }
            }

            if (s->multi.values.count != s->multi.n_names)
                break;
#define ST_IR_TEMP_SIZE 256
            ST_ir_inst_t *vals[ST_IR_TEMP_SIZE] = {0};
            ST_ty_t *tys[ST_IR_TEMP_SIZE] = {0};
            u32 n = s->multi.n_names;
            if (n > ST_array_len(vals)) {
                ST_diag_error(&c->diag, s->line, s->col,
                              "internal: more than %u in a multi bind is not lowered yet",
                              ST_array_len(vals));
                break;
            }

            ST_forrange(0, n) {
                ST_expr_t *v = s->multi.values.items[i];
                tys[i] = v->ty;
                if (!ST_lower_ty_is_scalar(tys[i])) {
                    ST_diag_error(&c->diag, s->line, s->col,
                                  "internal: Only scalar value are supported right now");
                    tys[i] = NULL;
                    continue;
                }

                vals[i] = ST_lower_expr(c, v);
            }

            ST_forrange(0, n) {
                ST_lower_multi_bind_one(c, s, i, vals[i], tys[i]);
            }
        } break;

        case ST_ST_DEFER: {
            ST_da_append_arena(c->arena, &c->defers, s->defer_stmt);
        } break;

        case ST_ST_FOR_RANGE: {
            ST_ir_inst_t *lo_v = ST_lower_expr(c, s->for_range.lo);
            ST_ir_inst_t *hi_v = ST_lower_expr(c, s->for_range.hi);
            if (!lo_v || !hi_v)
                break;

            ST_ty_t *lo_ty = s->for_range.lo->ty;
            ST_ty_t *hi_ty = s->for_range.hi->ty;
            ST_ty_t *ity = lo_ty;
            if (hi_ty && (!ity || hi_ty->width > ity->width))
                ity = hi_ty;
            if (!ity)
                ity = c->sema->tys.prim[ST_ti64];

            if (s->for_range.iter_te)
                ity = ST_lower_tyexpr(c, s->for_range.iter_te);

            if (lo_v->ty != ity)
                lo_v = ST_ir_cast(c->cur, ity, lo_v, s->line, s->col);
            if (hi_v->ty != ity)
                hi_v = ST_ir_cast(c->cur, ity, hi_v, s->line, s->col);

            ST_ty_t *bty = c->sema->tys.prim[ST_tbool];
            ST_ir_inst_t *reversed =
                ST_ir_binop(c->cur, ity->is_signed ? ST_IR_ICMP_SLT : ST_IR_ICMP_ULT, bty, hi_v,
                            lo_v, s->line, s->col);

            ST_ir_inst_t *start_v = lo_v;
            if (!s->for_range.inclusive) {
                ST_ir_inst_t *reversed_ext = ST_ir_cast(c->cur, ity, reversed, s->line, s->col);
                start_v = ST_ir_binop(c->cur, ST_IR_SUB, ity, lo_v, reversed_ext, s->line, s->col);
            }

            b8 taken = ST_lower_is_addr_taken(c, s->for_range.iter);
            ST_ir_inst_t *slot = NULL;
            if (taken) {
                slot = ST_ir_alloca(c->fn, &c->sema->tys, ity, s->line, s->col);
                ST_ir_store(c->cur, ity, slot, start_v, s->line, s->col);
                ST_lower_bind_addr(c, s->for_range.iter, slot, ity);
            } else {
                ST_ir_write_var(c->cur, s, start_v);
                ST_lower_bind_ssa(c, s->for_range.iter, s, ity);
            }

            ST_ir_block_t *for_begin = ST_ir_block_new(c->fn, "for_begin");
            ST_ir_block_t *for_body = ST_ir_block_new(c->fn, "for_body");
            ST_ir_block_t *for_step = ST_ir_block_new(c->fn, "for_step");
            ST_ir_block_t *for_end = ST_ir_block_new(c->fn, "for_end");

            ST_ir_term_br(c->cur, for_begin, s->line, s->col);
            c->cur = for_begin;
            ST_ir_inst_t *cur = taken ? ST_ir_load(c->cur, ity, slot, s->line, s->col)
                                      : ST_ir_read_var(c->cur, s, ity);

            ST_ir_op_t asc_op = s->for_range.inclusive
                                    ? (ity->is_signed ? ST_IR_ICMP_SLE : ST_IR_ICMP_ULE)
                                    : (ity->is_signed ? ST_IR_ICMP_SLT : ST_IR_ICMP_ULT);
            ST_ir_op_t desc_op = ity->is_signed ? ST_IR_ICMP_SGE : ST_IR_ICMP_UGE;
            // cond = reverse && cur desc_op
            ST_ir_inst_t *asc_cond = ST_ir_binop(c->cur, asc_op, bty, cur, hi_v, s->line, s->col);
            ST_ir_inst_t *desc_cond = ST_ir_binop(c->cur, desc_op, bty, cur, hi_v, s->line, s->col);
            ST_ir_inst_t *zero_b = ST_ir_const_int(c->cur, bty, 0);
            ST_ir_inst_t *not_rev =
                ST_ir_binop(c->cur, ST_IR_ICMP_EQ, bty, reversed, zero_b, s->line, s->col);
            ST_ir_inst_t *take_desc =
                ST_ir_binop(c->cur, ST_IR_AND, bty, reversed, desc_cond, s->line, s->col);
            ST_ir_inst_t *take_asc =
                ST_ir_binop(c->cur, ST_IR_AND, bty, not_rev, asc_cond, s->line, s->col);
            ST_ir_inst_t *cond =
                ST_ir_binop(c->cur, ST_IR_OR, bty, take_desc, take_asc, s->line, s->col);

            ST_ir_term_condbr(c->cur, cond, for_body, for_end, s->line, s->col);
            ST_ir_block_seal(for_body);
            c->cur = for_body;

            u32 defer_mark = c->defers.count;
            ST_lower_loop_t loop = {for_end, for_step, defer_mark, c->loop};
            c->loop = &loop;
            ST_lower_body(c, &s->for_range.body);
            c->loop = loop.parent;

            if (!ST_ir_block_is_terminated(c->cur)) {
                ST_lower_run_defers(c, defer_mark);
                ST_ir_term_br(c->cur, for_step, s->line, s->col);
            }
            c->defers.count = defer_mark;
            ST_ir_block_seal(for_step);
            c->cur = for_step;

            ST_ir_inst_t *cur2 = taken ? ST_ir_load(c->cur, ity, slot, s->line, s->col)
                                       : ST_ir_read_var(c->cur, s, ity);

            ST_ir_inst_t *rev_ext = ST_ir_cast(c->cur, ity, reversed, s->line, s->col);
            ST_ir_inst_t *step = ST_ir_binop(c->cur, ST_IR_MUL, ity, rev_ext,
                                             ST_ir_const_int(c->cur, ity, -2), s->line, s->col);

            step = ST_ir_binop(c->cur, ST_IR_ADD, ity, step, ST_ir_const_int(c->cur, ity, 1),
                               s->line, s->col);

            ST_ir_inst_t *next = ST_ir_binop(c->cur, ST_IR_ADD, ity, cur2, step, s->line, s->col);
            if (taken)
                ST_ir_store(c->cur, ity, slot, next, s->line, s->col);
            else
                ST_ir_write_var(c->cur, s, next);

            ST_ir_term_br(c->cur, for_begin, s->line, s->col);
            ST_ir_block_seal(for_begin);
            ST_ir_block_seal(for_end);
            c->cur = for_end;
        } break;

        case ST_ST_FOR_ARRAY: {
            ST_expr_t *target = s->for_array.target;
            ST_ty_t *tt = target->ty;
            if (!tt)
                break;

            ST_ty_t *ity = c->sema->tys.prim[ST_ti64];
            ST_ty_t *elem_ty = NULL;
            ST_ir_inst_t *data_ptr = NULL;
            ST_ir_inst_t *len_v = NULL;
            if (tt->kind == ST_TY_ARRAY) {
                elem_ty = tt->inner;
                data_ptr = ST_lower_lvalue_addr(c, target);
                len_v = ST_ir_const_int(c->cur, ity, (i64)tt->count);
            } else if (tt->kind == ST_TY_DYN_ARRAY || tt->kind == ST_TY_STRING) {
                elem_ty = tt->kind == ST_TY_STRING ? c->sema->tys.prim[ST_tchar] : tt->inner;
                ST_ir_inst_t *base = ST_lower_lvalue_addr(c, target);
                if (!base)
                    break;
                ST_ty_t *dptr_ty = ST_ty_ptr(&c->sema->tys, elem_ty);
                ST_ir_inst_t *pfield = ST_lower_field_ptr(c, base, 0, dptr_ty, s->line, s->col);
                data_ptr = ST_ir_load(c->cur, dptr_ty, pfield, s->line, s->col);
                ST_ir_inst_t *lfield = ST_lower_field_ptr(c, base, 8, ity, s->line, s->col);
                len_v = ST_ir_load(c->cur, ity, lfield, s->line, s->col);
            } else {
                ST_diag_error(&c->diag, s->line, s->col,
                              "internal: cannot iterate a value of this type yet.");
                break;
            }
            if (!data_ptr || !elem_ty)
                break;
            ST_ir_write_var(c->cur, s, ST_ir_const_int(c->cur, ity, 0));
            ST_ir_block_t *for_begin = ST_ir_block_new(c->fn, "forarr_begin");
            ST_ir_block_t *for_body = ST_ir_block_new(c->fn, "forarr_body");
            ST_ir_block_t *for_step = ST_ir_block_new(c->fn, "forarr_step");
            ST_ir_block_t *for_end = ST_ir_block_new(c->fn, "forarr_end");

            ST_ir_term_br(c->cur, for_begin, s->line, s->col);
            c->cur = for_begin;

            ST_ir_inst_t *idx = ST_ir_read_var(c->cur, s, ity);
            ST_ir_inst_t *cond = ST_ir_binop(c->cur, ST_IR_ICMP_SLT, c->sema->tys.prim[ST_tbool],
                                             idx, len_v, s->line, s->col);
            ST_ir_term_condbr(c->cur, cond, for_body, for_end, s->line, s->col);
            ST_ir_block_seal(for_body);
            c->cur = for_body;
            ST_ty_t *eptr_ty = ST_ty_ptr(&c->sema->tys, elem_ty);
            u32 scale = elem_ty->size ? elem_ty->size : 1;
            ST_ir_inst_t *elem_ptr =
                ST_ir_addr(c->cur, eptr_ty, data_ptr, idx, scale, 0, s->line, s->col);
            if (ST_lower_ty_is_scalar(elem_ty)) {
                ST_ir_inst_t *ev = ST_ir_load(c->cur, elem_ty, elem_ptr, s->line, s->col);
                b8 taken = ST_lower_is_addr_taken(c, s->for_array.iter);
                if (taken) {
                    ST_ir_inst_t *slot =
                        ST_ir_alloca(c->fn, &c->sema->tys, elem_ty, s->line, s->col);
                    ST_ir_store(c->cur, elem_ty, slot, ev, s->line, s->col);
                    ST_lower_bind_addr(c, s->for_array.iter, slot, elem_ty);
                } else {
                    ST_ir_write_var(c->cur, &s->for_array, ev);
                    ST_lower_bind_ssa(c, s->for_array.iter, &s->for_array, elem_ty);
                }
            } else {
                ST_lower_bind_addr(c, s->for_array.iter, elem_ptr, elem_ty);
            }

            u32 defer_mark = c->defers.count;
            ST_lower_loop_t loop = {for_end, for_step, defer_mark, c->loop};
            c->loop = &loop;
            ST_lower_body(c, &s->for_array.body);
            c->loop = loop.parent;

            if (!ST_ir_block_is_terminated(c->cur)) {
                ST_lower_run_defers(c, defer_mark);
                ST_ir_term_br(c->cur, for_step, s->line, s->col);
            }
            c->defers.count = defer_mark;
            ST_ir_block_seal(for_step);
            c->cur = for_step;

            ST_ir_inst_t *idx2 = ST_ir_read_var(c->cur, s, ity);
            ST_ir_inst_t *next = ST_ir_binop(c->cur, ST_IR_ADD, ity, idx2,
                                             ST_ir_const_int(c->cur, ity, 1), s->line, s->col);
            ST_ir_write_var(c->cur, s, next);
            ST_ir_term_br(c->cur, for_begin, s->line, s->col);

            ST_ir_block_seal(for_begin);
            ST_ir_block_seal(for_end);
            c->cur = for_end;
        } break;

        case ST_ST_LABEL: {
            ST_ir_block_t *blk = ST_lower_label_block(c, s->label);
            if (!ST_ir_block_is_terminated(c->cur))
                ST_ir_term_br(c->cur, blk, s->line, s->col);
            c->cur = blk;
        } break;

        case ST_ST_GODOWN: {
            ST_ir_block_t *blk = ST_lower_label_block(c, s->label);
            ST_ir_term_br(c->cur, blk, s->line, s->col);
            ST_lower_start_dead_block(c, s->line, s->col);
        } break;

        case ST_ST_SWITCH: {
            ST_ty_t *cond_ty = s->switch_.cond->ty;
            b8 is_union = cond_ty && cond_ty->kind == ST_TY_TAG_UNION;

            ST_ir_inst_t *cond_v;
            if (is_union) {
                ST_ir_inst_t *addr = ST_lower_lvalue_addr(c, s->switch_.cond);
                if (!addr)
                    break;
                ST_ir_inst_t *kindp =
                    ST_lower_field_ptr(c, addr, 0, c->sema->tys.prim[ST_ti64], s->line, s->col);
                cond_v = ST_ir_load(c->cur, c->sema->tys.prim[ST_ti64], kindp, s->line, s->col);
            } else {
                cond_v = ST_lower_expr(c, s->switch_.cond);
            }
            if (!cond_v)
                break;
            if (!is_union && !ST_lower_ty_is_scalar(cond_ty)) {
                ST_diag_error(&c->diag, s->line, s->col,
                              "internal: switching on this type is not lowered yet");
                break;
            }

            b8 is_f = !is_union && ST_ty_is_float(cond_ty);
            ST_ty_t *bty = c->sema->tys.prim[ST_tbool];
            ST_case_t *default_case = NULL;
            ST_forrange(0, s->switch_.cases.count) {
                if (s->switch_.cases.items[i].values.count == 0)
                    default_case = &s->switch_.cases.items[i];
            }

            ST_ir_block_t *join = ST_ir_block_new(c->fn, "switch_end");
            u32 mark = c->defers.count;

            ST_forrange(0, s->switch_.cases.count) {
                ST_case_t *cs = &s->switch_.cases.items[i];
                if (cs->values.count == 0)
                    continue;

                ST_ir_inst_t *case_v;
                if (is_union) {
                    ST_string_t vname = cs->values.items[0]->name;
                    i64 idx = -1;
                    ST_forrange(0, cond_ty->decl->tag_union.variants.count)
                        if (ST_string_eq(cond_ty->decl->tag_union.variants.items[i].name, vname)) {
                            idx = (i64)i;
                            break;
                        }
                    case_v = ST_ir_const_int(c->cur, c->sema->tys.prim[ST_ti64], idx);
                } else {
                    case_v = ST_lower_expr(c, cs->values.items[0]);
                }
                ST_ir_op_t eq_op = is_f ? ST_IR_FCMP_EQ : ST_IR_ICMP_EQ;
                ST_ir_inst_t *eq =
                    ST_ir_binop(c->cur, eq_op, bty, cond_v, case_v, cs->line, cs->col);

                ST_ir_block_t *body_b = ST_ir_block_new(c->fn, "case_body");
                ST_ir_block_t *next_b = ST_ir_block_new(c->fn, "case_test");
                ST_ir_term_condbr(c->cur, eq, body_b, next_b, cs->line, cs->col);

                ST_ir_block_seal(body_b);
                c->cur = body_b;
                ST_lower_body(c, &cs->body);
                if (!ST_ir_block_is_terminated(c->cur)) {
                    ST_lower_run_defers(c, mark);
                    ST_ir_term_br(c->cur, join, cs->line, cs->col);
                }
                c->defers.count = mark;
                ST_ir_block_seal(next_b);
                c->cur = next_b;
            }

            if (default_case) {
                ST_lower_body(c, &default_case->body);
                if (!ST_ir_block_is_terminated(c->cur)) {
                    ST_lower_run_defers(c, mark);
                    ST_ir_term_br(c->cur, join, s->line, s->col);
                }
                c->defers.count = mark;
            } else if (!ST_ir_block_is_terminated(c->cur)) {
                ST_ir_term_br(c->cur, join, s->line, s->col);
            }

            ST_ir_block_seal(join);
            c->cur = join;
            if (join->preds.count == 0)
                ST_ir_term_unreachable(join, s->line, s->col);
        } break;

        case ST_ST_ASM:
            ST_lower_asm_stmt(c, s);
            break;

        default:
            ST_diag_error(&c->diag, s->line, s->col,
                          "internal: control flow (switch) isn't lowered yet");
            break;
    }
}

static ST_ir_fn_t *ST_lower_register_fn(ST_lower_ctx_t *c, ST_string_t name, ST_fn_sig_t *sig,
                                        b8 is_pub, b8 is_extern) {
    ST_ty_t *fn_ty = ST_ty_fn_new(&c->sema->tys);
    ST_forrange(0, sig->params.count) {
        ST_param_t *p = &sig->params.items[i];
        ST_ty_t *pt = p->te ? ST_lower_tyexpr(c, p->te) : NULL;
        if (!pt && p->def)
            pt = p->def->ty;
        ST_da_append_arena(c->arena, &fn_ty->params, pt);
    }

    // @note: a single 'struct'/'tag_union' return value is expanded into its
    // real eightbyte-classified register slots here (same classification
    // ST_lower_push_struct_arg already uses for struct arguments), rather
    // than being kept as one raw aggregate type. This lets every existing
    // 'rets.count'-driven mechanism downstream -- the 1/2/>2 register
    // placement in the RET terminator, the rc==2 rax:rdx reassembly and
    // rc>2 hidden-pointer convention in the CALL codegen, and ST_ir_extract
    // consumption at call sites -- handle aggregate returns for free,
    // without ever needing to distinguish "one struct" from "N declared
    // return values". Not extern: the C ABI's own struct-return rules are
    // out of scope here and extern_fn keeps the plain 1:1 mapping.
    if (!is_extern && sig->rets.count == 1) {
        ST_ty_t *rt = ST_lower_tyexpr(c, sig->rets.items[0]);
        if (rt && (rt->kind == ST_TY_STRUCT || rt->kind == ST_TY_TAG_UNION) && rt->size > 0) {
            u32 n_eb = ST_lower_eight_bytes_count(rt);
            ST_forrange(0, n_eb)
                ST_da_append_arena(c->arena, &fn_ty->rets, ST_lower_eight_byte_ty(c, rt, i));
        } else
            ST_da_append_arena(c->arena, &fn_ty->rets, rt);
    } else {
        ST_forrange(0, sig->rets.count)
            ST_da_append_arena(c->arena, &fn_ty->rets, ST_lower_tyexpr(c, sig->rets.items[i]));
    }

    fn_ty->is_variadic = sig->is_variadic;

    ST_ir_fn_t *fn = ST_ir_fn_new(c->module, name, fn_ty);
    fn->is_pub = is_pub;
    fn->is_extern = is_extern;
    fn->is_variadic = sig->is_variadic;
    return fn;
}

static u32 ST_lower_eight_bytes_count(ST_ty_t *st) {
    return (st->size + 7) / 8;
}

static b8 ST_lower_eight_byte_is_sse(ST_ty_t *st, u32 eb) {
    u32 lo = eb * 8, hi = lo + 8;
    if (st->kind == ST_TY_ARRAY) {
        if (!ST_ty_is_float(st->inner))
            return 0;
        return lo < st->size; // this eightbyte overlaps real (float) array data
    }
    b8 saw_field = 0, all_float = 1;
    u32 float_count = 0;

    ST_forrange(0, st->fields.count) {
        ST_ty_field_t *f = &st->fields.items[i];
        u32 fend = f->offset + f->ty->size;
        if (fend <= lo || f->offset >= hi)
            continue;
        saw_field = 1;
        if (ST_ty_is_float(f->ty)) {
            float_count++;
            if (f->ty->size != 8)
                all_float = 0;
        } else {
            all_float = 0;
        }
    }
    return saw_field && all_float && float_count > 0;
}

static ST_ty_t *ST_lower_eight_byte_ty(ST_lower_ctx_t *c, ST_ty_t *st, u32 eb) {
    return ST_lower_eight_byte_is_sse(st, eb) ? c->sema->tys.prim[ST_tf64]
                                              : c->sema->tys.prim[ST_ti64];
}

static void ST_lower_string_zero(ST_lower_ctx_t *c, ST_ir_inst_t *slot, u32 line, u32 col) {
    ST_ty_t *dty = ST_ty_ptr(&c->sema->tys, c->sema->tys.prim[ST_tchar]);
    ST_ty_t *lty = c->sema->tys.prim[ST_ti64];

    ST_ir_inst_t *p = ST_lower_field_ptr(c, slot, 0, dty, line, col);
    ST_ir_store(c->cur, dty, p, ST_ir_const_int(c->cur, dty, 0), line, col);

    p = ST_lower_field_ptr(c, slot, 8, lty, line, col);
    ST_ir_store(c->cur, lty, p, ST_ir_const_int(c->cur, lty, 0), line, col);
}

static void ST_lower_fn_body(ST_lower_ctx_t *c, ST_decl_t *d) {
    if (d->fn.is_prototype)
        return;

    ST_ir_fn_t *fn = ST_ir_module_find_fn(c->module, d->name);
    ST_ty_t *fn_ty = fn->ty;

    if (ST_string_eq_cstr(d->name, "main") &&
        (fn_ty->rets.count != 1 || !ST_ty_is_int(fn_ty->rets.items[0]))) {
        ST_diag_error(&c->diag, d->line, d->col, "'main' must return a single integer type");
    }

    c->fn = fn;
    c->defers = (ST_lower_defers_t){0};
    ST_ht_init(c->arena, &c->scope, 16);
    ST_ht_init(c->arena, &c->addr_taken, 16);
    ST_ht_init(c->arena, &c->labels, 8);
    c->label_blocks = (ST_ir_blocks_t){0};

    ST_forrange(0, d->fn.body.count) ST_lower_scan_stmt(c, d->fn.body.items[i]);

    ST_ir_block_t *entry = ST_ir_block_new(fn, "entry");
    fn->entry = entry;
    c->cur = entry;
    u32 param_index = 0;

    // Pass 1: determine each param's slot count/types and read every one of
    // them via ST_ir_param immediately, contiguously -- no alloca, no zero-
    // init, no store, nothing else emitted until every incoming argument
    // register has been read into an SSA value. The backend materializes
    // ST_IR_PARAM lazily (reads its ABI register only when the codegen loop
    // reaches that instruction) and freely reuses registers like rcx as
    // scratch in between -- interleaving bookkeeping with these reads left
    // a window where a later param's own address-computation scratch could
    // clobber an earlier param's still-unread argument register.
    u32 total_slots = 0;
    ST_forrange(0, d->fn.sig.params.count) {
        ST_ty_t *pty = fn_ty->params.items[i];
        if (pty && pty->kind == ST_TY_STRING)
            total_slots += 2;
        else if (pty && (pty->kind == ST_TY_STRUCT || pty->kind == ST_TY_TAG_UNION ||
                        pty->kind == ST_TY_ARRAY)) {
            if (pty->size > 16 || pty->size <= 8)
                total_slots += 1;
            else
                total_slots += ST_lower_eight_bytes_count(pty);
        } else
            total_slots += 1;
    }
    ST_ir_inst_t **raw =
        total_slots ? ST_arena_push_zeroed(c->arena, (u64)total_slots * sizeof(*raw)) : NULL;
    u32 raw_n = 0;

    ST_forrange(0, d->fn.sig.params.count) {
        ST_param_t *p = &d->fn.sig.params.items[i];
        ST_ty_t *pty = fn_ty->params.items[i];
        if (pty && pty->kind == ST_TY_STRING) {
            raw[raw_n++] = ST_ir_param(entry, c->sema->tys.prim[ST_tchar], param_index, p->name);
            raw[raw_n++] = ST_ir_param(entry, c->sema->tys.prim[ST_ti64], param_index + 1, p->name);
            param_index += 2;
        } else if (pty && (pty->kind == ST_TY_STRUCT || pty->kind == ST_TY_TAG_UNION ||
                          pty->kind == ST_TY_ARRAY)) {
            if (pty->size > 16) {
                raw[raw_n++] = ST_ir_param(entry, ST_ty_ptr(&c->sema->tys, pty), param_index, p->name);
                param_index++;
            } else if (pty->size <= 8) {
                ST_ty_t *ebty = ST_lower_eight_byte_ty(c, pty, 0);
                raw[raw_n++] = ST_ir_param(entry, ebty, param_index, p->name);
                param_index++;
            } else {
                u32 n_eb = ST_lower_eight_bytes_count(pty);
                for (u32 k = 0; k < n_eb; k++) {
                    ST_ty_t *ebty = ST_lower_eight_byte_ty(c, pty, k);
                    raw[raw_n++] = ST_ir_param(entry, ebty, param_index, p->name);
                    param_index++;
                }
            }
        } else if (pty && !ST_lower_ty_is_scalar(pty)) {
            ST_diag_error(&c->diag, d->line, d->col,
                          "internal: passing '" ST_sv_fmt "' by value not lowered yet.",
                          ST_sv_args(p->name));
            raw[raw_n++] = NULL;
        } else {
            raw[raw_n++] = ST_ir_param(entry, pty, param_index, p->name);
            param_index++;
        }
    }

    // Pass 2: now do all the alloca/zero-init/field-store/bind bookkeeping,
    // consuming the values already captured in 'raw' -- freely using
    // rax/rcx/etc as scratch here is safe, since no argument register still
    // holds an unread value by this point.
    raw_n = 0;
    ST_forrange(0, d->fn.sig.params.count) {
        ST_param_t *p = &d->fn.sig.params.items[i];
        ST_ty_t *pty = fn_ty->params.items[i];
        if (pty && pty->kind == ST_TY_STRING) {
            ST_ir_inst_t *ptr_param = raw[raw_n++];
            ST_ir_inst_t *len_param = raw[raw_n++];

            ST_ir_inst_t *slot = ST_ir_alloca(fn, &c->sema->tys, pty, d->line, d->col);
            ST_lower_string_zero(c, slot, d->line, d->col);

            ST_ty_t *ptr_ty = ST_ty_ptr(&c->sema->tys, c->sema->tys.prim[ST_tchar]);
            ST_ir_inst_t *ptr_field = ST_lower_field_ptr(c, slot, 0, ptr_ty, d->line, d->col);
            ST_ir_store(entry, ptr_ty, ptr_field, ptr_param, d->line, d->col);

            ST_ir_inst_t *len_field =
                ST_lower_field_ptr(c, slot, 8, c->sema->tys.prim[ST_ti64], d->line, d->col);
            ST_ir_store(entry, c->sema->tys.prim[ST_ti64], len_field, len_param, d->line, d->col);

            ST_lower_bind_addr(c, p->name, slot, pty);
            continue;
        }
        if (pty && pty->kind == ST_TY_ARRAY) {
            ST_ir_inst_t *slot = ST_ir_alloca(fn, &c->sema->tys, pty, d->line, d->col);
            if (pty->size > 16) {
                ST_ir_inst_t *ptr = raw[raw_n++];
                ST_lower_raw_copy(c, slot, 0, ptr, 0, pty->size, d->line, d->col);
            } else if (pty->size <= 8) {
                ST_ty_t *ebty = ST_lower_eight_byte_ty(c, pty, 0);
                ST_ir_inst_t *pv = raw[raw_n++];
                ST_ir_inst_t *fp = ST_lower_field_ptr(c, slot, 0, ebty, d->line, d->col);
                ST_ir_store(entry, ebty, fp, pv, d->line, d->col);
            } else {
                u32 n_eb = ST_lower_eight_bytes_count(pty);
                for (u32 k = 0; k < n_eb; k++) {
                    ST_ty_t *ebty = ST_lower_eight_byte_ty(c, pty, k);
                    ST_ir_inst_t *pv = raw[raw_n++];
                    ST_ir_inst_t *fp =
                        ST_lower_field_ptr(c, slot, (i32)(k * 8), ebty, d->line, d->col);
                    ST_ir_store(entry, ebty, fp, pv, d->line, d->col);
                }
            }
            ST_lower_bind_addr(c, p->name, slot, pty);
            continue;
        }
        if (pty && pty->kind == ST_TY_STRUCT) {
            ST_ir_inst_t *slot = ST_ir_alloca(fn, &c->sema->tys, pty, d->line, d->col);
            if (pty->size > 16) {
                ST_ir_inst_t *ptr = raw[raw_n++];
                ST_lower_struct_copy_direct(c, slot, ptr, pty, d->line, d->col);
            } else if (pty->size <= 8) {
                ST_ty_t *ebty = ST_lower_eight_byte_ty(c, pty, 0);
                ST_ir_inst_t *pv = raw[raw_n++];
                ST_ir_inst_t *fp = ST_lower_field_ptr(c, slot, 0, ebty, d->line, d->col);
                ST_ir_store(entry, ebty, fp, pv, d->line, d->col);
            } else {
                u32 n_eb = ST_lower_eight_bytes_count(pty);
                for (u32 k = 0; k < n_eb; k++) {
                    ST_ty_t *ebty = ST_lower_eight_byte_ty(c, pty, k);
                    ST_ir_inst_t *pv = raw[raw_n++];
                    ST_ir_inst_t *fp =
                        ST_lower_field_ptr(c, slot, (i32)(k * 8), ebty, d->line, d->col);
                    ST_ir_store(entry, ebty, fp, pv, d->line, d->col);
                }
            }
            ST_lower_bind_addr(c, p->name, slot, pty);
            continue;
        }
        if (pty && pty->kind == ST_TY_TAG_UNION) {
            ST_ir_inst_t *slot = ST_ir_alloca(fn, &c->sema->tys, pty, d->line, d->col);
            if (pty->size > 16) {
                ST_ir_inst_t *ptr = raw[raw_n++];
                ST_lower_raw_copy(c, slot, 0, ptr, 0, pty->size, d->line, d->col);
            } else if (pty->size <= 8) {
                ST_ty_t *ebty = ST_lower_eight_byte_ty(c, pty, 0);
                ST_ir_inst_t *pv = raw[raw_n++];
                ST_ir_inst_t *fp = ST_lower_field_ptr(c, slot, 0, ebty, d->line, d->col);
                ST_ir_store(entry, ebty, fp, pv, d->line, d->col);
            } else {
                u32 n_eb = ST_lower_eight_bytes_count(pty);
                for (u32 k = 0; k < n_eb; k++) {
                    ST_ty_t *ebty = ST_lower_eight_byte_ty(c, pty, k);
                    ST_ir_inst_t *pv = raw[raw_n++];
                    ST_ir_inst_t *fp =
                        ST_lower_field_ptr(c, slot, (i32)(k * 8), ebty, d->line, d->col);
                    ST_ir_store(entry, ebty, fp, pv, d->line, d->col);
                }
            }
            ST_lower_bind_addr(c, p->name, slot, pty);
            continue;
        }
        if (pty && !ST_lower_ty_is_scalar(pty)) {
            raw_n++; // matches the NULL placeholder pushed in pass 1
            continue;
        }
        ST_ir_inst_t *pv = raw[raw_n++];

        if (ST_lower_is_addr_taken(c, p->name)) {
            ST_ir_inst_t *slot = ST_ir_alloca(fn, &c->sema->tys, pty, d->line, d->col);
            ST_ir_store(entry, pty, slot, pv, d->line, d->col);
            ST_lower_bind_addr(c, p->name, slot, pty);
        } else {
            ST_ir_write_var(entry, (void *)p, pv);
            ST_lower_bind_ssa(c, p->name, (void *)p, pty);
        }
    }

    ST_ir_block_seal(entry);

    ST_forrange(0, d->fn.body.count) ST_lower_stmt(c, d->fn.body.items[i]);

    b8 returns_void = fn_ty->rets.count == 0 || (fn_ty->rets.count == 1 && fn_ty->rets.items[0] &&
                                                 fn_ty->rets.items[0]->kind == ST_TY_VOID);

    if (!ST_ir_block_is_terminated(c->cur)) {
        if (returns_void) {
            ST_lower_run_defers(c, 0);
            ST_ir_term_ret(c->cur, NULL, 0, d->line, d->col);
        } else
            ST_diag_error(&c->diag, d->line, d->col,
                          "function '" ST_sv_fmt "' does not return a value on all paths",
                          ST_sv_args(d->name));
    }
}

b8 ST_lower_program(ST_arena_t *arena, ST_program_t *prog, ST_sema_t *sema, ST_string_t src,
                    ST_string_t file, ST_ir_module_t *out) {
    ST_ir_module_init(arena, file, out);

    ST_lower_ctx_t c = {0};
    c.arena = arena;
    c.module = out;
    c.sema = sema;
    c.prog = prog;
    c.diag.src = src;
    c.diag.file = file;
    c.diag.max_errors = ST_SEMA_MAX_ERRORS;

    ST_forrange(0, prog->decls.count) {
        ST_decl_t *d = prog->decls.items[i];
        if (d->kind == ST_DE_FN && d->fn.sig.generics.count)
            continue;
        if (d->kind == ST_DE_FN)
            ST_lower_register_fn(&c, d->name, &d->fn.sig, d->is_pub, d->fn.is_prototype);
        else if (d->kind == ST_DE_EXTERN_FN)
            ST_lower_register_fn(&c, d->name, &d->extern_fn.sig, d->is_pub, 1);
        else if (d->kind == ST_DE_GLOBAL) {
            ST_sym_t *sym = ST_ht_get(&sema->globals, (ST_ht_generic_t){.tag = d->name.data,
                                                                        .size = d->name.len})
                                .tag;
            ST_ty_t *ty = sym ? sym->t : NULL;
            if (!ty)
                continue;
            b8 has_init = d->global_.init != NULL;
            b8 is_float = has_init && ST_ty_is_float(ty);
            i64 iv = 0;
            f64 fv = 0.0;
            if (has_init) {
                if (is_float)
                    fv = d->global_.init->kind == ST_EX_FLOAT ? d->global_.init->fval : 0.0;
                else
                    ST_const_eval(sema, d->global_.init, &iv);
            }
            ST_ir_module_add_global(out, d->name, ty, d->is_pub, has_init, is_float, iv, fv);
        }
    }

    ST_forrange(0, prog->decls.count) {
        ST_decl_t *d = prog->decls.items[i];
        if (d->kind == ST_DE_FN && d->fn.sig.generics.count)
            continue;
        if (d->kind == ST_DE_FN)
            ST_lower_fn_body(&c, d);
    }

    return c.diag.n_errors == 0;
}
