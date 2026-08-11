#include <stdarg.h>
#include <string.h>

#include "st_parser.h"

static ST_token_t *ST_tok_at(ST_parser_t *p, u32 i) {
    return i < p->n_tokens ? &p->tokens[i] : NULL;
}

static ST_token_t *ST_peek(ST_parser_t *p) {
    return ST_tok_at(p, p->pos);
}
static ST_token_t *ST_peek2(ST_parser_t *p) {
    return ST_tok_at(p, p->pos + 1);
}

static b8 ST_tok_is_symbol(ST_token_t *t, const char *s) {
    return t && t->kind == ST_TSYMBOL && ST_string_eq_cstr(t->text, s);
}

static b8 ST_tok_is_keyword(ST_token_t *t, const char *s) {
    return t && t->kind == ST_TKEYWORD && ST_string_eq_cstr(t->text, s);
}

static b8 ST_tok_is_ident(ST_token_t *t) {
    return t && t->kind == ST_TIDENT;
}

static b8 ST_at_symbol(ST_parser_t *p, const char *s) {
    return ST_tok_is_symbol(ST_peek(p), s);
}

static b8 ST_at_keyword(ST_parser_t *p, const char *s) {
    return ST_tok_is_keyword(ST_peek(p), s);
}

static b8 ST_at_then(ST_parser_t *p) {
    ST_token_t *t = ST_peek(p);
    return t && t->kind == ST_TIDENT && ST_string_eq_cstr(t->text, "then");
}

static b8 ST_tok_is_compound_assign(ST_token_t *t) {
    static const char *ops[] = {"+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=", ">>="};
    if (!t || t->kind != ST_TSYMBOL)
        return 0;
    ST_forrange(0, ST_array_len(ops)) if (ST_string_eq_cstr(t->text, ops[i])) return 1;
    return 0;
}

static u32 ST_cur_line(ST_parser_t *p) {
    ST_token_t *t = ST_peek(p);
    if (t)
        return t->line;
    return p->n_tokens ? p->tokens[p->n_tokens - 1].line : 1;
}

static u32 ST_cur_col(ST_parser_t *p) {
    ST_token_t *t = ST_peek(p);
    if (t)
        return t->col;
    return p->n_tokens ? p->tokens[p->n_tokens - 1].col : 1;
}

static ST_string_t ST_src_line(ST_string_t src, u32 line) {
    u32 cur = 1, start = 0;
    ST_forrange(0, src.len) {
        if (cur == line) {
            start = i;
            u32 end = i;
            while (end < src.len && src.data[end] != '\n')
                end++;
            return (ST_string_t){.data = src.data + start, .len = end - start};
        }
        if (src.data[i] == '\n')
            cur++;
    }
    return (ST_string_t){0};
}

static void ST_snippet_src(ST_string_t src, u32 line, u32 col) {
    for (u32 l = line > 2 ? line - 2 : 1; l <= line; l++) {
        ST_string_t text = ST_src_line(src, l);
        if (l == line) {
            fprintf(stderr, ST_COLOR_BOLD_RED "%4u |" ST_COLOR_RESET " ", l);
            fprintf(stderr, ST_COLOR_BOLD_RED ST_sv_fmt ST_COLOR_RESET "\n", ST_sv_args(text));
            fprintf(stderr, ST_COLOR_BOLD ST_COLOR_BLUE "     |" ST_COLOR_RESET " ");
            ST_forrange(0, col > 0 ? col - 1 : 0) fputc(' ', stderr);
            fprintf(stderr, ST_COLOR_CYAN "^" ST_COLOR_RESET "\n");
        } else {
            fprintf(stderr, ST_COLOR_DIM ST_COLOR_BLUE "%4u |" ST_COLOR_RESET " ", l);
            fprintf(stderr, ST_COLOR_WHITE ST_sv_fmt ST_COLOR_RESET "\n", ST_sv_args(text));
        }
    }
}

static void ST_perr_full(ST_parser_t *p, ST_string_t file, ST_string_t src, u32 line, u32 col,
                         const char *fmt, va_list ap) {
    p->n_errors++;
    if (p->suppress_errors)
        return; // speculative attempt; caller will backtrack, don't print
    if (p->n_errors > ST_PARSE_MAX_ERRORS)
        return;
    if (p->n_errors == ST_PARSE_MAX_ERRORS) {
        fprintf(stderr, ST_COLOR_BOLD "too many errors, stopping\n" ST_COLOR_RESET);
        return;
    }
    fprintf(stderr,
            ST_COLOR_BOLD ST_sv_fmt ":%u:%u: " ST_COLOR_BOLD_RED
                                    "error: " ST_COLOR_RESET ST_COLOR_BOLD,
            ST_sv_args(file), line, col);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, ST_COLOR_RESET "\n");
    ST_snippet_src(src, line, col);
}

static void ST_perr_tok_at(ST_parser_t *p, ST_string_t file, u32 line, u32 col, const char *fmt,
                           ...) {
    ST_string_t src = p->srcs ? ST_srcmap_get(p->srcs, file) : (ST_string_t){0};
    if (!src.data)
        src = p->src;
    va_list ap;
    va_start(ap, fmt);
    ST_perr_full(p, file, src, line, col, fmt, ap);
    va_end(ap);
}

static void ST_perr(ST_parser_t *p, u32 line, u32 col, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    ST_perr_full(p, p->file, p->src, line, col, fmt, ap);
    va_end(ap);
}

static void ST_perr_tok(ST_parser_t *p, ST_token_t *t, const char *fmt, ...) {
    ST_string_t src = p->srcs ? ST_srcmap_get(p->srcs, t->file) : (ST_string_t){0};
    if (!src.data)
        src = p->src;
    va_list ap;
    va_start(ap, fmt);
    ST_perr_full(p, t->file, src, t->line, t->col, fmt, ap);
    va_end(ap);
}

static void ST_perr_here(ST_parser_t *p, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char buf[512];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    ST_token_t *t = ST_peek(p);
    if (t)
        ST_perr_tok(p, t, "%s", buf);
    else
        ST_perr(p, ST_cur_line(p), ST_cur_col(p), "%s", buf);
}

static void ST_sync_stmt(ST_parser_t *p) {
    i32 depth = 0;
    while (p->pos < p->n_tokens) {
        ST_token_t *t = ST_peek(p);
        if (depth == 0 && ST_tok_is_symbol(t, ";")) {
            p->pos++;
            return;
        }
        if (ST_tok_is_symbol(t, "{"))
            depth++;
        else if (ST_tok_is_symbol(t, "}")) {
            if (depth == 0)
                return;
            depth--;
        }
        p->pos++;
    }
}

static void ST_sync_decl(ST_parser_t *p) {
    i32 depth = 0;
    while (p->pos < p->n_tokens) {
        ST_token_t *t = ST_peek(p);
        if (depth == 0 && (ST_tok_is_keyword(t, "fn") || ST_tok_is_keyword(t, "struct") ||
                           ST_tok_is_keyword(t, "enum") || ST_tok_is_keyword(t, "enum_flag") ||
                           ST_tok_is_keyword(t, "tag_union") || ST_tok_is_keyword(t, "extern") ||
                           ST_tok_is_keyword(t, "const") || ST_tok_is_keyword(t, "pub")))
            return;
        if (ST_tok_is_symbol(t, "{"))
            depth++;
        else if (ST_tok_is_symbol(t, "}") && depth > 0)
            depth--;
        p->pos++;
    }
}

static b8 ST_expect_sym(ST_parser_t *p, const char *s) {
    if (ST_at_symbol(p, s)) {
        p->pos++;
        return 1;
    }
    ST_token_t *t = ST_peek(p);
    if (t)
        ST_perr_tok(p, t, "expected '%s', got '" ST_sv_fmt "'", s, ST_sv_args(t->text));
    else
        ST_perr(p, ST_cur_line(p), ST_cur_col(p), "expected '%s', got end of file", s);
    return 0;
}

static b8 ST_expect_semi(ST_parser_t *p) {
    if (ST_at_symbol(p, ";")) {
        p->pos++;
        return 1;
    }
    ST_token_t *prev = p->pos > 0 ? ST_tok_at(p, p->pos - 1) : NULL;
    if (prev)
        ST_perr_tok_at(p, prev->file, prev->line, prev->col + prev->text.len,
                       "expected ';' after statement");
    else
        ST_perr(p, ST_cur_line(p), ST_cur_col(p), "expected ';' after statement");
    return 0;
}

static ST_string_t ST_expect_ident(ST_parser_t *p, const char *what) {
    ST_token_t *t = ST_peek(p);
    if (ST_tok_is_ident(t)) {
        p->pos++;
        return t->text;
    }
    ST_perr_here(p, "expected %s", what);
    return (ST_string_t){0};
}

static ST_expr_t *ST_parse_expr(ST_parser_t *p);
static ST_stmt_t *ST_parse_stmt(ST_parser_t *p);
static b8 ST_parse_asm_tokens(ST_parser_t *p, ST_token_t **out_toks, u32 *out_n);
static b8 ST_parse_body(ST_parser_t *p, ST_stmts_t *out);

