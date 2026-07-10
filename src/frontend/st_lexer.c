#include "st_lexer.h"
#include "../utils/st_types.h"
#include "../utils/st_string.h"

#include <string.h>
#include <stdlib.h>

const char *ST_keyword_names[ST_KEYWORD_COUNT] = {
#define ST_KEYWORD(e, s) s,
    ST_KEYWORD_LIST
#undef ST_KEYWORD
};

const char *ST_type_names[ST_TYPE_COUNT] = {
#define ST_TYPE(e, s) s,
    ST_TYPE_LIST
#undef ST_TYPE
};

static const char *ST_multi_symbols[] = {
    "<<=", ">>=", "..=",
    "->", "=>", "==", "!=", "<=", ">=",
    "&&", "||", "<<", ">>",
    "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=",
    "::", ":=", "..", "++", "--",
};

static const char ST_single_symbols[] = "+-*/%=<>!&|^~(){}[],;:.#?@$";

static void ST_sb_char_push(ST_sb_t *sb, char c)
{
    ST_da_append(sb, (u8)c);
}

static ST_string_t ST_sb_to_arena(ST_arena_t *arena, ST_sb_t *sb)
{
    ST_string_t sv = {0};
    sv.data = ST_arena_push_zeroed(arena, sb->count ? sb->count : 1);
    if (sb->count) memcpy(sv.data, sb->items, sb->count);
    sv.len = sb->count;

    free(sb->items);
    sb->items = NULL;
    sb->count = 0;
    sb->capacity = 0;
    return sv;
}

ST_string_t ST_token_kind_to_string(ST_token_kind_t kind)
{
    _Static_assert(ST_TCOUNT == 9, "TCount exceeded");
    switch(kind)
    {
    case ST_TINT:       return ST_cstr_to_str("ST_TINT");
    case ST_TFLOAT:     return ST_cstr_to_str("ST_TFLOAT");
    case ST_TSTRING:    return ST_cstr_to_str("ST_TSTRING");
    case ST_TCHAR:      return ST_cstr_to_str("ST_TCHAR");
    case ST_TIDENT:     return ST_cstr_to_str("ST_TIDENT");
    case ST_TTYPE:      return ST_cstr_to_str("ST_TTYPE");
    case ST_TKEYWORD:   return ST_cstr_to_str("ST_TKEYWORD");
    case ST_TSYMBOL:    return ST_cstr_to_str("ST_TSYMBOL");
    case ST_TDOCCOMENT: return ST_cstr_to_str("ST_TDOCCOMENT");
    case ST_TCOUNT: default: ST_assert(0);
    }

    return (ST_string_t) {
        .data = NULL,
        .len = 0,
    };
}

static char ST_lx_advance_char(ST_lexer_t *l)
{
    if (l->pos >= l->src.len) return 0;
    char c = (char)l->src.data[l->pos++];
    if (c == '\n') {
        l->line++;
        l->col = 1;
    } else {
        l->col++;
    }

    return c;
}

static char ST_lx_peek_char(ST_lexer_t *l)
{
    return l->pos < l->src.len ? (char)l->src.data[l->pos] : 0;
}

static char ST_lx_peek_char_n(ST_lexer_t *l, u32 n)
{
    return l->pos + n < l->src.len ? (char)l->src.data[l->pos + n] : 0;
}

static b8 ST_isdigit(char c)
{
    return c >= '0' && c <= '9';
}

