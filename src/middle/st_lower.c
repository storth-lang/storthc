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
} ST_lower_ctx_t;

static ST_ir_inst_t *ST_lower_expr(ST_lower_ctx_t *c, ST_expr_t *e);
static void ST_lower_stmt(ST_lower_ctx_t *c, ST_stmt_t *s);
static ST_ty_t *ST_lower_tyexpr(ST_lower_ctx_t *c, ST_tyexpr_t *te);


static void ST_lower_scope_bind(ST_lower_ctx_t *c, ST_string_t name, void *key)
{
    ST_ht_generic_t *hk = ST_arena_push(c->arena, sizeof(*hk));
    hk->tag = name.data;
    hk->size = name.len;
    ST_ht_set(&c->scope, hk, (ST_ht_generic_t){ .tag = key, .size = 0 });
}

static void *ST_lower_scope_find(ST_lower_ctx_t *c, ST_string_t name)
{
    ST_ht_generic_t val = ST_ht_get(&c->scope, (ST_ht_generic_t)
                                    {.tag = name.data, .size = name.len});
    return val.tag;
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
        void *key = ST_lower_scope_find(c, e->name);
        if (key) return ST_ir_read_var(c->cur, key, e->ty);
        ST_diag_error(&c->diag, e->line, e->col,
                      "internal: '" ST_sv_fmt "' used as a value is not supported yet "
                      "(only local vars/params and direct calls are lowered so far)",
                      ST_sv_args(e->name));
        return ST_ir_const_int(c->cur, e->ty, 0);
    }
 
    case ST_EX_UNARY: {
        ST_ir_inst_t *v = ST_lower_expr(c, e->unary.operand);
        ST_ir_op_t op;
        if (ST_string_eq_cstr(e->unary.op, "-"))
            op = ST_ty_is_float(e->ty) ? ST_IR_FNEG : ST_IR_NEG;
        else if (ST_string_eq_cstr(e->unary.op, "not") || ST_string_eq_cstr(e->unary.op, "~"))
            op = ST_IR_NOT;
        else
        {
            ST_diag_error(&c->diag, e->line, e->col,
                          "internal: unsupported unary operator '" ST_sv_fmt "'", ST_sv_args(e->unary.op));
            return v;
        }
        return ST_ir_unop(c->cur, op, e->ty, v, e->line, e->col);
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
 
static void ST_lower_stmt(ST_lower_ctx_t *c, ST_stmt_t *s)
{
    switch (s->kind)
    {
    case ST_ST_EXPR:
        ST_lower_expr(c, s->expr);
        break;
 
    case ST_ST_DECL: {
        if (!s->decl.init)
        {
            ST_diag_error(&c->diag, s->line, s->col,
                          "internal: uninitialized declarations aren't lowered yet "
                          "(need a resolved type source once st_semantic.c is wired in)");
            break;
        }
        ST_ir_inst_t *v = ST_lower_expr(c, s->decl.init);
        ST_ir_write_var(c->cur, (void *)s, v);
        ST_lower_scope_bind(c, s->decl.name, (void *)s);
        break;
    }
 
    case ST_ST_ASSIGN: {
        if (s->assign.lhs->kind != ST_EX_IDENT)
        {
            ST_diag_error(&c->diag, s->line, s->col,
                          "internal: only assignment to simple local variables is lowered right now "
                          "(no memory operations yet)");
            break;
        }
        void *key = ST_lower_scope_find(c, s->assign.lhs->name);
        if (!key)
        {
            ST_diag_error(&c->diag, s->line, s->col,
                          "internal: assignment target '" ST_sv_fmt "' is not a known local",
                          ST_sv_args(s->assign.lhs->name));
            break;
        }
        ST_ir_inst_t *rhs = ST_lower_expr(c, s->assign.rhs);
        if (!ST_string_eq_cstr(s->assign.op, "="))
        {
            ST_ir_inst_t *cur = ST_ir_read_var(c->cur, key, s->assign.lhs->ty);
            ST_ir_op_t op = ST_lower_compound_op(&c->diag, s->assign.op, s->line, s->col,
                                                  ST_ty_is_float(s->assign.lhs->ty));
            rhs = ST_ir_binop(c->cur, op, s->assign.lhs->ty, cur, rhs, s->line, s->col);
        }
        ST_ir_write_var(c->cur, key, rhs);
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
 
    ST_ir_block_t *entry = ST_ir_block_new(fn, "entry");
    fn->entry = entry;
    c->cur = entry;
 
    ST_forrange(0, d->fn.sig.params.count)
    {
        ST_param_t *p = &d->fn.sig.params.items[i];
        ST_ir_inst_t *pv = ST_ir_param(entry, fn_ty->params.items[i], i, p->name);
        ST_ir_write_var(entry, (void *)p, pv);
        ST_lower_scope_bind(c, p->name, (void *)p);
    }

    ST_ir_block_seal(entry);
 
    ST_forrange(0, d->fn.body.count)
        ST_lower_stmt(c, d->fn.body.items[i]);
 
    if (!ST_ir_block_is_terminated(c->cur))
    {
        if (fn_ty->rets.count == 0)
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
