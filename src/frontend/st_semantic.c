#include "st_semantic.h"
#include "st_lexer.h"
#include "comptime/st_comptime.h"
#include "comptime/st_comptime_compile.h"

static const char *ST_builtin_fns[] = {"typeof"};
static ST_tys_t ST_builtin_rets;

#define ST_MAX_FN_INSTANCE 1024

static ST_ht_generic_t ST_name_key(ST_string_t name) {
    return (ST_ht_generic_t){.tag = name.data, .size = name.len};
}

static void ST_sym_insert(ST_sema_t *se, ST_ht_t *ht, ST_sym_t *sym) {
    ST_ht_generic_t *key = ST_arena_push(se->arena, sizeof(*key));
    key->tag = sym->name.data;
    key->size = sym->name.len;
    ST_ht_set(ht, key, (ST_ht_generic_t){.tag = sym, .size = sizeof(*sym)});
}

static ST_sym_t *ST_sym_find_in(ST_ht_t *ht, ST_string_t name) {
    return (ST_sym_t *)ST_ht_get(ht, ST_name_key(name)).tag;
}

static ST_sym_t *ST_sym_find(ST_sema_t *se, ST_string_t name) {
    for (ST_scope_t *s = se->scope; s; s = s->parent) {
        ST_sym_t *sym = ST_sym_find_in(&s->table, name);
        if (sym)
            return sym;
    }
    return ST_sym_find_in(&se->globals, name);
}

static ST_sym_t *ST_sym_new(ST_sema_t *se, ST_sym_kind_t kind, ST_string_t name, ST_decl_t *decl,
                            ST_ty_t *t, u32 line, u32 col) {
    ST_sym_t *sym = ST_arena_push_zeroed(se->arena, sizeof(*sym));
    sym->kind = kind;
    sym->name = name;
    sym->decl = decl;
    sym->t = t;
    sym->line = line;
    sym->col = col;
    return sym;
}

static void ST_scope_push(ST_sema_t *se) {
    ST_scope_t *s = ST_arena_push_zeroed(se->arena, sizeof(*s));
    ST_ht_init(se->arena, &s->table, 8);
    s->parent = se->scope;
    se->scope = s;
}

static void ST_scope_pop(ST_sema_t *se) {
    se->scope = se->scope->parent;
}

static const char *ST_sym_kind_str(ST_sym_kind_t kind) {
    switch (kind) {
        case ST_SYM_VAR:
            return "variable";
        case ST_SYM_FN:
            return "function";
        case ST_SYM_TYPE:
            return "type";
        case ST_SYM_CONST:
            return "constant";
        case ST_SYM_EXTERN_VAR:
            return "extern variable";
        case ST_SYM_GLOBAL:
            return "global variable";
        case ST_SYM_MODULE:
            return "module";
    }
    return "symbol";
}

// The name to show a person for a decl in a diagnostic: for a generic
// instantiation this is the readable 'Foo(i32, i32)' form, not the mangled
// internal name ('Foo$i32$i32').
static ST_string_t ST_decl_display_name(ST_decl_t *d) {
    return d->display_name.len ? d->display_name : d->name;
}

// The name to show a person in a diagnostic: for a generic function
// instantiation this is the original template name they actually wrote
// ('mod'), not the mangled per-instantiation symbol name ('mod$i32$string').
static ST_string_t ST_sym_display_name(ST_sym_t *sym) {
    return sym->template_name.len ? sym->template_name : sym->name;
}

static void ST_declare_local_ex(ST_sema_t *se, ST_string_t name, ST_ty_t *t, u32 line, u32 col,
                                b8 is_const) {
    ST_sym_t *prev = ST_sym_find_in(&se->scope->table, name);
    if (prev) {
        ST_diag_error(&se->diag, line, col, "redeclaration of '" ST_sv_fmt "' in the same scope",
                      ST_sv_args(name));
        ST_diag_note(&se->diag, prev->line, prev->col, "previous declaration is here");
        return;
    }
    ST_sym_t *sym = ST_sym_new(se, ST_SYM_VAR, name, NULL, t, line, col);
    sym->is_const = is_const;
    ST_sym_insert(se, &se->scope->table, sym);
}

static void ST_declare_local(ST_sema_t *se, ST_string_t name, ST_ty_t *t, u32 line, u32 col) {
    ST_declare_local_ex(se, name, t, line, col, 0);
}

// short-hand: a NULL type means "already reported".
static const char *ST_tstr(ST_sema_t *se, ST_ty_t *t) {
    return ST_ty_cstr(se->arena, t);
}

static ST_ty_t *ST_prim_by_name(ST_sema_t *se, ST_string_t name) {
    ST_forrange(
        0, ST_TYPE_COUNT) if (ST_string_eq_cstr(name, ST_type_names[i])) return se->tys.prim[i];
    return NULL;
}

static b8 ST_ty_is_bool(ST_ty_t *t) {
    return t && t->kind == ST_TY_BOOL;
}
static b8 ST_ty_is_ptr(ST_ty_t *t) {
    return t && t->kind == ST_TY_PTR;
}
static b8 ST_ty_is_layout(ST_ty_t *t) {
    return t && (t->kind == ST_TY_STRUCT || t->kind == ST_TY_TAG_UNION);
}

// untyped int -> i32, untyped float -> f32;
static ST_ty_t *ST_ty_defaulted(ST_sema_t *se, ST_ty_t *t) {
    if (!t)
        return NULL;
    if (t->kind == ST_TY_UNTYPED_INT)
        return se->tys.prim[ST_ti32];
    if (t->kind == ST_TY_UNTYPED_FLOAT)
        return se->tys.prim[ST_tf32];
    return t;
}

// NOTE(segfault): Can a value of type `from` be used where `to` is expected,
// without a cast? I am not to sure if this should be a real bug or not??
static b8 ST_ty_coerces(ST_sema_t *se, ST_ty_t *from, ST_ty_t *to) {
    ST_unused(se);
    if (!from || !to)
        return 1;
    if (from == to)
        return 1;
    if (to->kind == ST_TY_ANY)
        return 1;
    if (from->kind == ST_TY_UNTYPED_INT && to->kind == ST_TY_INT)
        return 1;
    if (from->kind == ST_TY_UNTYPED_FLOAT && to->kind == ST_TY_FLOAT)
        return 1;
    if (from->kind == ST_TY_PTR && to->kind == ST_TY_PTR &&
        (from->inner->kind == ST_TY_VOID || to->inner->kind == ST_TY_VOID))
        return 1;
    if (from->kind == ST_TY_ARRAY && to->kind == ST_TY_DYN_ARRAY && from->inner == to->inner)
        return 1;
    if (from->kind == ST_TY_FN && to->kind == ST_TY_FN)
        return ST_ty_equal(from, to);
    if (from->kind == ST_TY_PTR && to->kind == ST_TY_PTR && from->inner->kind == ST_TY_FN &&
        to->inner->kind == ST_TY_FN)
        return ST_ty_equal(from->inner, to->inner);
    return 0;
}

// Common type of two numeric operands, or NULL if they don't mix.
static ST_ty_t *ST_ty_num_unify(ST_sema_t *se, ST_ty_t *a, ST_ty_t *b) {
    if (!ST_ty_is_numeric(a) || !ST_ty_is_numeric(b))
        return NULL;
    if (a == b)
        return a;
    if (a->kind == ST_TY_UNTYPED_INT)
        return b;
    if (b->kind == ST_TY_UNTYPED_INT)
        return a;
    if (a->kind == ST_TY_UNTYPED_FLOAT && b->kind == ST_TY_FLOAT)
        return b;
    if (b->kind == ST_TY_UNTYPED_FLOAT && a->kind == ST_TY_FLOAT)
        return a;
    ST_unused(se);
    return NULL;
}

static ST_ty_t *ST_type_expr(ST_sema_t *se, ST_expr_t *e);
static b8 ST_ct_eval_expr(ST_sema_t *se, ST_expr_t *e, ST_ct_val_t *out);
static b8 ST_variant_has_payload(ST_sema_t *se, ST_variant_spec_t *v);
static ST_ty_t *ST_resolve_tyexpr(ST_sema_t *se, ST_tyexpr_t *te);
static void ST_complete_ty(ST_sema_t *se, ST_ty_t *t);
static void ST_build_fn_ty(ST_sema_t *se, ST_sym_t *sym, ST_fn_sig_t *sig);

b8 ST_const_eval(ST_sema_t *se, ST_expr_t *e, i64 *out);

static b8 ST_const_eval_bin(ST_sema_t *se, ST_expr_t *e, i64 *out) {
    i64 l, r;
    if (!ST_const_eval(se, e->bin.l, &l))
        return 0;
    if (!ST_const_eval(se, e->bin.r, &r))
        return 0;
    ST_string_t op = e->bin.op;
    if (ST_string_eq_cstr(op, "+")) {
        *out = l + r;
        return 1;
    }
    if (ST_string_eq_cstr(op, "-")) {
        *out = l - r;
        return 1;
    }
    if (ST_string_eq_cstr(op, "*")) {
        *out = l * r;
        return 1;
    }
    if (ST_string_eq_cstr(op, "/")) {
        if (!r)
            return 0;
        *out = l / r;
        return 1;
    }
    if (ST_string_eq_cstr(op, "%")) {
        if (!r)
            return 0;
        *out = l % r;
        return 1;
    }
    if (ST_string_eq_cstr(op, "<<")) {
        *out = (i64)((u64)l << (u64)r);
        return 1;
    }
    if (ST_string_eq_cstr(op, ">>")) {
        *out = l >> r;
        return 1;
    }
    if (ST_string_eq_cstr(op, "&")) {
        *out = l & r;
        return 1;
    }
    if (ST_string_eq_cstr(op, "|")) {
        *out = l | r;
        return 1;
    }
    if (ST_string_eq_cstr(op, "^")) {
        *out = l ^ r;
        return 1;
    }
    return 0;
}

static u32 ST_const_depth = 0;

b8 ST_const_eval(ST_sema_t *se, ST_expr_t *e, i64 *out) {
    if (!e || ST_const_depth > 128)
        return 0;
    switch (e->kind) {
        case ST_EX_INT:
        case ST_EX_CHAR:
        case ST_EX_BOOL:
            *out = e->ival;
            return 1;
        case ST_EX_IDENT: {
            ST_sym_t *sym = ST_sym_find(se, e->name);
            if (!sym || sym->kind != ST_SYM_CONST || !sym->decl)
                return 0;
            ST_const_depth++;
            b8 ok = ST_const_eval(se, sym->decl->const_.value, out);
            ST_const_depth--;
            return ok;
        }
        case ST_EX_UNARY: {
            i64 v;
            if (!ST_const_eval(se, e->unary.operand, &v))
                return 0;
            if (ST_string_eq_cstr(e->unary.op, "-")) {
                *out = -v;
                return 1;
            }
            if (ST_string_eq_cstr(e->unary.op, "~")) {
                *out = ~v;
                return 1;
            }
            if (ST_string_eq_cstr(e->unary.op, "!")) {
                *out = !v;
                return 1;
            }
            return 0;
        }
        case ST_EX_BINARY:
            return ST_const_eval_bin(se, e, out);
        case ST_EX_CAST:
            return ST_const_eval(se, e->cast.operand, out);
        case ST_EX_SIZEOF: {
            ST_ty_t *t = ST_resolve_tyexpr(se, e->tyop.te);
            if (!t)
                return 0;
            ST_complete_ty(se, t);
            *out = e->tyop.is_align ? (i64)t->align : (i64)t->size;
            return 1;
        }
        case ST_EX_FIELD: {
            ST_expr_t *base = e->field.base;
            if (!base || base->kind != ST_EX_IDENT)
                return 0;
            ST_sym_t *sym = ST_sym_find(se, base->name);
            if (!sym || sym->kind != ST_SYM_TYPE || !sym->decl)
                return 0;
            if (sym->decl->kind == ST_DE_ENUM) {
                ST_variant_specs_t *vs = &sym->decl->enum_.variants;
                ST_forrange(0, vs->count) {
                    if (!ST_string_eq(vs->items[i].name, e->field.name))
                        continue;
                    if (!vs->items[i].has_computed)
                        return 0; // not resolved yet (shouldn't happen post layout pass)
                    *out = vs->items[i].computed;
                    return 1;
                }
                return 0;
            }
            if (sym->decl->kind == ST_DE_TAG_UNION) {
                // bare 'TagUnionName.Variant' (no call) folds to its
                // declaration-order ordinal, comparable against '.kind'.
                ST_variant_specs_t *vs = &sym->decl->tag_union.variants;
                ST_forrange(0, vs->count) {
                    if (!ST_string_eq(vs->items[i].name, e->field.name))
                        continue;
                    *out = (i64)i;
                    return 1;
                }
                return 0;
            }
            return 0;
        }
        case ST_EX_FLOAT:
        case ST_EX_STR:
        case ST_EX_NULL:
        case ST_EX_CALL:
        case ST_EX_INDEX:
        case ST_EX_STRUCT_LIT:
        case ST_EX_ARRAY_NEW:
        case ST_EX_TYPEOF:
        case ST_EX_TYPEINFO:
        case ST_EX_KIND:
        case ST_EX_CSTR:
        case ST_EX_FIELDS:
        case ST_EX_COMP_ERROR:
        case ST_EX_ASM:
        case ST_EX_STR_FROM_RAW:
            return 0;
        case ST_EX_COUNT:
            ST_assert(0);
            break;
    }
    return 0;
}

typedef struct {
    ST_decl_t *tmpl;
    ST_tys_t args;
} ST_inst_info_t;

static ST_inst_info_t *ST_inst_info_find(ST_sema_t *se, ST_ty_t *t) {
    ST_ht_generic_t k = {
        .tag = &t, .size = sizeof(t)
    };
    return (ST_inst_info_t *)ST_ht_get(&se->inst_info, k).tag;
}

static void ST_inst_info_put(ST_sema_t *se, ST_ty_t *t, ST_decl_t *tmpl, ST_tys_t args) {
    ST_inst_info_t *info = ST_arena_push_zeroed(se->arena, sizeof(*info));
    info->tmpl = tmpl;
    info->args = args;
    ST_ty_t **pt = ST_arena_push_zeroed(se->arena, sizeof(*pt));
    *pt = t;
    ST_ht_generic_t *hk = ST_arena_push_zeroed(se->arena, sizeof(*hk));
    hk->tag = pt;
    hk->size = sizeof(*pt);
    ST_ht_set(&se->inst_info, hk, (ST_ht_generic_t){.tag = info, .size = 0});
}

static ST_ty_t *ST_instantiate_struct(ST_sema_t *se, ST_decl_t *tmpl, ST_tys_t args, u32 line,
                                      u32 col) {
    if (args.count != tmpl->struct_.generics.count) {
        ST_diag_error(&se->diag, line, col, "'" ST_sv_fmt "' expects %u type argument(s), got %u",
                      ST_sv_args(tmpl->name), tmpl->struct_.generics.count, args.count);
        return NULL;
    }
    ST_forrange(0, args.count) if (!args.items[i]) return NULL;
    ST_string_t mangled = ST_ty_mangle_instance_name(se->arena, tmpl->name, &args);
    ST_ht_generic_t key = {.tag = mangled.data, .size = mangled.len};
    ST_ht_generic_t existing = ST_ht_get(&se->instantiations, key);
    if (existing.tag)
        return (ST_ty_t *)existing.tag;

    ST_decl_t *id = ST_decl_new(se->arena, ST_DE_STRUCT, line, col);
    id->name = mangled;
    id->is_pub = tmpl->is_pub;
    id->struct_.packing = tmpl->struct_.packing;
    id->struct_.fields = tmpl->struct_.fields;
    {
        char buf[512];
        int n = snprintf(buf, sizeof(buf), "%.*s(", (int)tmpl->name.len, tmpl->name.data);
        ST_forrange(0, args.count) {
            if (n < (int)sizeof(buf))
                n += snprintf(buf + n, sizeof(buf) - (size_t)n, "%s%s", i ? ", " : "",
                              ST_ty_cstr(se->arena, args.items[i]));
        }
        if (n < (int)sizeof(buf))
            n += snprintf(buf + n, sizeof(buf) - (size_t)n, ")");
        if (n > (int)sizeof(buf))
            n = (int)sizeof(buf);
        u8 *copy = ST_arena_push(se->arena, (u32)n);
        memcpy(copy, buf, (size_t)n);
        id->display_name = (ST_string_t){.data = copy, .len = (u32)n};
    }

    ST_ty_t *t = ST_ty_for_decls(&se->tys, id);
    ST_ht_generic_t *hk = ST_arena_push(se->arena, sizeof(*hk));
    hk->tag = mangled.data;
    hk->size = mangled.len;
    ST_ht_set(&se->instantiations, hk, (ST_ht_generic_t){.tag = t, .size = 0});
    if (se->prog)
        ST_da_append_arena(se->arena, &se->prog->decls, id);

    ST_ht_t bindings;
    ST_ht_init(se->arena, &bindings, 8);
    ST_forrange(0, args.count) {
        ST_string_t *pname = tmpl->struct_.generics.items + i;
        ST_ht_generic_t *bk = ST_arena_push(se->arena, sizeof(*bk));
        bk->tag = pname->data;
        bk->size = pname->len;
        ST_ht_set(&bindings, bk, (ST_ht_generic_t){.tag = args.items[i], .size = 0});
    }

    ST_inst_info_put(se, t, tmpl, args);
    ST_ht_t *save = se->generic_bindings;
    b8 save_s = se->stamp_tyexprs;

    se->generic_bindings = &bindings;
    se->stamp_tyexprs = 0;

    ST_complete_ty(se, t);
    se->stamp_tyexprs = save_s;

    se->generic_bindings = save;

    return t;
}

static ST_ty_t *ST_resolve_tyexpr_raw(ST_sema_t *se, ST_tyexpr_t *te) {
    if (!te)
        return NULL;
    switch (te->kind) {
        case ST_TE_NAME: {
            if (te->is_generic_param) {
                if (se->generic_bindings) {
                    ST_ht_generic_t k = {.tag = te->name.data, .size = te->name.len};
                    ST_ht_generic_t found = ST_ht_get(se->generic_bindings, k);
                    if (found.tag)
                        return (ST_ty_t *)found.tag;
                    ST_diag_error(&se->diag, te->line, te->col,
                                  "generic parameter '$ " ST_sv_fmt
                                  "' used outside of instansiation",
                                  ST_sv_args(te->name));
                    return NULL;
                }
            }
            ST_ty_t *prim = ST_prim_by_name(se, te->name);
            if (prim)
                return prim;
            ST_sym_t *sym = ST_sym_find_in(&se->globals, te->name);
            if (!sym) {
                ST_diag_error(&se->diag, te->line, te->col, "unknown type '" ST_sv_fmt "'",
                              ST_sv_args(te->name));
                return NULL;
            }
            if (sym->kind != ST_SYM_TYPE) {
                ST_diag_error(&se->diag, te->line, te->col, "'" ST_sv_fmt "' is a %s, not a type",
                              ST_sv_args(te->name), ST_sym_kind_str(sym->kind));
                ST_diag_note(&se->diag, sym->line, sym->col, "'" ST_sv_fmt "' is declared here",
                             ST_sv_args(te->name));
                return NULL;
            }
            // A bare reference to a generic struct (no explicit '($T,$U)')
            // used somewhere that's currently being instantiated with its
            // own generic bindings active (e.g. a generic function param
            // 'f: *Foo' inside 'fn mod($T, $U, f: *Foo, ...)') implicitly
            // means 'Foo' instantiated with whichever of ITS OWN generic
            // parameter names are already bound in that scope -- so
            // 'f: *Foo' behaves exactly like 'f: *Foo($T, $U)' as long as
            // every one of Foo's generic names has a same-named binding
            // available. If any name isn't bound this way, fall through to
            // the plain (uninstantiated) skeleton type as before.
            if (se->generic_bindings && sym->decl->kind == ST_DE_STRUCT &&
                sym->decl->struct_.generics.count) {
                ST_tys_t args = {0};
                b8 all_bound = 1;
                ST_forrange(0, sym->decl->struct_.generics.count) {
                    ST_string_t g = sym->decl->struct_.generics.items[i];
                    ST_ht_generic_t k = {.tag = g.data, .size = g.len};
                    ST_ht_generic_t found = ST_ht_get(se->generic_bindings, k);
                    if (!found.tag) {
                        all_bound = 0;
                        break;
                    }
                    ST_da_append_arena(se->arena, &args, (ST_ty_t *)found.tag);
                }
                if (all_bound)
                    return ST_instantiate_struct(se, sym->decl, args, te->line, te->col);
            }
            return ST_ty_for_decls(&se->tys, sym->decl);
        }
        case ST_TE_PTR: {
            ST_ty_t *inner = ST_resolve_tyexpr(se, te->inner);
            if (!inner)
                return NULL;
            return ST_ty_ptr(&se->tys, inner);
        }
        case ST_TE_FN: {
            ST_ty_t *t = ST_ty_fn_new(&se->tys);
            t->is_variadic = te->fn_is_variadic;
            ST_forrange(0, te->fn_params.count) {
                ST_ty_t *pt = ST_resolve_tyexpr(se, te->fn_params.items[i]);
                ST_da_append_arena(se->arena, &t->params, pt);
            }
            ST_forrange(0, te->fn_rets.count) {
                ST_ty_t *rt = ST_resolve_tyexpr(se, te->fn_rets.items[i]);
                if (rt && rt->kind == ST_TY_VOID)
                    continue;
                ST_da_append_arena(se->arena, &t->rets, rt);
            }

            return t;
        }
        case ST_TE_TYPEOF:
            return ST_type_expr(se, te->typeof_operand);
        case ST_TE_GENERIC_INST: {
            ST_sym_t *tmpl = ST_sym_find_in(&se->templates, te->name);
            if (!tmpl) {
                if (ST_sym_find_in(&se->globals, te->name)) {
                    ST_diag_error(&se->diag, te->line, te->col,
                                  "'" ST_sv_fmt "' is not a generic type", ST_sv_args(te->name));
                } else
                    ST_diag_error(&se->diag, te->line, te->col, "unknown type '" ST_sv_fmt "' ",
                                  ST_sv_args(te->name));
                return NULL;
            }
            if (tmpl->decl->kind != ST_DE_STRUCT) {
                ST_diag_error(&se->diag, te->line, te->col,
                              "generic %s instaniation is not supported yet",
                              ST_sym_kind_str(tmpl->kind));
                return NULL;
            }
            ST_tys_t args = {0};
            b8 ok = 1;
            ST_forrange(0, te->generic_args.count) {
                ST_ty_t *at = ST_resolve_tyexpr(se, te->generic_args.items[i]);
                if (!at)
                    ok = 0;
                ST_da_append_arena(se->arena, &args, at);
            }
            if (!ok)
                return NULL;
            return ST_instantiate_struct(se, tmpl->decl, args, te->line, te->col);
        }
        case ST_TE_ARRAY: {
            ST_ty_t *inner = ST_resolve_tyexpr(se, te->inner);
            if (!inner)
                return NULL;
            if (te->is_dynamic)
                return ST_ty_dyn_array(&se->tys, inner);
            if (!te->count_expr) {
                ST_diag_error(&se->diag, te->line, te->col,
                              "array size can only be inferred on a declaration with "
                              "an array-literal initalizer ('x : []T = { ... }); "
                              "\ngive an explicit size '[N]T' here");
                return NULL;
            }
            ST_complete_ty(se, inner);
            i64 n = 0;
            if (!ST_const_eval(se, te->count_expr, &n)) {
                ST_diag_error(&se->diag, te->line, te->col,
                              "array size must be a constant integer expression");
                return NULL;
            }
            if (n < 0) {
                ST_diag_error(&se->diag, te->line, te->col, "array size cannot be negative (%ld)",
                              n);
                return NULL;
            }
            return ST_ty_array(&se->tys, inner, (u64)n);
        }
    }
    return NULL;
}