static ST_tyexpr_t *ST_parse_type(ST_parser_t *p) {
    ST_token_t *t = ST_peek(p);
    if (!t) {
        ST_perr_here(p, "expected a type, got end of file");
        return NULL;
    }
    if (ST_tok_is_keyword(t, "type_of")) {
        p->pos++;
        if (!ST_expect_sym(p, "("))
            return NULL;
        ST_tyexpr_t *te = ST_tyexpr_new(p->arena, ST_TE_TYPEOF, t->line, t->col);
        te->typeof_operand = ST_parse_expr(p);
        if (!te->typeof_operand)
            return NULL;

        if (!ST_expect_sym(p, ")"))
            return NULL;
        return te;
    }
    if (ST_tok_is_symbol(t, "*")) {
        p->pos++;
        ST_tyexpr_t *te = ST_tyexpr_new(p->arena, ST_TE_PTR, t->line, t->col);
        te->inner = ST_parse_type(p);
        return te->inner ? te : NULL;
    }
    if (ST_tok_is_symbol(t, "[")) {
        p->pos++;
        ST_tyexpr_t *te = ST_tyexpr_new(p->arena, ST_TE_ARRAY, t->line, t->col);
        if (ST_at_symbol(p, "..")) {
            p->pos++;
            te->is_dynamic = 1;
        } else if (!ST_at_symbol(p, "]")) {
            te->count_expr = ST_parse_expr(p);
            if (!te->count_expr)
                return NULL;
        }
        if (!ST_expect_sym(p, "]"))
            return NULL;
        te->inner = ST_parse_type(p);
        return te->inner ? te : NULL;
    }
    if (ST_tok_is_keyword(t, "fn")) {
        p->pos++;
        ST_tyexpr_t *te = ST_tyexpr_new(p->arena, ST_TE_FN, t->line, t->col);
        if (!ST_expect_sym(p, "("))
            return NULL;
        while (!ST_at_symbol(p, ")") && p->pos < p->n_tokens) {
            if (ST_at_symbol(p, ",")) {
                p->pos++;
                continue;
            }
            if (ST_at_symbol(p, "..") || ST_at_symbol(p, "...")) {
                p->pos++;
                if (ST_at_symbol(p, "."))
                    p->pos++;
                te->fn_is_variadic = 1;
                continue;
            }
            if (!ST_expect_ident(p, "a parameter name").len)
                return NULL;
            if (!ST_expect_sym(p, ":"))
                return NULL;
            ST_tyexpr_t *pte = ST_parse_type(p);
            if (!pte)
                return NULL;
            ST_da_append_arena(p->arena, &te->fn_params, pte);
        }
        if (!ST_expect_sym(p, ")"))
            return NULL;
        if (ST_at_symbol(p, "->")) {
            p->pos++;
            if (ST_at_symbol(p, "(")) {
                p->pos++;
                while (!ST_at_symbol(p, ")") && p->pos < p->n_tokens) {
                    if (ST_at_symbol(p, ",")) {
                        p->pos++;
                        continue;
                    }
                    ST_tyexpr_t *rte = ST_parse_type(p);
                    if (!rte)
                        return NULL;
                    ST_da_append_arena(p->arena, &te->fn_rets, rte);
                }
                if (!ST_expect_sym(p, ")"))
                    return NULL;
            } else {
                ST_tyexpr_t *rte = ST_parse_type(p);
                if (!rte)
                    return NULL;
                ST_da_append_arena(p->arena, &te->fn_rets, rte);
            }
        }
        return te;
    }
    if (ST_tok_is_symbol(t, "$")) {
        p->pos++;
        ST_string_t pname = ST_expect_ident(p, "a generic parameter name after '$'");
        if (!pname.len)
            return NULL;

        ST_tyexpr_t *te = ST_tyexpr_new(p->arena, ST_TE_NAME, t->line, t->col);
        te->name = pname;
        te->is_generic_param = 1;
        return te;
    }
    if (t->kind == ST_TTYPE || ST_tok_is_ident(t)) {
        p->pos++;
        if (ST_at_symbol(p, "(")) {
            ST_tyexpr_t *te = ST_tyexpr_new(p->arena, ST_TE_GENERIC_INST, t->line, t->col);
            te->name = t->text;
            p->pos++;

            while (!ST_at_symbol(p, ")") && p->pos < p->n_tokens) {
                if (ST_at_symbol(p, ",")) {
                    p->pos++;
                    continue;
                }
                ST_tyexpr_t *arg = ST_parse_type(p);
                if (!arg)
                    return NULL;
                ST_da_append_arena(p->arena, &te->generic_args, arg);
            }
            if (!ST_expect_sym(p, ")"))
                return NULL;

            if (te->generic_args.count == 0) {
                ST_perr_tok(p, t, "'" ST_sv_fmt " ()' needs at least one type argument",
                            ST_sv_args(t->text));
                return NULL;
            }
            return te;
        }

        ST_tyexpr_t *te = ST_tyexpr_new(p->arena, ST_TE_NAME, t->line, t->col);
        te->name = t->text;
        return te;
    }
    ST_perr_tok(p, t, "expected a type, got '" ST_sv_fmt "'", ST_sv_args(t->text));
    return NULL;
}

static ST_tyexpr_t *ST_try_type(ST_parser_t *p) {
    u32 save_pos = p->pos, save_err = p->n_errors;
    ST_token_t *t = ST_peek(p);
    if (!t)
        return NULL;
    if (t->kind != ST_TTYPE && !ST_tok_is_ident(t) && !ST_tok_is_symbol(t, "*") &&
        !ST_tok_is_symbol(t, "[") && !ST_tok_is_symbol(t, "$") && !ST_tok_is_keyword(t, "fn"))
        return NULL;
    p->suppress_errors++;
    ST_tyexpr_t *te = ST_parse_type(p);
    p->suppress_errors--;
    if (!te || p->n_errors != save_err) {
        p->pos = save_pos;
        p->n_errors = save_err;
        return NULL;
    }
    return te;
}

static ST_expr_t *ST_parse_struct_lit(ST_parser_t *p, ST_string_t type_name,
                                      ST_tyexprs_t generic_args, u32 line, u32 col) {
    ST_expr_t *e = ST_expr_new(p->arena, ST_EX_STRUCT_LIT, line, col);
    e->struct_lit.type_name = type_name;
    e->struct_lit.generic_args = generic_args;
    while (!ST_at_symbol(p, "}") && p->pos < p->n_tokens) {
        if (ST_at_symbol(p, ",")) {
            p->pos++;
            continue;
        }
        ST_token_t *ft = ST_peek(p);
        ST_field_init_t fi = {.line = ft->line, .col = ft->col};
        if (ST_tok_is_ident(ft) && (ST_tok_is_symbol(ST_peek2(p), ":") ||
                                    (ST_tok_is_symbol(ST_peek2(p), "=") &&
                                     !ST_tok_is_symbol(ST_tok_at(p, p->pos + 2), "=")))) {
            fi.name = ft->text;
            p->pos += 2;
        }
        fi.value = ST_parse_expr(p);
        if (!fi.value)
            return NULL;
        ST_da_append_arena(p->arena, &e->struct_lit.inits, fi);
    }
    if (!ST_expect_sym(p, "}"))
        return NULL;
    return e;
}

static b8 ST_parse_call_args(ST_parser_t *p, ST_args_t *out) {
    while (!ST_at_symbol(p, ")") && p->pos < p->n_tokens) {
        if (ST_at_symbol(p, ",")) {
            p->pos++;
            continue;
        }
        ST_arg_t arg = {0};
        ST_token_t *t = ST_peek(p);
        if (ST_tok_is_ident(t) && ST_tok_is_symbol(ST_peek2(p), "=") &&
            !ST_tok_is_symbol(ST_tok_at(p, p->pos + 2), "=")) {
            arg.name = t->text;
            p->pos += 2;
        }
        arg.value = ST_parse_expr(p);
        if (!arg.value)
            return 0;
        ST_da_append_arena(p->arena, out, arg);
    }
    return ST_expect_sym(p, ")");
}

