#include "st_semantic.h"
#include "st_lexer.h"

// Note that this will not a built in in the future.
static const char *ST_builtin_fns[] = { "println" };

static ST_ht_generic_t ST_name_key(ST_string_t name)
{
    return (ST_ht_generic_t){ .tag = name.data, .size = name.len };
}

static void ST_sym_insert(ST_sema_t *se, ST_ht_t *ht, ST_sym_t *sym)
{
    ST_ht_generic_t *key = ST_arena_push(se->arena, sizeof(*key));
    key->tag = sym->name.data;
    key->size = sym->name.len;
    ST_ht_set(ht, key, (ST_ht_generic_t){ .tag = sym, .size = sizeof(*sym) });
}

static ST_sym_t *ST_sym_find_in(ST_ht_t *ht, ST_string_t name)
{
    return (ST_sym_t *)ST_ht_get(ht, ST_name_key(name)).tag;
}

static ST_sym_t *ST_sym_find(ST_sema_t *se, ST_string_t name)
{
    for (ST_scope_t *s = se->scope; s; s = s->parent)
    {
        ST_sym_t *sym = ST_sym_find_in(&s->table, name);
        if (sym) return sym;
    }
    return ST_sym_find_in(&se->globals, name);
}

static ST_sym_t *ST_sym_new(ST_sema_t *se, ST_sym_kind_t kind, ST_string_t name,
                            ST_decl_t *decl, u32 line, u32 col)
{
    ST_sym_t *sym = ST_arena_push_zeroed(se->arena, sizeof(*sym));
    sym->kind = kind;
    sym->name = name;
    sym->decl = decl;
    sym->line = line;
    sym->col = col;
    return sym;
}

static void ST_scope_push(ST_sema_t *se)
{
    ST_scope_t *s = ST_arena_push_zeroed(se->arena, sizeof(*s));
    ST_ht_init(se->arena, &s->table, 8);
    s->parent = se->scope;
    se->scope = s;
}

static void ST_scope_pop(ST_sema_t *se)
{
    se->scope = se->scope->parent;
}

static const char *ST_sym_kind_str(ST_sym_kind_t kind)
{
    switch (kind)
    {
    case ST_SYM_VAR:        return "variable";
    case ST_SYM_FN:         return "function";
    case ST_SYM_TYPE:       return "type";
    case ST_SYM_CONST:      return "constant";
    case ST_SYM_EXTERN_VAR: return "extern variable";
    }
    return "symbol";
}

static void ST_declare_local(ST_sema_t *se, ST_string_t name, u32 line, u32 col)
{
    ST_sym_t *prev = ST_sym_find_in(&se->scope->table, name);
    if (prev)
    {
        ST_diag_error(&se->diag, line, col,
                      "redeclaration of '" ST_sv_fmt "' in the same scope",
                      ST_sv_args(name));
        ST_diag_note(&se->diag, prev->line, prev->col,
                     "previous declaration is here");
        return;
    }
    ST_sym_insert(se, &se->scope->table,
                  ST_sym_new(se, ST_SYM_VAR, name, NULL, line, col));
}

static void ST_check_expr(ST_sema_t *se, ST_expr_t *e);

static void ST_check_tyexpr(ST_sema_t *se, ST_tyexpr_t *te)
{
    while (te)
    {
        switch (te->kind)
        {
        case ST_TE_NAME: {
            if (ST_is_primitive(te->name)) return;
            ST_sym_t *sym = ST_sym_find_in(&se->globals, te->name);
            if (!sym)
            {
                ST_diag_error(&se->diag, te->line, te->col,
                              "unknown type '" ST_sv_fmt "'",
                              ST_sv_args(te->name));
                return;
            }
            if (sym->kind != ST_SYM_TYPE)
            {
                ST_diag_error(&se->diag, te->line, te->col,
                              "'" ST_sv_fmt "' is a %s, not a type",
                              ST_sv_args(te->name), ST_sym_kind_str(sym->kind));
                ST_diag_note(&se->diag, sym->line, sym->col,
                             "'" ST_sv_fmt "' is declared here",
                             ST_sv_args(te->name));
            }
            return;
        }
        case ST_TE_PTR:
            te = te->inner;
            break;
        case ST_TE_ARRAY:
            if (te->count_expr) ST_check_expr(se, te->count_expr);
            te = te->inner;
            break;
        }
    }
}