static ST_ty_t *ST_resolve_tyexpr(ST_sema_t *se, ST_tyexpr_t *te) {
    ST_ty_t *t = ST_resolve_tyexpr_raw(se, te);
    if (te && t && se->stamp_tyexprs)
        te->resolved = t;
    return t;
}

static ST_expr_t *ST_clone_expr(ST_arena_t *a, ST_expr_t *e);
static ST_stmt_t *ST_clone_stmt(ST_arena_t *a, ST_stmt_t *s);

static ST_tyexpr_t *ST_clone_tyexpr(ST_arena_t *a, ST_tyexpr_t *te) {
    if (!te)
        return NULL;
    ST_tyexpr_t *n = ST_tyexpr_new(a, te->kind, te->line, te->col);
    *n = *te;
    n->resolved = NULL;
    n->inner = ST_clone_tyexpr(a, te->inner);
    n->count_expr = ST_clone_expr(a, te->count_expr);
    n->typeof_operand = ST_clone_expr(a, te->typeof_operand);

    n->fn_params = (ST_tyexprs_t){0};
    ST_forrange(0, te->fn_params.count)
        ST_da_append_arena(a, &n->fn_params, ST_clone_tyexpr(a, te->fn_params.items[i]));

    n->fn_rets = (ST_tyexprs_t){0};
    ST_forrange(0, te->fn_rets.count)
        ST_da_append_arena(a, &n->fn_rets, ST_clone_tyexpr(a, te->fn_rets.items[i]));

    n->generic_args = (ST_tyexprs_t){0};
    ST_forrange(0, te->generic_args.count)
        ST_da_append_arena(a, &n->generic_args, ST_clone_tyexpr(a, te->generic_args.items[i]));

    return n;
}

static ST_expr_t *ST_clone_expr(ST_arena_t *a, ST_expr_t *e) {
    if (!e)
        return NULL;
    ST_expr_t *n = ST_expr_new(a, e->kind, e->line, e->col);
    *n = *e;
    n->ty = NULL;
    switch(e->kind) {
    case ST_EX_INT:
    case ST_EX_FLOAT:
    case ST_EX_STR:
    case ST_EX_CHAR:
    case ST_EX_BOOL:
    case ST_EX_NULL:
    case ST_EX_IDENT:
    case ST_EX_ASM:
        break;

    case ST_EX_STR_FROM_RAW:
        n->str_from_raw.ptr = ST_clone_expr(a, e->str_from_raw.ptr);
        n->str_from_raw.len = ST_clone_expr(a, e->str_from_raw.len);
        break;

    case ST_EX_UNARY:
        n->unary.operand = ST_clone_expr(a, e->unary.operand);
        break;

    case ST_EX_BINARY:
        n->bin.l = ST_clone_expr(a, e->bin.l);
        n->bin.r = ST_clone_expr(a, e->bin.r);
        break;

    case ST_EX_CALL:
        n->call.callee = ST_clone_expr(a, e->call.callee);
        n->call.args = (ST_args_t){0};
        ST_forrange(0, e->call.args.count) {
            ST_arg_t arg = e->call.args.items[i];
            arg.value = ST_clone_expr(a, arg.value);
            ST_da_append_arena(a, &n->call.args, arg);
        }
        break;

    case ST_EX_FIELD:
        n->field.base = ST_clone_expr(a, e->field.base);
        break;

    case ST_EX_INDEX:
        n->index.base = ST_clone_expr(a, e->index.base);
        n->index.index = ST_clone_expr(a, e->index.index);
        break;

    case ST_EX_CAST:
        n->cast.operand = ST_clone_expr(a, e->cast.operand);
        n->cast.to = ST_clone_tyexpr(a, e->cast.to);
        break;

    case ST_EX_STRUCT_LIT:
        n->struct_lit.generic_args = (ST_tyexprs_t){0};
        ST_forrange(0, e->struct_lit.generic_args.count)
            ST_da_append_arena(a, &n->struct_lit.generic_args,
                               ST_clone_tyexpr(a, e->struct_lit.generic_args.items[i]));

        n->struct_lit.inits = (ST_field_inits_t){0};
        ST_forrange(0, e->struct_lit.inits.count) {
            ST_field_init_t fi = e->struct_lit.inits.items[i];
            fi.value = ST_clone_expr(a, fi.value);
            ST_da_append_arena(a, &n->struct_lit.inits, fi);
        }
        break;
    case ST_EX_ARRAY_NEW:
        n->array_new.te = ST_clone_tyexpr(a, e->array_new.te);
        break;

    case ST_EX_SIZEOF:
    case ST_EX_TYPEOF:
    case ST_EX_TYPEINFO:
    case ST_EX_KIND:
    case ST_EX_CSTR:
    case ST_EX_FIELDS:
        n->tyop.te = ST_clone_tyexpr(a, e->tyop.te);
        n->tyop.operand = ST_clone_expr(a, e->tyop.operand);
        break;
    case ST_EX_COMP_ERROR:
        n->comp_error.args = (ST_exprs_t){0};
        ST_forrange(0, e->comp_error.args.count)
            ST_da_append_arena(a, &n->comp_error.args, ST_clone_expr(a, e->comp_error.args.items[i]));
        break;
    case ST_EX_COUNT:
        ST_assert(0);
        break;
    }

    return n;
}

static ST_stmts_t ST_clone_body(ST_arena_t *a, ST_stmts_t *body) {
    ST_stmts_t out = {0};
    ST_forrange(0, body->count)
        ST_da_append_arena(a, &out, ST_clone_stmt(a, body->items[i]));
    return out;
}

static ST_stmt_t *ST_clone_stmt(ST_arena_t *a, ST_stmt_t *s) {
    if (!s)
        return NULL;
    ST_stmt_t *n = ST_stmt_new(a, s->kind, s->line, s->col);
    *n = *s; // safe base copy (see ST_clone_expr/ST_clone_tyexpr); every
             // pointer/array field below is then deep-copied over it
    switch (s->kind) {

    case ST_ST_EXPR:
        n->expr = ST_clone_expr(a, s->expr);
        break;

    case ST_ST_DECL:
        n->decl.name = s->decl.name;
        n->decl.te = ST_clone_tyexpr(a, s->decl.te);
        n->decl.init = ST_clone_expr(a, s->decl.init);
        n->decl.is_static = s->decl.is_static;
        break;

    case ST_ST_ASSIGN:
        n->assign.lhs = ST_clone_expr(a, s->assign.lhs);
        n->assign.op = s->assign.op;
        n->assign.rhs = ST_clone_expr(a, s->assign.rhs);
        break;

    case ST_ST_MULTI_BIND:
        n->multi.n_names = s->multi.n_names;
        n->multi.names =
            s->multi.n_names ? ST_arena_push(a, sizeof(*n->multi.names) * s->multi.n_names) : NULL;
        ST_forrange(0, s->multi.n_names) n->multi.names[i] = s->multi.names[i];
        n->multi.declare = s->multi.declare;
        n->multi.values = (ST_exprs_t){0};
        ST_forrange(0, s->multi.values.count)
            ST_da_append_arena(a, &n->multi.values,
                               ST_clone_expr(a, s->multi.values.items[i]));
        break;

    case ST_ST_IF:
        n->if_.cond = ST_clone_expr(a, s->if_.cond);
        n->if_.then_body = ST_clone_body(a, &s->if_.then_body);
        n->if_.else_stmt = ST_clone_stmt(a, s->if_.else_stmt);
        break;

    case ST_ST_SWITCH:
        n->switch_.cond = ST_clone_expr(a, s->switch_.cond);
        n->switch_.cases = (ST_cases_t){0};
        ST_forrange(0, s->switch_.cases.count) {
            ST_case_t *c = &s->switch_.cases.items[i];
            ST_case_t nc = { .line = c->line, .col = c->col};
            for (u32 k = 0; k < c->values.count; k++)
            ST_da_append_arena(a, &nc.values,
                               ST_clone_expr(a, c->values.items[k]));
            nc.body = ST_clone_body(a, &c->body);
            ST_da_append_arena(a, &n->switch_.cases, nc);
        }
        break;

    case ST_ST_WHILE:
        n->while_.cond = ST_clone_expr(a, s->while_.cond);
        n->while_.body = ST_clone_body(a, &s->while_.body);
        break;

    case ST_ST_FOR_RANGE:
        n->for_range.iter = s->for_range.iter;
        n->for_range.lo = ST_clone_expr(a, s->for_range.lo);
        n->for_range.hi = ST_clone_expr(a, s->for_range.hi);
        n->for_range.iter_te = ST_clone_tyexpr(a, s->for_range.iter_te);
        n->for_range.inclusive = s->for_range.inclusive;
        n->for_range.body = ST_clone_body(a, &s->for_range.body);
        break;

    case ST_ST_FOR_ARRAY:
        n->for_array.iter = s->for_array.iter;
        n->for_array.target = ST_clone_expr(a, s->for_array.target);
        n->for_array.body = ST_clone_body(a, &s->for_array.body);
        break;

    case ST_ST_RETURN:
        n->ret.values = (ST_exprs_t){0};
        ST_forrange(0, s->ret.values.count)
            ST_da_append_arena(a, &n->ret.values,
                               ST_clone_expr(a, s->ret.values.items[i]));
        break;

    case ST_ST_BLOCK:
        n->block = ST_clone_body(a, &s->block);
        break;

    case ST_ST_DEFER:
        n->defer_stmt = ST_clone_stmt(a, s->defer_stmt);
        break;

    case ST_ST_ASM:
        n->asm_.n_tokens = s->asm_.n_tokens;
        n->asm_.tokens =
            s->asm_.n_tokens ? ST_arena_push(a, sizeof(*n->asm_.tokens) * s->asm_.n_tokens) : NULL;
        ST_forrange(0, s->asm_.n_tokens) n->asm_.tokens[i] = s->asm_.tokens[i];
        break;

    case ST_ST_LABEL:
    case ST_ST_GODOWN:
        n->label = s->label;
        break;

    case ST_ST_BREAK:
    case ST_ST_CONTINUE:
        break;

    case ST_ST_COUNT:
        ST_assert(0);
        break;

    }

    return n;
}

static ST_fn_sig_t ST_clone_fn_sig(ST_arena_t *a, ST_fn_sig_t *s) {
    ST_fn_sig_t out = {0};
    out.has_ret_ann = s->has_ret_ann;
    out.is_variadic = s->is_variadic;
    ST_forrange(0, s->params.count) {
        ST_param_t p = s->params.items[i];
        p.te = ST_clone_tyexpr(a, p.te);
        p.def = ST_clone_expr(a, p.def);
        ST_da_append_arena(a, &out.params, p);
    }

    ST_forrange(0, s->rets.count)
        ST_da_append_arena(a, &out.rets, ST_clone_tyexpr(a, s->rets.items[i]));
    return out;
}

static b8 ST_unify_tyexpr(ST_sema_t *se, ST_tyexpr_t *pt, ST_ty_t *at, ST_ht_t *bindings,
                          u32 arg_line, u32 arg_col) {
    if (!pt || !at)
        return 1;

    switch(pt->kind) {
    case ST_TE_NAME: {
        if (!pt->is_generic_param)
            return 1;
        ST_ty_t *want = ST_ty_defaulted(se, at);
        ST_ht_generic_t k = { .tag = pt->name.data, .size = pt->name.len };
        ST_ht_generic_t found = ST_ht_get(bindings, k);
        ST_ty_t *bound = (ST_ty_t *)found.tag;
        if (bound) {
            if (bound == want || ST_ty_equal(bound, want))
                return 1;
            ST_diag_error(&se->diag, arg_line, arg_col,
                          "generic parameter '$" ST_sv_fmt "' was already inferred as '%s' "
                          "from an earlier argument, but this one has type '%s'",
                          ST_sv_args(pt->name), ST_tstr(se, bound), ST_tstr(se, want));
            return 0;
        }

        ST_ht_generic_t *bk = ST_arena_push(se->arena, sizeof(*bk));
        bk->tag = pt->name.data;
        bk->size = pt->name.len;
        ST_ht_set(bindings, bk, (ST_ht_generic_t){.tag = want, .size = 0});
        return 1;
    }

    case ST_TE_PTR: {
        if (at->kind != ST_TY_PTR)
            return 1;
        return ST_unify_tyexpr(se, pt->inner, at->inner, bindings, arg_line, arg_col);
    }

    case ST_TE_ARRAY: {
        if (pt->is_dynamic)
            return at->kind == ST_TY_DYN_ARRAY
                       ? ST_unify_tyexpr(se, pt->inner, at->inner, bindings, arg_line, arg_col)
                       : 1;
        return at->kind == ST_TY_ARRAY
                   ? ST_unify_tyexpr(se, pt->inner, at->inner, bindings, arg_line, arg_col)
                   : 1;
    }

    case ST_TE_FN: {
        if (at->kind == ST_TY_PTR && at->inner && at->inner->kind == ST_TY_FN)
            at = at->inner;
        if (at->kind != ST_TY_FN)
            return 1;
        ST_forrange(0, pt->fn_params.count) {
            if (i >= at->params.count)
                break;
            if (!ST_unify_tyexpr(se, pt->fn_params.items[i], at->params.items[i], bindings,
                                 arg_line, arg_col))
                return 0;
        }
        return 1;
    }

    case ST_TE_GENERIC_INST: {
        if (at->kind != ST_TY_STRUCT)
            return 1;
        ST_inst_info_t *info = ST_inst_info_find(se, at);
        if (!info)
            return 1;

        ST_sym_t *tsym = ST_sym_find_in(&se->templates, pt->name);
        if (!tsym || tsym->decl != info->tmpl)
            return 1;

        if (pt->generic_args.count != info->args.count)
            return 1;

        ST_forrange(0, pt->generic_args.count)
            if (!ST_unify_tyexpr(se, pt->generic_args.items[i], info->args.items[i], bindings,
                                 arg_line, arg_col))
                return 0;
        return 1;
    }

    case ST_TE_TYPEOF:
        return 1;
    }

    return 1;
}

// Builds a clone of 'elem_te' (the pack param's declared element type, e.g.
// '$T') with its generic-param name suffixed '#k'. This is the one place
// that decides a pack element's *type* binding key; both the call-site
// unify loop (in ST_type_call) and ST_instantiate_fn_ex's param expansion
// call this, and MUST stay in lockstep -- if they ever compute different
// strings, a bound type silently fails to be found and you get a bogus
// "could not infer generic parameter" error pointing at the wrong thing.
static ST_tyexpr_t *ST_pack_elem_te(ST_sema_t *se, ST_tyexpr_t *elem_te, u32 k) {
    if (elem_te->kind != ST_TE_NAME || !elem_te->is_generic_param)
        return NULL; // nested pack element types ('*$T...') aren't wired up yet
    ST_tyexpr_t *indexed = ST_tyexpr_new(se->arena, elem_te->kind, elem_te->line, elem_te->col);
    *indexed = *elem_te;
    char buf[64];
    int n = snprintf(buf, sizeof(buf), ST_sv_fmt "#%u", ST_sv_args(elem_te->name), k);
    u8 *nb = ST_arena_push(se->arena, (u32)n);
    memcpy(nb, buf, (u32)n);
    indexed->name = (ST_string_t){.data = nb, .len = (u32)n};
    return indexed;
}

// Same idea, but for the synthetic *parameter's* name (a different
// namespace from the type-binding key above -- 'args: $T...' has param
// name 'args' and generic name 'T', which need not match). 'args#0' etc.
// is also the name a future 'args[0]' indexed-access sugar would need to
// resolve to deterministically, without a side table -- see the note on
// ST_type_index in the design writeup.
static ST_string_t ST_pack_param_name(ST_sema_t *se, ST_string_t base, u32 k) {
    char buf[64];
    int n = snprintf(buf, sizeof(buf), ST_sv_fmt "#%u", ST_sv_args(base), k);
    u8 *nb = ST_arena_push(se->arena, (u32)n);
    memcpy(nb, buf, (u32)n);
    return (ST_string_t){.data = nb, .len = (u32)n};
}