static ST_expr_t *ST_parse_primary(ST_parser_t *p) {
    ST_token_t *t = ST_peek(p);
    if (!t) {
        ST_perr_here(p, "unexpected end of file in expression");
        return NULL;
    }

    switch (t->kind) {
        case ST_TINT: {
            p->pos++;
            ST_expr_t *e = ST_expr_new(p->arena, ST_EX_INT, t->line, t->col);
            e->ival = t->val.i;
            return e;
        }
        case ST_TFLOAT: {
            p->pos++;
            ST_expr_t *e = ST_expr_new(p->arena, ST_EX_FLOAT, t->line, t->col);
            e->fval = t->val.f;
            return e;
        }
        case ST_TSTRING: {
            p->pos++;
            ST_expr_t *e = ST_expr_new(p->arena, ST_EX_STR, t->line, t->col);
            e->sval = t->str;
            return e;
        }
        case ST_TCHAR: {
            p->pos++;
            ST_expr_t *e = ST_expr_new(p->arena, ST_EX_CHAR, t->line, t->col);
            e->ival = (i64)t->val.c;
            return e;
        }
        case ST_TIDENT:
        case ST_TTYPE:
        case ST_TKEYWORD:
        case ST_TSYMBOL:
        case ST_TDOCCOMENT:
        case ST_TCOUNT:
            break;
    }

    if (ST_tok_is_keyword(t, "true") || ST_tok_is_keyword(t, "false")) {
        p->pos++;
        ST_expr_t *e = ST_expr_new(p->arena, ST_EX_BOOL, t->line, t->col);
        e->ival = ST_string_eq_cstr(t->text, "true");
        return e;
    }
    if (ST_tok_is_keyword(t, "null")) {
        p->pos++;
        return ST_expr_new(p->arena, ST_EX_NULL, t->line, t->col);
    }

    if (ST_tok_is_keyword(t, "sizeof") ||
        (ST_tok_is_ident(t) && ST_string_eq_cstr(t->text, "align_of") &&
         ST_tok_is_symbol(ST_peek2(p), "("))) {
        b8 is_align = ST_tok_is_ident(t);
        p->pos++;
        if (!ST_expect_sym(p, "("))
            return NULL;
        ST_expr_t *e = ST_expr_new(p->arena, ST_EX_SIZEOF, t->line, t->col);
        e->tyop.is_align = is_align;
        e->tyop.te = ST_parse_type(p);
        if (!e->tyop.te)
            return NULL;
        if (!ST_expect_sym(p, ")"))
            return NULL;
        return e;
    }

    if (ST_tok_is_keyword(t, "type_of")) {
        p->pos++;
        if (!ST_expect_sym(p, "("))
            return NULL;
        ST_expr_t *e = ST_expr_new(p->arena, ST_EX_TYPEOF, t->line, t->col);
        e->tyop.operand = ST_parse_expr(p);
        if (!e->tyop.operand)
            return NULL;
        if (!ST_expect_sym(p, ")"))
            return NULL;
        return e;
    }

    if (ST_tok_is_keyword(t, "type_info")) {
        p->pos++;
        if (!ST_expect_sym(p, "("))
            return NULL;
        ST_expr_t *e = ST_expr_new(p->arena, ST_EX_TYPEINFO, t->line, t->col);
        u32 save = p->pos;
        ST_tyexpr_t *te = ST_try_type(p);
        if (te && ST_at_symbol(p, ")") && te->kind != ST_TE_NAME)
            e->tyop.te = te;
        else if (te && ST_at_symbol(p, ")") && te->kind == ST_TE_NAME &&
                 ST_tok_at(p, save)->kind == ST_TTYPE)
            e->tyop.te = te;
        else {
            p->pos = save;
            e->tyop.operand = ST_parse_expr(p);
            if (!e->tyop.operand)
                return NULL;
        }
        if (!ST_expect_sym(p, ")"))
            return NULL;
        return e;
    }

    if (ST_tok_is_symbol(t, "#asm")) {
        p->pos++;
        ST_expr_t *e = ST_expr_new(p->arena, ST_EX_ASM, t->line, t->col);
        if (!ST_parse_asm_tokens(p, &e->asm_.tokens, &e->asm_.n_tokens))
            return NULL;
        return e;
    }

    if (ST_tok_is_symbol(t, "#comp_error")) {
        p->pos++;
        if (!ST_expect_sym(p, "("))
            return NULL;
        ST_expr_t *e = ST_expr_new(p->arena, ST_EX_COMP_ERROR, t->line, t->col);
        if (!ST_at_symbol(p, ")")) {
            for (;;) {
                ST_expr_t *arg = ST_parse_expr(p);
                if (!arg)
                    return NULL;
                ST_da_append_arena(p->arena, &e->comp_error.args, arg);
                if (ST_at_symbol(p, ",")) {
                    p->pos++;
                    continue;
                }
                break;
            }
        }
        if (!ST_expect_sym(p, ")"))
            return NULL;
        return e;
    }

    if (ST_tok_is_ident(t) && ST_string_eq_cstr(t->text, "str_from_raw") &&
        ST_tok_is_symbol(ST_peek2(p), "(")) {
        p->pos += 2;
        ST_expr_t *e = ST_expr_new(p->arena, ST_EX_STR_FROM_RAW, t->line, t->col);
        e->str_from_raw.ptr = ST_parse_expr(p);
        if (!e->str_from_raw.ptr)
            return NULL;
        if (!ST_expect_sym(p, ","))
            return NULL;
        e->str_from_raw.len = ST_parse_expr(p);
        if (!e->str_from_raw.len)
            return NULL;
        if (!ST_expect_sym(p, ")"))
            return NULL;
        return e;
    }

    if ((ST_tok_is_ident(t) && ST_tok_is_symbol(ST_peek2(p), "(") &&
         (ST_string_eq_cstr(t->text, "kind") || ST_string_eq_cstr(t->text, "cstr") ||
          ST_string_eq_cstr(t->text, "fields"))) ||
        (ST_tok_is_symbol(t, "#fields") && ST_tok_is_symbol(ST_peek2(p), "("))) {
        ST_expr_kind_t kind = ST_string_eq_cstr(t->text, "kind")   ? ST_EX_KIND
                              : ST_string_eq_cstr(t->text, "cstr") ? ST_EX_CSTR
                                                                    : ST_EX_FIELDS;
        p->pos += 2;
        ST_expr_t *e = ST_expr_new(p->arena, kind, t->line, t->col);
        e->tyop.operand = ST_parse_expr(p);
        if (!e->tyop.operand)
            return NULL;
        if (!ST_expect_sym(p, ")"))
            return NULL;
        return e;
    }

    if (ST_tok_is_symbol(t, "[")) {
        ST_expr_t *e = ST_expr_new(p->arena, ST_EX_ARRAY_NEW, t->line, t->col);
        e->array_new.te = ST_parse_type(p);
        if (!e->array_new.te)
            return NULL;
        if (e->array_new.te->kind != ST_TE_ARRAY) {
            ST_perr_tok(p, t, "expected an array type like [4]i64 or [..]i64");
            return NULL;
        }
        return e;
    }

    if (ST_tok_is_ident(t) || t->kind == ST_TTYPE) {
        p->pos++;
        if (ST_at_symbol(p, "{") && !p->no_struct_lit) {
            p->pos++;
            return ST_parse_struct_lit(p, t->text, (ST_tyexprs_t){0}, t->line, t->col);
        }

        if (ST_at_symbol(p, "(") && !p->no_struct_lit) {
            u32 save_pos = p->pos, save_err = p->n_errors;
            p->pos++;
            ST_tyexprs_t gargs = {0};
            b8 ok = 1;
            p->suppress_errors++;
            while (ok && !ST_at_symbol(p, ")") && p->pos < p->n_tokens) {
                if (ST_at_symbol(p, ",")) {
                    p->pos++;
                    continue;
                }
                ST_tyexpr_t *arg = ST_try_type(p);
                if (!arg) {
                    ok = 0;
                    break;
                }
                ST_da_append_arena(p->arena, &gargs, arg);
            }
            p->suppress_errors--;
            if (ok && gargs.count && ST_at_symbol(p, ")") && ST_tok_is_symbol(ST_peek2(p), "{")) {
                p->pos++; // )
                p->pos++; // }
                return ST_parse_struct_lit(p, t->text, gargs, t->line, t->col);
            }
            p->pos = save_pos;
            p->n_errors = save_err;
        }
        ST_expr_t *e = ST_expr_new(p->arena, ST_EX_IDENT, t->line, t->col);
        e->name = t->text;
        return e;
    }

    if (ST_tok_is_symbol(t, "{")) {
        p->pos++;
        return ST_parse_struct_lit(p, (ST_string_t){0}, (ST_tyexprs_t){0}, t->line, t->col);
    }

    if (ST_tok_is_symbol(t, "(")) {
        p->pos++;
        u32 save = p->no_struct_lit;
        p->no_struct_lit = 0;
        ST_expr_t *e = ST_parse_expr(p);
        p->no_struct_lit = save;
        if (!e)
            return NULL;
        if (!ST_expect_sym(p, ")"))
            return NULL;
        return e;
    }

    ST_perr_tok(p, t, "unexpected '" ST_sv_fmt "'; expected an expression", ST_sv_args(t->text));
    return NULL;
}

static ST_expr_t *ST_parse_postfix(ST_parser_t *p) {
    ST_expr_t *e = ST_parse_primary(p);
    if (!e)
        return NULL;
    for (;;) {
        ST_token_t *t = ST_peek(p);
        if (ST_tok_is_symbol(t, ".")) {
            p->pos++;
            ST_string_t name = ST_expect_ident(p, "a field name after '.'");
            if (!name.len)
                return NULL;
            ST_expr_t *f = ST_expr_new(p->arena, ST_EX_FIELD, t->line, t->col);
            f->field.base = e;
            f->field.name = name;
            e = f;
        } else if (ST_tok_is_symbol(t, "(")) {
            p->pos++;
            ST_expr_t *c = ST_expr_new(p->arena, ST_EX_CALL, e->line, e->col);
            c->call.callee = e;
            u32 save = p->no_struct_lit;
            p->no_struct_lit = 0;
            b8 ok = ST_parse_call_args(p, &c->call.args);
            p->no_struct_lit = save;
            if (!ok)
                return NULL;
            e = c;
        } else if (ST_tok_is_symbol(t, "[")) {
            p->pos++;
            ST_expr_t *ix = ST_expr_new(p->arena, ST_EX_INDEX, t->line, t->col);
            ix->index.base = e;
            u32 save = p->no_struct_lit;
            p->no_struct_lit = 0;
            ix->index.index = ST_parse_expr(p);
            p->no_struct_lit = save;
            if (!ix->index.index)
                return NULL;
            if (!ST_expect_sym(p, "]"))
                return NULL;
            e = ix;
        } else
            break;
    }
    return e;
}

static ST_expr_t *ST_parse_unary(ST_parser_t *p) {
    ST_token_t *t = ST_peek(p);
    const char *op = NULL;
    if (ST_tok_is_symbol(t, "-"))
        op = "-";
    else if (ST_tok_is_symbol(t, "!") || ST_tok_is_keyword(t, "not"))
        op = "!";
    else if (ST_tok_is_symbol(t, "~"))
        op = "~";
    else if (ST_tok_is_symbol(t, "*"))
        op = "*";
    else if (ST_tok_is_symbol(t, "&"))
        op = "&";
    if (op) {
        p->pos++;
        ST_expr_t *e = ST_expr_new(p->arena, ST_EX_UNARY, t->line, t->col);
        e->unary.op = ST_cstr_to_str((char *)op);
        e->unary.operand = ST_parse_unary(p);
        return e->unary.operand ? e : NULL;
    }
    return ST_parse_postfix(p);
}

typedef struct {
    const char *ops[7];
} ST_prec_level_t;

static const ST_prec_level_t ST_prec[] = {
    {{"||", "or"}}, {{"&&", "and"}}, {{"==", "!="}}, {{"<=", ">=", "<", ">"}}, {{"|"}}, {{"^"}},
    {{"&"}},        {{"<<", ">>"}},  {{"+", "-"}},   {{"*", "/", "%"}},
};

#define ST_N_PREC ST_array_len(ST_prec)