static void ST_check_call(ST_sema_t *se, ST_expr_t *e)
{
    ST_expr_t *callee = e->call.callee;
    ST_sym_t *sym = NULL;

    if (callee && callee->kind == ST_EX_IDENT)
    {
        sym = ST_sym_find(se, callee->name);
        if (!sym)
        {
            ST_diag_error(&se->diag, callee->line, callee->col,
                          "call to undeclared function '" ST_sv_fmt "'",
                          ST_sv_args(callee->name));
        }
        else if (sym->kind != ST_SYM_FN && sym->kind != ST_SYM_VAR)
        {
            ST_diag_error(&se->diag, callee->line, callee->col,
                          "'" ST_sv_fmt "' is a %s, it cannot be called",
                          ST_sv_args(callee->name), ST_sym_kind_str(sym->kind));
            ST_diag_note(&se->diag, sym->line, sym->col,
                         "'" ST_sv_fmt "' is declared here",
                         ST_sv_args(callee->name));
        }
    }
    else if (callee)
        ST_check_expr(se, callee);

    ST_forrange(0, e->call.args.count)
        ST_check_expr(se, e->call.args.items[i].value);

    if (!sym || sym->kind != ST_SYM_FN || !sym->decl) return;

    ST_fn_sig_t *sig = sym->decl->kind == ST_DE_FN
        ? &sym->decl->fn.sig : &sym->decl->extern_fn.sig;

    b8 has_named = 0;
    ST_forrange(0, e->call.args.count)
        if (e->call.args.items[i].name.len) has_named = 1;
    if (has_named) return;

    u32 min_args = 0;
    ST_forrange(0, sig->params.count)
        if (!sig->params.items[i].def) min_args++;

    u32 n = e->call.args.count;
    if (n < min_args || (!sig->is_variadic && n > sig->params.count))
    {
        if (sig->is_variadic)
            ST_diag_error(&se->diag, e->line, e->col,
                          "'" ST_sv_fmt "' expects at least %u argument%s, got %u",
                          ST_sv_args(sym->name), min_args,
                          min_args == 1 ? "" : "s", n);
        else if (min_args == sig->params.count)
            ST_diag_error(&se->diag, e->line, e->col,
                          "'" ST_sv_fmt "' expects %u argument%s, got %u",
                          ST_sv_args(sym->name), min_args,
                          min_args == 1 ? "" : "s", n);
        else
            ST_diag_error(&se->diag, e->line, e->col,
                          "'" ST_sv_fmt "' expects %u to %u arguments, got %u",
                          ST_sv_args(sym->name), min_args, sig->params.count, n);
        ST_diag_note(&se->diag, sym->line, sym->col,
                     "'" ST_sv_fmt "' is declared here", ST_sv_args(sym->name));
    }
}