static ST_sym_t *ST_instantiate_fn_ex(ST_sema_t *se, ST_decl_t *tmpl, ST_ht_t *bindings,
                                      b8 has_pack, u32 n_pack_args, b8 has_bound_str,
                                      ST_string_t bound_str_param, ST_string_t bound_str_value,
                                      u32 line, u32 col) {
    i32 pack_idx = has_pack ? (i32)tmpl->fn.sig.params.count - 1 : -1;
    ST_tyexpr_t *pack_elem_te = pack_idx >= 0 ? tmpl->fn.sig.params.items[pack_idx].te : NULL;

    ST_tys_t args = {0};
    ST_forrange(0, tmpl->fn.sig.generics.count) {
        ST_string_t g = tmpl->fn.sig.generics.items[i];
        // If 'g' is the pack's own generic name, it was never bound under
        // its bare name (the call-site loop binds "T#0", "T#1", ... --
        // see ST_pack_elem_te) -- walk all n_pack_args indexed keys
        // instead. Each bound type gets appended to 'args', so the mangled
        // name naturally differs both by pack element types *and* by pack
        // length (more elements -> more appended types -> different
        // mangled string), with no separate length-encoding needed.
        b8 is_pack_generic = pack_elem_te && pack_elem_te->kind == ST_TE_NAME &&
                             pack_elem_te->is_generic_param &&
                             ST_string_eq(pack_elem_te->name, g);
        if (is_pack_generic) {
            for (u32 j = 0; j < n_pack_args; j++) {
                char buf[64];
                int n = snprintf(buf, sizeof(buf), ST_sv_fmt "#%u", ST_sv_args(g), j);
                ST_ht_generic_t k = {.tag = buf, .size = (u32)n};
                ST_ty_t *bound = (ST_ty_t *)ST_ht_get(bindings, k).tag;
                if (!bound) {
                    ST_diag_error(&se->diag, line, col,
                                  "could not infer pack element %u of '$" ST_sv_fmt
                                  "...' for call to '" ST_sv_fmt "'",
                                  j, ST_sv_args(g), ST_sv_args(tmpl->name));
                    return NULL;
                }
                ST_da_append_arena(se->arena, &args, bound);
            }
            continue;
        }
        ST_ht_generic_t k = { .tag = g.data, .size = g.len };
        ST_ty_t *bound = (ST_ty_t *)ST_ht_get(bindings, k).tag;
        if (!bound) {
            ST_diag_error(&se->diag, line, col,
                          "could not infer generic parameter '$"ST_sv_fmt
                          "' for call to '"ST_sv_fmt"'", ST_sv_args(g),
                          ST_sv_args(tmpl->name));
            ST_diag_note(&se->diag, tmpl->line, tmpl->col, "'" ST_sv_fmt"' is declared here",
                         ST_sv_args(tmpl->name));
            return NULL;
        }
        ST_da_append_arena(se->arena, &args, bound);
    }

    ST_string_t mangled = ST_ty_mangle_instance_name(se->arena, tmpl->name, &args);
    if (has_pack && args.count == 0) {
        char buf[600];
        int n = snprintf(buf, sizeof(buf), ST_sv_fmt "$0pack", ST_sv_args(mangled));
        u8 *nb = ST_arena_push(se->arena, (u32)n);
        memcpy(nb, buf, (u32)n);
        mangled = (ST_string_t){.data = nb, .len = (u32)n};
    }
    if (has_bound_str) {
        // Fold the bound value's content into the mangled name via a plain
        // hash (not the raw text) so the mangled name stays identifier-safe
        // regardless of what characters the string contains.
        u64 h = 1469598103934665603ULL;
        ST_forrange(0, bound_str_value.len) {
            h ^= (u64)bound_str_value.data[i];
            h *= 1099511628211ULL;
        }
        char buf[700];
        int n = snprintf(buf, sizeof(buf), ST_sv_fmt "$s%llx", ST_sv_args(mangled),
                         (unsigned long long)h);
        u8 *nb = ST_arena_push(se->arena, (u32)n);
        memcpy(nb, buf, (u32)n);
        mangled = (ST_string_t){.data = nb, .len = (u32)n};
    }
    ST_ht_generic_t key = { .tag = mangled.data, .size = mangled.len };
    ST_sym_t *existing = ST_ht_get(&se->fn_instantiations, key).tag;
    if (existing)
        return existing;

    if (se->n_fn_instances >= ST_MAX_FN_INSTANCE) {
        ST_diag_error(&se->diag, line, col,
                      "too many generic function instantiations (limit %u); "
                      "likely runaway recursive instantiation of '"ST_sv_fmt"'",
                      ST_MAX_FN_INSTANCE, ST_sv_args(tmpl->name));
        return NULL;
    }
    se->n_fn_instances++;
    ST_decl_t *inst = ST_decl_new(se->arena, ST_DE_FN, tmpl->line, tmpl->col);
    inst->name = mangled;
    inst->is_pub = tmpl->is_pub;
    inst->fn.sig = ST_clone_fn_sig(se->arena, &tmpl->fn.sig);
    inst->fn.sig.generics = (ST_strings_t){0}; // fully resolved now -- see ST_lower_program's
                                                // 'sig.generics.count -> skip, still a template'
                                                // check; leaving the cloned list in place meant
                                                // every instantiation was silently never lowered
    inst->fn.is_prototype = tmpl->fn.is_prototype;
    inst->fn.body = ST_clone_body(se->arena, &tmpl->fn.body);
    inst->fn.has_bound_str = has_bound_str;
    inst->fn.bound_str_param = bound_str_param;
    inst->fn.bound_str_value = bound_str_value;

    if (pack_idx >= 0) {
        // Replace the template's single pack slot with 'n_pack_args' real
        // params, each independently typed.
        ST_param_t pack_param = inst->fn.sig.params.items[pack_idx];
        inst->fn.sig.params.count = (u32)pack_idx; // drop the pack slot itself
        for (u32 j = 0; j < n_pack_args; j++) {
            ST_param_t p = {0};
            p.name = ST_pack_param_name(se, pack_param.name, j);
            p.line = pack_param.line;
            p.col = pack_param.col;
            p.te = ST_pack_elem_te(se, pack_param.te, j);
            ST_da_append_arena(se->arena, &inst->fn.sig.params, p);
        }
        inst->fn.had_pack = 1;
        inst->fn.pack_name = pack_param.name;
        inst->fn.pack_count = n_pack_args;
    }

    ST_ht_t *pb = ST_arena_push_zeroed(se->arena, sizeof(*pb));
    *pb = *bindings;

    ST_sym_t *sym = ST_sym_new(se, ST_SYM_FN, mangled, inst, NULL, tmpl->line, tmpl->col);
    sym->generic_bindings = pb;
    sym->template_name = tmpl->name;

    ST_ht_generic_t *hk = ST_arena_push(se->arena, sizeof(*hk));
    hk->tag = mangled.data;
    hk->size = mangled.len;

    ST_ht_set(&se->fn_instantiations, hk, (ST_ht_generic_t){ .tag = sym, .size = 0});
    ST_sym_insert(se, &se->globals, sym);

    ST_ht_t *save_b = se->generic_bindings;
    b8 save_s = se->stamp_tyexprs;
    se->generic_bindings = pb;
    se->stamp_tyexprs = 1;

    ST_build_fn_ty(se, sym, &inst->fn.sig);
    se->stamp_tyexprs = save_s;
    se->generic_bindings = save_b;

    if (se->prog)
        ST_da_append_arena(se->arena, &se->prog->decls, inst);

    return sym;
}

static u32 ST_align_up(u32 x, u32 a) {
    if (a < 2)
        return x;
    return (x + a - 1) & ~(a - 1);
}

static void ST_complete_struct(ST_sema_t *se, ST_ty_t *t) {
    ST_decl_t *d = t->decl;
    b8 packed = d->struct_.packing == ST_PACK_PACKED;
    u32 off = 0, align = 1;

    ST_forrange(0, d->struct_.fields.count) {
        ST_field_spec_t *f = &d->struct_.fields.items[i];
        ST_ty_t *ft = NULL;
        if (f->te)
            ft = ST_resolve_tyexpr(se, f->te);
        else if (f->anon)
            ft = ST_ty_for_decls(&se->tys, f->anon);
        if (!ft)
            continue;

        ST_complete_ty(se, ft); // no-op unless struct/tag_union by value
        u32 a = packed ? 1 : (ft->align ? ft->align : 1);
        off = ST_align_up(off, a);

        ST_ty_field_t field = {.name = f->name, .ty = ft, .offset = off};
        ST_da_append_arena(se->arena, &t->fields, field);

        off += ft->size;
        if (a > align)
            align = a;
    }
    t->align = align;
    t->size = ST_align_up(off, align);
}

static void ST_complete_tag_union(ST_sema_t *se, ST_ty_t *t) {
    ST_decl_t *d = t->decl;
    u32 max = 0;
    ST_forrange(0, d->tag_union.variants.count) {
        ST_variant_spec_t *v = &d->tag_union.variants.items[i];
        if (!v->payload)
            continue;
        ST_ty_t *pt = ST_resolve_tyexpr(se, v->payload);
        if (!pt)
            continue;
        ST_complete_ty(se, pt);
        if (pt->size > max)
            max = pt->size;
    }
    t->align = 8;
    t->size = 8 + ST_align_up(max, 8);
}

static void ST_complete_ty(ST_sema_t *se, ST_ty_t *t) {
    if (!ST_ty_is_layout(t))
        return;
    if (t->state == ST_TY_STATE_DONE)
        return;
    if (t->state == ST_TY_STATE_COMPUTING) {
        ST_diag_error(&se->diag, t->decl->line, t->decl->col,
                      "recursive type '" ST_sv_fmt "' has infinite size",
                      ST_sv_args(ST_decl_display_name(t->decl)));
        ST_diag_note(&se->diag, t->decl->line, t->decl->col,
                     "break the cycle with a pointer, e.g. '*" ST_sv_fmt "'",
                     ST_sv_args(ST_decl_display_name(t->decl)));
        t->state = ST_TY_STATE_DONE;
        return;
    }
    t->state = ST_TY_STATE_COMPUTING;
    if (t->kind == ST_TY_STRUCT)
        ST_complete_struct(se, t);
    else
        ST_complete_tag_union(se, t);
    if (t->state != ST_TY_STATE_DONE)
        t->state = ST_TY_STATE_DONE;
}

// Constants without an explicit type keep their untyped type so `N :: 10`
// still coerces anywhere. `N : type : 10` pins the type up front and the
// value must coerce to it (checked once, here).
static ST_ty_t *ST_ty_of_const(ST_sema_t *se, ST_sym_t *sym) {
    if (sym->t)
        return sym->t;
    if (!sym->decl || sym->decl->kind != ST_DE_CONST)
        return NULL;
    if (ST_const_depth > 128) {
        ST_diag_error(&se->diag, sym->line, sym->col,
                      "constant '" ST_sv_fmt "' is defined in terms of itself",
                      ST_sv_args(sym->name));
        return NULL;
    }
    ST_const_depth++;
    ST_ty_t *vt = ST_type_expr(se, sym->decl->const_.value);
    if (sym->decl->const_.te) {
        ST_ty_t *at = ST_resolve_tyexpr(se, sym->decl->const_.te);
        if (at && vt && !ST_ty_coerces(se, vt, at))
            ST_diag_error(&se->diag, sym->decl->const_.value->line, sym->decl->const_.value->col,
                          "constant '" ST_sv_fmt "' is declared as '%s', but its value is '%s'",
                          ST_sv_args(sym->name), ST_tstr(se, at), ST_tstr(se, vt));
        sym->t = at ? at : vt;
    } else {
        sym->t = vt;
    }
    ST_const_depth--;
    return sym->t;
}

static ST_ty_t *ST_type_ident(ST_sema_t *se, ST_expr_t *e) {
    ST_sym_t *sym = ST_sym_find(se, e->name);
    if (!sym) {
        ST_diag_error(&se->diag, e->line, e->col, "use of undeclared identifier '" ST_sv_fmt "'",
                      ST_sv_args(e->name));
        return NULL;
    }
    switch (sym->kind) {
        case ST_SYM_VAR:
        case ST_SYM_EXTERN_VAR:
        case ST_SYM_GLOBAL:
        case ST_SYM_FN:
            return sym->t;
        case ST_SYM_CONST:
            return ST_ty_of_const(se, sym);
        case ST_SYM_TYPE:
            ST_diag_error(&se->diag, e->line, e->col,
                          "'" ST_sv_fmt "' is a type, it cannot be used as a value",
                          ST_sv_args(e->name));
            ST_diag_note(&se->diag, sym->line, sym->col, "'" ST_sv_fmt "' is declared here",
                         ST_sv_args(e->name));
            return NULL;
        case ST_SYM_MODULE:
            ST_diag_error(&se->diag, e->line, e->col,
                          "'" ST_sv_fmt "' is a module, access members with '" ST_sv_fmt ".member'",
                          ST_sv_args(e->name), ST_sv_args(e->name));
            return NULL;
    }
    return NULL;
}

// Determines whether 'e' denotes a location whose address can be taken with
// '&'. Named '::' constants are folded into their use sites everywhere else,
// but taking their address is allowed: the compiler lazily materializes a
// read-only backing symbol for a constant the moment its address is taken
// (see ST_lower_addr_of), so '&N' behaves like a pointer to read-only
// storage holding N's value. Enum/enum_flag variant access (Type.Variant) is
// NOT covered by this — a variant has no identity of its own to point at, so
// it stays non-addressable. Only variables (locals, params, globals, extern
// vars), functions, and now named constants are addressable.
static b8 ST_expr_is_addressable(ST_sema_t *se, ST_expr_t *e) {
    switch (e->kind) {
        case ST_EX_IDENT: {
            ST_sym_t *sym = ST_sym_find(se, e->name);
            if (!sym)
                return 0;
            switch (sym->kind) {
                case ST_SYM_VAR:
                case ST_SYM_EXTERN_VAR:
                case ST_SYM_GLOBAL:
                case ST_SYM_FN:
                case ST_SYM_CONST:
                    return 1;
                case ST_SYM_TYPE:
                case ST_SYM_MODULE:
                    return 0;
            }
            return 0;
        }
        case ST_EX_UNARY:
            return ST_string_eq_cstr(e->unary.op, "*"); // *p is addressable
        case ST_EX_FIELD:
            // Type.Variant (enum/enum_flag) is a folded constant, not a
            // location; everything else reaching here is a struct field
            // access, which is addressable regardless of its base.
            if (e->field.base && e->field.base->kind == ST_EX_IDENT) {
                ST_sym_t *bs = ST_sym_find(se, e->field.base->name);
                if (bs && bs->kind == ST_SYM_TYPE)
                    return 0;
            }
            return 1;
        case ST_EX_INDEX:
            return 1;
        default:
            return 0;
    }
}

static ST_ty_t *ST_type_unary(ST_sema_t *se, ST_expr_t *e) {
    ST_ty_t *t = ST_type_expr(se, e->unary.operand);
    if (!t)
        return NULL;
    ST_string_t op = e->unary.op;

    if (ST_string_eq_cstr(op, "-")) {
        if (!ST_ty_is_numeric(t)) {
            ST_diag_error(&se->diag, e->line, e->col, "unary '-' needs a numeric operand, got '%s'",
                          ST_tstr(se, t));
            return NULL;
        }
        return t;
    }
    if (ST_string_eq_cstr(op, "!")) {
        if (!ST_ty_is_bool(t)) {
            ST_diag_error(&se->diag, e->line, e->col, "'!' needs a 'bool' operand, got '%s'",
                          ST_tstr(se, t));
            return NULL;
        }
        return t;
    }
    if (ST_string_eq_cstr(op, "~")) {
        if (!ST_ty_is_int(t)) {
            ST_diag_error(&se->diag, e->line, e->col, "'~' needs an integer operand, got '%s'",
                          ST_tstr(se, t));
            return NULL;
        }
        return t;
    }
    if (ST_string_eq_cstr(op, "*")) {
        if (!ST_ty_is_ptr(t) || t->inner->kind == ST_TY_VOID) {
            ST_diag_error(&se->diag, e->line, e->col, "cannot dereference a value of type '%s'",
                          ST_tstr(se, t));
            return NULL;
        }
        return t->inner;
    }
    if (ST_string_eq_cstr(op, "&")) {
        if (!ST_expr_is_addressable(se, e->unary.operand)) {
            ST_expr_t *v = e->unary.operand;
            if (v->kind == ST_EX_FIELD) {
                ST_diag_error(&se->diag, e->line, e->col,
                              "cannot take the address of an enum variant: it is a "
                              "compile-time constant, not a variable");
                return NULL;
            }
            ST_diag_error(&se->diag, e->line, e->col,
                          "cannot take the address of this expression");
            return NULL;
        }
        return ST_ty_ptr(&se->tys, ST_ty_defaulted(se, t));
    }
    return t;
}

static b8 ST_op_is(ST_string_t op, const char *a, const char *b) {
    return ST_string_eq_cstr(op, a) || (b && ST_string_eq_cstr(op, b));
}

static ST_ty_t *ST_type_binary(ST_sema_t *se, ST_expr_t *e) {
    ST_ty_t *l = ST_type_expr(se, e->bin.l);
    ST_ty_t *r = ST_type_expr(se, e->bin.r);
    if (!l || !r)
        return NULL;
    ST_string_t op = e->bin.op;
    ST_ty_t *bool_ty = se->tys.prim[ST_tbool];

    // logical
    if (ST_op_is(op, "&&", "||")) {
        if (!ST_ty_is_bool(l) || !ST_ty_is_bool(r)) {
            ST_diag_error(&se->diag, e->line, e->col,
                          "'" ST_sv_fmt "' needs 'bool' operands, got '%s' and '%s'",
                          ST_sv_args(op), ST_tstr(se, l), ST_tstr(se, r));
            return NULL;
        }
        return bool_ty;
    }

    // equality: anything that coerces one way or the other
    if (ST_op_is(op, "==", "!=")) {
        if (!ST_ty_coerces(se, l, r) && !ST_ty_coerces(se, r, l) && !ST_ty_num_unify(se, l, r)) {
            ST_diag_error(&se->diag, e->line, e->col, "cannot compare '%s' with '%s'",
                          ST_tstr(se, l), ST_tstr(se, r));
            return NULL;
        }
        return bool_ty;
    }

    // ordering: numeric or char
    if (ST_op_is(op, "<", "<=") || ST_op_is(op, ">", ">=")) {
        if (l->kind == ST_TY_CHAR && r->kind == ST_TY_CHAR)
            return bool_ty;
        if (!ST_ty_num_unify(se, l, r)) {
            ST_diag_error(&se->diag, e->line, e->col,
                          "cannot order '%s' and '%s' with '" ST_sv_fmt "'", ST_tstr(se, l),
                          ST_tstr(se, r), ST_sv_args(op));
            return NULL;
        }
        return bool_ty;
    }

    // bitwise and shifts: integers only
    if (ST_op_is(op, "&", "|") || ST_op_is(op, "^", NULL) || ST_op_is(op, "<<", ">>")) {
        if (!ST_ty_is_int(l) || !ST_ty_is_int(r)) {
            ST_diag_error(&se->diag, e->line, e->col,
                          "'" ST_sv_fmt "' needs integer operands, got '%s' and '%s'",
                          ST_sv_args(op), ST_tstr(se, l), ST_tstr(se, r));
            return NULL;
        }
        if (ST_op_is(op, "<<", ">>"))
            return l->kind == ST_TY_UNTYPED_INT ? r : l;
        ST_ty_t *u = ST_ty_num_unify(se, l, r);
        if (!u) {
            ST_diag_error(&se->diag, e->line, e->col,
                          "mismatched integer types '%s' and '%s' for '" ST_sv_fmt "'",
                          ST_tstr(se, l), ST_tstr(se, r), ST_sv_args(op));
            return NULL;
        }
        return u;
    }

    // arithmetic
    if (ST_string_eq_cstr(op, "%") && (!ST_ty_is_int(l) || !ST_ty_is_int(r))) {
        ST_diag_error(&se->diag, e->line, e->col, "'%%' needs integer operands, got '%s' and '%s'",
                      ST_tstr(se, l), ST_tstr(se, r));
        return NULL;
    }
    ST_ty_t *u = ST_ty_num_unify(se, l, r);
    if (!u) {
        ST_diag_error(&se->diag, e->line, e->col,
                      "invalid operands to '" ST_sv_fmt "': '%s' and '%s'", ST_sv_args(op),
                      ST_tstr(se, l), ST_tstr(se, r));
        return NULL;
    }
    return u;
}

static void ST_arg_extern_decay(ST_sema_t *se, ST_sym_t *sym, ST_ty_t *pt, ST_arg_t *arg) {
    if (!sym || !sym->decl || sym->decl->kind != ST_DE_EXTERN_FN)
        return;
    if (!arg->value->ty || arg->value->ty->kind != ST_TY_STRING)
        return;
    if (pt &&
        (pt->kind != ST_TY_PTR || (pt->inner->kind != ST_TY_CHAR && pt->inner->kind != ST_TY_VOID)))
        return;

    ST_expr_t *cs = ST_expr_new(se->arena, ST_EX_CSTR, arg->value->line, arg->value->col);
    cs->tyop.operand = arg->value;
    cs->ty = ST_ty_ptr(&se->tys, se->tys.prim[ST_tchar]);
    arg->value = cs;
}

