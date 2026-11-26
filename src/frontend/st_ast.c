#include "st_ast.h"

// A struct field written with a generic struct's bare name ('entries: [..]Entry;' where 'Entry'
// has '$K' and '$V') is shorthand for passing along the generics the two share, as if
// '[..]Entry($K, $V)' had been written. The enclosing struct takes those generics over, so it is
// generic in them too. A generic is listed at its first appearance in field order; generics that
// were declared but are not used in a field keep their place at the front. It runs on the
// declarations of one file in the parser (so 'impl' blocks see the final list) and once more over
// the whole program for structs that name a generic struct from another module.
typedef struct {
    ST_decl_t **items;
    u32 count;
    u32 capacity;
} ST_gp_decls_t;

static ST_strings_t *ST_gp_generics(ST_decl_t *d) {
    if (d->kind == ST_DE_STRUCT)
        return &d->struct_.generics;
    if (d->kind == ST_DE_TAG_UNION)
        return &d->tag_union.generics;
    return NULL;
}

static ST_decl_t *ST_gp_find(ST_gp_decls_t *g, ST_string_t name) {
    ST_forrange(0, g->count) {
        if (ST_string_eq(g->items[i]->name, name))
            return g->items[i];
    }
    return NULL;
}

static void ST_gp_add(ST_arena_t *arena, ST_strings_t *out, ST_string_t name) {
    ST_forrange(0, out->count) if (ST_string_eq(out->items[i], name)) return;
    ST_da_append_arena(arena, out, name);
}

static void ST_gp_walk(ST_arena_t *arena, ST_gp_decls_t *g, ST_decl_t *self, ST_tyexpr_t *te,
                       ST_strings_t *order, b8 *bare) {
    if (!te)
        return;
    if (te->kind == ST_TE_NAME) {
        if (te->is_generic_param) {
            ST_gp_add(arena, order, te->name);
            return;
        }
        if (te->module_alias.len)
            return;
        ST_decl_t *d = ST_gp_find(g, te->name);
        if (d && d != self) {
            ST_strings_t *dg = ST_gp_generics(d);
            ST_forrange(0, dg->count) ST_gp_add(arena, order, dg->items[i]);
            *bare = 1;
        }
        return;
    }
    if (te->kind == ST_TE_GENERIC_INST && te->is_generic_param)
        ST_gp_add(arena, order, te->name);
    ST_gp_walk(arena, g, self, te->inner, order, bare);
    ST_forrange(0, te->fn_params.count) ST_gp_walk(arena, g, self, te->fn_params.items[i], order, bare);
    ST_forrange(0, te->fn_rets.count) ST_gp_walk(arena, g, self, te->fn_rets.items[i], order, bare);
    ST_forrange(0, te->generic_args.count)
        ST_gp_walk(arena, g, self, te->generic_args.items[i], order, bare);
}

static void ST_gp_fields(ST_arena_t *arena, ST_gp_decls_t *g, ST_decl_t *self,
                         ST_field_specs_t *fields, ST_strings_t *order, b8 *bare) {
    ST_forrange(0, fields->count) {
        ST_field_spec_t *f = &fields->items[i];
        ST_gp_walk(arena, g, self, f->te, order, bare);
        if (f->anon && f->anon->kind == ST_DE_STRUCT)
            ST_gp_fields(arena, g, self, &f->anon->struct_.fields, order, bare);
    }
}

