#include "st_module.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../utils/st_string.h"
#include "st_lexer.h"
#include "st_load.h"
#include "st_parser.h"

// ---------------------------------------------------------------------
// Small path helpers (mirrors the equivalents used by '#load' in
// st_load.c, which are file-local there and not worth exposing).
// ---------------------------------------------------------------------

static char *ST_mod_cstr(ST_arena_t *a, ST_string_t s) {
    char *buf = ST_arena_push(a, s.len + 1);
    memcpy(buf, s.data, s.len);
    buf[s.len] = 0;
    return buf;
}

static ST_string_t ST_mod_dirname(ST_string_t path) {
    u32 i = path.len;
    while (i > 0 && path.data[i - 1] != '/')
        i--;
    if (i == 0)
        return ST_cstr_to_str(".");
    return (ST_string_t){.data = path.data, .len = i - 1};
}

static ST_string_t ST_mod_join(ST_arena_t *a, const char *dir, ST_string_t rel) {
    u32 dlen = (u32)strlen(dir);
    u32 total = dlen + 1 + rel.len;
    u8 *buf = ST_arena_push(a, total + 1);
    memcpy(buf, dir, dlen);
    buf[dlen] = '/';
    memcpy(buf + dlen + 1, rel.data, rel.len);
    buf[total] = 0;
    return (ST_string_t){.data = buf, .len = total};
}

// Tries '<dir>/modules/<name>/module.st', then '$STORTHC_MODULE_PATH/<name>/module.st',
// then '/usr/local/storthc/modules/<name>/module.st'. Returns an absolute,
// NUL-terminated-backed path on success.
static b8 ST_mod_resolve(ST_arena_t *arena, ST_string_t importer_dir, ST_string_t name,
                         ST_string_t *out_path) {
    ST_string_t rel = {0};
    {
        u32 total = 8 + name.len + 10; // "modules/" + name + "/module.st"
        u8 *buf = ST_arena_push(arena, total + 1);
        u32 n = 0;
        memcpy(buf + n, "modules/", 8);
        n += 8;
        memcpy(buf + n, name.data, name.len);
        n += name.len;
        memcpy(buf + n, "/module.st", 10);
        n += 10;
        buf[n] = 0;
        rel = (ST_string_t){.data = buf, .len = n};
    }

    ST_string_t candidate = ST_mod_join(arena, ST_mod_cstr(arena, importer_dir), rel);
    if (access(ST_mod_cstr(arena, candidate), F_OK) == 0) {
        *out_path = ST_abs_path(arena, ST_mod_cstr(arena, candidate));
        return 1;
    }

    const char *env = getenv("STORTHC_MODULE_PATH");
    if (env && *env) {
        candidate = ST_mod_join(arena, env, rel);
        if (access(ST_mod_cstr(arena, candidate), F_OK) == 0) {
            *out_path = ST_abs_path(arena, ST_mod_cstr(arena, candidate));
            return 1;
        }
    }

    candidate = ST_mod_join(arena, "/usr/local/storthc/modules", (ST_string_t){0});
    // ST_mod_join expects a single 'rel' path; build the final candidate by
    // hand here since 'name' still needs '/module.st' appended.
    {
        const char *base = "/usr/local/storthc/modules";
        u32 blen = (u32)strlen(base);
        u32 total = blen + 1 + name.len + 10;
        u8 *buf = ST_arena_push(arena, total + 1);
        u32 n = 0;
        memcpy(buf + n, base, blen);
        n += blen;
        buf[n++] = '/';
        memcpy(buf + n, name.data, name.len);
        n += name.len;
        memcpy(buf + n, "/module.st", 10);
        n += 10;
        buf[n] = 0;
        candidate = (ST_string_t){.data = buf, .len = n};
    }
    if (access(ST_mod_cstr(arena, candidate), F_OK) == 0) {
        *out_path = ST_abs_path(arena, ST_mod_cstr(arena, candidate));
        return 1;
    }

    return 0;
}

// ---------------------------------------------------------------------
// Per-module bookkeeping.
// ---------------------------------------------------------------------

typedef struct {
    ST_string_t canon_path;
    ST_string_t prefix;
    b8 in_progress;
    b8 ok; // false if this module failed to load/process
    ST_ht_t all_decls; // bare name -> ST_string_t* mangled name (every decl)
    ST_ht_t exports;   // bare name -> ST_string_t* mangled name (pub only)
} ST_module_entry_t;