static ST_tys_t *ST_type_call(ST_sema_t *se, ST_expr_t *e) {
    ST_expr_t *callee = e->call.callee;
    ST_sym_t *sym = NULL;
    ST_ty_t *fnty = NULL;

    ST_decl_t *fn_tmpl = NULL;

    if (callee && callee->kind == ST_EX_IDENT) {
        sym = ST_sym_find(se, callee->name);
        if (!sym) {
            ST_sym_t *tsym = ST_sym_find_in(&se->templates, callee->name);
            if (tsym && tsym->decl && tsym->decl->kind == ST_DE_FN)
                fn_tmpl = tsym->decl;
            else if (tsym)
                ST_diag_error(&se->diag, callee->line, callee->col,
                              "'" ST_sv_fmt "' is a generic type, it cannot be called",
                              ST_sv_args(callee->name));
            else
                ST_diag_error(&se->diag, callee->line, callee->col,
                              "call to undeclared function '" ST_sv_fmt"'",
                              ST_sv_args(callee->name));

        } else if (sym->kind != ST_SYM_FN && sym->kind != ST_SYM_VAR) {
            ST_diag_error(&se->diag, callee->line, callee->col,
                          "'" ST_sv_fmt "' is a %s, it cannot be called", ST_sv_args(callee->name),
                          ST_sym_kind_str(sym->kind));
            ST_diag_note(&se->diag, sym->line, sym->col, "'" ST_sv_fmt "' is declared here",
                         ST_sv_args(callee->name));
            sym = NULL;
        } else {
            fnty = sym->t;
            if (fnty && fnty->kind == ST_TY_PTR && fnty->inner && fnty->inner->kind == ST_TY_FN)
                fnty = fnty->inner;
            if (sym->kind == ST_SYM_VAR && fnty && fnty->kind != ST_TY_FN) {
                ST_diag_error(&se->diag, callee->line, callee->col,
                              "cannot call a value of type '%s'", ST_tstr(se, fnty));
                fnty = NULL;
                sym = NULL;
            }
        }
    } else if (callee) {
        fnty = ST_type_expr(se, callee);
        if (fnty && fnty->kind == ST_TY_PTR && fnty->inner && fnty->inner->kind == ST_TY_FN)
            fnty = fnty->inner;
        if (fnty && fnty->kind != ST_TY_FN) {
            ST_diag_error(&se->diag, callee->line, callee->col, "cannot call a value of type '%s'",
                          ST_tstr(se, fnty));
            fnty = NULL;
        }
    }

    // type all argument expressions
    ST_forrange(0, e->call.args.count) ST_type_expr(se, e->call.args.items[i].value);

    if (fn_tmpl) {
        ST_ht_t bindings;
        ST_ht_init(se->arena, &bindings, 8);
        ST_fn_sig_t *tsig = &fn_tmpl->fn.sig;

        // A pack, if the template has one, is always the last param.
        i32 pack_idx = -1;
        if (tsig->has_generic_pack)
            pack_idx = (i32)tsig->params.count - 1;

        b8 has_bound_str = 0;
        ST_string_t bound_str_param = {0}, bound_str_value = {0};

        u32 pos = 0;
        u32 n_pack_args = 0;
        b8 unify_ok = 1;
        ST_forrange(0, e->call.args.count) {
            ST_arg_t *arg = &e->call.args.items[i];
            u32 idx = tsig->params.count;
            if (arg->name.len) {
                for (u32 k = 0; k < tsig->params.count; k++)
                    if (ST_string_eq(tsig->params.items[k].name, arg->name)) {
                        idx = k;
                        break;
                    }
                if (pack_idx >= 0 && (i32)idx >= pack_idx) {
                    ST_diag_error(&se->diag, arg->value->line, arg->value->col,
                                  "cannot pass a named argument into '" ST_sv_fmt "...' -- pack "
                                  "elements are always positional",
                                  ST_sv_args(tsig->params.items[pack_idx].name));
                    unify_ok = 0;
                    continue;
                }
            } else
                idx = pos++;

            if (pack_idx >= 0 && (i32)idx >= pack_idx) {
                // 'idx' would run off the end of tsig->params (it only has
                // one slot for the whole pack) -- reroute every positional
                // arg from here on into its own indexed binding key instead.
                u32 k = idx - (u32)pack_idx;
                n_pack_args = k + 1;
                ST_tyexpr_t *elem_te = tsig->params.items[pack_idx].te;
                ST_tyexpr_t *indexed = ST_pack_elem_te(se, elem_te, k);
                if (!indexed) {
                    ST_diag_error(&se->diag, arg->value->line, arg->value->col,
                                  "only a bare '$T...' pack element type is supported right now "
                                  "(not '*$T...' or similar)");
                    unify_ok = 0;
                    continue;
                }
                if (arg->value->ty)
                    if (!ST_unify_tyexpr(se, indexed, arg->value->ty, &bindings, arg->value->line,
                                         arg->value->col))
                        unify_ok = 0;
                continue;
            }

            if (idx < tsig->params.count && tsig->params.items[idx].te && arg->value->ty)
                if (!ST_unify_tyexpr(se, tsig->params.items[idx].te, arg->value->ty, &bindings,
                                     arg->value->line, arg->value->col))
                    unify_ok = 0;

            // A plain (non-generic) 'string' param whose call-site argument
            // happens to be a compile-time-constant string: bind its value
            // too, not just its type. Only the first such param is tracked.
            if (!has_bound_str && idx < tsig->params.count && tsig->params.items[idx].te &&
                !tsig->params.items[idx].te->is_generic_param) {
                ST_ty_t *pty = ST_resolve_tyexpr(se, tsig->params.items[idx].te);
                if (pty && pty->kind == ST_TY_STRING) {
                    ST_ct_val_t val;
                    if (ST_ct_eval_expr(se, arg->value, &val) && val.kind == ST_CT_STRING) {
                        has_bound_str = 1;
                        bound_str_param = tsig->params.items[idx].name;
                        u8 *buf = ST_arena_push(se->arena, val.str.len);
                        memcpy(buf, val.str.data, val.str.len);
                        bound_str_value = (ST_string_t){.data = buf, .len = val.str.len};
                    }
                }
            }
        }
        if (!unify_ok)
            return NULL;

        sym = ST_instantiate_fn_ex(se, fn_tmpl, &bindings, pack_idx >= 0, n_pack_args,
                                   has_bound_str, bound_str_param, bound_str_value, e->line,
                                   e->col);
        if (!sym)
            return NULL;

        callee->name = sym->name;
        fnty = sym->t;
    }

    // builtins: no signature yet, everything goes
    if (sym && sym->kind == ST_SYM_FN && !sym->decl)
        return &ST_builtin_rets;

    // declaration-based checks: defaults and named arguments
    ST_fn_sig_t *sig = NULL;
    if (sym && sym->kind == ST_SYM_FN && sym->decl)
        sig = sym->decl->kind == ST_DE_FN ? &sym->decl->fn.sig : &sym->decl->extern_fn.sig;

    if (!fnty)
        return NULL;

    b8 has_named = 0;
    ST_forrange(0, e->call.args.count) if (e->call.args.items[i].name.len) has_named = 1;

    if (sig && has_named) {
        u32 pos = 0;
        ST_forrange(0, e->call.args.count) {
            ST_arg_t *arg = &e->call.args.items[i];
            u32 idx = sig->params.count;
            if (arg->name.len) {
                for (u32 k = 0; k < sig->params.count; k++)
                    if (ST_string_eq(sig->params.items[k].name, arg->name)) {
                        idx = k;
                        break;
                    }
                if (idx == sig->params.count) {
                    ST_diag_error(&se->diag, arg->value->line, arg->value->col,
                                  "'" ST_sv_fmt "' has no parameter named '" ST_sv_fmt "'",
                                  ST_sv_args(ST_sym_display_name(sym)), ST_sv_args(arg->name));
                    continue;
                }
            } else
                idx = pos++;
            if (idx >= fnty->params.count)
                continue;
            ST_ty_t *pt = fnty->params.items[idx];
            ST_arg_extern_decay(se, sym, pt, arg);
            ST_ty_t *at = arg->value->ty;
            if (pt && at && !ST_ty_coerces(se, at, pt))
                ST_diag_error(&se->diag, arg->value->line, arg->value->col,
                              "argument '" ST_sv_fmt "' expects '%s', got '%s'",
                              ST_sv_args(sig->params.items[idx].name), ST_tstr(se, pt),
                              ST_tstr(se, at));
        }
        return &fnty->rets;
    }

    // positional: count checks
    u32 n = e->call.args.count;
    u32 max_p = fnty->params.count;
    u32 min_args = max_p;
    if (sig) {
        min_args = 0;
        ST_forrange(0, sig->params.count)
            if (!sig->params.items[i].def && !sig->params.items[i].is_pack) min_args++;
    }

    if (n < min_args || (!fnty->is_variadic && !fnty->has_any_pack && n > max_p)) {
        u32 line = sym ? sym->line : (callee ? callee->line : e->line);
        u32 col = sym ? sym->col : (callee ? callee->col : e->col);
        ST_string_t name = sym ? ST_sym_display_name(sym) : ST_cstr_to_str("function");
        if (fnty->is_variadic || fnty->has_any_pack)
            ST_diag_error(&se->diag, e->line, e->col,
                          "'" ST_sv_fmt "' expects at least %u argument%s, got %u",
                          ST_sv_args(name), min_args, min_args == 1 ? "" : "s", n);
        else if (min_args == max_p)
            ST_diag_error(&se->diag, e->line, e->col,
                          "'" ST_sv_fmt "' expects %u argument%s, got %u", ST_sv_args(name), max_p,
                          max_p == 1 ? "" : "s", n);
        else
            ST_diag_error(&se->diag, e->line, e->col,
                          "'" ST_sv_fmt "' expects %u to %u arguments, got %u", ST_sv_args(name),
                          min_args, max_p, n);
        if (sym)
            ST_diag_note(&se->diag, line, col, "'" ST_sv_fmt "' is declared here",
                         ST_sv_args(name));
    }

    // positional type checks
    ST_forrange(0, n) {
        if (i >= max_p)
            break;
        ST_arg_t *arg = &e->call.args.items[i];
        ST_ty_t *pt = fnty->params.items[i];
        ST_arg_extern_decay(se, sym, pt, arg);
        ST_ty_t *at = arg->value->ty;
        if (pt && at && !ST_ty_coerces(se, at, pt)) {
            ST_diag_error(&se->diag, arg->value->line, arg->value->col,
                          "argument %u expects '%s', got '%s'", i + 1, ST_tstr(se, pt),
                          ST_tstr(se, at));
            if (sym)
                ST_diag_note(&se->diag, sym->line, sym->col, "'" ST_sv_fmt "' is declared here",
                             ST_sv_args(ST_sym_display_name(sym)));
        }
    }
    if (fnty->is_variadic) {
        ST_forrange(max_p, n) {
            ST_arg_t *arg = &e->call.args.items[i];
            ST_arg_extern_decay(se, sym, NULL, arg);
            if (arg->value->ty && arg->value->ty->kind == ST_TY_UNTYPED_INT)
                arg->value->ty = se->tys.prim[ST_ti32];
            else if (arg->value->ty && arg->value->ty->kind == ST_TY_UNTYPED_FLOAT)
                arg->value->ty = se->tys.prim[ST_tf32];
        }
    }
    // Args past the pack slot itself (max_p already covers arg #max_p-1,
    // the first pack element, via the normal positional loop above since
    // its declared type is 'any' and everything coerces to 'any') --
    // these just need untyped literals resolved to a concrete size before
    // they get boxed into an 'any' at the call site. No decay: unlike the
    // raw C-ABI variadic path, the actual static type has to survive into
    // codegen so the boxed 'any' carries correct RTTI, not be flattened to
    // a bare pointer.
    if (fnty->has_any_pack) {
        ST_forrange(max_p, n) {
            ST_arg_t *arg = &e->call.args.items[i];
            if (arg->value->ty && arg->value->ty->kind == ST_TY_UNTYPED_INT)
                arg->value->ty = se->tys.prim[ST_ti32];
            else if (arg->value->ty && arg->value->ty->kind == ST_TY_UNTYPED_FLOAT)
                arg->value->ty = se->tys.prim[ST_tf32];
        }
    }
    return &fnty->rets;
}

static ST_ty_t *ST_type_field(ST_sema_t *se, ST_expr_t *e) {
    ST_expr_t *base = e->field.base;

    // Type.Member: enum variants fold to a value of the enum type;
    // tag_union variants named bare (no call) fold to their ordinal as an
    // integer, so they can be compared against '.kind'. Constructing an
    // actual tag_union value is 'Type.Member(payload)' -- see ST_type_call.
    if (base && base->kind == ST_EX_IDENT) {
        ST_sym_t *sym = ST_sym_find(se, base->name);
        if (sym && sym->kind == ST_SYM_TYPE && sym->decl) {
            ST_ty_t *t = ST_ty_for_decls(&se->tys, sym->decl);
            ST_variant_specs_t *vs = NULL;
            b8 is_union = sym->decl->kind == ST_DE_TAG_UNION;
            if (sym->decl->kind == ST_DE_ENUM)
                vs = &sym->decl->enum_.variants;
            else if (is_union)
                vs = &sym->decl->tag_union.variants;
            if (!vs) {
                ST_diag_error(&se->diag, e->line, e->col,
                              "'" ST_sv_fmt "' is a struct, use a struct literal "
                              "to build one",
                              ST_sv_args(base->name));
                return NULL;
            }
            ST_forrange(0, vs->count) if (ST_string_eq(vs->items[i].name, e->field.name))
                return is_union ? se->tys.prim[ST_ti64] : t;
            ST_diag_error(&se->diag, e->line, e->col,
                          "'" ST_sv_fmt "' has no variant '" ST_sv_fmt "'", ST_sv_args(base->name),
                          ST_sv_args(e->field.name));
            ST_diag_note(&se->diag, sym->line, sym->col, "'" ST_sv_fmt "' is declared here",
                         ST_sv_args(base->name));
            return NULL;
        }
    }

    ST_ty_t *t = ST_type_expr(se, base);
    if (!t)
        return NULL;
    if (t->kind == ST_TY_PTR)
        t = t->inner; // one auto-deref, like a.b on *A

    if (t->kind == ST_TY_STRUCT) {
        ST_complete_ty(se, t);
        ST_forrange(0, t->fields.count) if (ST_string_eq(t->fields.items[i].name,
                                                         e->field.name)) return t->fields.items[i]
            .ty;
        ST_diag_error(&se->diag, e->line, e->col, "'%s' has no field '" ST_sv_fmt "'",
                      ST_tstr(se, t), ST_sv_args(e->field.name));
        if (t->decl)
            ST_diag_note(&se->diag, t->decl->line, t->decl->col, "'" ST_sv_fmt "' is declared here",
                         ST_sv_args(ST_decl_display_name(t->decl)));
        return NULL;
    }

    // 'u.kind' reads the active variant's ordinal (comparable against bare
    // 'TagUnionName.Variant'); 'u.VariantName' reinterprets the payload
    // region as that variant's payload type -- an unchecked reinterpret,
    // same spirit as a C union, since checking it would need real pattern
    // matching this language doesn't have yet.
    if (t->kind == ST_TY_TAG_UNION) {
        ST_complete_ty(se, t);
        if (ST_string_eq_cstr(e->field.name, "kind"))
            return se->tys.prim[ST_ti64];
        ST_variant_specs_t *vs = &t->decl->tag_union.variants;
        ST_forrange(0, vs->count) if (ST_string_eq(vs->items[i].name, e->field.name)) {
            if (!ST_variant_has_payload(se, &vs->items[i])) {
                ST_diag_error(&se->diag, e->line, e->col,
                              "'" ST_sv_fmt "' has no payload; compare '.kind' against '"
                              ST_sv_fmt "." ST_sv_fmt "' instead",
                              ST_sv_args(e->field.name), ST_sv_args(ST_decl_display_name(t->decl)),
                              ST_sv_args(e->field.name));
                return NULL;
            }
            return ST_resolve_tyexpr(se, vs->items[i].payload);
        }
        ST_diag_error(&se->diag, e->line, e->col, "'%s' has no variant '" ST_sv_fmt "'",
                      ST_tstr(se, t), ST_sv_args(e->field.name));
        ST_diag_note(&se->diag, t->decl->line, t->decl->col, "'" ST_sv_fmt "' is declared here",
                     ST_sv_args(ST_decl_display_name(t->decl)));
        return NULL;
    }

    // built-in members on arrays and strings
    if (t->kind == ST_TY_ARRAY || t->kind == ST_TY_DYN_ARRAY || t->kind == ST_TY_STRING) {
        if (ST_string_eq_cstr(e->field.name, "len"))
            return se->tys.prim[ST_ti32];
        if (ST_string_eq_cstr(e->field.name, "ptr"))
            return ST_ty_ptr(&se->tys, t->kind == ST_TY_STRING ? se->tys.prim[ST_tchar] : t->inner);
    }

    ST_diag_error(&se->diag, e->line, e->col, "'%s' has no field '" ST_sv_fmt "'", ST_tstr(se, t),
                  ST_sv_args(e->field.name));
    return NULL;
}

static ST_ty_t *ST_type_index(ST_sema_t *se, ST_expr_t *e) {
    ST_ty_t *bt = ST_type_expr(se, e->index.base);
    ST_ty_t *it = ST_type_expr(se, e->index.index);
    if (it && !ST_ty_is_int(it))
        ST_diag_error(&se->diag, e->index.index->line, e->index.index->col,
                      "array index must be an integer, got '%s'", ST_tstr(se, it));
    if (!bt)
        return NULL;
    if (bt->kind == ST_TY_ARRAY) {
        i64 idx = 0;
        if (ST_const_eval(se, e->index.index, &idx) && ((idx < 0) || (u64)idx >= bt->count)) {
            ST_diag_error(&se->diag, e->line, e->col,
                          "array index '%lld' is out of bound for array of length %llu",
                          (long long)idx, (unsigned long long)bt->count);
            return bt->inner;
        }
        return bt->inner;
    }
    if (bt->kind == ST_TY_STRING)
        return se->tys.prim[ST_tchar];
    if (bt->kind == ST_TY_DYN_ARRAY)
        return bt->inner;
    if (bt->kind == ST_TY_PTR && bt->inner->kind != ST_TY_VOID)
        return bt->inner;
    ST_diag_error(&se->diag, e->line, e->col, "cannot index a value of type '%s'", ST_tstr(se, bt));
    return NULL;
}

static ST_ty_t *ST_type_cast(ST_sema_t *se, ST_expr_t *e) {
    ST_ty_t *from = ST_type_expr(se, e->cast.operand);
    ST_ty_t *to = ST_resolve_tyexpr(se, e->cast.to);
    if (!from || !to)
        return to;
    if (ST_ty_coerces(se, from, to))
        return to;

    b8 from_num = ST_ty_is_numeric(from) || from->kind == ST_TY_CHAR || from->kind == ST_TY_ENUM ||
                  from->kind == ST_TY_BOOL;
    b8 to_num = ST_ty_is_numeric(to) || to->kind == ST_TY_CHAR || to->kind == ST_TY_ENUM;
    if (from_num && to_num)
        return to;
    if (from->kind == ST_TY_PTR && to->kind == ST_TY_PTR)
        return to;
    if (from->kind == ST_TY_PTR && ST_ty_is_int(to))
        return to;
    if (ST_ty_is_int(from) && to->kind == ST_TY_PTR)
        return to;
    if (from->kind == ST_TY_ANY)
        return to;

    ST_diag_error(&se->diag, e->line, e->col, "cannot cast '%s' to '%s'", ST_tstr(se, from),
                  ST_tstr(se, to));
    return to;
}

// Infers $-generic bindings for 'td' (a generic struct template) from a
// literal's field-init values, unifying each init against the matching
// field's declared (possibly generic) type, then instantiates 'td' with the
// inferred args. Shared by both the named ('Foo{ .. }') and unnamed
// ('x : Foo = { .. }') struct-literal forms so both get the same inference.
static ST_ty_t *ST_infer_struct_lit(ST_sema_t *se, ST_expr_t *e, ST_decl_t *td) {
    ST_ht_t bindings;
    ST_ht_init(se->arena, &bindings, 8);

    ST_forrange(0, e->struct_lit.inits.count) {
        ST_field_init_t *fi = &e->struct_lit.inits.items[i];
        ST_ty_t *vt = ST_type_expr(se, fi->value);
        if (!vt)
            continue;
        ST_field_spec_t *fs = NULL;
        if (fi->name.len) {
            for (u32 k = 0; k < td->struct_.fields.count; ++k)
                if (ST_string_eq(td->struct_.fields.items[i].name, fi->name)) {
                    fs = &td->struct_.fields.items[i];
                    break;
                }
        } else if (i < td->struct_.fields.count)
            fs = &td->struct_.fields.items[i];
        if (fs && fs->te)
            ST_unify_tyexpr(se, fs->te, vt, &bindings, fi->value->line, fi->value->col);
    }

    ST_tys_t args = {0};
    ST_forrange(0, td->struct_.generics.count) {
        ST_string_t g = td->struct_.generics.items[i];
        ST_ht_generic_t k = {.tag = g.data, .size = g.len};
        ST_ty_t *bound = (ST_ty_t *)ST_ht_get(&bindings, k).tag;
        if (!bound) {
            ST_diag_error(&se->diag, e->line, e->col,
                          "could not infer generic parameter '$" ST_sv_fmt
                          "' for struct literal '" ST_sv_fmt "'; spell it out: " ST_sv_fmt
                          "(..){ .. }",
                          ST_sv_args(g), ST_sv_args(td->name), ST_sv_args(td->name));
            ST_diag_note(&se->diag, td->line, td->col, "'" ST_sv_fmt "' is declared here",
                         ST_sv_args(td->name));
            return NULL;
        }
        ST_da_append_arena(se->arena, &args, bound);
    }
    return ST_instantiate_struct(se, td, args, e->line, e->col);
}

static ST_ty_t *ST_type_struct_lit(ST_sema_t *se, ST_expr_t *e, ST_ty_t *expect);

// Validates and types a tag_union literal 'Foo { variant, payload }' /
// 'Foo { variant }'. The first (unnamed) init must be a bare identifier
// naming one of 'ut's variants -- not a general expression, since there's
// nothing named that at the value level, it's purely a selector. The second
// init, if the variant takes a payload, is its value (checked against the
// variant's payload type, which can be anything, including another struct
// literal for a nested-struct payload).
// A variant declared 'name : void;' has no real value to carry -- 'void'
// isn't a type you can hold, it's the "nothing" annotation, same spirit as
// a function returning void. Treat it identically to a bare 'name;' variant
// everywhere: no payload required or accepted at construction, and no
// payload readable via '.name'.
static b8 ST_variant_has_payload(ST_sema_t *se, ST_variant_spec_t *v) {
    if (!v->payload)
        return 0;
    ST_ty_t *pt = ST_resolve_tyexpr(se, v->payload);
    return pt && pt->kind != ST_TY_VOID;
}

