#include "st_lower.h"

typedef struct
{
    ST_arena_t *arena;
    ST_ir_module_t *module;
    ST_sema_t *sema;
    ST_program_t *prog;
    ST_diag_t diag;

    ST_ir_fn_t *fn;
    ST_ir_block_t *cur;
    ST_ht_t scope;
    ST_ht_t addr_taken;
} ST_lower_ctx_t;

typedef enum
{
    ST_BIND_SSA,
    ST_BIND_ADDR,
} ST_bind_kind_t;

typedef struct
{
    ST_bind_kind_t kind;
    void *key;
    ST_ir_inst_t *slot;
    ST_ty_t *ty;
} ST_lower_bind_t;

static ST_ir_inst_t *ST_lower_expr(ST_lower_ctx_t *c, ST_expr_t *e);
static void ST_lower_stmt(ST_lower_ctx_t *c, ST_stmt_t *s);
static ST_ty_t *ST_lower_tyexpr(ST_lower_ctx_t *c, ST_tyexpr_t *te);

static void ST_lower_scope_bind(ST_lower_ctx_t *c, ST_string_t name, ST_lower_bind_t *bind)
{
    ST_ht_generic_t *hk = ST_arena_push(c->arena, sizeof(*hk));
    hk->tag = name.data;
    hk->size = name.len;
    ST_ht_set(&c->scope, hk, (ST_ht_generic_t){ .tag = bind, .size = 0 });
}

static ST_lower_bind_t *ST_lower_scope_find(ST_lower_ctx_t *c, ST_string_t name)
{
    ST_ht_generic_t val = ST_ht_get(&c->scope, (ST_ht_generic_t)
                                    {.tag = name.data, .size = name.len});
    return (ST_lower_bind_t *)val.tag;
}

static ST_lower_bind_t *ST_lower_bind_ssa(ST_lower_ctx_t *c, ST_string_t name, void *key, ST_ty_t *ty)
{
    ST_lower_bind_t *b = ST_arena_push_zeroed(c->arena, sizeof(*b));
    b->kind = ST_BIND_SSA;
    b->key = key;
    b->ty = ty;
    ST_lower_scope_bind(c, name, b);
    return b;
}

static ST_lower_bind_t *ST_lower_bind_addr(ST_lower_ctx_t *c, ST_string_t name, ST_ir_inst_t *slot, ST_ty_t *ty)
{
    ST_lower_bind_t *b = ST_arena_push_zeroed(c->arena, sizeof(*b));
    b->kind = ST_BIND_ADDR;
    b->slot = slot;
    b->ty = ty;
    ST_lower_scope_bind(c, name, b);
    return b;
}

static void ST_lower_mark_addr_taken(ST_lower_ctx_t *c, ST_string_t name)
{
    ST_ht_generic_t *hk = ST_arena_push(c->arena, sizeof(*hk));
    hk->tag = name.data;
    hk->size = name.len;
    ST_ht_set(&c->addr_taken, hk, (ST_ht_generic_t){ .tag = (void *)1, .size = 0 });
}

static b8 ST_lower_is_addr_taken(ST_lower_ctx_t *c, ST_string_t name)
{
    ST_ht_generic_t val = ST_ht_get(&c->addr_taken, (ST_ht_generic_t)
                                    {.tag = name.data, .size = name.len});
    return val.tag != NULL;
}