static const char *ST_match_binop(ST_parser_t *p, u32 level) {
    ST_token_t *t = ST_peek(p);
    if (!t)
        return NULL;
    for (u32 k = 0; ST_prec[level].ops[k]; k++) {
        const char *op = ST_prec[level].ops[k];
        b8 is_word = (op[0] >= 'a' && op[0] <= 'z');
        if (is_word ? ST_tok_is_keyword(t, op) : ST_tok_is_symbol(t, op))
            return op[0] == 'o' && op[1] == 'r' ? "||" : is_word && op[0] == 'a' ? "&&" : op;
    }
    return NULL;
}

static ST_expr_t *ST_parse_cast_expr(ST_parser_t *p) {
    ST_expr_t *e = ST_parse_unary(p);
    if (!e)
        return NULL;
    while (ST_at_symbol(p, "#as")) {
        ST_token_t *t = ST_peek(p);
        p->pos++;
        ST_expr_t *c = ST_expr_new(p->arena, ST_EX_CAST, t->line, t->col);
        c->cast.operand = e;
        c->cast.to = ST_parse_type(p);
        if (!c->cast.to)
            return NULL;
        e = c;
    }
    return e;
}

static ST_expr_t *ST_parse_binary(ST_parser_t *p, u32 level) {
    if (level >= ST_N_PREC)
        return ST_parse_cast_expr(p);
    ST_expr_t *l = ST_parse_binary(p, level + 1);
    if (!l)
        return NULL;
    for (;;) {
        ST_token_t *t = ST_peek(p);
        const char *op = ST_match_binop(p, level);
        if (!op)
            break;
        p->pos++;
        ST_expr_t *r = ST_parse_binary(p, level + 1);
        if (!r)
            return NULL;
        ST_expr_t *b = ST_expr_new(p->arena, ST_EX_BINARY, t->line, t->col);
        b->bin.op = ST_cstr_to_str((char *)op);
        b->bin.l = l;
        b->bin.r = r;
        l = b;
    }
    return l;
}

static ST_expr_t *ST_parse_expr(ST_parser_t *p) {
    return ST_parse_binary(p, 0);
}

static ST_expr_t *ST_parse_cond(ST_parser_t *p) {
    p->no_struct_lit++;
    ST_expr_t *e = ST_parse_expr(p);
    p->no_struct_lit--;
    return e;
}

static b8 ST_is_lvalue(ST_expr_t *e) {
    switch (e->kind) {
        case ST_EX_IDENT:
        case ST_EX_FIELD:
        case ST_EX_INDEX:
            return 1;
        case ST_EX_UNARY:
            return ST_string_eq_cstr(e->unary.op, "*");
        case ST_EX_INT:
        case ST_EX_FLOAT:
        case ST_EX_STR:
        case ST_EX_CHAR:
        case ST_EX_BOOL:
        case ST_EX_NULL:
        case ST_EX_BINARY:
        case ST_EX_CALL:
        case ST_EX_CAST:
        case ST_EX_STRUCT_LIT:
        case ST_EX_ARRAY_NEW:
        case ST_EX_SIZEOF:
        case ST_EX_TYPEOF:
        case ST_EX_TYPEINFO:
        case ST_EX_KIND:
        case ST_EX_CSTR:
        case ST_EX_FIELDS:
        case ST_EX_COMP_ERROR:
        case ST_EX_COUNT:
            return 0;
    }
    return 0;
}

static ST_stmt_t *ST_parse_if(ST_parser_t *p, b8 force_comptime);

// Handles whatever follows an if/switch's own body (brace or 'then' form):
// an optional 'else'/'#else', itself either another if ('else if ...'),
// a braced block, or -- new -- a single inline 'else then STMT;' statement.
static ST_stmt_t *ST_parse_if_else(ST_parser_t *p, ST_stmt_t *s, b8 is_comptime) {
    if (ST_at_keyword(p, "else") || ST_at_symbol(p, "#else")) {
        ST_token_t *et = ST_peek(p);
        b8 else_is_comptime = ST_tok_is_symbol(et, "#else");
        if (is_comptime && !else_is_comptime) {
            ST_perr_tok(p, et,
                        "'#if' needs '#else', not plain 'else' -- "
                        "otherwise this arm looks comptime-pruned but isn't");
            return NULL;
        }
        if (!is_comptime && else_is_comptime) {
            ST_perr_tok(p, et, "'#else' needs a matching '#if', not plain 'if'");
            return NULL;
        }
        p->pos++;
        if (ST_at_keyword(p, "if")) {
            s->if_.else_stmt = ST_parse_if(p, is_comptime);
            if (!s->if_.else_stmt)
                return NULL;
        } else if (ST_at_then(p)) {
            p->pos++;
            ST_stmt_t *one = ST_parse_stmt(p);
            if (!one)
                return NULL;
            ST_stmt_t *blk = ST_stmt_new(p->arena, ST_ST_BLOCK, et->line, et->col);
            ST_da_append_arena(p->arena, &blk->block, one);
            s->if_.else_stmt = blk;
        } else {
            ST_stmt_t *blk = ST_stmt_new(p->arena, ST_ST_BLOCK, et->line, et->col);
            if (!ST_expect_sym(p, "{"))
                return NULL;
            if (!ST_parse_body(p, &blk->block))
                return NULL;
            if (!ST_expect_sym(p, "}"))
                return NULL;
            s->if_.else_stmt = blk;
        }
    }
    return s;
}

static ST_stmt_t *ST_parse_if(ST_parser_t *p, b8 force_comptime) {
    ST_token_t *t = ST_peek(p);
    b8 is_comptime = force_comptime || ST_tok_is_symbol(t, "#if");
    p->pos++;
    ST_expr_t *cond = ST_parse_cond(p);
    if (!cond)
        return NULL;

    if (ST_at_then(p)) {
        p->pos++;
        ST_stmt_t *one = ST_parse_stmt(p);
        if (!one)
            return NULL;
        ST_stmt_t *s = ST_stmt_new(p->arena, ST_ST_IF, t->line, t->col);
        s->if_.cond = cond;
        s->if_.is_comptime = is_comptime;
        ST_da_append_arena(p->arena, &s->if_.then_body, one);
        return ST_parse_if_else(p, s, is_comptime);
    }

    if (!ST_expect_sym(p, "{"))
        return NULL;

    u32 scan = p->pos;
    while (scan < p->n_tokens && ST_tok_is_symbol(&p->tokens[scan], ";"))
        scan++;
    ST_token_t *first = ST_tok_at(p, scan);
    if (ST_tok_is_keyword(first, "case") || ST_tok_is_keyword(first, "default") ||
        ST_tok_is_symbol(first, "#case") || ST_tok_is_symbol(first, "#default")) {
        ST_stmt_t *s = ST_stmt_new(p->arena, ST_ST_SWITCH, t->line, t->col);
        s->switch_.cond = cond;
        s->switch_.is_comptime = is_comptime;
        while (!ST_at_symbol(p, "}") && p->pos < p->n_tokens) {
            if (ST_at_symbol(p, ";")) {
                p->pos++;
                continue;
            }
            ST_token_t *ct = ST_peek(p);
            // '#case'/'#default' are accepted as plain synonyms for
            // 'case'/'default' -- purely for readability inside a '#if'
            // body (so the whole construct visually reads as comptime),
            // they don't carry any meaning of their own.
            b8 is_case = ST_tok_is_keyword(ct, "case") || ST_tok_is_symbol(ct, "#case");
            b8 is_default = ST_tok_is_keyword(ct, "default") || ST_tok_is_symbol(ct, "#default");
            if (!is_case && !is_default) {
                ST_perr_tok(p, ct, "expected 'case' or 'default' inside a switch body");
                ST_sync_stmt(p);
                continue;
            }
            p->pos++;
            ST_case_t c = {.line = ct->line, .col = ct->col};
            if (is_case) {
                p->no_struct_lit++;
                ST_expr_t *v = ST_parse_expr(p);
                p->no_struct_lit--;
                if (!v)
                    return NULL;
                ST_da_append_arena(p->arena, &c.values, v);
            }
            if (ST_at_symbol(p, ":") || ST_at_then(p)) {
                // 'case Label: stmt;' / 'case Label then stmt;' -- a single
                // statement, no block braces either way.
                p->pos++;
                ST_stmt_t *one = ST_parse_stmt(p);
                if (!one)
                    return NULL;
                ST_da_append_arena(p->arena, &c.body, one);
            } else {
                if (!ST_expect_sym(p, "{"))
                    return NULL;
                if (!ST_parse_body(p, &c.body))
                    return NULL;
                if (!ST_expect_sym(p, "}"))
                    return NULL;
            }
            ST_da_append_arena(p->arena, &s->switch_.cases, c);
        }
        if (!ST_expect_sym(p, "}"))
            return NULL;
        return s;
    }

    ST_stmt_t *s = ST_stmt_new(p->arena, ST_ST_IF, t->line, t->col);
    s->if_.cond = cond;
    s->if_.is_comptime = is_comptime;
    if (!ST_parse_body(p, &s->if_.then_body))
        return NULL;
    if (!ST_expect_sym(p, "}"))
        return NULL;
    return ST_parse_if_else(p, s, is_comptime);
}