typedef struct {
    ST_arena_t *arena;
    ST_diag_t *diag;
    ST_srcmap_t *srcs;
    ST_ht_t modules; // canonical path -> ST_module_entry_t*
    ST_string_t project_root; // directory containing the root file's 'modules/'
} ST_module_ctx_t;

static ST_ht_generic_t ST_mod_key(ST_string_t s) {
    return (ST_ht_generic_t){.tag = s.data, .size = s.len};
}

static void ST_mod_map_put(ST_arena_t *a, ST_ht_t *ht, ST_string_t key, ST_string_t value) {
    ST_string_t *v = ST_arena_push(a, sizeof(*v));
    *v = value;
    ST_ht_generic_t *hk = ST_arena_push(a, sizeof(*hk));
    hk->tag = key.data;
    hk->size = key.len;
    ST_ht_set(ht, hk, (ST_ht_generic_t){.tag = v, .size = 0});
}

static ST_string_t ST_mangle(ST_arena_t *a, ST_string_t prefix, ST_string_t name) {
    u32 total = prefix.len + 2 + name.len;
    u8 *buf = ST_arena_push(a, total);
    memcpy(buf, prefix.data, prefix.len);
    buf[prefix.len] = '_';
    buf[prefix.len + 1] = '_';
    memcpy(buf + prefix.len + 2, name.data, name.len);
    return (ST_string_t){.data = buf, .len = total};
}

// Sentinel value for an auto-exposed name that turned out ambiguous (two or
// more imported modules export the same bare name). It stays present in the
// map -- rather than being removed -- specifically so the lookup in
// ST_modrw_expr can tell "not exposed by anything" apart from "exposed by
// more than one thing, must be qualified", without a second table.
#define ST_MOD_AMBIGUOUS ((ST_string_t *)1)

// Merges one freshly-resolved import's public exports into the importing
// file's shared 'auto_exposed' map, so a name unique across all of that
// file's imports can be used bare (no 'alias.' prefix needed), while a name
// exported by more than one import falls back to requiring qualification
// rather than silently picking whichever import happened to be processed
// first.
static void ST_mod_merge_exports(ST_arena_t *arena, ST_ht_t *auto_exposed,
                                 ST_module_entry_t *dep) {
    ST_forrange(0, dep->exports.capacity) {
        ST_ht_generic_t *k = dep->exports.slots[i].key;
        if (!k || k == (ST_ht_generic_t *)1)
            continue; // empty slot or tombstone
        ST_string_t bare = {.data = k->tag, .len = k->size};
        ST_string_t *mangled = (ST_string_t *)dep->exports.slots[i].value->tag;

        ST_ht_generic_t existing = ST_ht_get(auto_exposed, ST_mod_key(bare));
        if (existing.tag == (void *)ST_MOD_AMBIGUOUS)
            continue; // already known-ambiguous, stays that way
        if (existing.tag && existing.tag != (void *)mangled) {
            // Second (or later) module exporting this same bare name --
            // mark ambiguous instead of overwriting with whichever export
            // happens to be seen now.
            ST_ht_generic_t *hk = ST_arena_push(arena, sizeof(*hk));
            hk->tag = bare.data;
            hk->size = bare.len;
            ST_ht_set(auto_exposed, hk, (ST_ht_generic_t){.tag = ST_MOD_AMBIGUOUS, .size = 0});
            continue;
        }
        if (existing.tag)
            continue; // same module re-seen (e.g. diamond import), no-op
        ST_ht_generic_t *hk = ST_arena_push(arena, sizeof(*hk));
        hk->tag = bare.data;
        hk->size = bare.len;
        ST_ht_set(auto_exposed, hk, (ST_ht_generic_t){.tag = mangled, .size = 0});
    }
}

// ---------------------------------------------------------------------
// Rewrite pass: applies a module's self-renames (bare top-level name ->
// mangled name) and cross-module rewrites ('alias.member' -> mangled
// ident) to every expression/statement/type-expr it contains. Mirrors the
// shape of the clone_* walkers in st_semantic.c, but mutates in place.
// ---------------------------------------------------------------------

