#include "st_ir.h"


void ST_ir_module_init(ST_arena_t *arena, ST_string_t name, ST_ir_module_t *out)
{
    out->arena = arena;
    out->name = name;
    out->fns = (ST_ir_fns_t){0};
}

ST_ir_fn_t *ST_ir_fn_new(ST_ir_module_t *m, ST_string_t name, ST_ty_t *fn_ty)
{
    ST_ir_fn_t *fn = ST_arena_push_zeroed(m->arena, sizeof(*fn));
    fn->arena = m->arena;
    fn->name = name;
    fn->ty = fn_ty;
    ST_da_append_arena(m->arena, &m->fns, fn);
    return fn;
}

ST_ir_fn_t *ST_ir_module_find_fn(ST_ir_module_t *m, ST_string_t name)
{
    ST_forrange(0, m->fns.count)
    {
        if (ST_string_eq(m->fns.items[i]->name, name))
            return m->fns.items[i];
    }
    return NULL;
}

ST_ir_block_t *ST_ir_block_new(ST_ir_fn_t *fn, const char *label_hint)
{
    ST_ir_block_t *b = ST_arena_push_zeroed(fn->arena, sizeof(*b));
    b->id = fn->next_block_id++;
    b->name = ST_cstr_to_str((char *)(label_hint ? label_hint : "bb"));
    b->fn = fn;
    ST_ht_init(fn->arena, &b->var_defs, 8);
    ST_da_append_arena(fn->arena, &fn->blocks, b);
    return b;
}
 
void ST_ir_inst_remove(ST_ir_inst_t *inst)
{
    ST_ir_block_t *b = inst->block;
    if (inst->prev) inst->prev->next = inst->next;
    else b->first = inst->next;

    if (inst->next) inst->next->prev = inst->prev;
    else b->last = inst->prev;
    
    inst->removed = 1;
}

static void ST_ir_replace_all_uses(ST_ir_fn_t *fn, ST_ir_inst_t *old, ST_ir_inst_t *new_)
{
    ST_forrange(0, fn->blocks.count)
    {
        ST_ir_block_t *blk = fn->blocks.items[i];
        for (ST_ir_inst_t *inst = blk->first; inst; inst = inst->next)
        {
            switch (inst->kind)
            {
            case ST_IR_ADD: case ST_IR_SUB: case ST_IR_MUL:
            case ST_IR_SDIV: case ST_IR_UDIV: case ST_IR_SREM: case ST_IR_UREM:
            case ST_IR_FADD: case ST_IR_FSUB: case ST_IR_FMUL: case ST_IR_FDIV:
            case ST_IR_AND: case ST_IR_OR: case ST_IR_XOR:
            case ST_IR_SHL: case ST_IR_LSHR: case ST_IR_ASHR:
            case ST_IR_ICMP_EQ: case ST_IR_ICMP_NE:
            case ST_IR_ICMP_SLT: case ST_IR_ICMP_SLE: case ST_IR_ICMP_SGT: case ST_IR_ICMP_SGE:
            case ST_IR_ICMP_ULT: case ST_IR_ICMP_ULE: case ST_IR_ICMP_UGT: case ST_IR_ICMP_UGE:
            case ST_IR_FCMP_EQ: case ST_IR_FCMP_NE:
            case ST_IR_FCMP_LT: case ST_IR_FCMP_LE: case ST_IR_FCMP_GT: case ST_IR_FCMP_GE:
                if (inst->bin.l == old) inst->bin.l = new_;
                if (inst->bin.r == old) inst->bin.r = new_;
                break;
            case ST_IR_NEG: case ST_IR_FNEG: case ST_IR_NOT:
                if (inst->unary.v == old) inst->unary.v = new_;
                break;
            case ST_IR_CAST:
                if (inst->cast.v == old) inst->cast.v = new_;
                break;
            case ST_IR_CALL:
                ST_forrange(0, inst->call.args.count)
                    if (inst->call.args.items[i] == old) inst->call.args.items[i] = new_;
                break;
            case ST_IR_CALL_INDIRECT:
                if (inst->call_ind.callee_ptr == old) inst->call_ind.callee_ptr = new_;
                ST_forrange(0, inst->call_ind.args.count)
                    if (inst->call_ind.args.items[i] == old) inst->call_ind.args.items[i] = new_;
                break;
            case ST_IR_PHI:
                ST_forrange(0, inst->phi.values.count)
                    if (inst->phi.values.items[i] == old) inst->phi.values.items[i] = new_;
                break;
            default:
                break;
            }
        }
        if (blk->term.cond == old) blk->term.cond = new_;
        ST_forrange(0, blk->term.rets.count)
            if (blk->term.rets.items[i] == old) blk->term.rets.items[i] = new_;
    }
}
 