static ST_stmt_t *ST_parse_for(ST_parser_t *p) {
    ST_token_t *t = ST_peek(p);
    b8 is_comptime = ST_tok_is_symbol(t, "#for");
    p->pos++;
    ST_string_t iter = ST_expect_ident(p, "an iterator name after 'for'");
    if (!iter.len)
        return NULL;

    ST_string_t spec_iter = {0};
    if (ST_at_symbol(p, ",")) {
        p->pos++;
        spec_iter = ST_expect_ident(p, "a second iterator name after ','");
        if (!spec_iter.len)
            return NULL;
    }

    if (!ST_expect_sym(p, ":"))
        return NULL;

    p->no_struct_lit++;
    ST_expr_t *first = ST_parse_expr(p);
    p->no_struct_lit--;
    if (!first)
        return NULL;

    if (ST_at_symbol(p, "..") || ST_at_symbol(p, "..=")) {
        if (spec_iter.len) {
            ST_perr_here(p,
                         "a second iterator name ('" ST_sv_fmt "') only makes sense when "
                         "iterating a string, not a range",
                         ST_sv_args(spec_iter));
            return NULL;
        }
        b8 inclusive = ST_at_symbol(p, "..=");
        p->pos++;
        ST_stmt_t *s = ST_stmt_new(p->arena, ST_ST_FOR_RANGE, t->line, t->col);
        s->for_range.iter = iter;
        s->for_range.lo = first;
        s->for_range.inclusive = inclusive;
        s->for_range.is_comptime = is_comptime;
        p->no_struct_lit++;
        s->for_range.hi = ST_parse_expr(p);
        p->no_struct_lit--;
        if (!s->for_range.hi)
            return NULL;
        if (!ST_expect_sym(p, "{"))
            return NULL;
        if (!ST_parse_body(p, &s->for_range.body))
            return NULL;
        if (!ST_expect_sym(p, "}"))
            return NULL;
        return s;
    }

    if (!is_comptime && spec_iter.len) {
        ST_perr_here(p,
                     "a second iterator name ('" ST_sv_fmt "') is only meaningful in a "
                     "'#for' comptime string walk, not a plain runtime 'for'",
                     ST_sv_args(spec_iter));
        return NULL;
    }

    ST_stmt_t *s = ST_stmt_new(p->arena, ST_ST_FOR_ARRAY, t->line, t->col);
    s->for_array.iter = iter;
    s->for_array.spec_iter = spec_iter;
    s->for_array.target = first;
    s->for_array.is_comptime = is_comptime;
    if (!ST_expect_sym(p, "{"))
        return NULL;
    if (!ST_parse_body(p, &s->for_array.body))
        return NULL;
    if (!ST_expect_sym(p, "}"))
        return NULL;
    return s;
}

static ST_stmt_t *ST_parse_decl_stmt(ST_parser_t *p, ST_token_t *name_tok) {
    // NOTE(segfault):  name := / name :: / name : T [= e]
    ST_stmt_t *s = ST_stmt_new(p->arena, ST_ST_DECL, name_tok->line, name_tok->col);
    s->decl.name = name_tok->text;
    p->pos++;

    if (ST_at_symbol(p, ":=") || ST_at_symbol(p, "::")) {
        s->decl.is_const = ST_at_symbol(p, "::");
        p->pos++;
        s->decl.init = ST_parse_expr(p);
        if (!s->decl.init)
            return NULL;
        if (ST_at_keyword(p, "static")) {
            p->pos++;
            s->decl.is_static = 1;
        }
        if (!ST_expect_semi(p))
            return NULL;
        return s;
    }

    // NOTE(segfault): name : T ...
    p->pos++;
    s->decl.te = ST_parse_type(p);
    if (!s->decl.te)
        return NULL;
    if (ST_at_symbol(p, ":=")) {
        ST_perr_here(p, "variable '" ST_sv_fmt "' has an explict type; use = instead of :=",
                     ST_sv_args(s->decl.name));
        return NULL;
    }
    if (ST_at_symbol(p, "=") && !ST_tok_is_symbol(ST_peek2(p), "=")) {
        p->pos++;
        s->decl.init = ST_parse_expr(p);
        if (!s->decl.init)
            return NULL;
    }
    if (!ST_expect_semi(p))
        return NULL;
    return s;
}

static ST_stmt_t *ST_parse_multi(ST_parser_t *p) {
    ST_token_t *t = ST_peek(p);
    ST_stmt_t *s = ST_stmt_new(p->arena, ST_ST_MULTI_BIND, t->line, t->col);
    struct {
        ST_string_t *items;
        u32 count, capacity;
    } names = {0};
    for (;;) {
        ST_string_t n = ST_expect_ident(p, "a name");
        if (!n.len)
            return NULL;
        ST_da_append_arena(p->arena, &names, n);
        if (ST_at_symbol(p, ",")) {
            p->pos++;
            continue;
        }
        break;
    }
    s->multi.n_names = names.count;
    s->multi.names = names.items;

    if (ST_at_symbol(p, ":=") || ST_at_symbol(p, "::"))
        s->multi.declare = 1;
    else if (!ST_at_symbol(p, "=")) {
        ST_perr_here(p, "expected ':=' or '=' after name list");
        return NULL;
    }
    p->pos++;

    for (;;) {
        ST_expr_t *e = ST_parse_expr(p);
        if (!e)
            return NULL;
        ST_da_append_arena(p->arena, &s->multi.values, e);
        if (ST_at_symbol(p, ",")) {
            p->pos++;
            continue;
        }
        break;
    }
    if (!ST_expect_semi(p))
        return NULL;
    return s;
}

// @note: collects the raw token stream inside a '#asm { .. }' block
// (balanced on '{'/'}'), used by both the statement form (ST_parse_asm_stmt)
// and the expression form (ST_parse_primary's '#asm' branch). Leaves the
// parser positioned just past the closing '}'. Returns 0 on error.
static b8 ST_parse_asm_tokens(ST_parser_t *p, ST_token_t **out_toks, u32 *out_n) {
    if (!ST_expect_sym(p, "{"))
        return 0;

    struct {
        ST_token_t *items;
        u32 count, capacity;
    } toks = {0};

    i32 depth = 0;
    for (;;) {
        ST_token_t *cur = ST_peek(p);
        if (!cur) {
            ST_perr_here(p, "unterminated '#asm' block, expected '}'");
            return 0;
        }
        if (ST_tok_is_symbol(cur, "{"))
            depth++;
        else if (ST_tok_is_symbol(cur, "}")) {
            if (depth == 0)
                break;
            depth--;
        }
        ST_da_append_arena(p->arena, &toks, *cur);
        p->pos++;
    }
    if (!ST_expect_sym(p, "}"))
        return 0;

    *out_toks = toks.items;
    *out_n = toks.count;
    return 1;
}

static ST_stmt_t *ST_parse_asm_stmt(ST_parser_t *p) {
    ST_token_t *t = ST_peek(p);
    p->pos++;

    ST_stmt_t *s = ST_stmt_new(p->arena, ST_ST_ASM, t->line, t->col);
    if (!ST_parse_asm_tokens(p, &s->asm_.tokens, &s->asm_.n_tokens))
        return NULL;
    return s;
}