static ST_ty_t *ST_type_union_lit(ST_sema_t *se, ST_expr_t *e, ST_ty_t *ut) {
    ST_complete_ty(se, ut);
    u32 n = e->struct_lit.inits.count;
    ST_string_t uname = ST_decl_display_name(ut->decl);
    if (n == 0 || n > 2) {
        ST_diag_error(&se->diag, e->line, e->col,
                      "'" ST_sv_fmt "' literal needs a variant name and, if that variant "
                      "has a payload, its value: '" ST_sv_fmt "{ variant, value }'",
                      ST_sv_args(uname), ST_sv_args(uname));
        return NULL;
    }
    ST_field_init_t *tag_fi = &e->struct_lit.inits.items[0];
    if (tag_fi->name.len || tag_fi->value->kind != ST_EX_IDENT) {
        ST_diag_error(&se->diag, tag_fi->value->line, tag_fi->value->col,
                      "expected a variant name here, e.g. '" ST_sv_fmt "{ SomeVariant, .. }'",
                      ST_sv_args(uname));
        return NULL;
    }
    ST_string_t vname = tag_fi->value->name;
    ST_variant_spec_t *variant = NULL;
    ST_forrange(0, ut->decl->tag_union.variants.count)
        if (ST_string_eq(ut->decl->tag_union.variants.items[i].name, vname)) {
            variant = &ut->decl->tag_union.variants.items[i];
            break;
        }
    if (!variant) {
        ST_diag_error(&se->diag, tag_fi->value->line, tag_fi->value->col,
                      "'" ST_sv_fmt "' has no variant '" ST_sv_fmt "'", ST_sv_args(uname),
                      ST_sv_args(vname));
        ST_diag_note(&se->diag, ut->decl->line, ut->decl->col, "'" ST_sv_fmt "' is declared here",
                     ST_sv_args(uname));
        return NULL;
    }
    b8 has_payload = ST_variant_has_payload(se, variant);
    if (!has_payload && n == 2) {
        ST_diag_error(&se->diag, e->line, e->col, "'" ST_sv_fmt "' takes no payload",
                      ST_sv_args(vname));
        return NULL;
    }
    if (has_payload && n != 2) {
        ST_diag_error(&se->diag, e->line, e->col,
                      "'" ST_sv_fmt "' needs a payload value: '" ST_sv_fmt "{ " ST_sv_fmt ", .. }'",
                      ST_sv_args(vname), ST_sv_args(uname), ST_sv_args(vname));
        return NULL;
    }
    if (n == 2) {
        ST_ty_t *pt = ST_resolve_tyexpr(se, variant->payload);
        ST_field_init_t *pfi = &e->struct_lit.inits.items[1];
        ST_ty_t *vt;
        if (pfi->value->kind == ST_EX_STRUCT_LIT && !pfi->value->struct_lit.type_name.len && pt)
            vt = ST_type_struct_lit(se, pfi->value, pt);
        else
            vt = ST_type_expr(se, pfi->value);
        if (pt && vt && !ST_ty_coerces(se, vt, pt))
            ST_diag_error(&se->diag, pfi->value->line, pfi->value->col,
                          "'" ST_sv_fmt "' expects a payload of '%s', got '%s'",
                          ST_sv_args(vname), ST_tstr(se, pt), ST_tstr(se, vt));
    }
    e->ty = ut;
    return ut;
}

static ST_ty_t *ST_type_struct_lit(ST_sema_t *se, ST_expr_t *e, ST_ty_t *expect) {
    ST_ty_t *t = NULL;
    ST_sym_t *lit_tmpl = e->struct_lit.type_name.len
        ? ST_sym_find_in(&se->templates, e->struct_lit.type_name)
        : NULL;

    b8 is_struct_lit = lit_tmpl && lit_tmpl->decl && lit_tmpl->decl->kind == ST_DE_STRUCT;

    if (e->struct_lit.type_name.len && e->struct_lit.generic_args.count) {
        if (!is_struct_lit)
            ST_diag_error(&se->diag, e->line, e->col, "'" ST_sv_fmt " ' is not a generic type",
                          ST_sv_args(e->struct_lit.type_name));
        else {
            ST_tys_t args = {0};
            b8 ok = 1;
            ST_forrange(0, e->struct_lit.generic_args.count) {
                ST_ty_t *at = ST_resolve_tyexpr(se, e->struct_lit.generic_args.items[i]);
                if (!at)
                    ok = 0;
                ST_da_append_arena(se->arena, &args, at);
            }
            if (ok)
                t = ST_instantiate_struct(se, lit_tmpl->decl, args, e->line, e->col);
        }

    } else if (is_struct_lit) {
        // Named literal against a generic template: 'Foo{ 20, "x" }'.
        t = ST_infer_struct_lit(se, e, lit_tmpl->decl);
    } else if (e->struct_lit.type_name.len) {
        ST_sym_t *sym = ST_sym_find_in(&se->globals, e->struct_lit.type_name);
        if (!sym)
            ST_diag_error(&se->diag, e->line, e->col,
                          "unknown type '" ST_sv_fmt "' in struct literal",
                          ST_sv_args(e->struct_lit.type_name));
        else if (sym->kind != ST_SYM_TYPE ||
                 (sym->decl->kind != ST_DE_STRUCT && sym->decl->kind != ST_DE_TAG_UNION)) {
            ST_diag_error(&se->diag, e->line, e->col, "'" ST_sv_fmt "' is not a struct type",
                          ST_sv_args(e->struct_lit.type_name));
            ST_diag_note(&se->diag, sym->line, sym->col, "'" ST_sv_fmt "' is declared here",
                         ST_sv_args(e->struct_lit.type_name));
        } else if (sym->decl->kind == ST_DE_TAG_UNION)
            return ST_type_union_lit(se, e, ST_ty_for_decls(&se->tys, sym->decl));
        else
            t = ST_ty_for_decls(&se->tys, sym->decl);
    } else if (expect && expect->kind == ST_TY_TAG_UNION) {
        // Unnamed literal against a declared tag_union type: 'f : Foo = { x, 33 };'.
        return ST_type_union_lit(se, e, expect);
    } else if (expect && expect->kind == ST_TY_STRUCT && expect->decl &&
               expect->decl->struct_.generics.count) {
        // Unnamed literal against a declared generic type: 'x : Foo = { .. }'.
        // 'expect' itself is still the uninstantiated template's skeleton
        // type here (never laid out, since generic structs' layout is only
        // completed on instantiation) -- infer args the same way the named
        // form does, from the literal's own values, rather than trying to
        // complete 'expect' directly.
        t = ST_infer_struct_lit(se, e, expect->decl);
    } else if (expect && (expect->kind == ST_TY_STRUCT || expect->kind == ST_TY_ARRAY))
        t = expect;
    else
        ST_diag_error(&se->diag, e->line, e->col,
                      "cannot infer the struct literal's type here, "
                      "name it: 'Type{ .. }'");

    if (t)
        ST_complete_ty(se, t);
    if (t && t->kind == ST_TY_ARRAY) {
        ST_ty_t *ety = t->inner;
        if (e->struct_lit.inits.count > t->count) {
            ST_diag_error(&se->diag, e->line, e->col,
                          "too many initalizers for an array of %llu elements%s",
                          (unsigned long long)t->count, t->count == 1 ? "" : "s");
        }
        ST_forrange(0, e->struct_lit.inits.count) {
            ST_field_init_t *fi = &e->struct_lit.inits.items[i];
            if (fi->name.len) {
                ST_diag_error(&se->diag, e->line, e->col,
                              "array literal do not take named initalizers.", "'(" ST_sv_fmt "')",
                              ST_sv_args(fi->name));
                continue;
            }
            ST_ty_t *vt;
            if (fi->value->kind == ST_EX_STRUCT_LIT && !fi->value->struct_lit.type_name.len) {
                vt = ST_type_struct_lit(se, fi->value, ety);
                fi->value->ty = vt;
            } else {
                vt = ST_type_expr(se, fi->value);
            }
            if (i >= t->count || !vt)
                continue;
            if (!ST_ty_coerces(se, vt, ety)) {
                ST_diag_error(&se->diag, fi->value->line, fi->value->col,
                              "array element expects '%s', got '%s'.", ST_tstr(se, ety),
                              ST_tstr(se, vt));
                if (vt && vt->kind == ST_TY_UNTYPED_INT && ST_ty_is_float(ety))
                    ST_diag_note(&se->diag, fi->value->line, fi->value->col,
                                 "whole-number float literals need an explicit '.0', "
                                 "e.g. '%lld.0'",
                                 (long long)fi->value->ival);
            } else if (vt != ety) {
                // The literal coerces (e.g. an untyped int/float literal
                // into a narrower/wider or differently-kinded destination
                // element type) -- retype it to the real destination type
                // now, rather than leaving its own natural (untyped_int /
                // untyped_float) type in place. Left alone, ST_default_expr
                // would default it independently of 'ety' (e.g. untyped_int
                // -> i32, untyped_float -> f64) and lowering would build the
                // constant with that mismatched width/register-class,
                // corrupting whatever a later float/int load reinterprets
                // those spilled bits as.
                //
                // An int literal landing in a float slot needs its VALUE
                // converted too, not just its type label -- otherwise
                // lowering still emits an integer constant (ST_EX_INT ->
                // ST_ir_const_int) now mislabeled with a float type, and the
                // generic per-instruction spill picks the float path
                // (movsd from xmm0) for an instruction that only ever wrote
                // to rax, spilling whatever was last left in xmm0.
                if (fi->value->kind == ST_EX_INT && ST_ty_is_float(ety)) {
                    fi->value->kind = ST_EX_FLOAT;
                    fi->value->fval = (f64)fi->value->ival;
                }
                fi->value->ty = ety;
            }
        }
        return t;
    }

    ST_forrange(0, e->struct_lit.inits.count) {
        ST_field_init_t *fi = &e->struct_lit.inits.items[i];

        ST_ty_t *ft = NULL;
        ST_string_t fname = fi->name;
        if (t && fi->name.len) {
            for (u32 k = 0; k < t->fields.count; k++)
                if (ST_string_eq(t->fields.items[k].name, fi->name)) {
                    ft = t->fields.items[k].ty;
                    break;
                }
            if (!ft) {
                ST_diag_error(&se->diag, fi->line, fi->col,
                              "struct '" ST_sv_fmt "' has no field '" ST_sv_fmt "'",
                              ST_sv_args(ST_decl_display_name(t->decl)), ST_sv_args(fi->name));
                ST_diag_note(&se->diag, t->decl->line, t->decl->col,
                             "'" ST_sv_fmt "' is declared here",
                             ST_sv_args(ST_decl_display_name(t->decl)));
                ST_type_expr(se, fi->value); // still walk it so e.g. undeclared names get caught
                continue;
            }
        } else if (t && !fi->name.len) {
            if (i >= t->fields.count) {
                ST_diag_error(&se->diag, fi->line, fi->col,
                              "too many initializers for struct '" ST_sv_fmt "', it has %u field%s",
                              ST_sv_args(ST_decl_display_name(t->decl)), t->fields.count,
                              t->fields.count == 1 ? "" : "s");
                ST_diag_note(&se->diag, t->decl->line, t->decl->col,
                             "'" ST_sv_fmt "' is declared here",
                             ST_sv_args(ST_decl_display_name(t->decl)));
                break;
            }
            ft = t->fields.items[i].ty;
            fname = t->fields.items[i].name;
        }

        ST_ty_t *vt;
        if (fi->value->kind == ST_EX_STRUCT_LIT && !fi->value->struct_lit.type_name.len && ft) {
            vt = ST_type_struct_lit(se, fi->value, ft);
            fi->value->ty = vt;
        } else {
            vt = ST_type_expr(se, fi->value);
        }
        if (!t)
            continue;

        if (ft && vt && !ST_ty_coerces(se, vt, ft)) {
            ST_diag_error(&se->diag, fi->value->line, fi->value->col,
                          "field '" ST_sv_fmt "' expects '%s', got '%s'", ST_sv_args(fname),
                          ST_tstr(se, ft), ST_tstr(se, vt));
            if (vt->kind == ST_TY_UNTYPED_INT && ST_ty_is_float(ft))
                ST_diag_note(&se->diag, fi->value->line, fi->value->col,
                             "whole-number float literals need an explicit '.0', e.g. '%lld.0'",
                             (long long)fi->value->ival);
        } else if (ft && vt && vt != ft) {
            // Same reasoning as the array-element branch above: retype a
            // coercing scalar literal to the field's real declared type so
            // it doesn't get defaulted (and lowered) at its own natural,
            // possibly mismatched-width/kind type instead. An int literal
            // into a float field needs its VALUE converted too -- see the
            // longer comment in the array-element branch.
            if (fi->value->kind == ST_EX_INT && ST_ty_is_float(ft)) {
                fi->value->kind = ST_EX_FLOAT;
                fi->value->fval = (f64)fi->value->ival;
            }
            fi->value->ty = ft;
        }
    }
    return t;
}

static ST_ty_t *ST_type_expr(ST_sema_t *se, ST_expr_t *e) {
    if (!e)
        return NULL;
    ST_ty_t *t = NULL;
    switch (e->kind) {
        case ST_EX_INT:
            t = se->tys.untyped_int;
            break;
        case ST_EX_FLOAT:
            t = se->tys.untyped_float;
            break;
        case ST_EX_STR:
            t = se->tys.prim[ST_tstring];
            break;
        case ST_EX_CHAR:
            t = se->tys.prim[ST_tchar];
            break;
        case ST_EX_BOOL:
            t = se->tys.prim[ST_tbool];
            break;
        case ST_EX_NULL:
            t = se->tys.null_ptr;
            break;
        case ST_EX_IDENT:
            t = ST_type_ident(se, e);
            break;
        case ST_EX_UNARY:
            t = ST_type_unary(se, e);
            break;
        case ST_EX_BINARY:
            t = ST_type_binary(se, e);
            break;
        case ST_EX_CALL: {
            ST_tys_t *rets = ST_type_call(se, e);
            if (!rets)
                break;
            if (rets->count == 0) {
                t = se->tys.prim[ST_tvoid];
                break;
            }
            if (rets->count > 1)
                ST_diag_error(&se->diag, e->line, e->col,
                              "call returns %u values, bind them all: 'a, b := f()'", rets->count);
            t = rets->items[0];
            break;
        }
        case ST_EX_FIELD:
            t = ST_type_field(se, e);
            break;
        case ST_EX_INDEX:
            t = ST_type_index(se, e);
            break;
        case ST_EX_CAST:
            t = ST_type_cast(se, e);
            break;
        case ST_EX_STRUCT_LIT:
            t = ST_type_struct_lit(se, e, NULL);
            break;
        case ST_EX_ARRAY_NEW:
            t = ST_resolve_tyexpr(se, e->array_new.te);
            break;
        case ST_EX_SIZEOF: {
            ST_ty_t *st = ST_resolve_tyexpr(se, e->tyop.te);
            if (st)
                ST_complete_ty(se, st);
            t = se->tys.untyped_int;
            break;
        }
        case ST_EX_TYPEOF:
            ST_type_expr(se, e->tyop.operand);
            t = se->tys.prim[ST_tu64]; // type id, until RTTI lands
            break;
        case ST_EX_TYPEINFO:
            if (e->tyop.te)
                ST_resolve_tyexpr(se, e->tyop.te);
            else
                ST_type_expr(se, e->tyop.operand);
            t = ST_ty_ptr(&se->tys, se->tys.prim[ST_tvoid]); // *Type_Info later
            break;
        case ST_EX_KIND:
            ST_type_expr(se, e->tyop.operand);
            t = se->tys.prim[ST_tu64];
            break;
        case ST_EX_FIELDS:
            ST_type_expr(se, e->tyop.operand);
            ST_diag_error(&se->diag, e->line, e->col,
                          "'fields(...)' is only valid as the target of a comptime '#for'");
            t = se->tys.prim[ST_tvoid];
            break;
        case ST_EX_CSTR: {
            ST_ty_t *ot = ST_type_expr(se, e->tyop.operand);
            if (ot && ot->kind != ST_TY_STRING)
                ST_diag_error(&se->diag, e->line, e->col, "'cstr' needs a 'string', got '%s'",
                              ST_tstr(se, ot));
            t = ST_ty_ptr(&se->tys, se->tys.prim[ST_tchar]);
            break;
        }
        case ST_EX_STR_FROM_RAW: {
            // 'str_from_raw(ptr, len)' -- the sanctioned way to build a
            // 'string' from a raw '*char' + length (e.g. a buffer written
            // by a raw syscall in an '#asm' block). Mirrors 'cstr' but in
            // reverse; a dedicated intrinsic rather than field assignment
            // because a string's '.ptr'/'.len' are otherwise read-only --
            // strings are meant to be treated as immutable value objects,
            // so this is the one blessed place a new one gets assembled.
            ST_ty_t *pt = ST_type_expr(se, e->str_from_raw.ptr);
            ST_ty_t *lt = ST_type_expr(se, e->str_from_raw.len);
            if (pt && !(pt->kind == ST_TY_PTR && pt->inner && pt->inner->kind == ST_TY_CHAR))
                ST_diag_error(&se->diag, e->str_from_raw.ptr->line, e->str_from_raw.ptr->col,
                              "'str_from_raw' needs a '*char' pointer, got '%s'", ST_tstr(se, pt));
            if (lt && !ST_ty_is_int(lt))
                ST_diag_error(&se->diag, e->str_from_raw.len->line, e->str_from_raw.len->col,
                              "'str_from_raw' needs an integer length, got '%s'", ST_tstr(se, lt));
            t = se->tys.prim[ST_tstring];
            break;
        }
        case ST_EX_ASM:
            // an '#asm { .. }' block used as an expression yields whatever
            // ends up in rax (see ST_lower_asm_tokens / ST_IR_INLINE_ASM) --
            // typed as untyped_int, same as an integer literal, so it
            // freely coerces to whatever int/float type the context wants
            // (the return type, an assignment target, etc).
            t = se->tys.untyped_int;
            break;
        case ST_EX_COMP_ERROR: {
            ST_ct_chunk_t chunk;
            ST_ct_chunk_init(se->arena, &chunk);
            ST_ct_compiler_t cc;
            ST_ct_compiler_init(&cc, se->arena, &chunk);
            ST_ct_compile_expr_return(&cc, e);
            if (cc.failed) {
                ST_diag_error(&se->diag, cc.err_line, cc.err_col, "%s", cc.err_msg);
            } else {
                ST_ct_vm_t vm;
                ST_ct_vm_init(&vm);
                ST_ct_val_t out;
                ST_ct_status_t st = ST_ct_run(&vm, &chunk, &out);
                if (st == ST_CT_ERR_COMPTIME)
                    ST_diag_error(&se->diag, vm.err_line, e->col, "%s", vm.err_msg);
                else if (st == ST_CT_ERR_RUNTIME)
                    ST_diag_error(&se->diag, e->line, e->col, "internal: comptime VM error: %s",
                                  vm.err_msg);
                // ST_CT_OK shouldn't happen -- ST_OP_COMP_ERROR always
                // ends the run with ST_CT_ERR_COMPTIME -- but isn't itself
                // an error worth reporting if it somehow does.
            }
            t = se->tys.prim[ST_tvoid];
            break;
        }
        case ST_EX_COUNT:
            ST_assert(0);
            break;
    }
    e->ty = t;
    return t;
}

static void ST_check_stmt(ST_sema_t *se, ST_stmt_t *s);
static void ST_check_comptime_if(ST_sema_t *se, ST_stmt_t *s);
static void ST_check_comptime_switch(ST_sema_t *se, ST_stmt_t *s);

static void ST_check_body(ST_sema_t *se, ST_stmts_t *body) {
    ST_scope_push(se);
    ST_forrange(0, body->count) ST_check_stmt(se, body->items[i]);
    ST_scope_pop(se);
}

static void ST_check_cond(ST_sema_t *se, ST_expr_t *cond, const char *what) {
    ST_ty_t *t = ST_type_expr(se, cond);
    if (t && !ST_ty_is_bool(t))
        ST_diag_error(&se->diag, cond->line, cond->col, "%s condition must be 'bool', got '%s'",
                      what, ST_tstr(se, t));
}