static void ST_check_struct_lit(ST_sema_t *se, ST_expr_t *e)
{
    ST_decl_t *sd = NULL;
    if (e->struct_lit.type_name.len)
    {
        ST_sym_t *sym = ST_sym_find_in(&se->globals, e->struct_lit.type_name);
        if (!sym)
            ST_diag_error(&se->diag, e->line, e->col,
                          "unknown type '" ST_sv_fmt "' in struct literal",
                          ST_sv_args(e->struct_lit.type_name));
        else if (sym->kind != ST_SYM_TYPE || sym->decl->kind != ST_DE_STRUCT)
        {
            ST_diag_error(&se->diag, e->line, e->col,
                          "'" ST_sv_fmt "' is not a struct type",
                          ST_sv_args(e->struct_lit.type_name));
            ST_diag_note(&se->diag, sym->line, sym->col,
                         "'" ST_sv_fmt "' is declared here",
                         ST_sv_args(e->struct_lit.type_name));
        }
        else
            sd = sym->decl;
    }

    ST_forrange(0, e->struct_lit.inits.count)
    {
        ST_field_init_t *fi = &e->struct_lit.inits.items[i];
        ST_check_expr(se, fi->value);
        if (!sd) continue;
        if (fi->name.len)
        {
            b8 found = 0;
            for (u32 k = 0; k < sd->struct_.fields.count; k++)
                if (ST_string_eq(sd->struct_.fields.items[k].name, fi->name))
                {
                    found = 1;
                    break;
                }
            if (!found)
            {
                ST_diag_error(&se->diag, fi->line, fi->col,
                              "struct '" ST_sv_fmt "' has no field '" ST_sv_fmt "'",
                              ST_sv_args(sd->name), ST_sv_args(fi->name));
                ST_diag_note(&se->diag, sd->line, sd->col,
                             "'" ST_sv_fmt "' is declared here",
                             ST_sv_args(sd->name));
            }
        }
        else if (i >= sd->struct_.fields.count)
        {
            ST_diag_error(&se->diag, fi->line, fi->col,
                          "too many initializers for struct '" ST_sv_fmt
                          "', it has %u field%s",
                          ST_sv_args(sd->name), sd->struct_.fields.count,
                          sd->struct_.fields.count == 1 ? "" : "s");
            ST_diag_note(&se->diag, sd->line, sd->col,
                         "'" ST_sv_fmt "' is declared here", ST_sv_args(sd->name));
            return;
        }
    }
}

static void ST_check_expr(ST_sema_t *se, ST_expr_t *e)
{
    if (!e) return;
    switch (e->kind)
    {
    case ST_EX_INT:
    case ST_EX_FLOAT:
    case ST_EX_STR:
    case ST_EX_CHAR:
    case ST_EX_BOOL:
    case ST_EX_NULL:
        break;
    case ST_EX_IDENT: {
        ST_sym_t *sym = ST_sym_find(se, e->name);
        if (!sym)
            ST_diag_error(&se->diag, e->line, e->col,
                          "use of undeclared identifier '" ST_sv_fmt "'",
                          ST_sv_args(e->name));
        break;
    }
    case ST_EX_UNARY:
        ST_check_expr(se, e->unary.operand);
        break;
    case ST_EX_BINARY:
        ST_check_expr(se, e->bin.l);
        ST_check_expr(se, e->bin.r);
        break;
    case ST_EX_CALL:
        ST_check_call(se, e);
        break;
    case ST_EX_FIELD:
        ST_check_expr(se, e->field.base);
        break;
    case ST_EX_INDEX:
        ST_check_expr(se, e->index.base);
        ST_check_expr(se, e->index.index);
        break;
    case ST_EX_CAST:
        ST_check_expr(se, e->cast.operand);
        ST_check_tyexpr(se, e->cast.to);
        break;
    case ST_EX_STRUCT_LIT:
        ST_check_struct_lit(se, e);
        break;
    case ST_EX_ARRAY_NEW:
        ST_check_tyexpr(se, e->array_new.te);
        break;
    case ST_EX_SIZEOF:
        ST_check_tyexpr(se, e->tyop.te);
        break;
    case ST_EX_TYPEOF:
    case ST_EX_KIND:
    case ST_EX_CSTR:
        ST_check_expr(se, e->tyop.operand);
        break;
    case ST_EX_TYPEINFO:
        if (e->tyop.te) ST_check_tyexpr(se, e->tyop.te);
        else ST_check_expr(se, e->tyop.operand);
        break;
    case ST_EX_COUNT:
        ST_assert(0);
        break;
    }
}

static void ST_check_stmt(ST_sema_t *se, ST_stmt_t *s);

static void ST_check_body(ST_sema_t *se, ST_stmts_t *body)
{
    ST_scope_push(se);
    ST_forrange(0, body->count)
        ST_check_stmt(se, body->items[i]);
    ST_scope_pop(se);
}