static ST_stmt_t *ST_parse_stmt(ST_parser_t *p) {
    ST_token_t *t = ST_peek(p);
    if (!t)
        return NULL;

    if (ST_tok_is_symbol(t, ";")) {
        p->pos++;
        return ST_parse_stmt(p);
    }

    if (ST_tok_is_symbol(t, "#asm"))
        return ST_parse_asm_stmt(p);

    if (t->kind == ST_TSYMBOL && t->text.len > 1 && t->text.data[0] == '#' &&
        !ST_string_eq_cstr(t->text, "#as") && !ST_string_eq_cstr(t->text, "#if") &&
        !ST_string_eq_cstr(t->text, "#comp_error") && !ST_string_eq_cstr(t->text, "#for")) {
        ST_perr_tok(p, t, "'" ST_sv_fmt "' is not supported in storthc yet", ST_sv_args(t->text));
        return NULL;
    }

    if (ST_tok_is_keyword(t, "return")) {
        p->pos++;
        ST_stmt_t *s = ST_stmt_new(p->arena, ST_ST_RETURN, t->line, t->col);
        if (!ST_at_symbol(p, ";")) {
            for (;;) {
                ST_expr_t *e = ST_parse_expr(p);
                if (!e)
                    return NULL;
                ST_da_append_arena(p->arena, &s->ret.values, e);
                if (ST_at_symbol(p, ",")) {
                    p->pos++;
                    continue;
                }
                break;
            }
        }
        if (!ST_expect_semi(p))
            return NULL;
        return s;
    }

    if (ST_tok_is_keyword(t, "if") || ST_tok_is_symbol(t, "#if"))
        return ST_parse_if(p, 0);
    if (ST_tok_is_keyword(t, "for") || ST_tok_is_symbol(t, "#for"))
        return ST_parse_for(p);

    if (ST_tok_is_keyword(t, "while")) {
        p->pos++;
        ST_token_t *maybe_ident = ST_peek(p);
        b8 has_assign =
            maybe_ident && maybe_ident->kind == ST_TIDENT && ST_tok_is_symbol(ST_peek2(p), ":=");

        b8 has_typed =
            maybe_ident && maybe_ident->kind == ST_TIDENT && ST_tok_is_symbol(ST_peek2(p), ":");

        if (has_assign || has_typed) {
            ST_string_t iter = maybe_ident->text;
            p->pos += 2;

            ST_tyexpr_t *iter_te = NULL;
            if (has_typed) {
                iter_te = ST_parse_type(p);
                if (!iter_te)
                    return NULL;
                if (!ST_expect_sym(p, "="))
                    return NULL;
            }

            p->no_struct_lit++;
            ST_expr_t *lo = ST_parse_expr(p);
            p->no_struct_lit--;
            if (!lo)
                return NULL;

            if (!ST_at_symbol(p, "..") && !ST_at_symbol(p, "..=")) {
                ST_perr_here(p, "expected '..' or '..=' after range start in 'while' iterator "
                                "binding");
                return NULL;
            }
            b8 inclusive = ST_at_symbol(p, "..=");
            p->pos++;
            ST_stmt_t *s = ST_stmt_new(p->arena, ST_ST_FOR_RANGE, t->line, t->col);
            s->for_range.iter = iter;
            s->for_range.iter_te = iter_te;
            s->for_range.lo = lo;
            s->for_range.inclusive = inclusive;

            p->no_struct_lit++;
            s->for_range.hi = ST_parse_expr(p);
            p->no_struct_lit--;
            if (!s->for_range.hi)
                return NULL;
            if (!ST_expect_sym(p, "{"))
                return NULL;
            if (!ST_parse_body(p, &s->for_range.body))
                return NULL;
            if (!ST_expect_sym(p, "}"))
                return NULL;
            return s;
        }

        p->no_struct_lit++;
        ST_expr_t *first = ST_parse_expr(p);
        p->no_struct_lit--;
        if (!first)
            return NULL;
        if (ST_at_symbol(p, "..") || ST_at_symbol(p, "..=")) {
            b8 inclusive = ST_at_symbol(p, "..=");
            p->pos++;
            ST_stmt_t *s = ST_stmt_new(p->arena, ST_ST_FOR_RANGE, t->line, t->col);
            static const ST_string_t ST_anon_range_iter = {(u8 *)"while_range", 12};
            s->for_range.iter = ST_anon_range_iter;
            s->for_range.lo = first;
            s->for_range.inclusive = inclusive;
            p->no_struct_lit++;
            s->for_range.hi = ST_parse_expr(p);
            p->no_struct_lit--;
            if (!s->for_range.hi)
                return NULL;
            if (!ST_expect_sym(p, "{"))
                return NULL;
            if (!ST_parse_body(p, &s->for_range.body))
                return NULL;
            if (!ST_expect_sym(p, "}"))
                return NULL;
            return s;
        }
        ST_stmt_t *s = ST_stmt_new(p->arena, ST_ST_WHILE, t->line, t->col);
        s->while_.cond = first;
        if (!s->while_.cond)
            return NULL;
        if (!ST_expect_sym(p, "{"))
            return NULL;
        if (!ST_parse_body(p, &s->while_.body))
            return NULL;
        if (!ST_expect_sym(p, "}"))
            return NULL;
        return s;
    }

    if (ST_tok_is_keyword(t, "defer")) {
        p->pos++;
        ST_stmt_t *s = ST_stmt_new(p->arena, ST_ST_DEFER, t->line, t->col);
        if (ST_at_symbol(p, "{")) {
            ST_stmt_t *blk = ST_stmt_new(p->arena, ST_ST_BLOCK, t->line, t->col);
            p->pos++;
            if (!ST_parse_body(p, &blk->block))
                return NULL;
            if (!ST_expect_sym(p, "}"))
                return NULL;
            s->defer_stmt = blk;
        } else {
            s->defer_stmt = ST_parse_stmt(p);
            if (!s->defer_stmt)
                return NULL;
        }
        return s;
    }

    if (ST_tok_is_keyword(t, "break") || ST_tok_is_keyword(t, "continue")) {
        b8 is_break = ST_tok_is_keyword(t, "break");
        p->pos++;
        ST_stmt_t *s =
            ST_stmt_new(p->arena, is_break ? ST_ST_BREAK : ST_ST_CONTINUE, t->line, t->col);
        if (!ST_expect_semi(p))
            return NULL;
        return s;
    }

    if (ST_tok_is_keyword(t, "label") || ST_tok_is_keyword(t, "goto")) {
        b8 is_label = ST_tok_is_keyword(t, "label");
        p->pos++;
        ST_stmt_t *s =
            ST_stmt_new(p->arena, is_label ? ST_ST_LABEL : ST_ST_GODOWN, t->line, t->col);
        s->label = ST_expect_ident(p, is_label ? "a label name" : "a label to jump to");
        if (!s->label.len)
            return NULL;
        if (is_label && ST_at_symbol(p, ":"))
            p->pos++;
        if (!ST_expect_semi(p))
            return NULL;
        return s;
    }

    if (ST_tok_is_symbol(t, "{")) {
        p->pos++;
        ST_stmt_t *s = ST_stmt_new(p->arena, ST_ST_BLOCK, t->line, t->col);
        if (!ST_parse_body(p, &s->block))
            return NULL;
        if (!ST_expect_sym(p, "}"))
            return NULL;
        return s;
    }

    if (ST_tok_is_ident(t)) {
        ST_token_t *t2 = ST_peek2(p);
        if (ST_tok_is_symbol(t2, ":=") || ST_tok_is_symbol(t2, "::") ||
            (ST_tok_is_symbol(t2, ":") && !ST_tok_is_symbol(ST_tok_at(p, p->pos + 2), "=")))
            return ST_parse_decl_stmt(p, t);

        if (ST_tok_is_symbol(t2, ",")) {
            u32 scan = p->pos;
            b8 is_multi = 0;
            while (scan < p->n_tokens) {
                if (!ST_tok_is_ident(ST_tok_at(p, scan)))
                    break;
                scan++;
                ST_token_t *sep = ST_tok_at(p, scan);
                if (ST_tok_is_symbol(sep, ",")) {
                    scan++;
                    continue;
                }
                if (ST_tok_is_symbol(sep, ":=") || ST_tok_is_symbol(sep, "::") ||
                    (ST_tok_is_symbol(sep, "=") && !ST_tok_is_symbol(ST_tok_at(p, scan + 1), "=")))
                    is_multi = 1;
                break;
            }
            if (is_multi)
                return ST_parse_multi(p);
        }
    }

    ST_expr_t *e = ST_parse_expr(p);
    if (!e)
        return NULL;

    ST_token_t *op = ST_peek(p);
    b8 plain = ST_tok_is_symbol(op, "=") && !ST_tok_is_symbol(ST_peek2(p), "=");
    if (plain || ST_tok_is_compound_assign(op)) {
        if (!ST_is_lvalue(e)) {
            ST_perr(p, e->line, e->col,
                    "left side of assignment is not assignable\n"
                    "assignable: a variable, a field chain, an index, or a "
                    "'*ptr' dereference");
            return NULL;
        }
        p->pos++;
        ST_stmt_t *s = ST_stmt_new(p->arena, ST_ST_ASSIGN, op->line, op->col);
        s->assign.lhs = e;
        s->assign.op = op->text;
        s->assign.rhs = ST_parse_expr(p);
        if (!s->assign.rhs)
            return NULL;
        if (!ST_expect_semi(p))
            return NULL;
        return s;
    }

    if (ST_tok_is_symbol(op, "++") || ST_tok_is_symbol(op, "--")) {
        if (!ST_is_lvalue(e)) {
            ST_perr_tok(p, op,
                        "left side of '" ST_sv_fmt "' is not assignable\n"
                        "assignable: a variable, a field chain, an index, or a "
                        "'*ptr' dereference",
                        ST_sv_args(op->text));
            return NULL;
        }
        b8 inc = ST_tok_is_symbol(op, "++");
        p->pos++;
        ST_stmt_t *s = ST_stmt_new(p->arena, ST_ST_ASSIGN, op->line, op->col);
        s->assign.lhs = e;
        s->assign.op = ST_cstr_to_str((char *)(inc ? "+=" : "-="));
        ST_expr_t *one = ST_expr_new(p->arena, ST_EX_INT, op->line, op->col);
        one->ival = 1;
        s->assign.rhs = one;
        if (!ST_expect_semi(p))
            return NULL;
        return s;
    }

    ST_stmt_t *s = ST_stmt_new(p->arena, ST_ST_EXPR, e->line, e->col);
    s->expr = e;
    if (!ST_expect_semi(p))
        return NULL;
    return s;
}

static b8 ST_parse_body(ST_parser_t *p, ST_stmts_t *out) {
    while (p->pos < p->n_tokens && !ST_at_symbol(p, "}")) {
        if (p->n_errors >= ST_PARSE_MAX_ERRORS)
            return 0;
        ST_stmt_t *s = ST_parse_stmt(p);
        if (!s) {
            ST_sync_stmt(p);
            continue;
        }
        ST_da_append_arena(p->arena, out, s);
    }
    return 1;
}

static ST_decl_t *ST_parse_struct_decl(ST_parser_t *p, ST_string_t name, u32 line, u32 col);

static b8 ST_parse_generic_list(ST_parser_t *p, ST_strings_t *out) {
    if (!(ST_at_symbol(p, "(") && ST_tok_is_symbol(ST_tok_at(p, p->pos + 1), "$")))
        return 1;
    p->pos++;
    while (!ST_at_symbol(p, ")") && p->pos < p->n_tokens) {
        if (ST_at_symbol(p, ",")) {
            p->pos++;
            continue;
        }
        if (!ST_expect_sym(p, "$"))
            return 0;
        ST_string_t gname = ST_expect_ident(p, "a generic paramter after '$'");
        if (!gname.len)
            return 0;
        ST_da_append_arena(p->arena, out, gname);
    }
    return ST_expect_sym(p, ")");
}

static b8 ST_parse_struct_fields(ST_parser_t *p, ST_field_specs_t *out) {
    while (!ST_at_symbol(p, "}") && p->pos < p->n_tokens) {
        if (ST_at_symbol(p, ";")) {
            p->pos++;
            continue;
        }
        ST_token_t *ft = ST_peek(p);
        ST_field_spec_t f = {.line = ft->line, .col = ft->col};
        f.name = ST_expect_ident(p, "a field name");
        if (!f.name.len)
            return 0;
        if (!ST_expect_sym(p, ":"))
            return 0;
        if (ST_at_keyword(p, "struct")) {
            ST_token_t *st = ST_peek(p);
            p->pos++;
            f.anon = ST_parse_struct_decl(p, (ST_string_t){0}, st->line, st->col);
            if (!f.anon)
                return 0;
        } else {
            f.te = ST_parse_type(p);
            if (!f.te)
                return 0;
            if (!ST_expect_semi(p))
                return 0;
        }
        ST_da_append_arena(p->arena, out, f);
    }
    return 1;
}