static b8 ST_ishex(char c)
{
    return ST_isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static b8 ST_isalpha(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static b8 ST_isident(char c)
{
    return ST_isalpha(c) || ST_isdigit(c);
}

b8 ST_iswhitespace(char c)
{
    static char buf[] = { '\r', '\n', '\t', ' ' };
    ST_forrange(0, sizeof(buf))  if (c == buf[i]) return 1;
    return 0;
}

b8 ST_is_keyword(ST_string_t s)
{
    ST_forrange(0, ST_KEYWORD_COUNT) if (ST_string_eq_cstr(s, ST_keyword_names[i])) return 1;
    return 0;
}

b8 ST_is_primitive(ST_string_t s)
{
   ST_forrange(0, ST_TYPE_COUNT) if (ST_string_eq_cstr(s, ST_type_names[i])) return 1;
   return 0;
}

b8 ST_is_symbol(ST_string_t s)
{
    if (s.len == 1)
    {
        ST_forrange(0, sizeof(ST_single_symbols) - 1)
            if ((char)s.data[0] == ST_single_symbols[i]) return 1;
        return 0;
    }
    ST_forrange(0, ST_array_len(ST_multi_symbols))
        if (ST_string_eq_cstr(s, ST_multi_symbols[i])) return 1;
    return 0;
}

static ST_string_t ST_lx_slice(ST_lexer_t *l, u32 start, u32 end)
{
    return (ST_string_t) {
        .data = l->src.data + start,
        .len = end - start,
    };
}

ST_string_t ST_lexer_error(ST_string_t src, u32 col, u32 pos)
{
    ST_unused(col);
    if (pos > src.len) pos = src.len;
    u32 start = pos;
    while (start > 0 && src.data[start - 1] != '\n') start--;
    u32 end = pos;
    while (end < src.len && src.data[end] != '\n') end++;
    return (ST_string_t) {
        .data = src.data + start,
        .len = end - start,
    };
}

void ST_colorize_source_line(FILE *out, ST_string_t text, const char *code_col, const char *com_col)
{
    u32 split = text.len;
    ST_forrange(0, text.len ? text.len - 1 : 0)
    {
        if (text.data[i] == '/' && text.data[i + 1] == '/')
        {
            split = i;
            break;
        }
    }
    fprintf(out, "%s%.*s", code_col, (int)split, (char *)text.data);
    if (split < text.len)
        fprintf(out, "%s%.*s", com_col, (int)(text.len - split), (char *)text.data + split);
    fprintf(out, ST_COLOR_RESET);
}

static void ST_lx_error(ST_lexer_t *l, u32 line, u32 col, u32 pos, const char *msg)
{
    l->failed = 1;
    fprintf(stderr,
            ST_COLOR_BOLD ST_sv_fmt ":%u:%u: " ST_COLOR_BOLD_RED "error: "
            ST_COLOR_RESET ST_COLOR_BOLD "%s" ST_COLOR_RESET "\n",
            ST_sv_args(l->file), line, col, msg);

    ST_string_t src_line = ST_lexer_error(l->src, col, pos);
    fprintf(stderr, " %4u " ST_COLOR_GREY "|" ST_COLOR_RESET " ", line);
    ST_colorize_source_line(stderr, src_line, ST_COLOR_WHITE, ST_COLOR_GREY);
    fprintf(stderr, "\n      " ST_COLOR_GREY "|" ST_COLOR_RESET " ");
    for (u32 i = 1; i < col; i++) fputc(' ', stderr);
    fprintf(stderr, ST_COLOR_BOLD_RED "^" ST_COLOR_RESET "\n");
}

void ST_token_error(ST_string_t src)
{
    ST_unused(src);
}

static void ST_lx_skip_line(ST_lexer_t *l)
{
    while (l->pos < l->src.len && ST_lx_peek_char(l) != '\n')
        ST_lx_advance_char(l);
}

static b8 ST_lx_skip_block_comment(ST_lexer_t *l)
{
    u32 line = l->line, col = l->col, pos = l->pos;
    ST_lx_advance_char(l);
    ST_lx_advance_char(l);
    u32 depth = 1;
    while (l->pos < l->src.len && depth > 0)
    {
        char c = ST_lx_peek_char(l);
        char c2 = ST_lx_peek_char_n(l, 1);
        if (c == '/' && c2 == '*')
        {
            depth++;
            ST_lx_advance_char(l);
            ST_lx_advance_char(l);
        }
        else if (c == '*' && c2 == '/')
        {
            depth--;
            ST_lx_advance_char(l);
            ST_lx_advance_char(l);
        }
        else
        {
            ST_lx_advance_char(l);
        }
    }
    if (depth > 0)
    {
        ST_lx_error(l, line, col, pos, "unterminated block comment");
        return 0;
    }
    return 1;
}

static b8 ST_lx_escape(ST_lexer_t *l, ST_sb_t *sb, u32 line, u32 col, u32 pos)
{
    char e = ST_lx_advance_char(l);
    switch (e)
    {
    case 'n':  ST_sb_char_push(sb, '\n'); return 1;
    case 't':  ST_sb_char_push(sb, '\t'); return 1;
    case 'r':  ST_sb_char_push(sb, '\r'); return 1;
    case '0':  ST_sb_char_push(sb, '\0'); return 1;
    case 'e':  ST_sb_char_push(sb, '\x1b'); return 1;
    case '\\': ST_sb_char_push(sb, '\\'); return 1;
    case '\'': ST_sb_char_push(sb, '\''); return 1;
    case '"':  ST_sb_char_push(sb, '"');  return 1;
    case 'x': {
        char h1 = ST_lx_peek_char(l);
        char h2 = ST_lx_peek_char_n(l, 1);
        if (!ST_ishex(h1) || !ST_ishex(h2))
        {
            ST_lx_error(l, line, col, pos, "invalid hex escape, expected \\xNN");
            return 0;
        }
        ST_lx_advance_char(l);
        ST_lx_advance_char(l);
        char buf[3] = { h1, h2, 0 };
        ST_sb_char_push(sb, (char)strtol(buf, NULL, 16));
        return 1;
    }
    default:
        ST_lx_error(l, line, col, pos, "invalid escape sequence");
        return 0;
    }
}

static b8 ST_lx_string(ST_lexer_t *l, ST_token_t *t)
{
    u32 line = l->line, col = l->col, start = l->pos;
    ST_lx_advance_char(l);
    ST_sb_t sb = {0};

    for (;;)
    {
        if (l->pos >= l->src.len || ST_lx_peek_char(l) == '\n')
        {
            ST_lx_error(l, line, col, start, "unterminated string literal");
            free(sb.items);
            return 0;
        }
        char c = ST_lx_advance_char(l);
        if (c == '"') break;
        if (c == '\\')
        {
            if (!ST_lx_escape(l, &sb, l->line, l->col - 1, l->pos - 1))
            {
                free(sb.items);
                ST_lx_skip_line(l);
                return 0;
            }
            continue;
        }
        ST_sb_char_push(&sb, c);
    }

    t->kind = ST_TSTRING;
    t->line = line;
    t->col = col;
    t->text = ST_lx_slice(l, start, l->pos);
    t->str = ST_sb_to_arena(l->arena, &sb);
    return 1;
}

static b8 ST_lx_char(ST_lexer_t *l, ST_token_t *t)
{
    u32 line = l->line, col = l->col, start = l->pos;
    ST_lx_advance_char(l);

    if (l->pos >= l->src.len || ST_lx_peek_char(l) == '\n')
    {
        ST_lx_error(l, line, col, start, "unterminated char literal");
        return 0;
    }

    char c = ST_lx_advance_char(l);
    if (c == '\'')
    {
        ST_lx_error(l, line, col, start, "empty char literal");
        return 0;
    }
    if (c == '\\')
    {
        ST_sb_t sb = {0};
        if (!ST_lx_escape(l, &sb, line, col, start))
        {
            free(sb.items);
            ST_lx_skip_line(l);
            return 0;
        }
        c = (char)sb.items[0];
        free(sb.items);
    }

    if (ST_lx_peek_char(l) != '\'')
    {
        ST_lx_error(l, line, col, start, "unterminated char literal, expected closing '");
        ST_lx_skip_line(l);
        return 0;
    }
    ST_lx_advance_char(l);

    t->kind = ST_TCHAR;
    t->line = line;
    t->col = col;
    t->text = ST_lx_slice(l, start, l->pos);
    t->val.c = c;
    return 1;
}

static b8 ST_lx_number(ST_lexer_t *l, ST_token_t *t)
{
    u32 line = l->line, col = l->col, start = l->pos;
    char buf[128];
    u32 n = 0;
    b8 is_float = 0;

    char c = ST_lx_peek_char(l);
    char c2 = ST_lx_peek_char_n(l, 1);

    if (c == '0' && (c2 == 'x' || c2 == 'X'))
    {
        ST_lx_advance_char(l);
        ST_lx_advance_char(l);
        while (ST_ishex(ST_lx_peek_char(l)) || ST_lx_peek_char(l) == '_')
        {
            char d = ST_lx_advance_char(l);
            if (d != '_' && n < sizeof(buf) - 1) buf[n++] = d;
        }
        if (n == 0)
        {
            ST_lx_error(l, line, col, start, "hex literal has no digits");
            return 0;
        }
        buf[n] = 0;
        t->val.i = (i64)strtoull(buf, NULL, 16);
    }
    else if (c == '0' && (c2 == 'b' || c2 == 'B'))
    {
        ST_lx_advance_char(l);
        ST_lx_advance_char(l);
        while (ST_lx_peek_char(l) == '0' || ST_lx_peek_char(l) == '1' || ST_lx_peek_char(l) == '_')
        {
            char d = ST_lx_advance_char(l);
            if (d != '_' && n < sizeof(buf) - 1) buf[n++] = d;
        }
        if (n == 0)
        {
            ST_lx_error(l, line, col, start, "binary literal has no digits");
            return 0;
        }
        buf[n] = 0;
        t->val.i = (i64)strtoull(buf, NULL, 2);
    }
    else
    {
        while (ST_isdigit(ST_lx_peek_char(l)) || ST_lx_peek_char(l) == '_')
        {
            char d = ST_lx_advance_char(l);
            if (d != '_' && n < sizeof(buf) - 1) buf[n++] = d;
        }
        if (ST_lx_peek_char(l) == '.' && ST_isdigit(ST_lx_peek_char_n(l, 1)))
        {
            is_float = 1;
            if (n < sizeof(buf) - 1) buf[n++] = ST_lx_advance_char(l);
            while (ST_isdigit(ST_lx_peek_char(l)) || ST_lx_peek_char(l) == '_')
            {
                char d = ST_lx_advance_char(l);
                if (d != '_' && n < sizeof(buf) - 1) buf[n++] = d;
            }
        }
        char e = ST_lx_peek_char(l);
        if (e == 'e' || e == 'E')
        {
            char sign = ST_lx_peek_char_n(l, 1);
            u32 save_pos = l->pos, save_line = l->line, save_col = l->col;
            if (n < sizeof(buf) - 1) buf[n++] = ST_lx_advance_char(l);
            if (sign == '+' || sign == '-')
                if (n < sizeof(buf) - 1) buf[n++] = ST_lx_advance_char(l);
            if (!ST_isdigit(ST_lx_peek_char(l)))
            {
                l->pos = save_pos;
                l->line = save_line;
                l->col = save_col;
                n -= (sign == '+' || sign == '-') ? 2 : 1;
            }
            else
            {
                is_float = 1;
                while (ST_isdigit(ST_lx_peek_char(l)))
                {
                    char d = ST_lx_advance_char(l);
                    if (n < sizeof(buf) - 1) buf[n++] = d;
                }
            }
        }
        buf[n] = 0;
        if (is_float) t->val.f = strtod(buf, NULL);
        else          t->val.i = (i64)strtoll(buf, NULL, 10);
    }

    if (ST_isalpha(ST_lx_peek_char(l)))
    {
        ST_lx_error(l, l->line, l->col, l->pos, "invalid character in numeric literal");
        while (ST_isident(ST_lx_peek_char(l))) ST_lx_advance_char(l);
        return 0;
    }

    t->kind = is_float ? ST_TFLOAT : ST_TINT;
    t->line = line;
    t->col = col;
    t->text = ST_lx_slice(l, start, l->pos);
    return 1;
}

static b8 ST_lx_ident(ST_lexer_t *l, ST_token_t *t)
{
    u32 line = l->line, col = l->col, start = l->pos;
    while (ST_isident(ST_lx_peek_char(l))) ST_lx_advance_char(l);

    ST_string_t text = ST_lx_slice(l, start, l->pos);
    if (ST_is_keyword(text))        t->kind = ST_TKEYWORD;
    else if (ST_is_primitive(text)) t->kind = ST_TTYPE;
    else                            t->kind = ST_TIDENT;
    t->line = line;
    t->col = col;
    t->text = text;
    return 1;
}

static b8 ST_lx_doc_comment(ST_lexer_t *l, ST_token_t *t)
{
    u32 line = l->line, col = l->col, start = l->pos;
    ST_lx_advance_char(l);
    ST_lx_advance_char(l);
    ST_lx_advance_char(l);
    if (ST_lx_peek_char(l) == ' ') ST_lx_advance_char(l);

    u32 body = l->pos;
    ST_lx_skip_line(l);

    t->kind = ST_TDOCCOMENT;
    t->line = line;
    t->col = col;
    t->text = ST_lx_slice(l, start, l->pos);
    t->str = ST_lx_slice(l, body, l->pos);
    return 1;
}

static b8 ST_lx_symbol(ST_lexer_t *l, ST_token_t *t)
{
    u32 line = l->line, col = l->col, start = l->pos;

    if (ST_lx_peek_char(l) == '#' && ST_isalpha(ST_lx_peek_char_n(l, 1)))
    {
        ST_lx_advance_char(l);
        while (ST_isident(ST_lx_peek_char(l))) ST_lx_advance_char(l);
        t->kind = ST_TSYMBOL;
        t->line = line;
        t->col = col;
        t->text = ST_lx_slice(l, start, l->pos);
        return 1;
    }

    ST_forrange(0, ST_array_len(ST_multi_symbols))
    {
        u32 len = (u32)strlen(ST_multi_symbols[i]);
        if (l->pos + len <= l->src.len &&
            memcmp(l->src.data + l->pos, ST_multi_symbols[i], len) == 0)
        {
            ST_forrange(0, len) ST_lx_advance_char(l);
            t->kind = ST_TSYMBOL;
            t->line = line;
            t->col = col;
            t->text = ST_lx_slice(l, start, l->pos);
            return 1;
        }
    }

    ST_string_t single = ST_lx_slice(l, start, start + 1);
    if (ST_is_symbol(single))
    {
        ST_lx_advance_char(l);
        t->kind = ST_TSYMBOL;
        t->line = line;
        t->col = col;
        t->text = single;
        return 1;
    }

    ST_lx_error(l, line, col, start, "unexpected character");
    ST_lx_advance_char(l);
    return 0;
}

static b8 ST_lx_next(ST_lexer_t *l, ST_token_t *t)
{
    for (;;)
    {
        while (ST_iswhitespace(ST_lx_peek_char(l)) && l->pos < l->src.len)
            ST_lx_advance_char(l);
        if (l->pos >= l->src.len) return 0;

        char c  = ST_lx_peek_char(l);
        char c2 = ST_lx_peek_char_n(l, 1);

        memset(t, 0, sizeof(*t));
        t->file = l->file;

        if (c == '/' && c2 == '/')
        {
            if (l->pos + 2 < l->src.len && l->src.data[l->pos + 2] == '/')
            {
                if (ST_lx_doc_comment(l, t)) return 1;
                continue;
            }
            ST_lx_skip_line(l);
            continue;
        }
        if (c == '/' && c2 == '*')
        {
            ST_lx_skip_block_comment(l);
            continue;
        }

        if (ST_isdigit(c))
        {
            if (ST_lx_number(l, t)) return 1;
            continue;
        }
        if (ST_isalpha(c))
        {
            if (ST_lx_ident(l, t)) return 1;
            continue;
        }
        if (c == '"')
        {
            if (ST_lx_string(l, t)) return 1;
            continue;
        }
        if (c == '\'')
        {
            if (ST_lx_char(l, t)) return 1;
            continue;
        }

        if (ST_lx_symbol(l, t)) return 1;
    }
}

ST_token_t ST_lexer_advance(ST_lexer_t *l)
{
    ST_token_t t = {0};
    if (!ST_lx_next(l, &t)) t.kind = ST_TCOUNT;
    return t;
}

ST_token_t ST_lexer_peek(ST_lexer_t *l)
{
    ST_lexer_t save = *l;
    ST_token_t t = ST_lexer_advance(l);
    *l = save;
    return t;
}

ST_tokens_t ST_lex(ST_arena_t *arena, ST_string_t src, ST_string_t file)
{
    ST_lexer_t l = {0};
    l.arena = arena;
    l.src = src;
    l.file = file;
    l.line = 1;
    l.col = 1;

    ST_token_t t;
    while (ST_lx_next(&l, &t)) ST_da_append(&l.tokens, t);

    l.tokens.ok = !l.failed;
    return l.tokens;
}


static void ST_print_escaped(ST_string_t s)
{
    ST_forrange(0, s.len)
    {
        char c = (char)s.data[i];
        switch (c)
        {
        case '\n': printf("\\n"); break;
        case '\t': printf("\\t"); break;
        case '\r': printf("\\r"); break;
        case '\0': printf("\\0"); break;
        case '\\': printf("\\\\"); break;
        case '"':  printf("\\\""); break;
        default:
            if ((u8)c < 0x20) printf("\\x%02x", (u8)c);
            else putchar(c);
        }
    }
}

void ST_dump_token(ST_tokens_t tokens)
{
    ST_forrange(0, tokens.count)
    {
        ST_token_t *t = &tokens.items[i];
        printf(ST_sv_fmt ":%u:%u: %-14.*s " ST_sv_fmt,
               ST_sv_args(t->file),
               t->line, t->col,
               ST_sv_args(ST_token_kind_to_string(t->kind)),
               ST_sv_args(t->text));
        switch (t->kind)
        {
        case ST_TINT:        printf("   (= %lld)", (long long)t->val.i); break;
        case ST_TFLOAT:      printf("   (= %g)", t->val.f); break;
        case ST_TSTRING:
        case ST_TDOCCOMENT:
            printf("   (str \"");
            ST_print_escaped(t->str);
            printf("\")");
            break;
        case ST_TCHAR:
            printf("   (= ");
            ST_print_escaped((ST_string_t){ .data = (u8 *)&t->val.c, .len = 1 });
            printf(")");
            break;
        case ST_TIDENT: printf("   (str \"" ST_sv_fmt "\")", ST_sv_args(t->text)); break;
        case ST_TTYPE:
        case ST_TKEYWORD:
        case ST_TSYMBOL:
        case ST_TCOUNT: break;
        }
        printf("\n");
    }        
}