static void ST_ir_try_removal_trivial_phi(ST_ir_inst_t *phi)
{
    ST_ir_inst_t *same = NULL;
    ST_forrange(0, phi->phi.values.count)
    {
        ST_ir_inst_t *op = phi->phi.values.items[i];

        if (op == same || op == phi) continue;
        if (same != NULL) return;
        same = op;
    }
    if (same == NULL) return;
    phi->repl = same;
    ST_ir_replace_all_uses(phi->block->fn, phi, same);
    ST_ir_inst_remove(phi);
}

static void ST_ir_add_phi_operand(ST_ir_block_t *b, void *var, ST_ir_inst_t *phi)
{
    ST_forrange(0, b->preds.count)
    {
        ST_ir_inst_t *v = ST_ir_read_var(b->preds.items[i], var, phi->ty);
        ST_da_append_arena(phi->block->fn->arena, &phi->phi.values, v);
        ST_da_append_arena(phi->block->fn->arena, &phi->phi.preds, b->preds.items[i]);
    }
    ST_ir_try_removal_trivial_phi(phi);
}

void ST_ir_block_seal(ST_ir_block_t *b)
{
    if (b->seald) return;
    b->seald = 1;
    ST_forrange(0, b->incomplete_phis.count)
    {
        ST_ir_pending_phi_t p = b->incomplete_phis.items[i];
        ST_ir_add_phi_operand(b, p.var, p.phi);
    }
}

void ST_ir_add_edge(ST_ir_block_t *from, ST_ir_block_t *to)
{
    ST_da_append_arena(to->fn->arena, &to->preds, from);
}

b8 ST_ir_block_is_terminated(ST_ir_block_t *b)
{
    return b->term.kind != ST_IR_TERM_NONE;
}

static void ST_ir_ht_set_ptr(ST_arena_t *arena, ST_ht_t *ht, void *var, void *val)
{
    void **p = ST_arena_push(arena, sizeof(*p));
    *p = var;
    ST_ht_generic_t *key = ST_arena_push(arena, sizeof(*key));
    key->tag = p;
    key->size = sizeof(*p);
    ST_ht_set(ht, key, (ST_ht_generic_t) { .tag = val, .size = 0});
}

static void *ST_ir_ht_get_ptr(ST_ht_t *ht, ST_ir_inst_t *val)
{
    void *k = val;
    ST_ht_generic_t found = ST_ht_get(ht, (ST_ht_generic_t)
                                      {.tag = &k, .size = sizeof(k)});
    return found.tag;
}

void ST_ir_write_var(ST_ir_block_t *b, void *var, ST_ir_inst_t *val)
{
    ST_ir_ht_set_ptr(b->fn->arena, &b->var_defs, var, val);
}

static void ST_ir_block_append_inst(ST_ir_block_t *b, ST_ir_inst_t *inst)
{
    inst->prev = b->last;
    inst->next = NULL;
    if (b->last) b->last->next = inst;
    else b->first = inst;

    b->last = inst;
}

static ST_ir_inst_t *ST_ir_emit(ST_ir_block_t *b, ST_ir_op_t op_kind,
                                ST_ty_t *ty, u32 line, u32 col)
{
    ST_ir_inst_t *inst = ST_arena_push_zeroed(b->fn->arena, sizeof(*inst));
    inst->kind = op_kind;
    inst->ty = ty;
    inst->line = line;
    inst->id = b->fn->next_value_id++;
    inst->col = col;
    inst->block = b;
    ST_ir_block_append_inst(b, inst);
    return inst;
}

static ST_ir_inst_t *ST_ir_read_var_rec(ST_ir_block_t *b, void *var, ST_ty_t *ty)
{
    ST_ir_inst_t *val;
    if (!b->seald)
    {
        val = ST_ir_emit(b, ST_IR_PHI, ty, 0, 0);
        ST_ir_pending_phi_t pending = { var, val };
        ST_da_append_arena(b->fn->arena, &b->incomplete_phis, pending);
    }
    else if (b->preds.count == 1)
    {
        val = ST_ir_read_var(b->preds.items[0], var, ty);
    }
    else if (b->preds.count == 0)
    {
        val = ST_ir_const_int(b, ty, 0);
    }
    else
    {
        val = ST_ir_emit(b, ST_IR_PHI, ty, 0, 0);
        ST_ir_write_var(b, var, val);
        ST_ir_add_phi_operand(b, var, val);
    }
    
    ST_ir_write_var(b, var, val);
    return val;
}