static void ST_check_decl_stmt(ST_sema_t *se, ST_stmt_t *s) {
    b8 infer_count = s->decl.te && s->decl.te->kind == ST_TE_ARRAY && !s->decl.te->is_dynamic &&
                     !s->decl.te->count_expr;
    ST_ty_t *dt = NULL;
    if (s->decl.te && !infer_count)
        dt = ST_resolve_tyexpr(se, s->decl.te);
    // A generic struct referenced bare (no '(args)') is still an
    // uninstantiated template skeleton at this point -- completing it here
    // would try to lay out its still-generic '$T'/'$U' fields and fail.
    // When the initializer is an unnamed struct literal, 'ST_type_struct_lit'
    // below infers the generic args from the literal's values instead; only
    // complete eagerly once we're not relying on that inference.
    b8 dt_is_generic_struct = dt && dt->kind == ST_TY_STRUCT && dt->decl &&
                              dt->decl->struct_.generics.count > 0;
    b8 infers_from_lit = dt_is_generic_struct && s->decl.init &&
                         s->decl.init->kind == ST_EX_STRUCT_LIT &&
                         !s->decl.init->struct_lit.type_name.len;
    if (dt && !infers_from_lit)
        ST_complete_ty(se, dt);
    if (infer_count) {
        ST_ty_t *ety = ST_resolve_tyexpr(se, s->decl.te->inner);
        b8 has_lit = s->decl.init && s->decl.init->kind == ST_EX_STRUCT_LIT &&
                     !s->decl.init->struct_lit.type_name.len;
        if (ety && has_lit) {
            ST_complete_ty(se, ety);
            dt = ST_ty_array(&se->tys, ety, s->decl.init->struct_lit.inits.count);
        } else if (ety)
            ST_diag_error(&se->diag, s->line, s->col,
                          "cannot infer the size of '" ST_sv_fmt "' give it an"
                          "explicit count '[N]T' or initalize it with an array"
                          "iteral '{....}'.",
                          ST_sv_args(s->decl.name));
    }

    ST_ty_t *it = NULL;
    if (s->decl.init) {
        // unnamed struct literal takes the annotated type
        if (s->decl.init->kind == ST_EX_STRUCT_LIT && !s->decl.init->struct_lit.type_name.len &&
            dt) {
            it = ST_type_struct_lit(se, s->decl.init, dt);
            s->decl.init->ty = it;
            if (infers_from_lit && it)
                dt = it; // instantiated concrete type replaces the generic skeleton
        } else
            it = ST_type_expr(se, s->decl.init);
    }

    if (dt && it && !ST_ty_coerces(se, it, dt)) {
        ST_diag_error(&se->diag, s->line, s->col,
                      "cannot initialize '" ST_sv_fmt "' of type '%s' "
                      "with a value of type '%s'",
                      ST_sv_args(s->decl.name), ST_tstr(se, dt), ST_tstr(se, it));
        if (it->kind == ST_TY_UNTYPED_INT && ST_ty_is_float(dt) && s->decl.init)
            ST_diag_note(&se->diag, s->decl.init->line, s->decl.init->col,
                         "whole-number float literals need an explicit '.0', e.g. '%lld.0'",
                         (long long)s->decl.init->ival);
    }

    if (!dt && !s->decl.init)
        ST_diag_error(&se->diag, s->line, s->col, "'" ST_sv_fmt "' needs a type or an initializer",
                      ST_sv_args(s->decl.name));

    ST_ty_t *t = dt ? dt : ST_ty_defaulted(se, it);
    if (t && t->kind == ST_TY_VOID)
        ST_diag_error(&se->diag, s->line, s->col, "cannot declare '" ST_sv_fmt "' of type 'void'",
                      ST_sv_args(s->decl.name));

    // '&N' where 'N' is a '::' constant points at read-only backing storage
    // (see ST_expr_is_addressable). Binding that address into a mutable
    // (':=') local hands out a writable-looking pointer to storage that's
    // conceptually immutable, so require the local itself to be const too.
    if (s->decl.init && s->decl.init->kind == ST_EX_UNARY &&
        ST_string_eq_cstr(s->decl.init->unary.op, "&") &&
        s->decl.init->unary.operand->kind == ST_EX_IDENT && !s->decl.is_const) {
        ST_sym_t *csym = ST_sym_find(se, s->decl.init->unary.operand->name);
        if (csym && csym->kind == ST_SYM_CONST) {
            ST_diag_error(&se->diag, s->line, s->col,
                          "cannot take the address of constant '" ST_sv_fmt
                          "' into mutable '" ST_sv_fmt "'; declare '" ST_sv_fmt
                          "' as a constant instead ('" ST_sv_fmt "' :: &" ST_sv_fmt ")",
                          ST_sv_args(csym->name), ST_sv_args(s->decl.name),
                          ST_sv_args(s->decl.name), ST_sv_args(s->decl.name),
                          ST_sv_args(csym->name));
            ST_diag_note(&se->diag, csym->line, csym->col, "'" ST_sv_fmt "' is declared here",
                         ST_sv_args(csym->name));
        }
    }

    ST_declare_local_ex(se, s->decl.name, t, s->line, s->col, s->decl.is_const);
}

// Walks down field/index chains to the identifier at the root of an lvalue,
// e.g. 'a.b[i].c' -> 'a'. Stops (returns NULL) at anything else, including a
// dereference, since '*p = x' writes through a pointer, not to a named
// variable, so it's never blocked by a global's own const-ness.
static ST_expr_t *ST_lvalue_root_ident(ST_expr_t *e) {
    for (;;) {
        if (e->kind == ST_EX_IDENT)
            return e;
        if (e->kind == ST_EX_FIELD) {
            e = e->field.base;
            continue;
        }
        if (e->kind == ST_EX_INDEX) {
            e = e->index.base;
            continue;
        }
        return NULL;
    }
}

static void ST_check_assign(ST_sema_t *se, ST_stmt_t *s) {
    ST_ty_t *lt = ST_type_expr(se, s->assign.lhs);
    ST_ty_t *rt = ST_type_expr(se, s->assign.rhs);
    if (!lt || !rt)
        return;
    ST_expr_t *lhs = s->assign.lhs;
    {
        ST_expr_t *root = ST_lvalue_root_ident(lhs);
        if (root) {
            ST_sym_t *sym = ST_sym_find(se, root->name);
            if (sym && (sym->kind == ST_SYM_CONST || (sym->kind == ST_SYM_VAR && sym->is_const))) {
                ST_diag_error(&se->diag, s->line, s->col,
                              "cannot assign to '" ST_sv_fmt "': it is a constant ('::')",
                              ST_sv_args(root->name));
                ST_diag_note(&se->diag, sym->line, sym->col,
                             "'" ST_sv_fmt "' is declared here", ST_sv_args(root->name));
                return;
            }
        }
    }
    if (lhs->kind == ST_EX_INDEX && lhs->index.base->ty &&
        lhs->index.base->ty->kind == ST_TY_STRING) {
        ST_diag_error(&se->diag, s->line, s->col,
                      "strings are immutable; cannot assign into a string");
        return;
    }
    if (lhs->kind == ST_EX_FIELD && lhs->field.base->ty &&
        lhs->field.base->ty->kind == ST_TY_STRING) {
        ST_diag_error(&se->diag, s->line, s->col,
                      "'" ST_sv_fmt "' of a string is read-only; "
                      "reassign the whole string instead",
                      ST_sv_args(lhs->field.name));
        return;
    }
    // Writing a variant's payload field is allowed -- symmetric with reads,
    // which are already documented as an unchecked reinterpret of the
    // payload region (same spirit as a C union). This does NOT touch the
    // tag, so it's only meaningful when that variant is already active;
    // it's on the caller to keep that in sync, exactly like a C union.
    // '.kind' itself stays protected: that's the tag, and changing it
    // without also changing the payload is how the two get out of sync, so
    // it can only be set by constructing a new value.
    if (lhs->kind == ST_EX_FIELD && lhs->field.base->ty &&
        lhs->field.base->ty->kind == ST_TY_TAG_UNION &&
        ST_string_eq_cstr(lhs->field.name, "kind")) {
        ST_diag_error(&se->diag, s->line, s->col,
                      "cannot assign to '.kind' directly; construct a new '" ST_sv_fmt
                      "' value instead, e.g. '" ST_sv_fmt "{ SomeVariant, .. }' "
                      "-- this keeps the active variant and its payload from getting out "
                      "of sync",
                      ST_sv_args(ST_decl_display_name(lhs->field.base->ty->decl)),
                      ST_sv_args(ST_decl_display_name(lhs->field.base->ty->decl)));
        return;
    }
    ST_string_t op = s->assign.op;

    if (ST_string_eq_cstr(op, "=")) {
        if (!ST_ty_coerces(se, rt, lt))
            ST_diag_error(&se->diag, s->line, s->col, "cannot assign '%s' to '%s'", ST_tstr(se, rt),
                          ST_tstr(se, lt));
        return;
    }

    // compound: `+=` `-=` `*=` `/=` need numeric, the rest need integers
    b8 arith = ST_op_is(op, "+=", "-=") || ST_op_is(op, "*=", "/=");
    if (arith) {
        if (!ST_ty_num_unify(se, lt, rt))
            ST_diag_error(&se->diag, s->line, s->col,
                          "invalid operands to '" ST_sv_fmt "': '%s' and '%s'", ST_sv_args(op),
                          ST_tstr(se, lt), ST_tstr(se, rt));
        return;
    }
    if (!ST_ty_is_int(lt) || !ST_ty_is_int(rt))
        ST_diag_error(&se->diag, s->line, s->col,
                      "'" ST_sv_fmt "' needs integer operands, got '%s' and '%s'", ST_sv_args(op),
                      ST_tstr(se, lt), ST_tstr(se, rt));
}

static void ST_check_multi(ST_sema_t *se, ST_stmt_t *s) {
    // a, b := f()  -> a single call producing every value
    b8 from_call = s->multi.n_names > 1 && s->multi.values.count == 1 &&
                   s->multi.values.items[0]->kind == ST_EX_CALL;

    ST_ty_t *tys[16] = {0};
    u32 n_tys = 0;

    if (from_call) {
        ST_expr_t *call = s->multi.values.items[0];
        ST_tys_t *rets = ST_type_call(se, call);
        if (rets) {
            if (rets->count != s->multi.n_names)
                ST_diag_error(&se->diag, s->line, s->col,
                              "call returns %u value%s, but %u name%s bound", rets->count,
                              rets->count == 1 ? "" : "s", s->multi.n_names,
                              s->multi.n_names == 1 ? " is" : "s are");
            ST_forrange(0, rets->count) {
                if (n_tys >= ST_array_len(tys))
                    break;
                tys[n_tys++] = rets->items[i];
            }
            call->ty = rets->count ? rets->items[0] : se->tys.prim[ST_tvoid];
        }
    } else {
        if (s->multi.values.count != s->multi.n_names)
            ST_diag_error(&se->diag, s->line, s->col, "expected %u value%s, got %u",
                          s->multi.n_names, s->multi.n_names == 1 ? "" : "s",
                          s->multi.values.count);
        ST_forrange(0, s->multi.values.count) {
            ST_ty_t *t = ST_type_expr(se, s->multi.values.items[i]);
            if (n_tys < ST_array_len(tys))
                tys[n_tys++] = t;
        }
    }

    ST_forrange(0, s->multi.n_names) {
        ST_ty_t *t = i < n_tys ? tys[i] : NULL;
        if (s->multi.declare)
            ST_declare_local(se, s->multi.names[i], ST_ty_defaulted(se, t), s->line, s->col);
        else {
            ST_sym_t *sym = ST_sym_find(se, s->multi.names[i]);
            if (!sym)
                ST_diag_error(&se->diag, s->line, s->col,
                              "use of undeclared identifier '" ST_sv_fmt "'",
                              ST_sv_args(s->multi.names[i]));
            else if (sym->t && t && !ST_ty_coerces(se, t, sym->t))
                ST_diag_error(&se->diag, s->line, s->col,
                              "cannot assign '%s' to '" ST_sv_fmt "' of type '%s'", ST_tstr(se, t),
                              ST_sv_args(s->multi.names[i]), ST_tstr(se, sym->t));
        }
    }
}

static void ST_check_return(ST_sema_t *se, ST_stmt_t *s) {
    u32 want = se->cur_rets ? se->cur_rets->count : 0;
    u32 got = s->ret.values.count;
    if (want != got) {
        if (want == 0)
            ST_diag_error(&se->diag, s->line, s->col, "this function does not return a value");
        else
            ST_diag_error(&se->diag, s->line, s->col, "this function returns %u value%s, got %u",
                          want, want == 1 ? "" : "s", got);
    }
    ST_forrange(0, got) {
        ST_expr_t *rv = s->ret.values.items[i];
        ST_ty_t *rt = (i < want && se->cur_rets) ? se->cur_rets->items[i] : NULL;
        ST_ty_t *t;
        // unnamed struct/array literal takes the declared return type, same
        // as an unnamed literal against an annotated 'x : T = { .. }' decl.
        if (rv->kind == ST_EX_STRUCT_LIT && !rv->struct_lit.type_name.len && rt) {
            t = ST_type_struct_lit(se, rv, rt);
            rv->ty = t;
        } else
            t = ST_type_expr(se, rv);
        if (i >= want || !se->cur_rets)
            continue;
        if (t && rt && !ST_ty_coerces(se, t, rt))
            ST_diag_error(&se->diag, s->ret.values.items[i]->line, s->ret.values.items[i]->col,
                          "return value %u expects '%s', got '%s'", i + 1, ST_tstr(se, rt),
                          ST_tstr(se, t));
    }
}

// Runs 'e' through the comptime compiler+VM and returns its value. On
// failure (uncompilable expression, or a runtime VM error -- both compiler
// bugs or genuinely invalid comptime code, not user-facing #comp_errors)
// reports a diagnostic and returns nil; the caller should treat that as "no
// branch matched, stop here" rather than press on with a bogus mutation.
static void ST_stamp_kind_operands(ST_sema_t *se, ST_expr_t *e) {
    if (!e)
        return;
    switch (e->kind) {
        case ST_EX_KIND:
        case ST_EX_FIELDS:
            ST_type_expr(se, e->tyop.operand);
            return;
        case ST_EX_UNARY:
            ST_stamp_kind_operands(se, e->unary.operand);
            return;
        case ST_EX_BINARY:
            ST_stamp_kind_operands(se, e->bin.l);
            ST_stamp_kind_operands(se, e->bin.r);
            return;
        case ST_EX_FIELD:
            ST_stamp_kind_operands(se, e->field.base);
            return;
        case ST_EX_INDEX:
            ST_stamp_kind_operands(se, e->index.base);
            ST_stamp_kind_operands(se, e->index.index);
            return;
        case ST_EX_COMP_ERROR:
            ST_forrange(0, e->comp_error.args.count)
                ST_stamp_kind_operands(se, e->comp_error.args.items[i]);
            return;
        default:
            return; // literals, idents, etc: nothing to stamp
    }
}

static b8 ST_ct_eval_expr(ST_sema_t *se, ST_expr_t *e, ST_ct_val_t *out) {
    ST_stamp_kind_operands(se, e);
    ST_ct_chunk_t chunk;
    ST_ct_chunk_init(se->arena, &chunk);
    ST_ct_compiler_t cc;
    ST_ct_compiler_init(&cc, se->arena, &chunk);
    ST_ct_compile_expr_return(&cc, e);
    if (cc.failed) {
        ST_diag_error(&se->diag, cc.err_line, cc.err_col, "%s", cc.err_msg);
        return 0;
    }
    ST_ct_vm_t vm;
    ST_ct_vm_init(&vm);
    ST_ct_status_t st = ST_ct_run(&vm, &chunk, out);
    if (st == ST_CT_ERR_COMPTIME) {
        ST_diag_error(&se->diag, vm.err_line, e->col, "%s", vm.err_msg);
        return 0;
    }
    if (st == ST_CT_ERR_RUNTIME) {
        ST_diag_error(&se->diag, e->line, e->col, "internal: comptime VM error: %s", vm.err_msg);
        return 0;
    }
    return 1;
}

// Turns 's' into a plain block wrapping exactly 'body', then type-checks
// (and, later, lowers) only that. The untaken branch's ST_stmts_t is simply
// dropped -- nothing downstream (lowering, control-flow checks) ever sees
// it, since this rewrite happens before any of those passes run.
static void ST_rewrite_as_block(ST_stmt_t *s, ST_stmts_t body) {
    s->kind = ST_ST_BLOCK;
    s->block = body;
}

static void ST_check_comptime_if(ST_sema_t *se, ST_stmt_t *s) {
    ST_ct_val_t cond;
    if (!ST_ct_eval_expr(se, s->if_.cond, &cond)) {
        ST_rewrite_as_block(s, (ST_stmts_t){0}); // already reported; leave nothing to check further
        return;
    }
    if (ST_ct_truthy(cond)) {
        ST_rewrite_as_block(s, s->if_.then_body);
    } else if (s->if_.else_stmt) {
        // else_stmt is itself ST_ST_BLOCK (plain '#else { }') or ST_ST_IF
        // (an '#else if' link in the chain, already marked is_comptime by
        // the parser) -- either way just splice it in and let the normal
        // dispatch below (recursive ST_check_stmt) handle which one it is.
        *s = *s->if_.else_stmt;
    } else {
        ST_rewrite_as_block(s, (ST_stmts_t){0});
    }
    ST_check_stmt(se, s);
}

static void ST_check_comptime_switch(ST_sema_t *se, ST_stmt_t *s) {
    ST_ct_val_t scrutinee;
    if (!ST_ct_eval_expr(se, s->switch_.cond, &scrutinee)) {
        ST_rewrite_as_block(s, (ST_stmts_t){0});
        return;
    }
    ST_case_t *matched = NULL, *default_case = NULL;
    ST_forrange(0, s->switch_.cases.count) {
        ST_case_t *c = &s->switch_.cases.items[i];
        if (c->values.count == 0) {
            default_case = c; // '#default:' / '#case:' -- fallback if nothing else matches
            continue;
        }
        b8 hit = 0;
        for (u32 k = 0; k < c->values.count && !hit; k++) {
            ST_ct_val_t v;
            if (!ST_ct_eval_expr(se, c->values.items[k], &v)) {
                ST_rewrite_as_block(s, (ST_stmts_t){0});
                return;
            }
            if (v.kind != scrutinee.kind) {
                // no implicit cross-kind matching -- '#case 1' never
                // matches a string scrutinee, etc. Mirrors the strictness
                // ST_OP_EQ already has for '=='/'!=' on mismatched kinds.
                continue;
            }
            switch (v.kind) {
                case ST_CT_INT: hit = v.i == scrutinee.i; break;
                case ST_CT_BOOL: hit = v.b == scrutinee.b; break;
                case ST_CT_STRING:
                    hit = v.str.len == scrutinee.str.len &&
                          memcmp(v.str.data, scrutinee.str.data, v.str.len) == 0;
                    break;
                default: hit = 0; break;
            }
        }
        if (hit) {
            matched = c;
            break;
        }
    }
    if (!matched)
        matched = default_case;
    if (matched)
        ST_rewrite_as_block(s, matched->body);
    else
        ST_rewrite_as_block(s, (ST_stmts_t){0}); // no arm matched and no default -- compiles to nothing
    ST_check_stmt(se, s);
}

// Replaces every ST_EX_IDENT named 'target' with a fresh clone of
// 'replacement' (used for '#for' loop-variable substitution: each unrolled
// copy gets 'i' replaced by a literal). Also replaces 'pack_name[K]' (K
// const-foldable) with an ident named 'pack_name#K', and 'pack_name.count'
// with a literal -- both no-ops when se->has_pack is false or K isn't
// foldable yet (e.g. still contains the loop var pre-substitution), so it's
// safe to call unconditionally at multiple points without double-rewriting
// something already rewritten (an ST_EX_IDENT named 'args#2' doesn't match
// 'target' or 'pack_name' on a second pass).
static void ST_ast_substitute_expr(ST_sema_t *se, ST_expr_t *e, ST_string_t target,
                                   ST_expr_t *replacement) {
    if (!e)
        return;
    switch (e->kind) {
        case ST_EX_IDENT:
            if (target.len && ST_string_eq(e->name, target)) {
                ST_ty_t *save_ty = e->ty;
                u32 save_line = e->line, save_col = e->col;
                *e = *replacement;
                e->ty = save_ty; // let normal re-typechecking overwrite this properly
                e->line = save_line;
                e->col = save_col;
            }
            return;
        case ST_EX_INDEX: {
            ST_ast_substitute_expr(se, e->index.base, target, replacement);
            ST_ast_substitute_expr(se, e->index.index, target, replacement);
            if (se->has_pack && e->index.base->kind == ST_EX_IDENT &&
                ST_string_eq(e->index.base->name, se->cur_pack_name)) {
                i64 k;
                if (ST_const_eval(se, e->index.index, &k) && k >= 0 &&
                    (u32)k < se->cur_pack_count) {
                    ST_string_t nm = ST_pack_param_name(se, se->cur_pack_name, (u32)k);
                    e->kind = ST_EX_IDENT;
                    e->name = nm;
                }
            }
            return;
        }
        case ST_EX_FIELD:
            ST_ast_substitute_expr(se, e->field.base, target, replacement);
            if (se->has_pack && e->field.base->kind == ST_EX_IDENT &&
                ST_string_eq(e->field.base->name, se->cur_pack_name) &&
                ST_string_eq_cstr(e->field.name, "count")) {
                e->kind = ST_EX_INT;
                e->ival = (i64)se->cur_pack_count;
            }
            return;
        case ST_EX_UNARY:
            ST_ast_substitute_expr(se, e->unary.operand, target, replacement);
            return;
        case ST_EX_SIZEOF:
        case ST_EX_TYPEOF:
        case ST_EX_TYPEINFO:
        case ST_EX_KIND:
        case ST_EX_CSTR:
        case ST_EX_FIELDS:
            ST_ast_substitute_expr(se, e->tyop.operand, target, replacement);
            return;
        case ST_EX_BINARY:
            ST_ast_substitute_expr(se, e->bin.l, target, replacement);
            ST_ast_substitute_expr(se, e->bin.r, target, replacement);
            return;
        case ST_EX_CALL:
            ST_ast_substitute_expr(se, e->call.callee, target, replacement);
            ST_forrange(0, e->call.args.count)
                ST_ast_substitute_expr(se, e->call.args.items[i].value, target, replacement);
            return;
        case ST_EX_CAST:
            ST_ast_substitute_expr(se, e->cast.operand, target, replacement);
            return;
        case ST_EX_COMP_ERROR:
            ST_forrange(0, e->comp_error.args.count)
                ST_ast_substitute_expr(se, e->comp_error.args.items[i], target, replacement);
            return;
        default:
            return; // literals: nothing to substitute
    }
}