static void ST_collect_generic_te(ST_parser_t *p, ST_tyexpr_t *te, ST_strings_t *out) {
    if (!te)
        return;
    if (te->kind == ST_TE_NAME) {
        if (!te->is_generic_param)
            return;
        ST_forrange(0, out->count)
            if (ST_string_eq(out->items[i], te->name))
                return;

        ST_da_append_arena(p->arena, out, te->name);
        return;
    }
    ST_collect_generic_te(p, te->inner, out);
    ST_forrange(0,te->fn_params.count) ST_collect_generic_te(p, te->fn_params.items[i], out);
    ST_forrange(0,te->fn_rets.count) ST_collect_generic_te(p, te->fn_rets.items[i], out);
    ST_forrange(0,te->generic_args.count)
        ST_collect_generic_te(p, te->generic_args.items[i], out);
}

static void ST_collect_generic_fields(ST_parser_t *p, ST_field_specs_t *fields,
                                      ST_strings_t *out) {
    ST_forrange(0, fields->count) {
        ST_field_spec_t *f = &fields->items[i];
        ST_collect_generic_te(p, f->te, out);
        if (f->anon && f->anon->kind == ST_DE_STRUCT)
            ST_collect_generic_fields(p, &f->anon->struct_.fields, out);
    }
}

static ST_decl_t *ST_parse_struct_decl(ST_parser_t *p, ST_string_t name, u32 line, u32 col) {
    ST_decl_t *d = ST_decl_new(p->arena, ST_DE_STRUCT, line, col);
    d->name = name;
    if (!ST_parse_generic_list(p, &d->struct_.generics))
        return NULL;
    if (ST_at_symbol(p, "#pad")) {
        d->struct_.packing = ST_PACK_C;
        p->pos++;
    } else if (ST_at_symbol(p, "#pack")) {
        d->struct_.packing = ST_PACK_PACKED;
        p->pos++;
    }
    if (!ST_expect_sym(p, "{"))
        return NULL;
    if (!ST_parse_struct_fields(p, &d->struct_.fields))
        return NULL;
    if (!ST_expect_sym(p, "}"))
        return NULL;
    ST_collect_generic_fields(p, &d->struct_.fields, &d->struct_.generics);
    return d;
}

static ST_decl_t *ST_parse_enum_decl(ST_parser_t *p, b8 is_flag, u32 line, u32 col) {
    ST_decl_t *d = ST_decl_new(p->arena, ST_DE_ENUM, line, col);
    d->enum_.is_flag = is_flag;
    d->name = ST_expect_ident(p, "an enum name");
    if (!d->name.len)
        return NULL;
    if (ST_at_symbol(p, ":") && !ST_tok_is_symbol(ST_peek2(p), "{")) {
        p->pos++;
        d->enum_.ty = ST_parse_type(p);
        if (!d->enum_.ty)
            return NULL;
    }
    if (!ST_expect_sym(p, "{"))
        return NULL;
    while (!ST_at_symbol(p, "}") && p->pos < p->n_tokens) {
        if (ST_at_symbol(p, ",") || ST_at_symbol(p, ";")) {
            p->pos++;
            continue;
        }
        ST_token_t *vt = ST_peek(p);
        ST_variant_spec_t v = {.line = vt->line, .col = vt->col};
        v.name = ST_expect_ident(p, "a variant name");
        if (!v.name.len)
            return NULL;
        if (ST_at_symbol(p, ":=")) {
            p->pos++;
            v.value = ST_parse_expr(p);
            if (!v.value)
                return NULL;
        }
        ST_da_append_arena(p->arena, &d->enum_.variants, v);
    }
    if (!ST_expect_sym(p, "}"))
        return NULL;
    return d;
}

static ST_decl_t *ST_parse_tag_union_decl(ST_parser_t *p, u32 line, u32 col) {
    ST_decl_t *d = ST_decl_new(p->arena, ST_DE_TAG_UNION, line, col);
    d->name = ST_expect_ident(p, "a tag_union name");
    if (!d->name.len)
        return NULL;
    if (!ST_expect_sym(p, "{"))
        return NULL;
    while (!ST_at_symbol(p, "}") && p->pos < p->n_tokens) {
        if (ST_at_symbol(p, ",") || ST_at_symbol(p, ";")) {
            p->pos++;
            continue;
        }
        ST_token_t *vt = ST_peek(p);
        ST_variant_spec_t v = {.line = vt->line, .col = vt->col};
        v.name = ST_expect_ident(p, "a variant name");
        if (!v.name.len)
            return NULL;
        if (ST_at_symbol(p, ":")) {
            p->pos++;
            v.payload = ST_parse_type(p);
            if (!v.payload)
                return NULL;
        }
        ST_da_append_arena(p->arena, &d->tag_union.variants, v);
    }
    if (!ST_expect_sym(p, "}"))
        return NULL;
    return d;
}

static b8 ST_parse_fn_sig(ST_parser_t *p, ST_fn_sig_t *sig, b8 is_extern) {
    if (!ST_expect_sym(p, "("))
        return 0;
    while (!ST_at_symbol(p, ")") && p->pos < p->n_tokens) {
        if (ST_at_symbol(p, ",")) {
            p->pos++;
            continue;
        }
        if (ST_at_symbol(p, "..") || ST_at_symbol(p, "...")) {
            p->pos++;
            if (ST_at_symbol(p, "."))
                p->pos++;
            if (!is_extern) {
                ST_perr_here(p, "variadic parameters are only allowed on extern functions");
                return 0;
            }
            sig->is_variadic = 1;
            continue;
        }
        if (ST_at_keyword(p, "using")) {
            ST_perr_here(p, "'using' parameters are not supported in storthc yet");
            return 0;
        }
        ST_token_t *pt = ST_peek(p);
        ST_param_t param = {.line = pt ? pt->line : 0, .col = pt ? pt->col : 0};
        param.name = ST_expect_ident(p, "a parameter name");
        if (!param.name.len)
            return 0;
        if (ST_at_symbol(p, ":")) {
            p->pos++;
            param.te = ST_parse_type(p);
            if (!param.te)
                return 0;
        }

        if (param.te) {
            if (ST_at_symbol(p, ":=")) {
                ST_perr_here(
                    p, "parameter '" ST_sv_fmt "' has an explicit type; use '=' instead of ':='",
                    ST_sv_args(param.name));
                return 0;
            }

            if (ST_at_symbol(p, "..") || ST_at_symbol(p, "...")) {
                p->pos++;
                if (ST_at_symbol(p, "."))
                    p->pos++;
                if (is_extern) {
                    ST_perr_here(p,
                                 "'" ST_sv_fmt ": T...' isn't the extern C variadic -- extern "
                                 "functions use a bare trailing '...' with no name/type",
                                 ST_sv_args(param.name));
                    return 0;
                }
                b8 is_any = param.te->kind == ST_TE_NAME && ST_string_eq_cstr(param.te->name, "any");
                b8 is_generic = param.te->is_generic_param;
                if (!is_any && !is_generic) {
                    ST_perr_here(p,
                                 "'" ST_sv_fmt "...' needs 'any' (a runtime RTTI pack) or a "
                                 "generic '$T' (a compile-time pack), not a concrete type",
                                 ST_sv_args(param.name));
                    return 0;
                }
                param.is_pack = 1;
                if (is_any)
                    sig->has_any_pack = 1; // runtime: collected into a real array, see ST_lower.c
                else
                    sig->has_generic_pack = 1; // comptime: one synthetic param per call-site arg,
                                                // see ST_instantiate_fn's pack-expansion below
                ST_da_append_arena(p->arena, &sig->params, param);
                if (!ST_at_symbol(p, ")")) {
                    ST_perr_here(p, "'" ST_sv_fmt "...' must be the last parameter",
                                 ST_sv_args(param.name));
                    return 0;
                }
                break;
            }

            if (ST_at_symbol(p, "=")) {
                p->pos++;
                param.def = ST_parse_expr(p);
                if (!param.def)
                    return 0;
            }
        } else {
            if (ST_at_symbol(p, ":=")) {
                p->pos++;
                param.def = ST_parse_expr(p);
                if (!param.def)
                    return 0;
            } else if (ST_at_symbol(p, "=")) {
                ST_perr_here(
                    p, "parameter '" ST_sv_fmt "' has no explicit type; use ':=' or specify a type",
                    ST_sv_args(param.name));
                return 0;
            }
        }

        if (!param.te && !param.def) {
            ST_perr(p, param.line, param.col,
                    "parameter '" ST_sv_fmt "' needs a type (`name: T`) or a "
                    "default (`name := value`)",
                    ST_sv_args(param.name));
            return 0;
        }
        ST_token_t *nx = ST_peek(p);
        if (nx && !ST_tok_is_symbol(nx, ",") && !ST_tok_is_symbol(nx, ")")) {
            ST_perr(p, nx->line, nx->col,
                    "unexpected '" ST_sv_fmt "' after parameter '" ST_sv_fmt "'\n"
                    "parameters are `name: Type`, defaults use ':=' (e.g. `b: "
                    "i64 := 10`)",
                    ST_sv_args(nx->text), ST_sv_args(param.name));
            return 0;
        }
        ST_da_append_arena(p->arena, &sig->params, param);
    }
    if (!ST_expect_sym(p, ")"))
        return 0;

    if (ST_at_symbol(p, "->")) {
        p->pos++;
        sig->has_ret_ann = 1;
        if (ST_at_symbol(p, "(")) {
            p->pos++;
            while (!ST_at_symbol(p, ")") && p->pos < p->n_tokens) {
                if (ST_at_symbol(p, ",")) {
                    p->pos++;
                    continue;
                }
                ST_tyexpr_t *te = ST_parse_type(p);
                if (!te)
                    return 0;
                ST_da_append_arena(p->arena, &sig->rets, te);
            }
            if (!ST_expect_sym(p, ")"))
                return 0;
        } else {
            ST_tyexpr_t *te = ST_parse_type(p);
            if (!te)
                return 0;
            ST_da_append_arena(p->arena, &sig->rets, te);
        }
    }
    return 1;
}