void ST_propagate_struct_generics(ST_arena_t *arena, ST_decls_t *decls) {
    ST_gp_decls_t gen = {0};
    ST_forrange(0, decls->count) {
        ST_decl_t *d = decls->items[i];
        ST_strings_t *dg = d ? ST_gp_generics(d) : NULL;
        if (dg && dg->count)
            ST_da_append_arena(arena, &gen, d);
    }
    if (!gen.count)
        return;
    // a struct that picks up generics can make another one generic in turn: repeat until stable
    for (b8 changed = 1; changed;) {
        changed = 0;
        ST_forrange(0, decls->count) {
            ST_decl_t *d = decls->items[i];
            if (!d || d->kind != ST_DE_STRUCT)
                continue;
            ST_strings_t order = {0};
            b8 bare = 0;
            ST_gp_fields(arena, &gen, d, &d->struct_.fields, &order, &bare);
            if (!bare)
                continue;
            b8 grew = 0;
            for (u32 k = 0; k < order.count; k++) {
                b8 have = 0;
                for (u32 j = 0; j < d->struct_.generics.count; j++)
                    if (ST_string_eq(d->struct_.generics.items[j], order.items[k]))
                        have = 1;
                if (!have)
                    grew = 1;
            }
            if (!grew)
                continue;
            ST_strings_t merged = {0};
            for (u32 j = 0; j < d->struct_.generics.count; j++) {
                b8 used = 0;
                for (u32 k = 0; k < order.count; k++)
                    if (ST_string_eq(order.items[k], d->struct_.generics.items[j]))
                        used = 1;
                if (!used)
                    ST_da_append_arena(arena, &merged, d->struct_.generics.items[j]);
            }
            for (u32 k = 0; k < order.count; k++)
                ST_da_append_arena(arena, &merged, order.items[k]);
            d->struct_.generics = merged;
            b8 listed = 0;
            for (u32 k = 0; k < gen.count; k++)
                if (gen.items[k] == d)
                    listed = 1;
            if (!listed)
                ST_da_append_arena(arena, &gen, d);
            changed = 1;
        }
    }
}

ST_expr_t *ST_expr_new(ST_arena_t *a, ST_expr_kind_t kind, u32 line, u32 col) {
    ST_expr_t *e = ST_arena_push_zeroed(a, sizeof(*e));
    e->kind = kind;
    e->line = line;
    e->col = col;
    return e;
}

ST_stmt_t *ST_stmt_new(ST_arena_t *a, ST_stmt_kind_t kind, u32 line, u32 col) {
    ST_stmt_t *s = ST_arena_push_zeroed(a, sizeof(*s));
    s->kind = kind;
    s->line = line;
    s->col = col;
    return s;
}

ST_decl_t *ST_decl_new(ST_arena_t *a, ST_decl_kind_t kind, u32 line, u32 col) {
    ST_decl_t *d = ST_arena_push_zeroed(a, sizeof(*d));
    d->kind = kind;
    d->line = line;
    d->col = col;
    return d;
}

ST_tyexpr_t *ST_tyexpr_new(ST_arena_t *a, ST_tyexpr_kind_t kind, u32 line, u32 col) {
    ST_tyexpr_t *te = ST_arena_push_zeroed(a, sizeof(*te));
    te->kind = kind;
    te->line = line;
    te->col = col;
    return te;
}

static void ST_dump_body(FILE *out, ST_stmts_t *body, u32 depth);

static void ST_indent(FILE *out, u32 depth) {
    fprintf(out, "%*s", (int)(depth * 2), "");
}

void ST_dump_tyexpr(FILE *out, ST_tyexpr_t *te) {
    if (!te) {
        fprintf(out, "<none>");
        return;
    }
    switch (te->kind) {
        case ST_TE_FN:
        fputs("fn(", out);
        ST_forrange(0, te->fn_params.count) {
            if (i)
                fputs(", ", out);
            ST_dump_tyexpr(out, te->fn_params.items[i]);
        }
        if (te->fn_is_variadic)
            fprintf(out, te->fn_params.count ? ", .." : "..");
        fputs(")", out);
        if (te->fn_rets.count) {
            fputs("->", out);
            ST_forrange(0, te->fn_rets.count) {
                if (i)
                    fputs(", ", out);
                ST_dump_tyexpr(out, te->fn_rets.items[i]);
            }
        }
        break;
        
        case ST_TE_NAME:
        if (te->is_generic_param)
            fputc('$', out);
        fprintf(out, ST_sv_fmt, ST_sv_args(te->name));
        break;
        
        case ST_TE_GENERIC_INST:
        if (te->is_generic_param)
            fputc('$', out);
        fprintf(out, ST_sv_fmt, ST_sv_args(te->name));
        fputs("(", out);
        ST_forrange(0, te->generic_args.count) {
            if (i)
                fputs(", ", out);
            ST_dump_tyexpr(out, te->generic_args.items[i]);
        }
        fputs(")", out);
        
        break;
        
        case ST_TE_PTR:
        fprintf(out, "*");
        ST_dump_tyexpr(out, te->inner);
        break;
        
        case ST_TE_ARRAY:
        if (te->is_dynamic)
            fputs("[..]", out);
        else if (te->count_expr && te->count_expr->kind == ST_EX_INT)
            fprintf(out, "[%ld]", (long)te->count_expr->ival);
        else if (te->count_expr)
            fputs("[expr]", out);
        else
            fputs("[?]", out);
        ST_dump_tyexpr(out, te->inner);
        break;
        case ST_TE_TYPEOF:
        fputs("type_of(", out);
        ST_dump_expr(out, te->typeof_operand, 0);
        fputs(")", out);
        break;
    }
}