static void ST_ast_substitute_stmt(ST_sema_t *se, ST_stmt_t *s, ST_string_t target,
                                   ST_expr_t *replacement) {
    if (!s)
        return;
    switch (s->kind) {
        case ST_ST_EXPR:
            ST_ast_substitute_expr(se, s->expr, target, replacement);
            return;
        case ST_ST_DECL:
            ST_ast_substitute_expr(se, s->decl.init, target, replacement);
            return;
        case ST_ST_ASSIGN:
            ST_ast_substitute_expr(se, s->assign.lhs, target, replacement);
            ST_ast_substitute_expr(se, s->assign.rhs, target, replacement);
            return;
        case ST_ST_IF:
            ST_ast_substitute_expr(se, s->if_.cond, target, replacement);
            ST_forrange(0, s->if_.then_body.count)
                ST_ast_substitute_stmt(se, s->if_.then_body.items[i], target, replacement);
            ST_ast_substitute_stmt(se, s->if_.else_stmt, target, replacement);
            return;
        case ST_ST_SWITCH:
            ST_ast_substitute_expr(se, s->switch_.cond, target, replacement);
            ST_forrange(0, s->switch_.cases.count) {
                ST_case_t *c = &s->switch_.cases.items[i];
                for (u32 k = 0; k < c->values.count; k++)
                    ST_ast_substitute_expr(se, c->values.items[k], target, replacement);
                for (u32 k = 0; k < c->body.count; k++)
                    ST_ast_substitute_stmt(se, c->body.items[k], target, replacement);
            }
            return;
        case ST_ST_WHILE:
            ST_ast_substitute_expr(se, s->while_.cond, target, replacement);
            ST_forrange(0, s->while_.body.count)
                ST_ast_substitute_stmt(se, s->while_.body.items[i], target, replacement);
            return;
        case ST_ST_FOR_RANGE:
            ST_ast_substitute_expr(se, s->for_range.lo, target, replacement);
            ST_ast_substitute_expr(se, s->for_range.hi, target, replacement);
            ST_forrange(0, s->for_range.body.count)
                ST_ast_substitute_stmt(se, s->for_range.body.items[i], target, replacement);
            return;
        case ST_ST_FOR_ARRAY:
            ST_ast_substitute_expr(se, s->for_array.target, target, replacement);
            ST_forrange(0, s->for_array.body.count)
                ST_ast_substitute_stmt(se, s->for_array.body.items[i], target, replacement);
            return;
        case ST_ST_RETURN:
            ST_forrange(0, s->ret.values.count)
                ST_ast_substitute_expr(se, s->ret.values.items[i], target, replacement);
            return;
        case ST_ST_BLOCK:
            ST_forrange(0, s->block.count)
                ST_ast_substitute_stmt(se, s->block.items[i], target, replacement);
            return;
        case ST_ST_DEFER:
            ST_ast_substitute_stmt(se, s->defer_stmt, target, replacement);
            return;
        default:
            return;
    }
}

// Runs just the pack-substitution half (no loop-var target) over a whole
// body -- called once by ST_check_fn_body before any statement checking, so
// e.g. a '#for i: 0..args.count' loop bound is already a literal by the
// time its own is_comptime handling tries to const-eval it.
static void ST_pack_substitute_body(ST_sema_t *se, ST_stmts_t *body) {
    if (!se->has_pack)
        return;
    ST_string_t no_target = {0};
    ST_forrange(0, body->count) ST_ast_substitute_stmt(se, body->items[i], no_target, NULL);
}

static void ST_check_comptime_for_fields(ST_sema_t *se, ST_stmt_t *s) {
    ST_expr_t *operand = s->for_array.target->tyop.operand;
    ST_ty_t *t = ST_type_expr(se, operand);
    if (!t) {
        ST_rewrite_as_block(s, (ST_stmts_t){0});
        return;
    }
    if (t->kind == ST_TY_PTR)
        t = t->inner; // '#fields(&x)' -- one auto-deref, same courtesy '.field' access gives
    if (t->kind != ST_TY_STRUCT && t->kind != ST_TY_ARRAY) {
        ST_diag_error(&se->diag, s->for_array.target->line, s->for_array.target->col,
                      "'fields(...)' needs a struct or a fixed-size array, got '%s'", ST_tstr(se, t));
        ST_rewrite_as_block(s, (ST_stmts_t){0});
        return;
    }
    b8 is_struct = t->kind == ST_TY_STRUCT;
    if (is_struct)
        ST_complete_ty(se, t);
    u32 count = is_struct ? t->fields.count : (u32)t->count;

    ST_stmts_t unrolled = {0};
    ST_forrange(0, count) {
        ST_expr_t *access;
        if (is_struct) {
            access = ST_expr_new(se->arena, ST_EX_FIELD, s->line, s->col);
            access->field.base = ST_clone_expr(se->arena, operand);
            access->field.name = t->fields.items[i].name;
        } else {
            ST_expr_t *idx = ST_expr_new(se->arena, ST_EX_INT, s->line, s->col);
            idx->ival = (i64)i;
            access = ST_expr_new(se->arena, ST_EX_INDEX, s->line, s->col);
            access->index.base = ST_clone_expr(se->arena, operand);
            access->index.index = idx;
        }

        ST_expr_t idx_lit = {0};
        idx_lit.kind = ST_EX_INT;
        idx_lit.line = s->line;
        idx_lit.col = s->col;
        idx_lit.ival = (i64)i;

        ST_stmts_t copy = ST_clone_body(se->arena, &s->for_array.body);
        ST_forrange(0, copy.count)
            ST_ast_substitute_stmt(se, copy.items[i], s->for_array.iter, access);
        if (s->for_array.spec_iter.len)
            ST_forrange(0, copy.count)
                ST_ast_substitute_stmt(se, copy.items[i], s->for_array.spec_iter, &idx_lit);
        if (se->has_pack) {
            ST_string_t no_target = {0};
            ST_forrange(0, copy.count) ST_ast_substitute_stmt(se, copy.items[i], no_target, NULL);
        }
        ST_forrange(0, copy.count) ST_da_append_arena(se->arena, &unrolled, copy.items[i]);
    }

    ST_rewrite_as_block(s, unrolled);
    ST_check_stmt(se, s);
}

static void ST_check_comptime_for_array(ST_sema_t *se, ST_stmt_t *s) {
    ST_ct_val_t str;
    if (!ST_ct_eval_expr(se, s->for_array.target, &str) || str.kind != ST_CT_STRING) {
        ST_diag_error(&se->diag, s->line, s->col,
                      "'#for' over a value needs a compile-time-constant string here "
                      "(e.g. a string literal or a 'string ::' constant)");
        ST_rewrite_as_block(s, (ST_stmts_t){0});
        return;
    }

    ST_stmts_t unrolled = {0};
    u32 spec = 0;
    for (u32 pos = 0; pos < str.str.len; pos++) {
        u8 ch = (u8)str.str.data[pos];

        ST_expr_t ch_lit = {0};
        ch_lit.kind = ST_EX_INT;
        ch_lit.line = s->line;
        ch_lit.col = s->col;
        ch_lit.ival = (i64)ch;

        ST_expr_t spec_lit = {0};
        spec_lit.kind = ST_EX_INT;
        spec_lit.line = s->line;
        spec_lit.col = s->col;
        spec_lit.ival = (i64)spec; // running count of '%' strictly before 'pos'

        ST_stmts_t copy = ST_clone_body(se->arena, &s->for_array.body);
        ST_forrange(0, copy.count) ST_ast_substitute_stmt(se, copy.items[i], s->for_array.iter, &ch_lit);
        if (s->for_array.spec_iter.len)
            ST_forrange(0, copy.count)
                ST_ast_substitute_stmt(se, copy.items[i], s->for_array.spec_iter, &spec_lit);
        if (se->has_pack) {
            ST_string_t no_target = {0};
            ST_forrange(0, copy.count) ST_ast_substitute_stmt(se, copy.items[i], no_target, NULL);
        }
        ST_forrange(0, copy.count) ST_da_append_arena(se->arena, &unrolled, copy.items[i]);

        if (ch == '%')
            spec++;
    }

    if (se->has_pack && spec != se->cur_pack_count)
        ST_diag_error(&se->diag, s->for_array.target->line, s->for_array.target->col,
                      "format string has %u '%%' but %u argument%s were passed", spec,
                      se->cur_pack_count, se->cur_pack_count == 1 ? "" : "s");

    ST_rewrite_as_block(s, unrolled);
    ST_check_stmt(se, s);
}

static void ST_check_comptime_for(ST_sema_t *se, ST_stmt_t *s) {
    i64 lo, hi;
    if (!ST_const_eval(se, s->for_range.lo, &lo) || !ST_const_eval(se, s->for_range.hi, &hi)) {
        ST_diag_error(&se->diag, s->line, s->col,
                      "'#for' bounds must be compile-time constants (e.g. a literal, or "
                      "'args.count' inside a '$T...' pack)");
        ST_rewrite_as_block(s, (ST_stmts_t){0});
        return;
    }
    if (s->for_range.inclusive)
        hi++;
    if (hi < lo)
        hi = lo; // empty range, not an error -- same as the runtime 'for' would just not iterate

    ST_stmts_t unrolled = {0};
    for (i64 k = lo; k < hi; k++) {
        ST_expr_t lit = {0};
        lit.kind = ST_EX_INT;
        lit.line = s->line;
        lit.col = s->col;
        lit.ival = k;
        ST_stmts_t copy = ST_clone_body(se->arena, &s->for_range.body);
        ST_forrange(0, copy.count)
            ST_ast_substitute_stmt(se, copy.items[i], s->for_range.iter, &lit);
        // Substituting 'i' above can turn e.g. 'args[i]' into 'args[2]' --
        // now foldable, so run the pack half again to catch it.
        if (se->has_pack) {
            ST_string_t no_target = {0};
            ST_forrange(0, copy.count) ST_ast_substitute_stmt(se, copy.items[i], no_target, NULL);
        }
        ST_forrange(0, copy.count) ST_da_append_arena(se->arena, &unrolled, copy.items[i]);
    }
    ST_rewrite_as_block(s, unrolled);
    ST_check_stmt(se, s);
}

static void ST_check_stmt(ST_sema_t *se, ST_stmt_t *s) {
    if (!s)
        return;
    switch (s->kind) {
        case ST_ST_EXPR:
            if (s->expr && s->expr->kind == ST_EX_CALL)
                ST_type_call(se, s->expr); // rets may be ignored as a statement
            else
                ST_type_expr(se, s->expr);
            break;
        case ST_ST_DECL:
            ST_check_decl_stmt(se, s);
            break;
        case ST_ST_ASSIGN:
            ST_check_assign(se, s);
            break;
        case ST_ST_MULTI_BIND:
            ST_check_multi(se, s);
            break;
        case ST_ST_IF:
            if (s->if_.is_comptime) {
                ST_check_comptime_if(se, s);
                break;
            }
            ST_check_cond(se, s->if_.cond, "'if'");
            ST_check_body(se, &s->if_.then_body);
            ST_check_stmt(se, s->if_.else_stmt);
            break;
        case ST_ST_SWITCH: {
            if (s->switch_.is_comptime) {
                ST_check_comptime_switch(se, s);
                break;
            }
            ST_ty_t *ct = ST_type_expr(se, s->switch_.cond);
            if (ct && ct->kind == ST_TY_TAG_UNION) {
                ST_complete_ty(se, ct);
                ST_forrange(0, s->switch_.cases.count) {
                    ST_case_t *c = &s->switch_.cases.items[i];
                    if (c->values.count == 0) {
                        // 'default:' / 'case:' - matches whatever no other
                        // case did; nothing to validate.
                        ST_check_body(se, &c->body);
                        continue;
                    }
                    if (c->values.count != 1 || c->values.items[0]->kind != ST_EX_IDENT) {
                        ST_diag_error(&se->diag, c->line, c->col,
                                      "expected a single variant name here, e.g. 'case "
                                      "SomeVariant:'");
                        ST_check_body(se, &c->body);
                        continue;
                    }
                    ST_string_t vname = c->values.items[0]->name;
                    b8 found = 0;
                    ST_forrange(0, ct->decl->tag_union.variants.count)
                        if (ST_string_eq(ct->decl->tag_union.variants.items[i].name, vname)) {
                            found = 1;
                            break;
                        }
                    if (!found) {
                        ST_diag_error(&se->diag, c->values.items[0]->line,
                                      c->values.items[0]->col,
                                      "'" ST_sv_fmt "' has no variant '" ST_sv_fmt "'",
                                      ST_sv_args(ST_decl_display_name(ct->decl)),
                                      ST_sv_args(vname));
                        ST_diag_note(&se->diag, ct->decl->line, ct->decl->col,
                                     "'" ST_sv_fmt "' is declared here",
                                     ST_sv_args(ST_decl_display_name(ct->decl)));
                    }
                    c->values.items[0]->ty = se->tys.prim[ST_ti64]; // stamp: it's a tag ordinal, not a real value
                    ST_check_body(se, &c->body);
                }
                break;
            }
            ST_forrange(0, s->switch_.cases.count) {
                ST_case_t *c = &s->switch_.cases.items[i];
                for (u32 k = 0; k < c->values.count; k++) {
                    ST_ty_t *vt = ST_type_expr(se, c->values.items[k]);
                    if (ct && vt && !ST_ty_coerces(se, vt, ct) && !ST_ty_coerces(se, ct, vt) &&
                        !ST_ty_num_unify(se, ct, vt))
                        ST_diag_error(&se->diag, c->values.items[k]->line, c->values.items[k]->col,
                                      "case of type '%s' cannot match a '%s' switch",
                                      ST_tstr(se, vt), ST_tstr(se, ct));
                }
                ST_check_body(se, &c->body);
            }
            break;
        }
        case ST_ST_WHILE:
            ST_check_cond(se, s->while_.cond, "'while'");
            ST_check_body(se, &s->while_.body);
            break;
        case ST_ST_FOR_RANGE: {
            if (s->for_range.is_comptime) {
                ST_check_comptime_for(se, s);
                break;
            }
            ST_ty_t *lo = ST_type_expr(se, s->for_range.lo);
            ST_ty_t *hi = ST_type_expr(se, s->for_range.hi);
            if (lo && !ST_ty_is_int(lo))
                ST_diag_error(&se->diag, s->for_range.lo->line, s->for_range.lo->col,
                              "range bound must be an integer, got '%s'", ST_tstr(se, lo));
            if (hi && !ST_ty_is_int(hi))
                ST_diag_error(&se->diag, s->for_range.hi->line, s->for_range.hi->col,
                              "range bound must be an integer, got '%s'", ST_tstr(se, hi));
            ST_ty_t *iter = lo && hi ? ST_ty_num_unify(se, lo, hi) : NULL;
            ST_ty_t *decl_ty = iter ? iter : lo;
            if (s->for_range.iter_te) {
                ST_ty_t *annotated = ST_resolve_tyexpr(se, s->for_range.iter_te);
                if (annotated && !ST_ty_is_int(annotated))
                    ST_diag_error(&se->diag, s->for_range.iter_te->line, s->for_range.iter_te->col,
                                  "range iterator must be an integer, got '%s'",
                                  ST_tstr(se, annotated));
                else if (annotated) {
                    if (lo && !ST_ty_coerces(se, lo, annotated))
                        ST_diag_error(&se->diag, s->for_range.iter_te->line,
                                      s->for_range.iter_te->col,
                                      "range start of type '%s' does not fit iterator type '%s'",
                                      ST_tstr(se, lo), ST_tstr(se, annotated));
                    if (hi && !ST_ty_coerces(se, hi, annotated))
                        ST_diag_error(&se->diag, s->for_range.iter_te->line,
                                      s->for_range.iter_te->col,
                                      "range end of type '%s' does not fit iterator type '%s'",
                                      ST_tstr(se, hi), ST_tstr(se, annotated));
                    decl_ty = annotated;
                }
            }
            ST_scope_push(se);
            ST_declare_local(se, s->for_range.iter, ST_ty_defaulted(se, decl_ty), s->line, s->col);
            ST_forrange(0, s->for_range.body.count) ST_check_stmt(se, s->for_range.body.items[i]);
            ST_scope_pop(se);
            break;
        }
        case ST_ST_FOR_ARRAY: {
            if (s->for_array.is_comptime) {
                if (s->for_array.target->kind == ST_EX_FIELDS)
                    ST_check_comptime_for_fields(se, s);
                else
                    ST_check_comptime_for_array(se, s);
                break;
            }
            ST_ty_t *tt = ST_type_expr(se, s->for_array.target);
            ST_ty_t *iter = NULL;
            if (tt) {
                if (tt->kind == ST_TY_ARRAY || tt->kind == ST_TY_DYN_ARRAY)
                    iter = tt->inner;
                else if (tt->kind == ST_TY_STRING)
                    iter = se->tys.prim[ST_tchar];
                else
                    ST_diag_error(&se->diag, s->for_array.target->line, s->for_array.target->col,
                                  "cannot iterate a value of type '%s'", ST_tstr(se, tt));
            }
            ST_scope_push(se);
            ST_declare_local(se, s->for_array.iter, iter, s->line, s->col);
            ST_forrange(0, s->for_array.body.count) ST_check_stmt(se, s->for_array.body.items[i]);
            ST_scope_pop(se);
            break;
        }
        case ST_ST_RETURN:
            ST_check_return(se, s);
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
                ST_diag_error(&se->diag, s->line, s->col, "goto to unknown label '" ST_sv_fmt "'",
                              ST_sv_args(s->label));
            break;
        case ST_ST_ASM:
            break;
        case ST_ST_COUNT:
            ST_assert(0);
            break;
    }
}