ST_ir_inst_t *ST_ir_read_var(ST_ir_block_t *b, void *var, ST_ty_t *ty)
{
    ST_ir_inst_t *found = ST_ir_ht_get_ptr(&b->var_defs, var);
    while (found && found->removed && found->repl) found = found->repl;
    if (found) return found;
    return ST_ir_read_var_rec(b, var, ty);
}

ST_ir_inst_t *ST_ir_const_int(ST_ir_block_t *b, ST_ty_t *ty, i64 v)
{
    ST_ir_inst_t *inst = ST_ir_emit(b, ST_IR_CONST_INT, ty, 0, 0);
    inst->const_int = v;
    return inst;
}

ST_ir_inst_t *ST_ir_const_float(ST_ir_block_t *b, ST_ty_t *ty, f64 v)
{
    ST_ir_inst_t *inst = ST_ir_emit(b, ST_IR_CONST_FLOAT, ty, 0, 0);
    inst->const_float = v;
    return inst;
}

ST_ir_inst_t *ST_ir_binop(ST_ir_block_t *b, ST_ir_op_t op, ST_ty_t *ty,
                          ST_ir_inst_t *l, ST_ir_inst_t *r, u32 line, u32 col)
{
    ST_ir_inst_t *inst = ST_ir_emit(b, op, ty, line, col);
    inst->bin.l = l;
    inst->bin.r = r;
    return inst;
}

ST_ir_inst_t *ST_ir_unop(ST_ir_block_t *b, ST_ir_op_t op, ST_ty_t *ty,
                         ST_ir_inst_t *v, u32 line, u32 col)
{
    ST_ir_inst_t *inst = ST_ir_emit(b, op, ty, line, col);
    inst->unary.v = v;
    return inst;
}

ST_ir_inst_t *ST_ir_cast(ST_ir_block_t *b, ST_ty_t *to_ty, ST_ir_inst_t *v, u32 line, u32 col)
{
    ST_ir_inst_t *inst = ST_ir_emit(b, ST_IR_CAST, to_ty, line, col);
    inst->cast.v = v;
    return inst;
}

ST_ir_inst_t *ST_ir_param(ST_ir_block_t *b, ST_ty_t *ty, u32 index, ST_string_t name)
{
    ST_ir_inst_t *inst = ST_ir_emit(b, ST_IR_PARAM, ty, 0, 0);
    inst->params.index = index;
    inst->params.name = name;
    return inst;
}

ST_ir_inst_t *ST_ir_call(ST_ir_block_t *b, ST_ty_t *ret_ty, ST_string_t callee_name,
                          ST_ir_fn_t *callee, ST_ir_inst_t **args, u32 n_args, u32 line, u32 col)
{
    ST_ir_inst_t *inst = ST_ir_emit(b, ST_IR_CALL, ret_ty, line, col);
    inst->call.callee_name = callee_name;
    inst->call.callee = callee;
    ST_forrange(0, n_args)
    {
        ST_da_append_arena(b->fn->arena, &inst->call.args, args[i]);
    }
    return inst;
}

ST_ir_inst_t *ST_ir_call_indirect(ST_ir_block_t *b, ST_ty_t *ret_ty, ST_ir_inst_t *callee_ptr,
                                   ST_ir_inst_t **args, u32 n_args, u32 line, u32 col)
{
    ST_ir_inst_t *inst = ST_ir_emit(b, ST_IR_CALL_INDIRECT, ret_ty, line, col);
    inst->call_ind.callee_ptr = callee_ptr;
    ST_forrange(0, n_args)
    {
        ST_da_append_arena(b->fn->arena, &inst->call_ind.args, args[i]);
    }
    return inst;
}

void ST_ir_term_ret(ST_ir_block_t *b, ST_ir_inst_t **vals, u32 n_vals, u32 line, u32 col)
{
    ST_assert(b->term.kind == ST_IR_TERM_NONE);
    b->term.kind = ST_IR_TERM_RET;
    b->term.line = line;
    b->term.col = col;
    ST_forrange(0, n_vals)
    {
        ST_da_append_arena(b->fn->arena, &b->term.rets, vals[i]);
    }
}

void ST_ir_term_br(ST_ir_block_t *b, ST_ir_block_t *target, u32 line, u32 col)
{
    ST_assert(b->term.kind == ST_IR_TERM_NONE);
    b->term.kind = ST_IR_TERM_BR;
    b->term.t_block = target;
    b->term.line = line;
    b->term.col = col;
    ST_ir_add_edge(b, target);
}