void ST_dump_expr(FILE *out, ST_expr_t *e, u32 depth) {
    ST_indent(out, depth);
    if (!e) {
        fprintf(out, "<null expr>\n");
        return;
    }
    switch (e->kind) {
        case ST_EX_INT:
        fprintf(out, "int %ld\n", (long)e->ival);
        break;
        case ST_EX_FLOAT:
        fprintf(out, "float %g\n", e->fval);
        break;
        case ST_EX_STR:
        fprintf(out, "str \"" ST_sv_fmt "\"\n", ST_sv_args(e->sval));
        break;
        case ST_EX_CHAR:
        fprintf(out, "char '%c'\n", (char)e->ival);
        break;
        case ST_EX_BOOL:
        fprintf(out, "bool %s\n", e->ival ? "true" : "false");
        break;
        case ST_EX_NULL:
        fprintf(out, "null\n");
        break;
        case ST_EX_IDENT:
        fprintf(out, "ident " ST_sv_fmt "\n", ST_sv_args(e->name));
        break;
        case ST_EX_UNARY:
        fprintf(out, "unary " ST_sv_fmt "\n", ST_sv_args(e->unary.op));
        ST_dump_expr(out, e->unary.operand, depth + 1);
        break;
        case ST_EX_BINARY:
        fprintf(out, "binary " ST_sv_fmt "\n", ST_sv_args(e->bin.op));
        ST_dump_expr(out, e->bin.l, depth + 1);
        ST_dump_expr(out, e->bin.r, depth + 1);
        break;
        case ST_EX_CALL:
        fprintf(out, "call\n");
        ST_dump_expr(out, e->call.callee, depth + 1);
        ST_forrange(0, e->call.args.count) {
            ST_arg_t *arg = &e->call.args.items[i];
            if (arg->name.len) {
                ST_indent(out, depth + 1);
                fprintf(out, "named " ST_sv_fmt " =\n", ST_sv_args(arg->name));
                ST_dump_expr(out, arg->value, depth + 2);
            } else
                ST_dump_expr(out, arg->value, depth + 1);
        }
        break;
        case ST_EX_FIELD:
        fprintf(out, "field ." ST_sv_fmt "\n", ST_sv_args(e->field.name));
        ST_dump_expr(out, e->field.base, depth + 1);
        break;
        case ST_EX_INDEX:
        fprintf(out, "index\n");
        ST_dump_expr(out, e->index.base, depth + 1);
        ST_dump_expr(out, e->index.index, depth + 1);
        break;
        case ST_EX_CAST:
        fprintf(out, "cast #as ");
        ST_dump_tyexpr(out, e->cast.to);
        fprintf(out, "\n");
        ST_dump_expr(out, e->cast.operand, depth + 1);
        break;
        case ST_EX_STRUCT_LIT:
        if (e->struct_lit.type_name.len) {
            fprintf(out, "struct_lit " ST_sv_fmt, ST_sv_args(e->struct_lit.type_name));
            if (e->struct_lit.generic_args.count) {
                fputs("(", out);
                ST_forrange(0, e->struct_lit.generic_args.count) {
                    if (i)
                        fputs(", ", out);
                    ST_dump_tyexpr(out, e->struct_lit.generic_args.items[i]);
                }
                fputs(")", out);
            }
            fputc('\n', out);
        }
        
        else
            fprintf(out, "struct_lit <inferred>\n");
        ST_forrange(0, e->struct_lit.inits.count) {
            ST_field_init_t *fi = &e->struct_lit.inits.items[i];
            ST_indent(out, depth + 1);
            if (fi->name.len)
                fprintf(out, ST_sv_fmt ":\n", ST_sv_args(fi->name));
            else
                fprintf(out, "[%u]:\n", i);
            ST_dump_expr(out, fi->value, depth + 2);
        }
        break;
        case ST_EX_ARRAY_NEW:
        fprintf(out, "array_new ");
        ST_dump_tyexpr(out, e->array_new.te);
        fprintf(out, "\n");
        break;
        case ST_EX_SIZEOF:
        fprintf(out, "%s ", e->tyop.is_align ? "align_of" : "sizeof");
        ST_dump_tyexpr(out, e->tyop.te);
        fprintf(out, "\n");
        break;
        case ST_EX_TYPEOF:
        fprintf(out, "type_of\n");
        ST_dump_expr(out, e->tyop.operand, depth + 1);
        break;
        case ST_EX_TYPEINFO:
        fprintf(out, "type_info");
        if (e->tyop.te) {
            fprintf(out, " ");
            ST_dump_tyexpr(out, e->tyop.te);
            fprintf(out, "\n");
        } else {
            fprintf(out, "\n");
            ST_dump_expr(out, e->tyop.operand, depth + 1);
        }
        break;
        case ST_EX_KIND:
        fprintf(out, "kind\n");
        ST_dump_expr(out, e->tyop.operand, depth + 1);
        break;
        case ST_EX_FIELDS:
        fprintf(out, "fields\n");
        ST_dump_expr(out, e->tyop.operand, depth + 1);
        break;
        case ST_EX_CSTR:
        fprintf(out, "cstr\n");
        ST_dump_expr(out, e->tyop.operand, depth + 1);
        break;
        case ST_EX_ASM:
        fprintf(out, "asm_expr (%u tokens)\n", e->asm_.n_tokens);
        break;
        case ST_EX_STR_FROM_RAW:
        fprintf(out, "str_from_raw\n");
        ST_dump_expr(out, e->str_from_raw.ptr, depth + 1);
        ST_dump_expr(out, e->str_from_raw.len, depth + 1);
        break;
        case ST_EX_COMP_ERROR:
        fprintf(out, "comp_error\n");
        ST_forrange(0, e->comp_error.args.count) ST_dump_expr(out, e->comp_error.args.items[i], depth + 1);
        break;
        case ST_EX_PACK_FOLD:
        fprintf(out, "pack_fold (" ST_sv_fmt " " ST_sv_fmt " ...)\n",
                ST_sv_args(e->pack_fold.pack_name), ST_sv_args(e->pack_fold.op));
        break;
        case ST_EX_TRAIT_IMPL:
        fprintf(out, "trait_impl " ST_sv_fmt "(", ST_sv_args(e->trait_impl.trait_name));
        ST_forrange(0, e->trait_impl.trait_args.count) {
            if (i)
                fputs(", ", out);
            ST_dump_tyexpr(out, e->trait_impl.trait_args.items[i]);
        }
        fputs(")\n", out);
        ST_dump_body(out, &e->trait_impl.body, depth + 1);
        break;
        case ST_EX_CODE_LOC:
        fprintf(out, "code " ST_sv_fmt ":%u:%u\n", ST_sv_args(e->sval), e->line, e->col);
        break;
        case ST_EX_COUNT:
        ST_assert(0);
        break;
    }
}