static void ST_collect_labels(ST_sema_t *se, ST_ht_t *labels, ST_stmts_t *body) {
    ST_forrange(0, body->count) {
        ST_stmt_t *s = body->items[i];
        if (!s)
            continue;
        switch (s->kind) {
            case ST_ST_LABEL: {
                ST_sym_t *prev = ST_sym_find_in(labels, s->label);
                if (prev) {
                    ST_diag_error(&se->diag, s->line, s->col, "duplicate label '" ST_sv_fmt "'",
                                  ST_sv_args(s->label));
                    ST_diag_note(&se->diag, prev->line, prev->col, "previous label is here");
                    break;
                }
                ST_sym_insert(se, labels,
                              ST_sym_new(se, ST_SYM_VAR, s->label, NULL, NULL, s->line, s->col));
                break;
            }
            case ST_ST_IF:
                ST_collect_labels(se, labels, &s->if_.then_body);
                if (s->if_.else_stmt) {
                    ST_stmts_t one = {.items = &s->if_.else_stmt, .count = 1};
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
                ST_stmts_t one = {.items = &s->defer_stmt, .count = 1};
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
            case ST_ST_ASM:
                break;
            case ST_ST_COUNT:
                ST_assert(0);
                break;
        }
    }
}

static ST_sym_kind_t ST_decl_sym_kind(ST_decl_t *d) {
    switch (d->kind) {
        case ST_DE_STRUCT:
        case ST_DE_ENUM:
        case ST_DE_TAG_UNION:
            return ST_SYM_TYPE;
        case ST_DE_FN:
        case ST_DE_EXTERN_FN:
            return ST_SYM_FN;
        case ST_DE_CONST:
            return ST_SYM_CONST;
        case ST_DE_EXTERN_VAR:
            return ST_SYM_EXTERN_VAR;
        case ST_DE_GLOBAL:
            return ST_SYM_GLOBAL;
        case ST_DE_IMPORT:
            return ST_SYM_MODULE;
        case ST_DE_COUNT:
            ST_assert(0);
            break;
    }
    return ST_SYM_VAR;
}

// Pass 1: register every top-level name.
static void ST_sema_collect(ST_sema_t *se, ST_program_t *prog) {
    ST_forrange(0, prog->decls.count) {
        ST_decl_t *d = prog->decls.items[i];
        if (!d)
            continue;
        b8 is_generic_struct = d->kind == ST_DE_STRUCT && d->struct_.generics.count;
        b8 is_generic_fn = d->kind == ST_DE_FN && d->fn.sig.generics.count;

        if (is_generic_struct || is_generic_fn) {
            ST_sym_t *prev = ST_sym_find_in(&se->templates, d->name);
            if (prev) {
                ST_diag_error(&se->diag, d->line, d->col, "redefinition of '" ST_sv_fmt "'",
                              ST_sv_args(d->name));
                ST_diag_note(&se->diag, prev->line, prev->col, "previous definition is here");
                continue;
            }
            ST_sym_insert(se, &se->templates,
                          ST_sym_new(se, is_generic_fn ? ST_SYM_FN : ST_SYM_TYPE,
                                     d->name, d, NULL, d->line, d->col));

            if (is_generic_fn)
                continue;
        }
        ST_sym_t *prev = ST_sym_find_in(&se->globals, d->name);
        if (prev) {
            b8 prev_proto =
                prev->decl && prev->decl->kind == ST_DE_FN && prev->decl->fn.is_prototype;
            b8 cur_proto = d->kind == ST_DE_FN && d->fn.is_prototype;
            b8 both_fn = prev->decl && prev->decl->kind == ST_DE_FN && d->kind == ST_DE_FN;
            if (both_fn && (prev_proto ^ cur_proto)) {
                if (prev_proto) {
                    prev->decl = d;
                    prev->line = d->line;
                    prev->col = d->col;
                }
                continue;
            }
            // Multiple files (e.g. several imported modules) independently
            // declaring the same 'extern fn'/'extern var' is normal -- it's
            // just re-announcing the same real external symbol, not a
            // conflicting definition. Keep the first and skip re-inserting,
            // as long as the shapes actually match.
            if (prev->decl && prev->decl->kind == d->kind &&
                (d->kind == ST_DE_EXTERN_FN || d->kind == ST_DE_EXTERN_VAR)) {
                b8 same_shape = 1;
                if (d->kind == ST_DE_EXTERN_FN) {
                    ST_fn_sig_t *a = &prev->decl->extern_fn.sig, *b = &d->extern_fn.sig;
                    same_shape = a->params.count == b->params.count &&
                                 a->rets.count == b->rets.count && a->is_variadic == b->is_variadic;
                }
                if (same_shape)
                    continue;
                ST_diag_error(&se->diag, d->line, d->col,
                              "redeclaration of extern '" ST_sv_fmt
                              "' doesn't match its previous declaration",
                              ST_sv_args(d->name));
                ST_diag_note(&se->diag, prev->line, prev->col, "previous declaration is here");
                continue;
            }
            ST_diag_error(&se->diag, d->line, d->col, "redefinition of '" ST_sv_fmt "'",
                          ST_sv_args(d->name));
            ST_diag_note(&se->diag, prev->line, prev->col, "previous definition is here");
            continue;
        }
        ST_sym_insert(se, &se->globals,
                      ST_sym_new(se, ST_decl_sym_kind(d), d->name, d, NULL, d->line, d->col));
    }
}

static void ST_check_dup_fields(ST_sema_t *se, ST_decl_t *d) {
    ST_forrange(0, d->struct_.fields.count) {
        ST_field_spec_t *f = &d->struct_.fields.items[i];
        for (u32 k = 0; k < i; k++)
            if (ST_string_eq(d->struct_.fields.items[k].name, f->name) && f->name.len) {
                ST_diag_error(&se->diag, f->line, f->col,
                              "duplicate field '" ST_sv_fmt "' in struct '" ST_sv_fmt "'",
                              ST_sv_args(f->name), ST_sv_args(d->name));
                ST_diag_note(&se->diag, d->struct_.fields.items[k].line,
                             d->struct_.fields.items[k].col, "previous field is here");
                break;
            }
    }
}

// Builds the ST_ty_t for a function signature and attaches it to the symbol.
static void ST_build_fn_ty(ST_sema_t *se, ST_sym_t *sym, ST_fn_sig_t *sig) {
    ST_ty_t *t = ST_ty_fn_new(&se->tys);
    t->is_variadic = sig->is_variadic;
    t->has_any_pack = sig->has_any_pack;
    ST_forrange(0, sig->params.count) {
        ST_param_t *p = &sig->params.items[i];
        ST_ty_t *pt = p->te ? ST_resolve_tyexpr(se, p->te) : NULL;
        if (!pt && p->def)
            pt = ST_ty_defaulted(se, ST_type_expr(se, p->def));
        else if (p->def) {
            ST_ty_t *dt = ST_type_expr(se, p->def);
            if (pt && dt && !ST_ty_coerces(se, dt, pt))
                ST_diag_error(&se->diag, p->line, p->col,
                              "default for '" ST_sv_fmt "' expects '%s', got '%s'",
                              ST_sv_args(p->name), ST_tstr(se, pt), ST_tstr(se, dt));
        }
        ST_da_append_arena(se->arena, &t->params, pt);
    }
    ST_forrange(0, sig->rets.count) {
        ST_ty_t *rt = ST_resolve_tyexpr(se, sig->rets.items[i]);
        if (rt && rt->kind == ST_TY_VOID)
            continue;
        ST_da_append_arena(se->arena, &t->rets, rt);
    }
    sym->t = t;
}

// Assigns a constant integer value to every variant of an 'enum'/'enum_flag'
// declaration. Regular enums start at 0 and increment by 1, with any variant
// free to override its value with '= expr'. 'enum_flag' enums are bit flags:
// only the first variant may specify a value ('= expr', defaulting to 1 if
// omitted); every following variant is that base value shifted left by its
// index (1, 2, 4, 8, ...) and may NOT specify its own value.
static void ST_sema_enum_values(ST_sema_t *se, ST_decl_t *d) {
    ST_variant_specs_t *vs = &d->enum_.variants;
    if (d->enum_.is_flag) {
        i64 base = 1;
        if (vs->count && vs->items[0].value) {
            if (!ST_const_eval(se, vs->items[0].value, &base)) {
                ST_diag_error(&se->diag, vs->items[0].line, vs->items[0].col,
                              "value for enum_flag variant '" ST_sv_fmt
                              "' must be a compile-time constant",
                              ST_sv_args(vs->items[0].name));
                base = 1;
            }
        }
        ST_forrange(0, vs->count) {
            ST_variant_spec_t *v = &vs->items[i];
            if (i != 0 && v->value) {
                ST_diag_error(&se->diag, v->line, v->col,
                              "'" ST_sv_fmt "' cannot have an explicit value: only the "
                              "first variant of an enum_flag may be assigned",
                              ST_sv_args(v->name));
            }
            v->computed = i == 0 ? base : (base << i);
            v->has_computed = 1;
        }
        return;
    }
    i64 next = 0;
    ST_forrange(0, vs->count) {
        ST_variant_spec_t *v = &vs->items[i];
        if (v->value) {
            if (!ST_const_eval(se, v->value, &next)) {
                ST_diag_error(&se->diag, v->line, v->col,
                              "value for enum variant '" ST_sv_fmt
                              "' must be a compile-time constant",
                              ST_sv_args(v->name));
                next = i == 0 ? 0 : vs->items[i - 1].computed + 1;
            }
        }
        v->computed = next;
        v->has_computed = 1;
        next++;
        for (u32 k = 0; k < i; k++)
            if (vs->items[k].computed == v->computed) {
                ST_diag_error(&se->diag, v->line, v->col,
                              "enum variant '" ST_sv_fmt "' has the same value as '" ST_sv_fmt "'",
                              ST_sv_args(v->name), ST_sv_args(vs->items[k].name));
                break;
            }
    }
}

// Pass 2: make types for type declarations, lay them out, build signatures,
// and type constants and extern variables.
static void ST_sema_types(ST_sema_t *se, ST_program_t *prog) {
    ST_forrange(0, prog->decls.count) {
        ST_decl_t *d = prog->decls.items[i];
        if (!d)
            continue;
        ST_sym_t *sym = ST_sym_find_in(&se->globals, d->name);
        if (!sym || sym->decl != d)
            continue; // redefinition, already reported
        switch (d->kind) {
            case ST_DE_STRUCT:
                ST_check_dup_fields(se, d);
                sym->t = ST_ty_for_decls(&se->tys, d);
                break;
            case ST_DE_ENUM:
            case ST_DE_TAG_UNION:
                sym->t = ST_ty_for_decls(&se->tys, d);
                break;
            case ST_DE_CONST:
            case ST_DE_EXTERN_FN:
            case ST_DE_EXTERN_VAR:
            case ST_DE_GLOBAL:
            case ST_DE_FN:
            case ST_DE_IMPORT:
                break;
            case ST_DE_COUNT:
                ST_assert(0);
                break;
        }
    }

    // layout after every type name is known, so structs can reference each
    // other
    for (u32 i = 0; i < prog->decls.count; i++) {
        ST_decl_t *d = prog->decls.items[i];
        if (!d)
            continue;
        ST_sym_t *sym = ST_sym_find_in(&se->globals, d->name);
        if (!sym || sym->decl != d)
            continue;
        switch (d->kind) {
            case ST_DE_STRUCT:
                if (d->struct_.generics.count)
                    break;
                ST_complete_ty(se, sym->t);
                break;
            case ST_DE_TAG_UNION:
                ST_complete_ty(se, sym->t);
                break;
            case ST_DE_ENUM:
                ST_sema_enum_values(se, d); // fixed 8-byte layout, values assigned here
                break;
            case ST_DE_CONST:
                ST_ty_of_const(se, sym);
                break;
            case ST_DE_EXTERN_VAR:
                sym->t = ST_resolve_tyexpr(se, d->extern_var.te);
                break;
            case ST_DE_EXTERN_FN:
                ST_build_fn_ty(se, sym, &d->extern_fn.sig);
                break;
            case ST_DE_FN:
                ST_build_fn_ty(se, sym, &d->fn.sig);
                break;
            case ST_DE_GLOBAL: {
                ST_ty_t *dt = d->global_.te ? ST_resolve_tyexpr(se, d->global_.te) : NULL;
                if (dt)
                    ST_complete_ty(se, dt);
                ST_ty_t *it = NULL;
                if (d->global_.init)
                    it = ST_type_expr(se, d->global_.init);
                if (!dt && !it) {
                    ST_diag_error(&se->diag, d->line, d->col,
                                  "'" ST_sv_fmt "' needs a type or an initializer",
                                  ST_sv_args(d->name));
                    break;
                }
                sym->t = dt ? dt : ST_ty_defaulted(se, it);
                if (sym->t && sym->t->kind == ST_TY_VOID)
                    ST_diag_error(&se->diag, d->line, d->col,
                                  "cannot declare '" ST_sv_fmt "' of type 'void'",
                                  ST_sv_args(d->name));
                if (sym->t && d->global_.init) {
                    if (dt && it && !ST_ty_coerces(se, it, dt))
                        ST_diag_error(&se->diag, d->global_.init->line, d->global_.init->col,
                                      "global '" ST_sv_fmt "' expects '%s', got '%s'",
                                      ST_sv_args(d->name), ST_tstr(se, dt), ST_tstr(se, it));
                    else if (!ST_ty_is_float(sym->t)) {
                        i64 iv;
                        if (!ST_const_eval(se, d->global_.init, &iv))
                            ST_diag_error(&se->diag, d->global_.init->line, d->global_.init->col,
                                          "initializer for global '" ST_sv_fmt "' must be a "
                                          "compile-time constant",
                                          ST_sv_args(d->name));
                    }
                }
                break;
            }
            case ST_DE_IMPORT:
                break;
            case ST_DE_COUNT:
                ST_assert(0);
                break;
        }
    }
}

static void ST_check_fn_body(ST_sema_t *se, ST_sym_t *sym, ST_decl_t *d) {
    ST_fn_sig_t *sig = &d->fn.sig;
    ST_ty_t *fnty = sym->t;

    ST_ht_t *save_bindings = se->generic_bindings;
    b8 save_stamp = se->stamp_tyexprs;
    if (sym->generic_bindings) {
        se->generic_bindings = sym->generic_bindings;
        se->stamp_tyexprs = 1;
    }

    ST_ht_t labels;
    ST_ht_init(se->arena, &labels, 8);
    se->labels = &labels;
    if (!d->fn.is_prototype)
        ST_collect_labels(se, &labels, &d->fn.body);

    b8 save_has_pack = se->has_pack;
    ST_string_t save_pack_name = se->cur_pack_name;
    u32 save_pack_count = se->cur_pack_count;
    b8 save_has_bound_str = se->has_bound_str;
    ST_string_t save_bound_str_param = se->cur_bound_str_param;
    ST_string_t save_bound_str_value = se->cur_bound_str_value;

    se->has_bound_str = d->fn.has_bound_str;
    se->cur_bound_str_param = d->fn.bound_str_param;
    se->cur_bound_str_value = d->fn.bound_str_value;
    if (se->has_bound_str && !d->fn.is_prototype) {
        ST_expr_t lit = {0};
        lit.kind = ST_EX_STR;
        lit.sval = d->fn.bound_str_value;
        lit.line = d->line;
        lit.col = d->col;
        ST_forrange(0, d->fn.body.count)
            ST_ast_substitute_stmt(se, d->fn.body.items[i], d->fn.bound_str_param, &lit);
    }

    se->has_pack = d->fn.had_pack;
    se->cur_pack_name = d->fn.pack_name;
    se->cur_pack_count = d->fn.pack_count;
    if (se->has_pack && !d->fn.is_prototype)
        ST_pack_substitute_body(se, &d->fn.body);

    ST_scope_push(se);
    ST_forrange(0, sig->params.count) {
        ST_param_t *p = &sig->params.items[i];
        ST_ty_t *pt = fnty && i < fnty->params.count ? fnty->params.items[i] : NULL;
        ST_declare_local(se, p->name, pt, p->line, p->col);
    }

    se->cur_rets = fnty ? &fnty->rets : NULL;
    if (!d->fn.is_prototype)
        ST_forrange(0, d->fn.body.count) ST_check_stmt(se, d->fn.body.items[i]);
    se->cur_rets = NULL;

    ST_scope_pop(se);
    se->labels = NULL;

    se->has_pack = save_has_pack;
    se->cur_pack_name = save_pack_name;
    se->cur_pack_count = save_pack_count;
    se->has_bound_str = save_has_bound_str;
    se->cur_bound_str_param = save_bound_str_param;
    se->cur_bound_str_value = save_bound_str_value;
    se->stamp_tyexprs = save_stamp;
    se->generic_bindings = save_bindings;
}

// Pass 3: walk every function body with the full typed environment.
static void ST_sema_check(ST_sema_t *se, ST_program_t *prog) {
    ST_forrange(0, prog->decls.count) {
        ST_decl_t *d = prog->decls.items[i];
        if (!d)
            continue;
        ST_sym_t *sym = ST_sym_find_in(&se->globals, d->name);
        if (!sym || sym->decl != d)
            continue;
        if (d->kind == ST_DE_FN)
            ST_check_fn_body(se, sym, d);
    }
}

// Pass 4 Typechecking

static void ST_default_expr(ST_sema_t *se, ST_expr_t *e);
static void ST_default_exprs(ST_sema_t *se, ST_exprs_t *es) {
    ST_forrange(0, es->count) ST_default_expr(se, es->items[i]);
}

static void ST_default_body(ST_sema_t *se, ST_stmts_t *body);
static void ST_default_stmt(ST_sema_t *se, ST_stmt_t *s) {
    if (!s)
        return;
    switch (s->kind) {
        case ST_ST_EXPR:
            ST_default_expr(se, s->expr);
            break;
        case ST_ST_DECL:
            ST_default_expr(se, s->decl.init);
            break;
        case ST_ST_ASSIGN:
            ST_default_expr(se, s->assign.lhs);
            ST_default_expr(se, s->assign.rhs);
            break;
        case ST_ST_MULTI_BIND:
            ST_default_exprs(se, &s->multi.values);
            break;
        case ST_ST_IF:
            ST_default_expr(se, s->if_.cond);
            ST_default_body(se, &s->if_.then_body);
            ST_default_stmt(se, s->if_.else_stmt);
            break;
        case ST_ST_SWITCH:
            ST_default_expr(se, s->switch_.cond);
            ST_forrange(0, s->switch_.cases.count) {
                ST_case_t *c = &s->switch_.cases.items[i];
                ST_default_exprs(se, &c->values);
                ST_default_body(se, &c->body);
            }
            break;
        case ST_ST_WHILE:
            ST_default_expr(se, s->while_.cond);
            ST_default_body(se, &s->while_.body);
            break;
        case ST_ST_FOR_RANGE:
            ST_default_expr(se, s->for_range.lo);
            ST_default_expr(se, s->for_range.hi);
            ST_default_body(se, &s->for_range.body);
            break;
        case ST_ST_FOR_ARRAY:
            ST_default_expr(se, s->for_array.target);
            ST_default_body(se, &s->for_array.body);
            break;
        case ST_ST_RETURN:
            ST_default_exprs(se, &s->ret.values);
            break;
        case ST_ST_BLOCK:
            ST_default_body(se, &s->block);
            break;
        case ST_ST_DEFER:
            ST_default_stmt(se, s->defer_stmt);
            break;
        case ST_ST_BREAK:
        case ST_ST_CONTINUE:
        case ST_ST_LABEL:
        case ST_ST_GODOWN:
        case ST_ST_ASM:
            break;
        case ST_ST_COUNT:
            ST_assert(0);
            break;
    }
}

static void ST_default_body(ST_sema_t *se, ST_stmts_t *body) {
    ST_forrange(0, body->count) ST_default_stmt(se, body->items[i]);
}

static void ST_default_expr(ST_sema_t *se, ST_expr_t *e) {
    if (!e)
        return;
    e->ty = ST_ty_defaulted(se, e->ty);
    switch (e->kind) {
        case ST_EX_INT:
        case ST_EX_FLOAT:
        case ST_EX_STR:
        case ST_EX_CHAR:
        case ST_EX_BOOL:
        case ST_EX_NULL:
        case ST_EX_IDENT:
        case ST_EX_ARRAY_NEW:
        case ST_EX_SIZEOF:
        case ST_EX_ASM:
            break;
        case ST_EX_STR_FROM_RAW:
            ST_default_expr(se, e->str_from_raw.ptr);
            ST_default_expr(se, e->str_from_raw.len);
            break;
        case ST_EX_UNARY:
            ST_default_expr(se, e->unary.operand);
            break;
        case ST_EX_BINARY:
            ST_default_expr(se, e->bin.l);
            ST_default_expr(se, e->bin.r);
            break;
        case ST_EX_CALL:
            ST_default_expr(se, e->call.callee);
            ST_forrange(0, e->call.args.count) ST_default_expr(se, e->call.args.items[i].value);
            break;
        case ST_EX_FIELD:
            ST_default_expr(se, e->field.base);
            break;
        case ST_EX_INDEX:
            ST_default_expr(se, e->index.base);
            ST_default_expr(se, e->index.index);
            break;
        case ST_EX_CAST:
            ST_default_expr(se, e->cast.operand);
            break;
        case ST_EX_STRUCT_LIT:
            ST_forrange(0, e->struct_lit.inits.count)
                ST_default_expr(se, e->struct_lit.inits.items[i].value);
            break;
        case ST_EX_TYPEOF:
        case ST_EX_KIND:
        case ST_EX_CSTR:
        case ST_EX_FIELDS:
            ST_default_expr(se, e->tyop.operand);
            break;
        case ST_EX_TYPEINFO:
            if (!e->tyop.te)
                ST_default_expr(se, e->tyop.operand);
            break;
        case ST_EX_COMP_ERROR:
            ST_forrange(0, e->comp_error.args.count) ST_default_expr(se, e->comp_error.args.items[i]);
            break;
        case ST_EX_COUNT:
            ST_assert(0);
            break;
    }
}

static void ST_sema_default_types(ST_sema_t *se, ST_program_t *prog) {
    ST_forrange(0, prog->decls.count) {
        ST_decl_t *d = prog->decls.items[i];
        if (!d)
            continue;
        if (d->kind == ST_DE_FN && d->fn.sig.generics.count)
                continue;

        switch (d->kind) {
            case ST_DE_CONST:
                ST_default_expr(se, d->const_.value);
                break;
            case ST_DE_FN:
                ST_forrange(0, d->fn.sig.params.count)
                    ST_default_expr(se, d->fn.sig.params.items[i].def);
                if (!d->fn.is_prototype)
                    ST_default_body(se, &d->fn.body);
                break;
            case ST_DE_EXTERN_FN:
                ST_forrange(0, d->extern_fn.sig.params.count)
                    ST_default_expr(se, d->extern_fn.sig.params.items[i].def);
            case ST_DE_STRUCT:
            case ST_DE_ENUM:
            case ST_DE_TAG_UNION:
            case ST_DE_EXTERN_VAR:
            case ST_DE_IMPORT:
            case ST_DE_COUNT:
                break;
            case ST_DE_GLOBAL:
                if (d->global_.init)
                    ST_default_expr(se, d->global_.init);
                break;
        }
    }
}

b8 ST_sema_run(ST_arena_t *arena, ST_program_t *prog, ST_string_t src, ST_string_t file,
               ST_sema_t *out) {
    ST_sema_t *se = out;
    *se = (ST_sema_t){0};
    se->arena = arena;
    se->diag.src = src;
    se->diag.file = file;
    se->diag.max_errors = ST_SEMA_MAX_ERRORS;
    ST_ht_init(arena, &se->globals, 64);
    ST_ht_init(arena, &se->templates, 16);
    ST_ht_init(arena, &se->instantiations, 16);
    ST_ht_init(arena, &se->fn_instantiations, 16);
    ST_ht_init(arena, &se->inst_info, 16);
    se->prog = prog;
    ST_ty_ctx_init(&se->tys, arena);
    ST_forrange(0, ST_array_len(ST_builtin_fns)) {
        ST_string_t name = ST_cstr_to_str((char *)ST_builtin_fns[i]);
        ST_sym_insert(se, &se->globals, ST_sym_new(se, ST_SYM_FN, name, NULL, NULL, 0, 0));
    }
    ST_sema_collect(se, prog);
    ST_sema_types(se, prog);
    ST_sema_check(se, prog);
    ST_sema_default_types(se, prog);
    return se->diag.n_errors == 0;
}