typedef struct {
    ST_module_ctx_t *ctx;
    ST_ht_t *self_renames;   // bare name -> ST_string_t* (this file's own decls)
    ST_ht_t *import_aliases; // alias -> ST_module_entry_t*
    ST_ht_t *auto_exposed;   // bare name -> ST_string_t* mangled, or ST_MOD_AMBIGUOUS
} ST_module_rw_t;

static void ST_modrw_expr(ST_module_rw_t *rw, ST_expr_t *e);
static void ST_modrw_stmt(ST_module_rw_t *rw, ST_stmt_t *s);

static void ST_modrw_tyexpr(ST_module_rw_t *rw, ST_tyexpr_t *te) {
    if (!te)
        return;
    if (te->kind == ST_TE_NAME) {
        ST_ht_generic_t r = ST_ht_get(rw->self_renames, ST_mod_key(te->name));
        if (r.tag)
            te->name = *(ST_string_t *)r.tag;
    }
    ST_modrw_tyexpr(rw, te->inner);
    ST_modrw_expr(rw, te->count_expr);
    ST_modrw_expr(rw, te->typeof_operand);
    ST_forrange(0, te->fn_params.count) ST_modrw_tyexpr(rw, te->fn_params.items[i]);
    ST_forrange(0, te->fn_rets.count) ST_modrw_tyexpr(rw, te->fn_rets.items[i]);
    ST_forrange(0, te->generic_args.count) ST_modrw_tyexpr(rw, te->generic_args.items[i]);
}

static void ST_modrw_body(ST_module_rw_t *rw, ST_stmts_t *body) {
    ST_forrange(0, body->count) ST_modrw_stmt(rw, body->items[i]);
}

static void ST_modrw_expr(ST_module_rw_t *rw, ST_expr_t *e) {
    if (!e)
        return;
    switch (e->kind) {
        case ST_EX_INT:
        case ST_EX_FLOAT:
        case ST_EX_STR:
        case ST_EX_CHAR:
        case ST_EX_BOOL:
        case ST_EX_NULL:
            break;

        case ST_EX_IDENT: {
            ST_ht_generic_t r = ST_ht_get(rw->self_renames, ST_mod_key(e->name));
            if (r.tag) {
                e->name = *(ST_string_t *)r.tag;
                break;
            }
            if (rw->auto_exposed) {
                ST_ht_generic_t a = ST_ht_get(rw->auto_exposed, ST_mod_key(e->name));
                if (a.tag && a.tag != (void *)ST_MOD_AMBIGUOUS)
                    e->name = *(ST_string_t *)a.tag;
            }
            break;
        }

        case ST_EX_UNARY:
            ST_modrw_expr(rw, e->unary.operand);
            break;

        case ST_EX_BINARY:
            ST_modrw_expr(rw, e->bin.l);
            ST_modrw_expr(rw, e->bin.r);
            break;

        case ST_EX_CALL:
            ST_modrw_expr(rw, e->call.callee);
            ST_forrange(0, e->call.args.count) ST_modrw_expr(rw, e->call.args.items[i].value);
            break;

        case ST_EX_FIELD: {
            ST_expr_t *base = e->field.base;
            if (base->kind == ST_EX_IDENT) {
                ST_ht_generic_t mr = ST_ht_get(rw->import_aliases, ST_mod_key(base->name));
                if (mr.tag) {
                    ST_module_entry_t *dep = mr.tag;
                    ST_ht_generic_t exp = ST_ht_get(&dep->exports, ST_mod_key(e->field.name));
                    if (!exp.tag) {
                        ST_ht_generic_t any = ST_ht_get(&dep->all_decls, ST_mod_key(e->field.name));
                        if (any.tag)
                            ST_diag_error(rw->ctx->diag, e->line, e->col,
                                          "'" ST_sv_fmt "' is private in module '" ST_sv_fmt
                                          "' (missing 'pub')",
                                          ST_sv_args(e->field.name), ST_sv_args(base->name));
                        else
                            ST_diag_error(rw->ctx->diag, e->line, e->col,
                                          "module '" ST_sv_fmt "' has no member '" ST_sv_fmt "'",
                                          ST_sv_args(base->name), ST_sv_args(e->field.name));
                        break;
                    }
                    e->kind = ST_EX_IDENT;
                    e->name = *(ST_string_t *)exp.tag;
                    break;
                }
            }
            ST_modrw_expr(rw, e->field.base);
            break;
        }

        case ST_EX_INDEX:
            ST_modrw_expr(rw, e->index.base);
            ST_modrw_expr(rw, e->index.index);
            break;

        case ST_EX_CAST:
            ST_modrw_expr(rw, e->cast.operand);
            ST_modrw_tyexpr(rw, e->cast.to);
            break;

        case ST_EX_STRUCT_LIT: {
            ST_ht_generic_t r = ST_ht_get(rw->self_renames, ST_mod_key(e->struct_lit.type_name));
            if (r.tag)
                e->struct_lit.type_name = *(ST_string_t *)r.tag;
            ST_forrange(0, e->struct_lit.generic_args.count)
                ST_modrw_tyexpr(rw, e->struct_lit.generic_args.items[i]);
            ST_forrange(0, e->struct_lit.inits.count)
                ST_modrw_expr(rw, e->struct_lit.inits.items[i].value);
            break;
        }

        case ST_EX_ARRAY_NEW:
            ST_modrw_tyexpr(rw, e->array_new.te);
            break;

        case ST_EX_SIZEOF:
        case ST_EX_TYPEOF:
        case ST_EX_TYPEINFO:
        case ST_EX_KIND:
        case ST_EX_CSTR:
            ST_modrw_tyexpr(rw, e->tyop.te);
            ST_modrw_expr(rw, e->tyop.operand);
            break;

        case ST_EX_COUNT:
            break;
    }
}