static void ST_dump_body(FILE *out, ST_stmts_t *body, u32 depth) {
    ST_forrange(0, body->count) ST_dump_stmt(out, body->items[i], depth);
}

void ST_dump_stmt(FILE *out, ST_stmt_t *s, u32 depth) {
    ST_indent(out, depth);
    if (!s) {
        fprintf(out, "<null stmt>\n");
        return;
    }
    switch (s->kind) {
        case ST_ST_EXPR:
        fprintf(out, "expr_stmt\n");
        ST_dump_expr(out, s->expr, depth + 1);
        break;
        case ST_ST_PACK_EXPAND:
        fprintf(out, "pack_expand_stmt\n");
        ST_dump_expr(out, s->expr, depth + 1);
        break;
        case ST_ST_DECL:
        fprintf(out, "decl " ST_sv_fmt, ST_sv_args(s->decl.name));
        if (s->decl.te) {
            fprintf(out, ": ");
            ST_dump_tyexpr(out, s->decl.te);
        }
        if (s->decl.is_static)
            fprintf(out, " static");
        fprintf(out, "\n");
        if (s->decl.init)
            ST_dump_expr(out, s->decl.init, depth + 1);
        break;
        case ST_ST_ASSIGN:
        fprintf(out, "assign " ST_sv_fmt "\n", ST_sv_args(s->assign.op));
        ST_dump_expr(out, s->assign.lhs, depth + 1);
        ST_dump_expr(out, s->assign.rhs, depth + 1);
        break;
        case ST_ST_MULTI_BIND:
        fprintf(out, "multi_%s", s->multi.declare ? "decl" : "assign");
        ST_forrange(0, s->multi.n_names)
            fprintf(out, " " ST_sv_fmt, ST_sv_args(s->multi.names[i]));
        fprintf(out, "\n");
        ST_forrange(0, s->multi.values.count)
            ST_dump_expr(out, s->multi.values.items[i], depth + 1);
        break;
        case ST_ST_IF:
        fprintf(out, "if\n");
        ST_dump_expr(out, s->if_.cond, depth + 1);
        ST_indent(out, depth);
        fprintf(out, "then\n");
        ST_dump_body(out, &s->if_.then_body, depth + 1);
        if (s->if_.else_stmt) {
            ST_indent(out, depth);
            fprintf(out, "else\n");
            ST_dump_stmt(out, s->if_.else_stmt, depth + 1);
        }
        break;
        case ST_ST_SWITCH:
        fprintf(out, "switch\n");
        ST_dump_expr(out, s->switch_.cond, depth + 1);
        ST_forrange(0, s->switch_.cases.count) {
            ST_case_t *c = &s->switch_.cases.items[i];
            ST_indent(out, depth);
            fprintf(out, c->values.count ? "case\n" : "default\n");
            for (u32 k = 0; k < c->values.count; k++)
                ST_dump_expr(out, c->values.items[k], depth + 1);
            ST_dump_body(out, &c->body, depth + 1);
        }
        break;
        case ST_ST_WHILE:
        fprintf(out, "while\n");
        ST_dump_expr(out, s->while_.cond, depth + 1);
        ST_dump_body(out, &s->while_.body, depth + 1);
        break;
        case ST_ST_FOR_RANGE:
        fprintf(out, "for_range " ST_sv_fmt "\n", ST_sv_args(s->for_range.iter));
        if (s->for_range.iter_te) {
            fprintf(out, ": ");
            ST_dump_tyexpr(out, s->for_range.iter_te);
        }
        fprintf(out, "%s\n", s->for_range.inclusive ? "..=" : "..");
        ST_dump_expr(out, s->for_range.lo, depth + 1);
        ST_dump_expr(out, s->for_range.hi, depth + 1);
        ST_dump_body(out, &s->for_range.body, depth + 1);
        break;
        case ST_ST_FOR_ARRAY:
        fprintf(out, "for_array " ST_sv_fmt "\n", ST_sv_args(s->for_array.iter));
        ST_dump_expr(out, s->for_array.target, depth + 1);
        ST_dump_body(out, &s->for_array.body, depth + 1);
        break;
        case ST_ST_RETURN:
        fprintf(out, "return\n");
        ST_forrange(0, s->ret.values.count)
            ST_dump_expr(out, s->ret.values.items[i], depth + 1);
        break;
        case ST_ST_BLOCK:
        fprintf(out, "block\n");
        ST_dump_body(out, &s->block, depth + 1);
        break;
        case ST_ST_COMPTIME_BLOCK:
        fprintf(out, "#comptime block\n");
        ST_dump_body(out, &s->block, depth + 1);
        break;
        case ST_ST_DEFER:
        fprintf(out, "defer\n");
        ST_dump_stmt(out, s->defer_stmt, depth + 1);
        break;
        case ST_ST_BREAK:
        fprintf(out, "break\n");
        break;
        case ST_ST_CONTINUE:
        fprintf(out, "continue\n");
        break;
        case ST_ST_LABEL:
        fprintf(out, "label " ST_sv_fmt "\n", ST_sv_args(s->label));
        break;
        case ST_ST_GODOWN:
        fprintf(out, "godown " ST_sv_fmt "\n", ST_sv_args(s->label));
        break;
        case ST_ST_ASM:
            fprintf(out, "asm (%u tokens)\n", (u32)s->asm_.n_tokens);
            break;
        case ST_ST_NESTED_FN:
            fprintf(out, "nested_fn " ST_sv_fmt "\n", ST_sv_args(s->nested_fn->name));
            break;
        case ST_ST_COUNT:
        ST_assert(0);
        break;
    }
}

