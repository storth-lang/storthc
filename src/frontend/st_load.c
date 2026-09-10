#include <string.h>

#include "st_load.h"
#include "../utils/platform/st_platform.h"

typedef struct {
    ST_arena_t *arena;
    ST_ht_t seen;
    ST_srcmap_t *srcs;
} ST_load_ctx_t;

static char *ST_str_cstr(ST_arena_t *a, ST_string_t s) {
    char *buf = ST_arena_push(a, s.len + 1);
    memcpy(buf, s.data, s.len);
    buf[s.len] = 0;
    return buf;
}

static ST_string_t ST_dirname_sv(ST_string_t path) {
    u32 i = path.len;
    while (i > 0 && path.data[i - 1] != '/' && path.data[i - 1] != '\\')
        i--;
    if (i == 0)
        return ST_cstr_to_str(".");
    return (ST_string_t){.data = path.data, .len = i - 1};
}

static ST_string_t ST_join_dir(ST_arena_t *a, ST_string_t dir, ST_string_t rel) {
    if (rel.len && ST_path_is_absolute((const char *)rel.data))
        return rel;
    u32 total = dir.len + 1 + rel.len;
    u8 *buf = ST_arena_push(a, total + 1);
    memcpy(buf, dir.data, dir.len);
    buf[dir.len] = '/';
    memcpy(buf + dir.len + 1, rel.data, rel.len);
    buf[total] = 0;
    return (ST_string_t){.data = buf, .len = total};
}

static b8 ST_load_expand(ST_load_ctx_t *ctx, ST_string_t path, ST_string_t from_dir,
                         ST_tokens_t *out) {
    ST_string_t full = ST_join_dir(ctx->arena, from_dir, path);
    ST_string_t canon = ST_abs_path(ctx->arena, ST_str_cstr(ctx->arena, full));

    ST_ht_generic_t key = {.tag = canon.data, .size = canon.len};
    if (ST_ht_get(&ctx->seen, key).tag)
        return 1; // pragma-once: already loaded, contributes nothing

    ST_ht_generic_t *hk = ST_arena_push(ctx->arena, sizeof(*hk));
    hk->tag = canon.data;
    hk->size = canon.len;
    ST_ht_set(&ctx->seen, hk, (ST_ht_generic_t){.tag = (void *)1, .size = 0});

    ST_string_t src;
    if (!ST_read_entire_file(ctx->arena, &src, ST_str_cstr(ctx->arena, canon))) {
        fprintf(stderr,
                ST_COLOR_BOLD_RED "error: " ST_COLOR_RESET "could not load '" ST_sv_fmt "'\n",
                ST_sv_args(canon));
        return 0;
    }
    if (ctx->srcs)
        ST_srcmap_put(ctx->srcs, canon, src);

    ST_tokens_t toks = ST_lex(ctx->arena, src, canon);
    if (!toks.ok)
        return 0;

    ST_string_t dir = ST_dirname_sv(canon);

    for (u32 i = 0; i < toks.count; i++) {
        ST_token_t *t = &toks.items[i];
        if (t->kind == ST_TSYMBOL && ST_string_eq_cstr(t->text, "#load")) {
            if (i + 1 >= toks.count || toks.items[i + 1].kind != ST_TSTRING) {
                fprintf(stderr,
                        ST_COLOR_BOLD_RED "error: " ST_COLOR_RESET ST_sv_fmt
                                          ":%u:%u: expected a string after 'load'\n",
                        ST_sv_args(canon), t->line, t->col);
                return 0;
            }
            ST_string_t rel = toks.items[i + 1].str;
            u32 next = i + 2;
            if (next < toks.count && toks.items[next].kind == ST_TSYMBOL &&
                ST_string_eq_cstr(toks.items[next].text, ";"))
                next++;

            if (!ST_load_expand(ctx, rel, dir, out))
                return 0;
            i = next - 1;
            continue;
        }
        ST_da_append_arena(ctx->arena, out, *t);
    }
    return 1;
}

b8 ST_load_file(ST_arena_t *arena, ST_string_t entry_path, ST_srcmap_t *srcs, ST_tokens_t *out) {
    ST_load_ctx_t ctx = {.arena = arena, .srcs = srcs};
    ST_ht_init(arena, &ctx.seen, 16);
    *out = (ST_tokens_t){0};
    b8 ok = ST_load_expand(&ctx, entry_path, ST_cstr_to_str("."), out);
    out->ok = ok;
    return ok;
}