void ST_ir_term_condbr(ST_ir_block_t *b, ST_ir_inst_t *cond, ST_ir_block_t *t, ST_ir_block_t *f, u32 line, u32 col)
{
    ST_assert(b->term.kind == ST_IR_TERM_NONE);
    b->term.kind = ST_IR_TERM_COND_BR;
    b->term.cond = cond;
    b->term.t_block = t;
    b->term.f_block = f;
    b->term.line = line;
    b->term.col = col;
    ST_ir_add_edge(b, t);
    ST_ir_add_edge(b, f);
}

void ST_ir_term_unreachable(ST_ir_block_t *b, u32 line, u32 col)
{
    b->term.kind = ST_IR_TERM_UNREACHABLE;
    b->term.line = line;
    b->term.col = col;
}


static const char *ST_ir_op_name(ST_ir_op_t op)
{
    switch (op)
    {
    case ST_IR_CONST_INT: return "const_int";
    case ST_IR_CONST_FLOAT: return "const_float";
    case ST_IR_ADD: return "add"; case ST_IR_SUB: return "sub"; case ST_IR_MUL: return "mul";
    case ST_IR_SDIV: return "sdiv"; case ST_IR_UDIV: return "udiv";
    case ST_IR_SREM: return "srem"; case ST_IR_UREM: return "urem";
    case ST_IR_FADD: return "fadd"; case ST_IR_FSUB: return "fsub";
    case ST_IR_FMUL: return "fmul"; case ST_IR_FDIV: return "fdiv";
    case ST_IR_NEG: return "neg"; case ST_IR_FNEG: return "fneg";
    case ST_IR_AND: return "and"; case ST_IR_OR: return "or"; case ST_IR_XOR: return "xor";
    case ST_IR_SHL: return "shl"; case ST_IR_LSHR: return "lshr"; case ST_IR_ASHR: return "ashr";
    case ST_IR_NOT: return "not";
    case ST_IR_ICMP_EQ: return "icmp_eq"; case ST_IR_ICMP_NE: return "icmp_ne";
    case ST_IR_ICMP_SLT: return "icmp_slt"; case ST_IR_ICMP_SLE: return "icmp_sle";
    case ST_IR_ICMP_SGT: return "icmp_sgt"; case ST_IR_ICMP_SGE: return "icmp_sge";
    case ST_IR_ICMP_ULT: return "icmp_ult"; case ST_IR_ICMP_ULE: return "icmp_ule";
    case ST_IR_ICMP_UGT: return "icmp_ugt"; case ST_IR_ICMP_UGE: return "icmp_uge";
    case ST_IR_FCMP_EQ: return "fcmp_eq"; case ST_IR_FCMP_NE: return "fcmp_ne";
    case ST_IR_FCMP_LT: return "fcmp_lt"; case ST_IR_FCMP_LE: return "fcmp_le";
    case ST_IR_FCMP_GT: return "fcmp_gt"; case ST_IR_FCMP_GE: return "fcmp_ge";
    case ST_IR_CAST: return "cast";
    case ST_IR_PARAM: return "param";
    case ST_IR_CALL: return "call";
    case ST_IR_CALL_INDIRECT: return "call_indirect";
    case ST_IR_PHI: return "phi";
    case ST_IR_COUNT: break;
    }
    return "<?>";
}
 
static void ST_ir_dump_val(FILE *out, ST_ir_inst_t *v)
{
    if (!v) { fprintf(out, "<null>"); return; }
    fprintf(out, "v%u", v->id);
}
 