static void ST_dump_sig(FILE *out, ST_fn_sig_t *sig, u32 depth) {
    ST_forrange(0, sig->params.count) {
        ST_param_t *p = &sig->params.items[i];
        ST_indent(out, depth);
        fprintf(out, "param " ST_sv_fmt, ST_sv_args(p->name));
        if (p->te) {
            fprintf(out, ": ");
            ST_dump_tyexpr(out, p->te);
        }
        fprintf(out, "\n");
        if (p->def) {
            ST_indent(out, depth + 1);
            fprintf(out, "default\n");
            ST_dump_expr(out, p->def, depth + 2);
        }
    }
    if (sig->is_variadic) {
        ST_indent(out, depth);
        fprintf(out, "variadic\n");
    }
    ST_forrange(0, sig->rets.count) {
        ST_indent(out, depth);
        fprintf(out, "ret ");
        ST_dump_tyexpr(out, sig->rets.items[i]);
        fprintf(out, "\n");
    }
    if (!sig->has_ret_ann) {
        ST_indent(out, depth);
        fprintf(out, "ret <missing>\n");
    }
    ST_forrange(0, sig->wheres.count) {
        ST_where_clause_t *w = &sig->wheres.items[i];
        ST_indent(out, depth);
        fprintf(out, "where " ST_sv_fmt " : " ST_sv_fmt "(", ST_sv_args(w->witness_name),
                ST_sv_args(w->trait_name));
        ST_forrange(0, w->trait_args.count) {
            if (i)
                fputs(", ", out);
            ST_dump_tyexpr(out, w->trait_args.items[i]);
        }
        fputs(")\n", out);
    }
}