static void ST_check_stmt(ST_sema_t *se, ST_stmt_t *s)
{
    if (!s) return;
    switch (s->kind)
    {
    case ST_ST_EXPR:
        ST_check_expr(se, s->expr);
        break;
    case ST_ST_DECL:
        if (s->decl.te) ST_check_tyexpr(se, s->decl.te);
        ST_check_expr(se, s->decl.init);
        ST_declare_local(se, s->decl.name, s->line, s->col);
        break;
    case ST_ST_ASSIGN:
        ST_check_expr(se, s->assign.lhs);
        ST_check_expr(se, s->assign.rhs);
        break;
    case ST_ST_MULTI_BIND:
        ST_forrange(0, s->multi.values.count)
            ST_check_expr(se, s->multi.values.items[i]);
        ST_forrange(0, s->multi.n_names)
        {
            if (s->multi.declare)
                ST_declare_local(se, s->multi.names[i], s->line, s->col);
            else if (!ST_sym_find(se, s->multi.names[i]))
                ST_diag_error(&se->diag, s->line, s->col,
                              "use of undeclared identifier '" ST_sv_fmt "'",
                              ST_sv_args(s->multi.names[i]));
        }
        break;
    case ST_ST_IF:
        ST_check_expr(se, s->if_.cond);
        ST_check_body(se, &s->if_.then_body);
        ST_check_stmt(se, s->if_.else_stmt);
        break;
    case ST_ST_SWITCH:
        ST_check_expr(se, s->switch_.cond);
        ST_forrange(0, s->switch_.cases.count)
        {
            ST_case_t *c = &s->switch_.cases.items[i];
            for (u32 k = 0; k < c->values.count; k++)
                ST_check_expr(se, c->values.items[k]);
            ST_check_body(se, &c->body);
        }
        break;
    case ST_ST_WHILE:
        ST_check_expr(se, s->while_.cond);
        ST_check_body(se, &s->while_.body);
        break;
    case ST_ST_FOR_RANGE:
        ST_check_expr(se, s->for_range.lo);
        ST_check_expr(se, s->for_range.hi);
        ST_scope_push(se);
        ST_declare_local(se, s->for_range.iter, s->line, s->col);
        ST_forrange(0, s->for_range.body.count)
            ST_check_stmt(se, s->for_range.body.items[i]);
        ST_scope_pop(se);
        break;
    case ST_ST_FOR_ARRAY:
        ST_check_expr(se, s->for_array.target);
        ST_scope_push(se);
        ST_declare_local(se, s->for_array.iter, s->line, s->col);
        ST_forrange(0, s->for_array.body.count)
            ST_check_stmt(se, s->for_array.body.items[i]);
        ST_scope_pop(se);
        break;
    case ST_ST_RETURN:
        ST_forrange(0, s->ret.values.count)
            ST_check_expr(se, s->ret.values.items[i]);
        break;
    case ST_ST_BLOCK:
        ST_check_body(se, &s->block);
        break;
    case ST_ST_DEFER:
        ST_check_stmt(se, s->defer_stmt);
        break;
    case ST_ST_BREAK:
    case ST_ST_CONTINUE:
        break;
    case ST_ST_LABEL:
        break;
    case ST_ST_GODOWN:
        if (!ST_sym_find_in(se->labels, s->label))
            ST_diag_error(&se->diag, s->line, s->col,
                          "godown to unknown label '" ST_sv_fmt "'",
                          ST_sv_args(s->label));
        break;
    case ST_ST_COUNT:
        ST_assert(0);
        break;
    }
}