static void ST_modrw_stmt(ST_module_rw_t *rw, ST_stmt_t *s) {
    if (!s)
        return;
    switch (s->kind) {
        case ST_ST_EXPR:
            ST_modrw_expr(rw, s->expr);
            break;
        case ST_ST_DECL:
            ST_modrw_tyexpr(rw, s->decl.te);
            ST_modrw_expr(rw, s->decl.init);
            break;
        case ST_ST_ASSIGN:
            ST_modrw_expr(rw, s->assign.lhs);
            ST_modrw_expr(rw, s->assign.rhs);
            break;
        case ST_ST_MULTI_BIND:
            ST_forrange(0, s->multi.values.count) ST_modrw_expr(rw, s->multi.values.items[i]);
            break;
        case ST_ST_IF:
            ST_modrw_expr(rw, s->if_.cond);
            ST_modrw_body(rw, &s->if_.then_body);
            ST_modrw_stmt(rw, s->if_.else_stmt);
            break;
        case ST_ST_SWITCH:
            ST_modrw_expr(rw, s->switch_.cond);
            ST_forrange(0, s->switch_.cases.count) {
                ST_case_t *c = &s->switch_.cases.items[i];
                for (u32 k = 0; k < c->values.count; k++) ST_modrw_expr(rw, c->values.items[k]);
                ST_modrw_body(rw, &c->body);
            }
            break;
        case ST_ST_WHILE:
            ST_modrw_expr(rw, s->while_.cond);
            ST_modrw_body(rw, &s->while_.body);
            break;
        case ST_ST_FOR_RANGE:
            ST_modrw_expr(rw, s->for_range.lo);
            ST_modrw_expr(rw, s->for_range.hi);
            ST_modrw_tyexpr(rw, s->for_range.iter_te);
            ST_modrw_body(rw, &s->for_range.body);
            break;
        case ST_ST_FOR_ARRAY:
            ST_modrw_expr(rw, s->for_array.target);
            ST_modrw_body(rw, &s->for_array.body);
            break;
        case ST_ST_RETURN:
            ST_forrange(0, s->ret.values.count) ST_modrw_expr(rw, s->ret.values.items[i]);
            break;
        case ST_ST_BLOCK:
            ST_modrw_body(rw, &s->block);
            break;
        case ST_ST_DEFER:
            ST_modrw_stmt(rw, s->defer_stmt);
            break;
        case ST_ST_BREAK:
        case ST_ST_CONTINUE:
        case ST_ST_LABEL:
        case ST_ST_GODOWN:
        case ST_ST_ASM:
            break;
        case ST_ST_COUNT:
            break;
    }
}