void ST_dump_decl(FILE *out, ST_decl_t *d, u32 depth) {
    ST_indent(out, depth);
    if (!d) {
        fprintf(out, "<null decl>\n");
        return;
    }
    switch (d->kind) {
        case ST_DE_STRUCT: {
            const char *pack = d->struct_.packing == ST_PACK_C        ? " #pad"
                : d->struct_.packing == ST_PACK_PACKED ? " #pack"
                : "";
            fprintf(out, "struct " ST_sv_fmt "%s%s%s\n", ST_sv_args(d->name), pack,
                    d->struct_.is_union ? " #union" : "", d->is_pub ? " pub" : "");
            ST_forrange(0, d->struct_.fields.count) {
                ST_field_spec_t *f = &d->struct_.fields.items[i];
                ST_indent(out, depth + 1);
                fprintf(out, "field " ST_sv_fmt, ST_sv_args(f->name));
                if (f->te) {
                    fprintf(out, ": ");
                    ST_dump_tyexpr(out, f->te);
                    fprintf(out, "\n");
                } else if (f->anon) {
                    fprintf(out, ": anon\n");
                    ST_dump_decl(out, f->anon, depth + 2);
                } else
                    fprintf(out, ": <none>\n");
            }
            break;
        }
        case ST_DE_ENUM:
        fprintf(out, "%s " ST_sv_fmt "\n", d->enum_.is_flag ? ST_keyword_names[ST_kenum_flag] : ST_keyword_names[ST_kenum],
                ST_sv_args(d->name));
        ST_forrange(0, d->enum_.variants.count) {
            ST_variant_spec_t *v = &d->enum_.variants.items[i];
            ST_indent(out, depth + 1);
            fprintf(out, "variant " ST_sv_fmt "\n", ST_sv_args(v->name));
            if (v->value)
                ST_dump_expr(out, v->value, depth + 2);
        }
        break;
        case ST_DE_TAG_UNION:
        fprintf(out, "%s " ST_sv_fmt "\n", ST_keyword_names[ST_kunion], ST_sv_args(d->name));
        ST_forrange(0, d->tag_union.variants.count) {
            ST_variant_spec_t *v = &d->tag_union.variants.items[i];
            ST_indent(out, depth + 1);
            fprintf(out, "variant " ST_sv_fmt, ST_sv_args(v->name));
            if (v->payload) {
                fprintf(out, "(");
                ST_dump_tyexpr(out, v->payload);
                fprintf(out, ")");
            }
            fprintf(out, "\n");
        }
        break;
        case ST_DE_CONST:
        fprintf(out, "const " ST_sv_fmt, ST_sv_args(d->name));
        if (d->const_.te) {
            fprintf(out, ": ");
            ST_dump_tyexpr(out, d->const_.te);
        }
        fprintf(out, "\n");
        ST_dump_expr(out, d->const_.value, depth + 1);
        break;
        case ST_DE_TYPE_ALIAS:
        fprintf(out, "using " ST_sv_fmt "%s ", ST_sv_args(d->name), d->is_pub ? " pub" : "");
        ST_dump_tyexpr(out, d->type_alias.te);
        fprintf(out, "\n");
        break;
        case ST_DE_EXTERN_FN:
        fprintf(out, "extern_fn " ST_sv_fmt "\n", ST_sv_args(d->name));
        ST_dump_sig(out, &d->extern_fn.sig, depth + 1);
        break;
        case ST_DE_EXTERN_VAR:
        fprintf(out, "extern_var " ST_sv_fmt ": ", ST_sv_args(d->name));
        ST_dump_tyexpr(out, d->extern_var.te);
        fprintf(out, "\n");
        break;
        case ST_DE_FN:
        fprintf(out, "fn " ST_sv_fmt "%s%s\n", ST_sv_args(d->name), d->is_pub ? " pub" : "",
                d->fn.is_prototype ? " prototype" : "");
        ST_dump_sig(out, &d->fn.sig, depth + 1);
        if (!d->fn.is_prototype) {
            ST_indent(out, depth + 1);
            fprintf(out, "body\n");
            ST_dump_body(out, &d->fn.body, depth + 2);
        }
        break;
        case ST_DE_GLOBAL:
        fprintf(out, "global " ST_sv_fmt "%s: ", ST_sv_args(d->name), d->is_pub ? " pub" : "");
        ST_dump_tyexpr(out, d->global_.te);
        fprintf(out, "\n");
        if (d->global_.init)
            ST_dump_expr(out, d->global_.init, depth + 1);
        break;
        case ST_DE_IMPORT:
        fprintf(out, "import " ST_sv_fmt " as " ST_sv_fmt "\n",
                ST_sv_args(d->import_.module_name), ST_sv_args(d->import_.alias));
        break;
        case ST_DE_TRAIT:
        fprintf(out, "trait " ST_sv_fmt "%s", ST_sv_args(d->name), d->is_pub ? " pub" : "");
        ST_forrange(0, d->trait_.generics.count) {
            fprintf(out, " $" ST_sv_fmt, ST_sv_args(d->trait_.generics.items[i]));
        }
        fputc('\n', out);
        if (d->trait_.self_alias.len) {
            ST_indent(out, depth + 1);
            fprintf(out, "self_alias " ST_sv_fmt "\n", ST_sv_args(d->trait_.self_alias));
        }
        ST_forrange(0, d->trait_.methods.count) {
            ST_trait_method_t *m = &d->trait_.methods.items[i];
            ST_indent(out, depth + 1);
            fprintf(out, "method " ST_sv_fmt "\n", ST_sv_args(m->name));
            ST_dump_sig(out, &m->sig, depth + 2);
        }
        break;
        case ST_DE_COUNT:
        ST_assert(0);
        break;
    }
}

void ST_dump_program(FILE *out, ST_program_t *prog) {
    fprintf(out, "program " ST_sv_fmt "\n", ST_sv_args(prog->file));
    ST_forrange(0, prog->decls.count) ST_dump_decl(out, prog->decls.items[i], 1);
}