static ST_decl_t *ST_parse_top_decl(ST_parser_t *p) {
    ST_token_t *t = ST_peek(p);
    if (!t)
        return NULL;
    b8 is_pub = 0;
    if (ST_tok_is_keyword(t, "pub")) {
        is_pub = 1;
        p->pos++;
        t = ST_peek(p);
        if (!t) {
            ST_perr_here(p, "expected a declaration after 'pub'");
            return NULL;
        }
    }

    if (ST_tok_is_keyword(t, "struct")) {
        p->pos++;
        ST_string_t name = ST_expect_ident(p, "a struct name");
        if (!name.len)
            return NULL;
        ST_decl_t *d = ST_parse_struct_decl(p, name, t->line, t->col);
        if (d)
            d->is_pub = is_pub;
        return d;
    }
    if (ST_tok_is_keyword(t, "enum") || ST_tok_is_keyword(t, "enum_flag")) {
        b8 is_flag = ST_tok_is_keyword(t, "enum_flag");
        p->pos++;
        ST_decl_t *d = ST_parse_enum_decl(p, is_flag, t->line, t->col);
        if (d)
            d->is_pub = is_pub;
        return d;
    }
    if (ST_tok_is_keyword(t, "tag_union")) {
        p->pos++;
        ST_decl_t *d = ST_parse_tag_union_decl(p, t->line, t->col);
        if (d)
            d->is_pub = is_pub;
        return d;
    }
    if (ST_tok_is_keyword(t, "const")) {
        p->pos++;
        t = ST_peek(p);
        ST_decl_t *d = ST_decl_new(p->arena, ST_DE_CONST, t ? t->line : 0, t ? t->col : 0);
        d->is_pub = is_pub;
        d->name = ST_expect_ident(p, "a constant name");
        if (!d->name.len)
            return NULL;
        if (!ST_expect_sym(p, "::"))
            return NULL;
        d->const_.value = ST_parse_expr(p);
        if (!d->const_.value)
            return NULL;
        if (!ST_expect_semi(p))
            return NULL;
        return d;
    }

    if (ST_tok_is_ident(t) && ST_tok_is_symbol(ST_peek2(p), "::")) {
        ST_decl_t *d = ST_decl_new(p->arena, ST_DE_CONST, t->line, t->col);
        d->is_pub = is_pub;
        d->name = t->text;
        p->pos += 2;
        d->const_.value = ST_parse_expr(p);
        if (!d->const_.value)
            return NULL;
        if (!ST_expect_semi(p))
            return NULL;
        return d;
    }
    if (ST_tok_is_ident(t) && ST_tok_is_symbol(ST_peek2(p), ":=")) {
        ST_decl_t *d = ST_decl_new(p->arena, ST_DE_GLOBAL, t->line, t->col);
        d->is_pub = is_pub;
        d->name = t->text;
        p->pos += 2;
        d->global_.init = ST_parse_expr(p);
        if (!d->global_.init)
            return NULL;
        if (!ST_expect_semi(p))
            return NULL;
        return d;
    }
    if (ST_tok_is_ident(t) && ST_tok_is_symbol(ST_peek2(p), ":")) {
        ST_token_t *nt = t;
        p->pos += 2;
        ST_tyexpr_t *te = ST_parse_type(p);
        if (!te)
            return NULL;
        if (ST_at_symbol(p, ":")) {
            // 'x : T : expr;' -- typed constant.
            p->pos++;
            ST_decl_t *d = ST_decl_new(p->arena, ST_DE_CONST, nt->line, nt->col);
            d->is_pub = is_pub;
            d->name = nt->text;
            d->const_.te = te;
            d->const_.value = ST_parse_expr(p);
            if (!d->const_.value)
                return NULL;
            if (!ST_expect_semi(p))
                return NULL;
            return d;
        }
        // 'x : T = expr;' or 'x : T;' -- typed (optionally zero-init) global.
        ST_decl_t *d = ST_decl_new(p->arena, ST_DE_GLOBAL, nt->line, nt->col);
        d->is_pub = is_pub;
        d->name = nt->text;
        d->global_.te = te;
        if (ST_at_symbol(p, "=") && !ST_tok_is_symbol(ST_peek2(p), "=")) {
            p->pos++;
            d->global_.init = ST_parse_expr(p);
            if (!d->global_.init)
                return NULL;
        }
        if (!ST_expect_semi(p))
            return NULL;
        return d;
    }
    if (ST_tok_is_keyword(t, "extern")) {
        p->pos++;
        if (ST_at_keyword(p, "fn")) {
            p->pos++;
            ST_token_t *nt = ST_peek(p);
            ST_decl_t *d = ST_decl_new(p->arena, ST_DE_EXTERN_FN, nt ? nt->line : t->line,
                                       nt ? nt->col : t->col);
            d->is_pub = is_pub;
            d->name = ST_expect_ident(p, "an extern function name");
            if (!d->name.len)
                return NULL;
            if (!ST_parse_fn_sig(p, &d->extern_fn.sig, 1))
                return NULL;
            if (!d->extern_fn.sig.has_ret_ann) {
                ST_perr(p, d->line, d->col,
                        "extern fn '" ST_sv_fmt "' needs a return annotation "
                        "(`-> void` if none)",
                        ST_sv_args(d->name));
                return NULL;
            }
            if (!ST_expect_semi(p))
                return NULL;
            return d;
        }
        ST_token_t *nt = ST_peek(p);
        ST_decl_t *d =
            ST_decl_new(p->arena, ST_DE_EXTERN_VAR, nt ? nt->line : t->line, nt ? nt->col : t->col);
        d->is_pub = is_pub;
        d->name = ST_expect_ident(p, "an extern variable name");
        if (!d->name.len)
            return NULL;
        if (!ST_expect_sym(p, ":"))
            return NULL;
        d->extern_var.te = ST_parse_type(p);
        if (!d->extern_var.te)
            return NULL;
        if (!ST_expect_semi(p))
            return NULL;
        return d;
    }
    if (ST_tok_is_keyword(t, "fn")) {
        p->pos++;
        ST_token_t *nt = ST_peek(p);
        ST_decl_t *d =
            ST_decl_new(p->arena, ST_DE_FN, nt ? nt->line : t->line, nt ? nt->col : t->col);
        d->is_pub = is_pub;
        d->name = ST_expect_ident(p, "a function name");
        if (!ST_parse_generic_list(p, &d->fn.sig.generics))
            return NULL;
        if (!d->name.len)
            return NULL;
        if (!ST_parse_fn_sig(p, &d->fn.sig, 0))
            return NULL;
        ST_forrange(0, d->fn.sig.params.count)
            ST_collect_generic_te(p, d->fn.sig.params.items[i].te, &d->fn.sig.generics);
        ST_forrange(0, d->fn.sig.rets.count)
            ST_collect_generic_te(p, d->fn.sig.rets.items[i], &d->fn.sig.generics);
        if (ST_at_symbol(p, ";")) {
            p->pos++;
            d->fn.is_prototype = 1;
            return d;
        }
        if (!ST_expect_sym(p, "{"))
            return NULL;
        if (!ST_parse_body(p, &d->fn.body))
            return NULL;
        if (!ST_expect_sym(p, "}"))
            return NULL;
        return d;
    }

    if (ST_tok_is_symbol(t, "#import")) {
        p->pos++;
        ST_token_t *st = ST_peek(p);
        if (!st || st->kind != ST_TSTRING) {
            ST_perr_here(p, "expected a module path string after '#import', e.g. "
                            "'#import \"io\";'");
            return NULL;
        }
        p->pos++;
        ST_decl_t *d = ST_decl_new(p->arena, ST_DE_IMPORT, t->line, t->col);
        d->import_.module_name = st->str;
        d->name = st->str;
        d->import_.alias = st->str;
        if (ST_tok_is_keyword(ST_peek(p), "as") ||
            (ST_tok_is_ident(ST_peek(p)) && ST_string_eq_cstr(ST_peek(p)->text, "as")) ||
            ST_tok_is_keyword(ST_peek(p), "using") ||
            (ST_tok_is_ident(ST_peek(p)) && ST_string_eq_cstr(ST_peek(p)->text, "using"))) {
            p->pos++;
            d->import_.alias = ST_expect_ident(p, "an alias after 'as'/'using'");
            if (!d->import_.alias.len)
                return NULL;
            d->name = d->import_.alias;
        }
        if (!ST_expect_semi(p))
            return NULL;
        return d;
    }

    if (t->kind == ST_TSYMBOL && t->text.len > 1 && t->text.data[0] == '#') {
        ST_perr_tok(p, t, "'" ST_sv_fmt "' is not supported in storthc yet", ST_sv_args(t->text));
        return NULL;
    }

    ST_perr_tok(p, t,
                "expected a top-level declaration (fn, struct, enum, tag_union, "
                "const, extern), got '" ST_sv_fmt "'",
                ST_sv_args(t->text));
    return NULL;
}

b8 ST_parse(ST_arena_t *arena, ST_tokens_t tokens, ST_string_t src, ST_string_t file,
            ST_srcmap_t *srcs, ST_program_t *out) {
    ST_parser_t parser = {0};
    ST_parser_t *p = &parser;
    p->arena = arena;
    p->src = src;
    p->file = file;
    p->srcs = srcs;

    p->tokens = ST_arena_push_zeroed(arena, sizeof(ST_token_t) * (tokens.count ? tokens.count : 1));
    ST_forrange(0, tokens.count) if (tokens.items[i].kind != ST_TDOCCOMENT)
        p->tokens[p->n_tokens++] = tokens.items[i];

    out->file = file;
    while (p->pos < p->n_tokens) {
        if (p->n_errors >= ST_PARSE_MAX_ERRORS)
            break;
        ST_decl_t *d = ST_parse_top_decl(p);
        if (!d) {
            p->pos++;
            ST_sync_decl(p);
            continue;
        }
        ST_da_append_arena(arena, &out->decls, d);
    }
    return p->n_errors == 0;
}