static void ST_modrw_decl(ST_module_rw_t *rw, ST_decl_t *d) {
    switch (d->kind) {
        case ST_DE_STRUCT:
            ST_forrange(0, d->struct_.fields.count) {
                ST_field_spec_t *f = &d->struct_.fields.items[i];
                ST_modrw_tyexpr(rw, f->te);
                if (f->anon)
                    ST_modrw_decl(rw, f->anon);
            }
            break;
        case ST_DE_ENUM:
            ST_modrw_tyexpr(rw, d->enum_.ty);
            ST_forrange(0, d->enum_.variants.count) ST_modrw_expr(rw, d->enum_.variants.items[i].value);
            break;
        case ST_DE_TAG_UNION:
            ST_forrange(0, d->tag_union.variants.count)
                ST_modrw_tyexpr(rw, d->tag_union.variants.items[i].payload);
            break;
        case ST_DE_CONST:
            ST_modrw_tyexpr(rw, d->const_.te);
            ST_modrw_expr(rw, d->const_.value);
            break;
        case ST_DE_EXTERN_FN:
            ST_forrange(0, d->extern_fn.sig.params.count) {
                ST_modrw_tyexpr(rw, d->extern_fn.sig.params.items[i].te);
                ST_modrw_expr(rw, d->extern_fn.sig.params.items[i].def);
            }
            ST_forrange(0, d->extern_fn.sig.rets.count) ST_modrw_tyexpr(rw, d->extern_fn.sig.rets.items[i]);
            break;
        case ST_DE_EXTERN_VAR:
            ST_modrw_tyexpr(rw, d->extern_var.te);
            break;
        case ST_DE_GLOBAL:
            ST_modrw_tyexpr(rw, d->global_.te);
            ST_modrw_expr(rw, d->global_.init);
            break;
        case ST_DE_FN:
            ST_forrange(0, d->fn.sig.params.count) {
                ST_modrw_tyexpr(rw, d->fn.sig.params.items[i].te);
                ST_modrw_expr(rw, d->fn.sig.params.items[i].def);
            }
            ST_forrange(0, d->fn.sig.rets.count) ST_modrw_tyexpr(rw, d->fn.sig.rets.items[i]);
            if (!d->fn.is_prototype)
                ST_modrw_body(rw, &d->fn.body);
            break;
        case ST_DE_IMPORT:
        case ST_DE_COUNT:
            break;
    }
}

// ---------------------------------------------------------------------
// Load + recursively process one file (root or imported module).
// ---------------------------------------------------------------------

static b8 ST_module_load_file(ST_module_ctx_t *ctx, ST_string_t path, ST_program_t *out) {
    ctx->diag->file = path;

    // ST_load_file both reads 'path' and recursively expands any '#load
    // "sibling.st";' directives reachable from it into one flat token
    // stream, the same way the root file is loaded -- calling ST_lex
    // directly here (as this used to) skips that expansion entirely, so a
    // module.st using '#load' would have the raw '#load' token reach the
    // parser unexpanded and get rejected as an unsupported directive.
    ST_tokens_t toks;
    if (!ST_load_file(ctx->arena, path, ctx->srcs, &toks)) {
        ST_diag_error(ctx->diag, 0, 0, "could not load module file '" ST_sv_fmt "'",
                      ST_sv_args(path));
        return 0;
    }

    // ST_load_file doesn't hand back 'path's own source text (only the
    // expanded tokens, registering every file it touched into ctx->srcs
    // along the way) -- read it separately here just for diag/parse
    // bookkeeping; per-token diagnostics resolve their own source via each
    // token's own .file against ctx->srcs regardless of what's passed here.
    ST_string_t src;
    if (!ST_read_entire_file(ctx->arena, &src, ST_mod_cstr(ctx->arena, path))) {
        ST_diag_error(ctx->diag, 0, 0, "could not read module file '" ST_sv_fmt "'",
                      ST_sv_args(path));
        return 0;
    }

    u32 save_n = ctx->diag->n_errors;
    ctx->diag->src = src;
    ctx->diag->file = path;
    b8 ok = ST_parse(ctx->arena, toks, src, path, ctx->srcs, out);
    return ok && ctx->diag->n_errors == save_n;
}