void ST_ir_dump_func(FILE *out, ST_ir_fn_t *fn)
{
    fprintf(out, "fn " ST_sv_fmt "(", ST_sv_args(fn->name));
    ST_forrange(0, fn->ty->params.count)
    {
        if (i) fprintf(out, ", ");
        fprintf(out, "%s", ST_ty_cstr(fn->arena, fn->ty->params.items[i]));
    }
    fprintf(out, ")");
    if (fn->ty->rets.count)
    {
        fprintf(out, " -> ");
        ST_forrange(0, fn->ty->rets.count)
        {
            if (i) fprintf(out, ", ");
            fprintf(out, "%s", ST_ty_cstr(fn->arena, fn->ty->rets.items[i]));
        }
    }
    if (fn->is_extern) { fprintf(out, " extern\n"); return; }
    fprintf(out, "\n");
 
    ST_forrange(0, fn->blocks.count)
    {
        ST_ir_block_t *b = fn->blocks.items[i];
        fprintf(out, "  " ST_sv_fmt ".%u:", ST_sv_args(b->name), b->id);
        if (b->preds.count)
        {
            fprintf(out, "    ; preds =");
            for (u32 k = 0; k < b->preds.count; k++)
                fprintf(out, " " ST_sv_fmt ".%u", ST_sv_args(b->preds.items[k]->name), b->preds.items[k]->id);
        }
        fprintf(out, "\n");
 
        for (ST_ir_inst_t *inst = b->first; inst; inst = inst->next)
        {
            fprintf(out, "    ");
            ST_ir_dump_val(out, inst);
            fprintf(out, " = %s", ST_ir_op_name(inst->kind));
            switch (inst->kind)
            {
            case ST_IR_CONST_INT: fprintf(out, " %lld", (long long)inst->const_int); break;
            case ST_IR_CONST_FLOAT: fprintf(out, " %g", inst->const_float); break;
            case ST_IR_PARAM: fprintf(out, " %u \"" ST_sv_fmt "\"", inst->params.index, ST_sv_args(inst->params.name)); break;
            case ST_IR_CALL:
                fprintf(out, " " ST_sv_fmt "(", ST_sv_args(inst->call.callee_name));
                ST_forrange(0, inst->call.args.count) { if (i) fprintf(out, ", "); ST_ir_dump_val(out, inst->call.args.items[i]); }
                fprintf(out, ")");
                break;
            case ST_IR_CALL_INDIRECT:
                fprintf(out, " ");
                ST_ir_dump_val(out, inst->call_ind.callee_ptr);
                fprintf(out, "(");
                ST_forrange(0, inst->call_ind.args.count) { if (i) fprintf(out, ", "); ST_ir_dump_val(out, inst->call_ind.args.items[i]); }
                fprintf(out, ")");
                break;
            case ST_IR_PHI:
                fprintf(out, " ");
                ST_forrange(0, inst->phi.values.count)
                {
                    if (i) fprintf(out, ", ");
                    fprintf(out, "[" ST_sv_fmt ".%u -> ", ST_sv_args(inst->phi.preds.items[i]->name), inst->phi.preds.items[i]->id);
                    ST_ir_dump_val(out, inst->phi.values.items[i]);
                    fprintf(out, "]");
                }
                break;
            case ST_IR_CAST: ST_ir_dump_val(out, inst->cast.v); break;
            case ST_IR_NEG: case ST_IR_FNEG: case ST_IR_NOT:
                ST_ir_dump_val(out, inst->unary.v);
                break;
            default:
                fprintf(out, " ");
                ST_ir_dump_val(out, inst->bin.l);
                fprintf(out, ", ");
                ST_ir_dump_val(out, inst->bin.r);
                break;
            }
            if (inst->ty) fprintf(out, "  : %s", ST_ty_cstr(fn->arena, inst->ty));
            fprintf(out, "\n");
        }
 
        switch (b->term.kind)
        {
        case ST_IR_TERM_RET:
            fprintf(out, "    ret");
            ST_forrange(0, b->term.rets.count) { fprintf(out, " "); ST_ir_dump_val(out, b->term.rets.items[i]); }
            fprintf(out, "\n");
            break;
        case ST_IR_TERM_BR:
            fprintf(out, "    br " ST_sv_fmt ".%u\n", ST_sv_args(b->term.t_block->name), b->term.t_block->id);
            break;
        case ST_IR_TERM_COND_BR:
            fprintf(out, "    condbr ");
            ST_ir_dump_val(out, b->term.cond);
            fprintf(out, ", " ST_sv_fmt ".%u, " ST_sv_fmt ".%u\n",
                    ST_sv_args(b->term.t_block->name), b->term.t_block->id,
                    ST_sv_args(b->term.f_block->name), b->term.f_block->id);
            break;
        case ST_IR_TERM_UNREACHABLE:
            fprintf(out, "    unreachable\n");
            break;
        case ST_IR_TERM_NONE:
            fprintf(out, "    <!! unterminated !!>\n");
            break;
        }
    }
}
 
void ST_ir_dump_module(FILE *out, ST_ir_module_t *m)
{
    fprintf(out, "module " ST_sv_fmt "\n", ST_sv_args(m->name));
    ST_forrange(0, m->fns.count)
    {
        ST_ir_dump_func(out, m->fns.items[i]);
        fprintf(out, "\n");
    }
}