static void ST_lower_scan_expr(ST_lower_ctx_t *c, ST_expr_t *e)
{
    if (!e) return;
    switch (e->kind)
    {
    case ST_EX_UNARY:
        if (ST_string_eq_cstr(e->unary.op, "&") && e->unary.operand
            && e->unary.operand->kind == ST_EX_IDENT)
            ST_lower_mark_addr_taken(c, e->unary.operand->name);
        ST_lower_scan_expr(c, e->unary.operand);
        break;
    case ST_EX_BINARY:
        ST_lower_scan_expr(c, e->bin.l);
        ST_lower_scan_expr(c, e->bin.r);
        break;
    case ST_EX_CALL:
        ST_lower_scan_expr(c, e->call.callee);
        ST_forrange(0, e->call.args.count)
            ST_lower_scan_expr(c, e->call.args.items[i].value);
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

static void ST_lower_scan_stmt(ST_lower_ctx_t *c, ST_stmt_t *s)
{
    if (!s) return;
    switch (s->kind)
    {
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
        ST_forrange(0, s->multi.values.count)
            ST_lower_scan_expr(c, s->multi.values.items[i]);
        break;
    case ST_ST_IF:
        ST_lower_scan_expr(c, s->if_.cond);
        ST_forrange(0, s->if_.then_body.count)
            ST_lower_scan_stmt(c, s->if_.then_body.items[i]);
        ST_lower_scan_stmt(c, s->if_.else_stmt);
        break;
    case ST_ST_SWITCH:
        ST_lower_scan_expr(c, s->switch_.cond);
        ST_forrange(0, s->switch_.cases.count)
        {
            ST_case_t *cs = &s->switch_.cases.items[i];
            for (u32 k = 0; k < cs->values.count; k++)
                ST_lower_scan_expr(c, cs->values.items[k]);
            for (u32 k = 0; k < cs->body.count; k++)
                ST_lower_scan_stmt(c, cs->body.items[k]);
        }
        break;
    case ST_ST_WHILE:
        ST_lower_scan_expr(c, s->while_.cond);
        ST_forrange(0, s->while_.body.count)
            ST_lower_scan_stmt(c, s->while_.body.items[i]);
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
        ST_forrange(0, s->ret.values.count)
            ST_lower_scan_expr(c, s->ret.values.items[i]);
        break;
    case ST_ST_BLOCK:
        ST_forrange(0, s->block.count)
            ST_lower_scan_stmt(c, s->block.items[i]);
        break;
    case ST_ST_DEFER:
        ST_lower_scan_stmt(c, s->defer_stmt);
        break;
    default:
        break;
    }
}

static void *ST_lower_prim_by_name(ST_ty_ctx_t *c, ST_string_t name)
{
    ST_forrange(0, ST_TYPE_COUNT)
    {
        if (ST_string_eq_cstr(name, ST_type_names[i])) return ST_ty_prim(c, (ST_type_t)i);
    }
    return NULL;
}

static void *ST_lower_find_named_decl(ST_program_t *prog_name, ST_string_t name)
{
    ST_forrange(0, prog_name->decls.count)
    {
        ST_decl_t *d = prog_name->decls.items[i];
        if ((d->kind == ST_DE_STRUCT || d->kind == ST_DE_ENUM ||
             d->kind == ST_DE_TAG_UNION) &&
            ST_string_eq(d->name, name)) return d;
    }
    return NULL;
}

static ST_ty_t *ST_lower_tyexpr(ST_lower_ctx_t *c, ST_tyexpr_t *te)
{
    if (!te) return NULL;

    switch (te->kind)
    {
    case ST_TE_NAME: {
        ST_ty_t *prim = ST_lower_prim_by_name(&c->sema->tys, te->name);
        if (prim) return prim;
        ST_decl_t *d = ST_lower_find_named_decl(c->prog, te->name);
        if (d) return ST_ty_for_decls(&c->sema->tys, d);
        ST_diag_error(&c->diag, te->line, te->col,
                      "internal: unknown type name '" ST_sv_fmt "'", ST_sv_args(te->name));
        return ST_ty_prim(&c->sema->tys, ST_ti32);
    }
    case ST_TE_PTR:
        return ST_ty_ptr(&c->sema->tys, ST_lower_tyexpr(c, te->inner));
    case ST_TE_ARRAY: {
        ST_ty_t *inner = ST_lower_tyexpr(c, te->inner);
        if (!te->count_expr) return ST_ty_dyn_array(&c->sema->tys, inner);
        u64 count = te->count_expr->kind == ST_EX_INT ? (u64)te->count_expr->ival : 0;
        return ST_ty_array(&c->sema->tys, inner, count);
    }
    }
    return NULL;
}

static b8 ST_lower_ty_is_scalar(ST_ty_t *t)
{
    if (!t) return 0;
    switch (t->kind)
    {
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

static ST_ir_op_t ST_lower_binop(ST_diag_t *diag, ST_expr_t *e, b8 is_f, b8 uns)
{
    ST_string_t op = e->bin.op;
    if (ST_string_eq_cstr(op, "+")) return is_f ? ST_IR_FADD : ST_IR_ADD;
    if (ST_string_eq_cstr(op, "-")) return is_f ? ST_IR_FSUB : ST_IR_SUB;
    if (ST_string_eq_cstr(op, "*")) return is_f ? ST_IR_FMUL : ST_IR_MUL;
    if (ST_string_eq_cstr(op, "/")) return is_f ? ST_IR_FDIV : (uns ? ST_IR_UDIV : ST_IR_SDIV);
    if (ST_string_eq_cstr(op, "%")) return uns ? ST_IR_UREM : ST_IR_SREM;
    if (ST_string_eq_cstr(op, "&")) return ST_IR_AND;
    if (ST_string_eq_cstr(op, "|")) return ST_IR_OR;
    if (ST_string_eq_cstr(op, "^")) return ST_IR_XOR;
    if (ST_string_eq_cstr(op, "<<")) return ST_IR_SHL;
    if (ST_string_eq_cstr(op, ">>")) return uns ? ST_IR_LSHR : ST_IR_ASHR;
    if (ST_string_eq_cstr(op, "==")) return is_f ? ST_IR_FCMP_EQ : ST_IR_ICMP_EQ;
    if (ST_string_eq_cstr(op, "!=")) return is_f ? ST_IR_FCMP_NE : ST_IR_ICMP_NE;
    if (ST_string_eq_cstr(op, "<"))  return is_f ? ST_IR_FCMP_LT : (uns ? ST_IR_ICMP_ULT : ST_IR_ICMP_SLT);
    if (ST_string_eq_cstr(op, "<=")) return is_f ? ST_IR_FCMP_LE : (uns ? ST_IR_ICMP_ULE : ST_IR_ICMP_SLE);
    if (ST_string_eq_cstr(op, ">"))  return is_f ? ST_IR_FCMP_GT : (uns ? ST_IR_ICMP_UGT : ST_IR_ICMP_SGT);
    if (ST_string_eq_cstr(op, ">=")) return is_f ? ST_IR_FCMP_GE : (uns ? ST_IR_ICMP_UGE : ST_IR_ICMP_SGE);
    if (ST_string_eq_cstr(op, "and")) return ST_IR_AND;
    if (ST_string_eq_cstr(op, "or"))  return ST_IR_OR;
    ST_diag_error(diag, e->line, e->col, "internal: unsupported binary operator '" ST_sv_fmt "'", ST_sv_args(op));
    return ST_IR_ADD;
}

static ST_ir_op_t ST_lower_compound_op(ST_diag_t *diag, ST_string_t op, u32 line, u32 col, b8 is_f)
{
    if (ST_string_eq_cstr(op, "+=")) return is_f ? ST_IR_FADD : ST_IR_ADD;
    if (ST_string_eq_cstr(op, "-=")) return is_f ? ST_IR_FSUB : ST_IR_SUB;
    if (ST_string_eq_cstr(op, "*=")) return is_f ? ST_IR_FMUL : ST_IR_MUL;
    if (ST_string_eq_cstr(op, "/=")) return is_f ? ST_IR_FDIV : ST_IR_SDIV;
    if (ST_string_eq_cstr(op, "%=")) return ST_IR_SREM;
    if (ST_string_eq_cstr(op, "&=")) return ST_IR_AND;
    if (ST_string_eq_cstr(op, "|=")) return ST_IR_OR;
    if (ST_string_eq_cstr(op, "^=")) return ST_IR_XOR;
    ST_diag_error(diag, line, col, "internal: unsupported compound-assign operator '" ST_sv_fmt "'", ST_sv_args(op));
    return ST_IR_ADD;
}

static ST_ir_inst_t *ST_lower_addr_of(ST_lower_ctx_t *c, ST_expr_t *e, ST_ty_t *ptr_ty)
{
    ST_expr_t *v = e->unary.operand;

    if (v->kind == ST_EX_UNARY && ST_string_eq_cstr(v->unary.op, "*"))
        return ST_lower_expr(c, v->unary.operand);

    if (v->kind == ST_EX_IDENT)
    {
        ST_lower_bind_t *bind = ST_lower_scope_find(c, v->name);
        if (!bind)
        {
            ST_diag_error(&c->diag, e->line, e->col,
                          "internal: cannot take the address of '" ST_sv_fmt "' "
                          "(not a known local)", ST_sv_args(v->name));
            return ST_ir_const_int(c->cur, ptr_ty, 0);
        }
        if (bind->kind != ST_BIND_ADDR)
        {
            ST_diag_error(&c->diag, e->line, e->col,
                          "internal: '" ST_sv_fmt "' was not demoted to a stack slot "
                          "before its address was taken", ST_sv_args(v->name));
            return ST_ir_const_int(c->cur, ptr_ty, 0);
        }
        return bind->slot;
    }

    ST_diag_error(&c->diag, e->line, e->col,
                  "internal: address-of this expression form isn't lowered yet "
                  "(field/index addressing lands with struct lowering)");
    return ST_ir_const_int(c->cur, ptr_ty, 0);
}

static ST_ir_inst_t *ST_lower_call(ST_lower_ctx_t *c, ST_expr_t *e)
{
    ST_ir_inst_t **args = e->call.args.count
        ? ST_arena_push(c->arena, sizeof(*args) * e->call.args.count) : NULL;
    ST_forrange(0, e->call.args.count)
        args[i] = ST_lower_expr(c, e->call.args.items[i].value);

    if (e->call.callee->kind == ST_EX_IDENT && !ST_lower_scope_find(c, e->call.callee->name))
    {
        ST_string_t name = e->call.callee->name;
        ST_ir_fn_t *target = ST_ir_module_find_fn(c->module, name);
        if (!target)
            ST_diag_error(&c->diag, e->line, e->col,
                          "internal: call to unknown function '" ST_sv_fmt "'", ST_sv_args(name));
        return ST_ir_call(c->cur, e->ty, name, target, args, e->call.args.count, e->line, e->col);
    }

    ST_ir_inst_t *ptr = ST_lower_expr(c, e->call.callee);
    return ST_ir_call_indirect(c->cur, e->ty, ptr, args, e->call.args.count, e->line, e->col);
}

static ST_ir_inst_t *ST_lower_expr(ST_lower_ctx_t *c, ST_expr_t *e)
{
    switch (e->kind)
    {
    case ST_EX_INT:  return ST_ir_const_int(c->cur, e->ty, e->ival);
    case ST_EX_BOOL:
    case ST_EX_CHAR:
    case ST_EX_NULL: return ST_ir_const_int(c->cur, e->ty, e->kind == ST_EX_NULL ? 0 : e->ival);
    case ST_EX_FLOAT: return ST_ir_const_float(c->cur, e->ty, e->fval);

    case ST_EX_IDENT: {
        ST_lower_bind_t *bind = ST_lower_scope_find(c, e->name);
        if (bind)
        {
            if (bind->kind == ST_BIND_SSA)
                return ST_ir_read_var(c->cur, bind->key, e->ty);
            if (!ST_lower_ty_is_scalar(e->ty))
            {
                ST_diag_error(&c->diag, e->line, e->col,
                              "internal: loading aggregate locals isn't lowered yet "
                              "(lands with struct lowering)");
                return ST_ir_const_int(c->cur, e->ty, 0);
            }
            return ST_ir_load(c->cur, e->ty, bind->slot, e->line, e->col);
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

        if (ST_string_eq_cstr(op, "*"))
        {
            ST_ir_inst_t *p = ST_lower_expr(c, e->unary.operand);
            if (!ST_lower_ty_is_scalar(e->ty))
            {
                ST_diag_error(&c->diag, e->line, e->col,
                              "internal: dereferencing to an aggregate isn't lowered yet "
                              "(lands with struct lowering)");
                return ST_ir_const_int(c->cur, e->ty, 0);
            }
            return ST_ir_load(c->cur, e->ty, p, e->line, e->col);
        }

        ST_ir_inst_t *v = ST_lower_expr(c, e->unary.operand);
        if (ST_string_eq_cstr(op, "-"))
            return ST_ir_unop(c->cur, ST_ty_is_float(e->ty) ? ST_IR_FNEG : ST_IR_NEG,
                              e->ty, v, e->line, e->col);
        if (ST_string_eq_cstr(op, "~"))
            return ST_ir_unop(c->cur, ST_IR_NOT, e->ty, v, e->line, e->col);
        if (ST_string_eq_cstr(op, "!") || ST_string_eq_cstr(op, "not"))
        {
            ST_ir_inst_t *zero = ST_ir_const_int(c->cur, e->unary.operand->ty, 0);
            return ST_ir_binop(c->cur, ST_IR_ICMP_EQ, e->ty, v, zero, e->line, e->col);
        }
        ST_diag_error(&c->diag, e->line, e->col,
                      "internal: unsupported unary operator '" ST_sv_fmt "'", ST_sv_args(op));
        return v;
    }

    case ST_EX_BINARY: {
        ST_ir_inst_t *l = ST_lower_expr(c, e->bin.l);
        ST_ir_inst_t *r = ST_lower_expr(c, e->bin.r);
        b8 is_f = ST_ty_is_float(e->bin.l->ty) || ST_ty_is_float(e->bin.r->ty);
        b8 uns  = e->bin.l->ty && !e->bin.l->ty->is_signed;
        ST_ir_op_t op = ST_lower_binop(&c->diag, e, is_f, uns);
        return ST_ir_binop(c->cur, op, e->ty, l, r, e->line, e->col);
    }

    case ST_EX_CALL:
        return ST_lower_call(c, e);

    case ST_EX_CAST:
        return ST_ir_cast(c->cur, e->ty, ST_lower_expr(c, e->cast.operand), e->line, e->col);

    default:
        ST_diag_error(&c->diag, e->line, e->col,
                      "internal: this expression form isn't lowered yet");
        return ST_ir_const_int(c->cur, e->ty, 0);
    }
}

static void ST_lower_store_assign(ST_lower_ctx_t *c, ST_stmt_t *s, ST_ty_t *ty,
                                  ST_ir_inst_t *addr)
{
    ST_ir_inst_t *rhs = ST_lower_expr(c, s->assign.rhs);
    if (!ST_string_eq_cstr(s->assign.op, "="))
    {
        ST_ir_inst_t *cur = ST_ir_load(c->cur, ty, addr, s->line, s->col);
        ST_ir_op_t op = ST_lower_compound_op(&c->diag, s->assign.op, s->line, s->col,
                                              ST_ty_is_float(ty));
        rhs = ST_ir_binop(c->cur, op, ty, cur, rhs, s->line, s->col);
    }
    ST_ir_store(c->cur, ty, addr, rhs, s->line, s->col);
}

static void ST_lower_stmt(ST_lower_ctx_t *c, ST_stmt_t *s)
{
    switch (s->kind)
    {
    case ST_ST_EXPR:
        ST_lower_expr(c, s->expr);
        break;

    case ST_ST_DECL: {
        b8 taken = ST_lower_is_addr_taken(c, s->decl.name);

        ST_ty_t *ty = s->decl.init ? s->decl.init->ty
                                   : ST_lower_tyexpr(c, s->decl.te);
        if (!ty)
        {
            ST_diag_error(&c->diag, s->line, s->col,
                          "internal: declaration has no resolvable type");
            break;
        }

        if (!ST_lower_ty_is_scalar(ty) && taken)
        {
            ST_diag_error(&c->diag, s->line, s->col,
                          "internal: aggregate locals aren't lowered yet "
                          "(lands with struct lowering)");
            break;
        }

        ST_ir_inst_t *init;
        if (s->decl.init)
            init = ST_lower_expr(c, s->decl.init);
        else if (ST_ty_is_float(ty))
            init = ST_ir_const_float(c->cur, ty, 0.0);
        else if (ST_lower_ty_is_scalar(ty))
            init = ST_ir_const_int(c->cur, ty, 0);
        else
        {
            ST_diag_error(&c->diag, s->line, s->col,
                          "internal: uninitialized aggregate declarations aren't lowered yet "
                          "(lands with struct lowering)");
            break;
        }

        if (taken)
        {
            ST_ir_inst_t *slot = ST_ir_alloca(c->fn, &c->sema->tys, ty, s->line, s->col);
            ST_ir_store(c->cur, ty, slot, init, s->line, s->col);
            ST_lower_bind_addr(c, s->decl.name, slot, ty);
        }
        else
        {
            ST_ir_write_var(c->cur, (void *)s, init);
            ST_lower_bind_ssa(c, s->decl.name, (void *)s, ty);
        }
        break;
    }

    case ST_ST_ASSIGN: {
        ST_expr_t *lhs = s->assign.lhs;

        if (lhs->kind == ST_EX_UNARY && ST_string_eq_cstr(lhs->unary.op, "*"))
        {
            if (!ST_lower_ty_is_scalar(lhs->ty))
            {
                ST_diag_error(&c->diag, s->line, s->col,
                              "internal: storing aggregates through pointers isn't lowered yet "
                              "(lands with struct lowering)");
                break;
            }
            ST_ir_inst_t *addr = ST_lower_expr(c, lhs->unary.operand);
            ST_lower_store_assign(c, s, lhs->ty, addr);
            break;
        }

        if (lhs->kind != ST_EX_IDENT)
        {
            ST_diag_error(&c->diag, s->line, s->col,
                          "internal: only assignment to locals and '*ptr' targets is "
                          "lowered right now (field/index targets land with structs)");
            break;
        }

        ST_lower_bind_t *bind = ST_lower_scope_find(c, lhs->name);
        if (!bind)
        {
            ST_diag_error(&c->diag, s->line, s->col,
                          "internal: assignment target '" ST_sv_fmt "' is not a known local",
                          ST_sv_args(lhs->name));
            break;
        }

        if (bind->kind == ST_BIND_ADDR)
        {
            ST_lower_store_assign(c, s, lhs->ty ? lhs->ty : bind->ty, bind->slot);
            break;
        }

        ST_ir_inst_t *rhs = ST_lower_expr(c, s->assign.rhs);
        if (!ST_string_eq_cstr(s->assign.op, "="))
        {
            ST_ir_inst_t *cur = ST_ir_read_var(c->cur, bind->key, lhs->ty);
            ST_ir_op_t op = ST_lower_compound_op(&c->diag, s->assign.op, s->line, s->col,
                                                  ST_ty_is_float(lhs->ty));
            rhs = ST_ir_binop(c->cur, op, lhs->ty, cur, rhs, s->line, s->col);
        }
        ST_ir_write_var(c->cur, bind->key, rhs);
        break;
    }

    case ST_ST_RETURN: {
        ST_ir_inst_t **vals = s->ret.values.count
            ? ST_arena_push(c->arena, sizeof(*vals) * s->ret.values.count) : NULL;
        ST_forrange(0, s->ret.values.count) vals[i] = ST_lower_expr(c, s->ret.values.items[i]);
        ST_ir_term_ret(c->cur, vals, s->ret.values.count, s->line, s->col);
        break;
    }

    case ST_ST_BLOCK:
        ST_forrange(0, s->block.count) ST_lower_stmt(c, s->block.items[i]);
        break;

    default:
        ST_diag_error(&c->diag, s->line, s->col,
                      "internal: control flow (if/while/for/switch/goto/defer) isn't lowered yet");
        break;
    }
}

static ST_ir_fn_t *ST_lower_register_fn(ST_lower_ctx_t *c, ST_string_t name,
                                           ST_fn_sig_t *sig, b8 is_pub, b8 is_extern)
{
    ST_ty_t *fn_ty = ST_ty_fn_new(&c->sema->tys);
    ST_forrange(0, sig->params.count)
        ST_da_append_arena(c->arena, &fn_ty->params, ST_lower_tyexpr(c, sig->params.items[i].te));
    ST_forrange(0, sig->rets.count)
        ST_da_append_arena(c->arena, &fn_ty->rets, ST_lower_tyexpr(c, sig->rets.items[i]));
    fn_ty->is_variadic = sig->is_variadic;

    ST_ir_fn_t *fn = ST_ir_fn_new(c->module, name, fn_ty);
    fn->is_pub = is_pub;
    fn->is_extern = is_extern;
    fn->is_variadic = sig->is_variadic;
    return fn;
}

static void ST_lower_fn_body(ST_lower_ctx_t *c, ST_decl_t *d)
{
    if (d->fn.is_prototype) return;

    ST_ir_fn_t *fn = ST_ir_module_find_fn(c->module, d->name);
    ST_ty_t *fn_ty = fn->ty;

    if (ST_string_eq_cstr(d->name, "main") &&
        (fn_ty->rets.count != 1 || !ST_ty_is_int(fn_ty->rets.items[0])))
    {
        ST_diag_error(&c->diag, d->line, d->col, "'main' must return a single integer type");
    }

    c->fn = fn;
    ST_ht_init(c->arena, &c->scope, 16);
    ST_ht_init(c->arena, &c->addr_taken, 16);

    ST_forrange(0, d->fn.body.count)
        ST_lower_scan_stmt(c, d->fn.body.items[i]);

    ST_ir_block_t *entry = ST_ir_block_new(fn, "entry");
    fn->entry = entry;
    c->cur = entry;

    ST_forrange(0, d->fn.sig.params.count)
    {
        ST_param_t *p = &d->fn.sig.params.items[i];
        ST_ty_t *pty = fn_ty->params.items[i];
        ST_ir_inst_t *pv = ST_ir_param(entry, pty, i, p->name);

        if (ST_lower_is_addr_taken(c, p->name))
        {
            ST_ir_inst_t *slot = ST_ir_alloca(fn, &c->sema->tys, pty, d->line, d->col);
            ST_ir_store(entry, pty, slot, pv, d->line, d->col);
            ST_lower_bind_addr(c, p->name, slot, pty);
        }
        else
        {
            ST_ir_write_var(entry, (void *)p, pv);
            ST_lower_bind_ssa(c, p->name, (void *)p, pty);
        }
    }

    ST_ir_block_seal(entry);

    ST_forrange(0, d->fn.body.count)
        ST_lower_stmt(c, d->fn.body.items[i]);

    b8 returns_void = fn_ty->rets.count == 0
        || (fn_ty->rets.count == 1 && fn_ty->rets.items[0]
            && fn_ty->rets.items[0]->kind == ST_TY_VOID);

    if (!ST_ir_block_is_terminated(c->cur))
    {
        if (returns_void)
            ST_ir_term_ret(c->cur, NULL, 0, d->line, d->col);
        else
            ST_diag_error(&c->diag, d->line, d->col,
                          "function '" ST_sv_fmt "' does not return a value on all paths",
                          ST_sv_args(d->name));
    }
}

b8 ST_lower_program(ST_arena_t *arena, ST_program_t *prog, ST_sema_t *sema,
                     ST_string_t src, ST_string_t file, ST_ir_module_t *out)
{
    ST_ir_module_init(arena, file, out);

    ST_lower_ctx_t c = {0};
    c.arena = arena;
    c.module = out;
    c.sema = sema;
    c.prog = prog;
    c.diag.src = src;
    c.diag.file = file;
    c.diag.max_errors = ST_SEMA_MAX_ERRORS;

    ST_forrange(0, prog->decls.count)
    {
        ST_decl_t *d = prog->decls.items[i];
        if (d->kind == ST_DE_FN)
            ST_lower_register_fn(&c, d->name, &d->fn.sig, d->is_pub, d->fn.is_prototype);
        else if (d->kind == ST_DE_EXTERN_FN)
            ST_lower_register_fn(&c, d->name, &d->extern_fn.sig, d->is_pub, 1);
    }

    ST_forrange(0, prog->decls.count)
    {
        ST_decl_t *d = prog->decls.items[i];
        if (d->kind == ST_DE_FN) ST_lower_fn_body(&c, d);
    }

    return c.diag.n_errors == 0;
}