static b8 ST_module_process_file(ST_module_ctx_t *ctx, ST_string_t path, ST_string_t prefix,
                                 b8 is_root, ST_decls_t *merged, ST_module_entry_t **out_entry) {
    ST_ht_generic_t existing = ST_ht_get(&ctx->modules, ST_mod_key(path));
    if (existing.tag) {
        ST_module_entry_t *e = existing.tag;
        if (e->in_progress) {
            ST_diag_error(ctx->diag, 0, 0,
                          "circular import: '" ST_sv_fmt "' imports itself (directly or "
                          "transitively)",
                          ST_sv_args(path));
            return 0;
        }
        if (out_entry)
            *out_entry = e;
        return e->ok;
    }

    ST_module_entry_t *entry = ST_arena_push_zeroed(ctx->arena, sizeof(*entry));
    entry->canon_path = path;
    entry->prefix = prefix;
    entry->in_progress = 1;
    ST_ht_init(ctx->arena, &entry->all_decls, 8);
    ST_ht_init(ctx->arena, &entry->exports, 8);
    {
        ST_ht_generic_t *hk = ST_arena_push(ctx->arena, sizeof(*hk));
        hk->tag = path.data;
        hk->size = path.len;
        ST_ht_set(&ctx->modules, hk, (ST_ht_generic_t){.tag = entry, .size = 0});
    }
    if (out_entry)
        *out_entry = entry;

    ST_program_t prog = {0};
    if (!ST_module_load_file(ctx, path, &prog)) {
        entry->in_progress = 0;
        entry->ok = 0;
        return 0;
    }

    // Pass 1: register this file's own top-level names (self-renames), and
    // resolve+recursively process every '#import' it makes (dependencies
    // are fully loaded and renamed before we rewrite our own references).
    // Note: resolution always searches from the *project root*
    // (ctx->project_root), not this module's own directory -- otherwise a
    // module importing another module would look under
    // '<its own dir>/modules/<name>/', nesting modules inside modules.
    ST_ht_t self_renames;
    ST_ht_init(ctx->arena, &self_renames, 16);
    ST_ht_t import_aliases;
    ST_ht_init(ctx->arena, &import_aliases, 8);
    ST_ht_t auto_exposed;
    ST_ht_init(ctx->arena, &auto_exposed, 16);

    b8 ok = 1;
    ST_forrange(0, prog.decls.count) {
        ST_decl_t *d = prog.decls.items[i];
        if (!d)
            continue;
        if (d->kind == ST_DE_IMPORT) {
            ST_string_t dep_path;
            if (!ST_mod_resolve(ctx->arena, ctx->project_root, d->import_.module_name, &dep_path)) {
                ST_diag_error(ctx->diag, d->line, d->col,
                              "cannot find module '" ST_sv_fmt "' (looked in "
                              "'<file dir>/modules/" ST_sv_fmt "/module.st', "
                              "$STORTHC_MODULE_PATH, and "
                              "/usr/local/storthc/modules/" ST_sv_fmt "/module.st)",
                              ST_sv_args(d->import_.module_name), ST_sv_args(d->import_.module_name),
                              ST_sv_args(d->import_.module_name));
                ok = 0;
                continue;
            }
            ST_module_entry_t *dep = NULL;
            b8 dep_ok = ST_module_process_file(ctx, dep_path, d->import_.module_name, 0, merged,
                                               &dep);
            if (!dep_ok || !dep) {
                ok = 0;
                continue;
            }
            ST_ht_generic_t *hk = ST_arena_push(ctx->arena, sizeof(*hk));
            hk->tag = d->import_.alias.data;
            hk->size = d->import_.alias.len;
            ST_ht_set(&import_aliases, hk, (ST_ht_generic_t){.tag = dep, .size = 0});
            ST_mod_merge_exports(ctx->arena, &auto_exposed, dep);
            continue;
        }
        if (!is_root) {
            b8 is_extern = d->kind == ST_DE_EXTERN_FN || d->kind == ST_DE_EXTERN_VAR;
            // extern fn/var names are real external linkage symbols (e.g.
            // libc's 'printf'), not ours to rename -- mangling them would
            // break the link. Map them to themselves instead: internal
            // references stay literal, and 'alias.printf' from an importer
            // resolves straight to the real symbol too.
            ST_string_t mangled = is_extern ? d->name : ST_mangle(ctx->arena, prefix, d->name);
            ST_mod_map_put(ctx->arena, &self_renames, d->name, mangled);
            ST_mod_map_put(ctx->arena, &entry->all_decls, d->name, mangled);
            if (d->is_pub)
                ST_mod_map_put(ctx->arena, &entry->exports, d->name, mangled);
        }
    }

    // Pass 2: rewrite every decl's internals (self-renames + alias.member),
    // then rename the decl itself and append it to the merged program.
    ST_module_rw_t rw = {.ctx = ctx, .self_renames = &self_renames, .import_aliases = &import_aliases, .auto_exposed = &auto_exposed};
    ctx->diag->src = ST_srcmap_get(ctx->srcs, path);
    ctx->diag->file = path;
    ST_forrange(0, prog.decls.count) {
        ST_decl_t *d = prog.decls.items[i];
        if (!d || d->kind == ST_DE_IMPORT)
            continue;
        ST_modrw_decl(&rw, d);
        if (!is_root) {
            ST_ht_generic_t r = ST_ht_get(&self_renames, ST_mod_key(d->name));
            if (r.tag)
                d->name = *(ST_string_t *)r.tag;
        }
        ST_da_append_arena(ctx->arena, merged, d);
    }

    entry->in_progress = 0;
    entry->ok = ok && ctx->diag->n_errors == 0;
    return entry->ok;
}