static void ST_collect_labels(ST_sema_t *se, ST_ht_t *labels, ST_stmts_t *body)
{
    ST_forrange(0, body->count)
    {
        ST_stmt_t *s = body->items[i];
        if (!s) continue;
        switch (s->kind)
        {
        case ST_ST_LABEL: {
            ST_sym_t *prev = ST_sym_find_in(labels, s->label);
            if (prev)
            {
                ST_diag_error(&se->diag, s->line, s->col,
                              "duplicate label '" ST_sv_fmt "'",
                              ST_sv_args(s->label));
                ST_diag_note(&se->diag, prev->line, prev->col,
                             "previous label is here");
                break;
            }
            ST_sym_insert(se, labels,
                          ST_sym_new(se, ST_SYM_VAR, s->label, NULL,
                                     s->line, s->col));
            break;
        }
        case ST_ST_IF:
            ST_collect_labels(se, labels, &s->if_.then_body);
            if (s->if_.else_stmt)
            {
                ST_stmts_t one = { .items = &s->if_.else_stmt, .count = 1 };
                ST_collect_labels(se, labels, &one);
            }
            break;
        case ST_ST_SWITCH:
            for (u32 k = 0; k < s->switch_.cases.count; k++)
                ST_collect_labels(se, labels, &s->switch_.cases.items[k].body);
            break;
        case ST_ST_WHILE:
            ST_collect_labels(se, labels, &s->while_.body);
            break;
        case ST_ST_FOR_RANGE:
            ST_collect_labels(se, labels, &s->for_range.body);
            break;
        case ST_ST_FOR_ARRAY:
            ST_collect_labels(se, labels, &s->for_array.body);
            break;
        case ST_ST_BLOCK:
            ST_collect_labels(se, labels, &s->block);
            break;
        case ST_ST_DEFER: {
            ST_stmts_t one = { .items = &s->defer_stmt, .count = 1 };
            ST_collect_labels(se, labels, &one);
            break;
        }
        case ST_ST_EXPR:
        case ST_ST_DECL:
        case ST_ST_ASSIGN:
        case ST_ST_MULTI_BIND:
        case ST_ST_RETURN:
        case ST_ST_BREAK:
        case ST_ST_CONTINUE:
        case ST_ST_GODOWN:
            break;
        case ST_ST_COUNT:
            ST_assert(0);
            break;
        }
    }
}

static ST_sym_kind_t ST_decl_sym_kind(ST_decl_t *d)
{
    switch (d->kind)
    {
    case ST_DE_STRUCT:
    case ST_DE_ENUM:
    case ST_DE_TAG_UNION:  return ST_SYM_TYPE;
    case ST_DE_FN:
    case ST_DE_EXTERN_FN:  return ST_SYM_FN;
    case ST_DE_CONST:      return ST_SYM_CONST;
    case ST_DE_EXTERN_VAR: return ST_SYM_EXTERN_VAR;
    case ST_DE_COUNT:
        ST_assert(0);
        break;
    }
    return ST_SYM_VAR;
}

static void ST_sema_collect(ST_sema_t *se, ST_program_t *prog)
{
    ST_forrange(0, prog->decls.count)
    {
        ST_decl_t *d = prog->decls.items[i];
        if (!d) continue;
        ST_sym_t *prev = ST_sym_find_in(&se->globals, d->name);
        if (prev)
        {
            b8 prev_proto = prev->decl && prev->decl->kind == ST_DE_FN
                && prev->decl->fn.is_prototype;
            b8 cur_proto = d->kind == ST_DE_FN && d->fn.is_prototype;
            b8 both_fn = prev->decl && prev->decl->kind == ST_DE_FN
                && d->kind == ST_DE_FN;
            if (both_fn && (prev_proto ^ cur_proto))
            {
                if (prev_proto)
                {
                    prev->decl = d;
                    prev->line = d->line;
                    prev->col = d->col;
                }
                continue;
            }
            ST_diag_error(&se->diag, d->line, d->col,
                          "redefinition of '" ST_sv_fmt "'", ST_sv_args(d->name));
            ST_diag_note(&se->diag, prev->line, prev->col,
                         "previous definition is here");
            continue;
        }
        ST_sym_insert(se, &se->globals,
                      ST_sym_new(se, ST_decl_sym_kind(d), d->name, d,
                                 d->line, d->col));
    }
}