b8 ST_modules_process(ST_arena_t *arena, ST_program_t *root_prog, ST_string_t root_file,
                      ST_srcmap_t *srcs, ST_diag_t *diag) {
    b8 has_import = 0;
    ST_forrange(0, root_prog->decls.count) if (root_prog->decls.items[i] &&
                                               root_prog->decls.items[i]->kind == ST_DE_IMPORT) {
        has_import = 1;
        break;
    }
    if (!has_import)
        return 1;

    ST_string_t root_dir = ST_mod_dirname(root_file);
    ST_module_ctx_t ctx = {.arena = arena, .diag = diag, .srcs = srcs, .project_root = root_dir};
    ST_ht_init(arena, &ctx.modules, 8);

    // The root file is already loaded/parsed by the caller; process it
    // in-place through the same machinery (prefix "" and is_root=1 mean no
    // self-renaming), reusing 'root_prog->decls' as the source and building
    // a fresh merged list that replaces it.
    ST_decls_t merged = {0};
    ST_ht_t self_renames, import_aliases, auto_exposed;
    ST_ht_init(arena, &self_renames, 1);
    ST_ht_init(arena, &import_aliases, 8);
    ST_ht_init(arena, &auto_exposed, 16);

    b8 ok = 1;
    ST_forrange(0, root_prog->decls.count) {
        ST_decl_t *d = root_prog->decls.items[i];
        if (!d || d->kind != ST_DE_IMPORT)
            continue;
        ST_string_t dep_path;
        if (!ST_mod_resolve(arena, ctx.project_root, d->import_.module_name, &dep_path)) {
            ST_diag_error(diag, d->line, d->col,
                          "cannot find module '" ST_sv_fmt "' (looked in "
                          "'<file dir>/modules/" ST_sv_fmt "/module.st', "
                          "$STORTHC_MODULE_PATH, and "
                          "/usr/local/storthc/modules/" ST_sv_fmt "/module.st)",
                          ST_sv_args(d->import_.module_name), ST_sv_args(d->import_.module_name),
                          ST_sv_args(d->import_.module_name));
            ok = 0;
            continue;
        }
        ST_module_entry_t *dep = NULL;
        b8 dep_ok = ST_module_process_file(&ctx, dep_path, d->import_.module_name, 0, &merged, &dep);
        if (!dep_ok || !dep) {
            ok = 0;
            continue;
        }
        ST_ht_generic_t *hk = ST_arena_push(arena, sizeof(*hk));
        hk->tag = d->import_.alias.data;
        hk->size = d->import_.alias.len;
        ST_ht_set(&import_aliases, hk, (ST_ht_generic_t){.tag = dep, .size = 0});
        ST_mod_merge_exports(arena, &auto_exposed, dep);
    }

    ST_module_rw_t rw = {.ctx = &ctx, .self_renames = &self_renames, .import_aliases = &import_aliases, .auto_exposed = &auto_exposed};
    diag->src = ST_srcmap_get(srcs, root_file);
    diag->file = root_file;
    ST_forrange(0, root_prog->decls.count) {
        ST_decl_t *d = root_prog->decls.items[i];
        if (!d || d->kind == ST_DE_IMPORT)
            continue;
        ST_modrw_decl(&rw, d);
        ST_da_append_arena(arena, &merged, d);
    }

    root_prog->decls = merged;
    return ok && diag->n_errors == 0;
}