static void ST_check_sig(ST_sema_t *se, ST_fn_sig_t *sig)
{
    ST_forrange(0, sig->params.count)
    {
        ST_param_t *p = &sig->params.items[i];
        if (p->te) ST_check_tyexpr(se, p->te);
        if (p->def) ST_check_expr(se, p->def);
        ST_declare_local(se, p->name, p->line, p->col);
    }
    ST_forrange(0, sig->rets.count)
        ST_check_tyexpr(se, sig->rets.items[i]);
}

static void ST_check_fields(ST_sema_t *se, ST_decl_t *d)
{
    ST_forrange(0, d->struct_.fields.count)
    {
        ST_field_spec_t *f = &d->struct_.fields.items[i];
        if (f->te) ST_check_tyexpr(se, f->te);
        for (u32 k = 0; k < i; k++)
            if (ST_string_eq(d->struct_.fields.items[k].name, f->name)
                && f->name.len)
            {
                ST_diag_error(&se->diag, f->line, f->col,
                              "duplicate field '" ST_sv_fmt "' in struct '"
                              ST_sv_fmt "'",
                              ST_sv_args(f->name), ST_sv_args(d->name));
                ST_diag_note(&se->diag, d->struct_.fields.items[k].line,
                             d->struct_.fields.items[k].col,
                             "previous field is here");
                break;
            }
    }
}

static void ST_sema_check(ST_sema_t *se, ST_program_t *prog)
{
    ST_forrange(0, prog->decls.count)
    {
        ST_decl_t *d = prog->decls.items[i];
        if (!d) continue;
        switch (d->kind)
        {
        case ST_DE_STRUCT:
            ST_check_fields(se, d);
            break;
        case ST_DE_ENUM:
        case ST_DE_TAG_UNION: {
            ST_variant_specs_t *vs = d->kind == ST_DE_ENUM
                ? &d->enum_.variants : &d->tag_union.variants;
            for (u32 k = 0; k < vs->count; k++)
                if (vs->items[k].payload)
                    ST_check_tyexpr(se, vs->items[k].payload);
            break;
        }
        case ST_DE_CONST:
            ST_check_expr(se, d->const_.value);
            break;
        case ST_DE_EXTERN_VAR:
            ST_check_tyexpr(se, d->extern_var.te);
            break;
        case ST_DE_EXTERN_FN:
            ST_scope_push(se);
            ST_check_sig(se, &d->extern_fn.sig);
            ST_scope_pop(se);
            break;
        case ST_DE_FN: {
            ST_ht_t labels;
            ST_ht_init(se->arena, &labels, 8);
            se->labels = &labels;
            if (!d->fn.is_prototype)
                ST_collect_labels(se, &labels, &d->fn.body);
            ST_scope_push(se);
            ST_check_sig(se, &d->fn.sig);
            if (!d->fn.is_prototype)
                ST_forrange(0, d->fn.body.count)
                    ST_check_stmt(se, d->fn.body.items[i]);
            ST_scope_pop(se);
            se->labels = NULL;
            break;
        }
        case ST_DE_COUNT:
            ST_assert(0);
            break;
        }
    }
}

b8 ST_sema_run(ST_arena_t *arena, ST_program_t *prog, ST_string_t src,
                ST_string_t file)
{
    ST_sema_t se = {0};
    se.arena = arena;
    se.diag.src = src;
    se.diag.file = file;
    se.diag.max_errors = ST_SEMA_MAX_ERRORS;
    ST_ht_init(arena, &se.globals, 64);

    ST_forrange(0, ST_array_len(ST_builtin_fns))
    {
        ST_string_t name = ST_cstr_to_str((char *)ST_builtin_fns[i]);
        ST_sym_insert(&se, &se.globals,
                      ST_sym_new(&se, ST_SYM_FN, name, NULL, 0, 0));
    }

    ST_sema_collect(&se, prog);
    ST_sema_check(&se, prog);
    return se.diag.n_errors == 0;
}
