#include "st_types.h"

static ST_ty_t *ST_ty_alloc(ST_ty_ctx_t *ctx, ST_ty_kind_t kind, u32 width, u32 align)
{
    ST_ty_t *t = ST_arena_push_zeroed(ctx->arena, sizeof(*t));
    t->kind = kind;
    t->width = width;
    t->align = align;
    return t;
}

static ST_ty_t *ST_ty_int(ST_ty_ctx_t *ctx, u32 width, b8 is_signed)
{
    u32 size  = width / 8;
    ST_ty_t *t = ST_ty_alloc(ctx, ST_TY_INT, size, size);
    t->width = width;
    t->is_signed = is_signed;
    return t;
}

static ST_ty_t *ST_ty_float(ST_ty_ctx_t *ctx, u32 width)
{
    u32 size  = width / 8;
    ST_ty_t *t = ST_ty_alloc(ctx, ST_TY_INT, size, size);
    t->width = width;
    return t;
}

void ST_ty_ctx_init(ST_ty_ctx_t *ctx, ST_arena_t *arena)
{
    ctx->arena = arena;
    ST_ht_init(arena, &ctx->interned, 64);
    ST_ht_init(arena, &ctx->decl_type, 64);

    ctx->prim[ST_ti8] = ST_ty_int(ctx, 8, 1);
    ctx->prim[ST_ti16] = ST_ty_int(ctx, 16, 1);
    ctx->prim[ST_ti32] = ST_ty_int(ctx, 32, 1);
    ctx->prim[ST_ti64] = ST_ty_int(ctx, 64, 1);
    ctx->prim[ST_tu8] = ST_ty_int(ctx, 8, 0);
    ctx->prim[ST_tu16] = ST_ty_int(ctx, 16, 0);
    ctx->prim[ST_tu32] = ST_ty_int(ctx, 32, 0);
    ctx->prim[ST_tu64] = ST_ty_int(ctx, 64, 0);
    ctx->prim[ST_tf32] = ST_ty_float(ctx, 32);
    ctx->prim[ST_tf64] = ST_ty_float(ctx, 64);
    ctx->prim[ST_tf128] = ST_ty_float(ctx, 128);
    ctx->prim[ST_tchar] = ST_ty_alloc(ctx, ST_TY_CHAR, 1, 1);
    ctx->prim[ST_tvoid] = ST_ty_alloc(ctx, ST_TY_VOID, 0, 1);
    ctx->prim[ST_tany] = ST_ty_alloc(ctx, ST_TY_ANY, 16, 8);
    ctx->prim[ST_tbool] = ST_ty_alloc(ctx, ST_TY_BOOL, 1, 1);
    ctx->prim[ST_tstring] = ST_ty_alloc(ctx, ST_TY_STRING, 16, 8);

    ctx->untyped_int = ST_ty_alloc(ctx, ST_TY_UNTYPED_INT, 8, 8);
    ctx->untyped_int->width = 64;
    ctx->untyped_int->is_signed = 1;
    ctx->untyped_float = ST_ty_alloc(ctx, ST_TY_UNTYPED_FLOAT, 8, 8);
    ctx->untyped_float->width = 64;
    ctx->null_ptr = ST_ty_alloc(ctx, ST_TY_PTR, 8, 8);
    ctx->null_ptr->inner = ctx->prim[ST_tvoid];
    
}

ST_ty_t *ST_ty_prim(ST_ty_ctx_t *ctx, ST_type_t t)
{
    return ctx->prim[t];
}

ST_ty_t *ST_ty_prim_named(ST_ty_ctx_t *ctx, ST_type_t t, ST_string_t name)
{
    ST_forrange(0, ST_TYPE_COUNT)
    {
        if (ST_string_eq_cstr(name, ST_type_names[i])) return ctx->prim[t];
    }
    return NULL;
}

typedef struct
{
    u64 kind;
    u64 inner;
    u64 count;
} ST_intern_key_t;

static ST_ty_t *ST_ty_intern(ST_ty_ctx_t *ctx, ST_ty_kind_t kind,
                            ST_ty_t *inner, u64 count,
                            u32 width, u32 align)
{
    ST_intern_key_t key = {
        (u64)kind,
        (u64)(uintptr_t)inner,
        count,
    };

    ST_ht_generic_t k = {
        .tag = &key,
        .size = sizeof(key),
    };
    ST_ty_t *found = (ST_ty_t *)ST_ht_get(&ctx->interned, k).tag;
    if (found) return found;

    ST_ty_t *t = ST_ty_alloc(ctx, kind, width, align);
    t->inner = inner;
    t->count = count;

    ST_intern_key_t *pk = ST_arena_push(ctx->arena, sizeof(*pk));
    *pk = key;

    ST_ht_generic_t *hk = ST_arena_push(ctx->arena, sizeof(*hk));
    hk->tag = pk;
    hk->size = sizeof(*pk);
    ST_ht_set(&ctx->interned, hk, (ST_ht_generic_t){ .tag = t, .size = 0});

    return t;
}

ST_ty_t *ST_ty_ptr(ST_ty_ctx_t *ctx, ST_ty_t *inner)
{
    return ST_ty_intern(ctx, ST_TY_PTR, inner, 0, 8, 8);
}

ST_ty_t *ST_ty_array(ST_ty_ctx_t *ctx, ST_ty_t *inner, u64 count)
{
    u32 align = inner->align ? inner->align : 1;
    return ST_ty_intern(ctx, ST_TY_ARRAY, inner,
                        (u32)(inner->size * count), 8, align);
    
}

ST_ty_t *ST_ty_dyn_array(ST_ty_ctx_t *ctx, ST_ty_t *inner)
{
    return ST_ty_intern(ctx, ST_TY_DYN_ARRAY, inner, 0, 24, 8);
}

ST_ty_t *ST_ty_fn_new(ST_ty_ctx_t *ctx)
{
    return ST_ty_alloc(ctx, ST_TY_FN, 8, 8);
}

ST_ty_t *ST_ty_for_decls(ST_ty_ctx_t *ctx, ST_decl_t *d)
{
    ST_ht_generic_t k = {
        .tag = &d,
        .size = sizeof(d),
    };
    ST_ty_t *found = (ST_ty_t *)ST_ht_get(&ctx->decl_type, k).tag;
    if (found) return found;
    ST_ty_kind_t kind = ST_TY_STRUCT;
    if (d->kind == ST_DE_ENUM) kind = ST_TY_ENUM;
    else if (d->kind == ST_DE_TAG_UNION) kind = ST_TY_TAG_UNION;

    ST_ty_t *t = ST_ty_alloc(ctx, kind, 0, 1);
    t->decl = d;
    if (kind == ST_TY_ENUM)
    {
        t->size = 8;
        t->align = 8;
        t->state = ST_TY_STATE_DONE;
    }
    ST_decl_t **pd = ST_arena_push_zeroed(ctx->arena, sizeof(*pd));
    *pd = d;
    ST_ht_generic_t *hk = ST_arena_push(ctx->arena, sizeof(*hk));
    hk->tag = pd;
    hk->size = sizeof(*pd);
    ST_ht_set(&ctx->decl_type, hk, (ST_ht_generic_t){ .tag = t, .size = 0});
    return t;
}

b8 ST_ty_equal(ST_ty_t *a, ST_ty_t *b)
{
    if (a == b) return 1;
    if (!a || !b) return 0;
    if (a->kind != b->kind) return 0;

    if (a->kind == ST_TY_FN)
    {
        if (a->params.count != b->params.count) return 0;
        if (a->rets.count != b->rets.count) return 0;
        if (a->is_variadic != b->is_variadic) return 0;
        ST_forrange(0, a->params.count)
            if (!ST_ty_equal(a->params.items[i], b->params.items[i])) return 0;
        ST_forrange(0, a->rets.count)
            if (!ST_ty_equal(a->rets.items[i], b->rets.items[i])) return 0;
        return 1;
    }
    return 0;
}

b8 ST_ty_is_int(ST_ty_t *t)
{
    return t && (t->kind == ST_TY_INT || t->kind == ST_TY_UNTYPED_INT);
}

b8 ST_ty_is_float(ST_ty_t *t)
{
    return t && (t->kind == ST_TY_FLOAT || t->kind == ST_TY_UNTYPED_FLOAT);
}

b8 ST_ty_is_numeric(ST_ty_t *t)
{
    return ST_ty_is_int(t) || ST_ty_is_float(t);
}

b8 ST_ty_is_untyped(ST_ty_t *t)
{
    return t && (t->kind == ST_TY_UNTYPED_INT || t->kind == ST_TY_UNTYPED_INT);
}

static void ST_ty_dump(ST_sb_t *sb, ST_ty_t *t)
{
    if (!t)
    {
        ST_append_to_builder(sb, "<unknown>");
        return;
    }

    char buf[256];
    switch(t->kind) {
    case ST_TY_VOID: ST_append_to_builder(sb, "void"); break;
    case ST_TY_BOOL: ST_append_to_builder(sb, "bool"); break;
    case ST_TY_CHAR: ST_append_to_builder(sb, "char"); break;
    case ST_TY_STRING: ST_append_to_builder(sb, "string"); break;
    case ST_TY_ANY: ST_append_to_builder(sb, "any"); break;
    case ST_TY_INT:
        snprintf(buf, sizeof(buf), "%c%u", t->is_signed ? 'i' : 'u', t->width);
        ST_append_to_builder(sb, buf); break;        
    case ST_TY_FLOAT:
        snprintf(buf, sizeof(buf), "f%u", t->width);
        ST_append_to_builder(sb, buf); break;        
    case ST_TY_UNTYPED_INT: ST_append_to_builder(sb, "untyped int"); break;
    case ST_TY_UNTYPED_FLOAT: ST_append_to_builder(sb, "untyped float"); break;
    case ST_TY_PTR:
        ST_append_to_builder(sb, "*");
        ST_ty_dump(sb, t->inner);
        break;
    case ST_TY_ARRAY: 
        snprintf(buf, sizeof(buf), "[%llu]", (unsigned long long)t->count);
        ST_append_to_builder(sb, buf);
        ST_ty_dump(sb, t->inner);
        break;
    case ST_TY_DYN_ARRAY:
        ST_append_to_builder(sb, "[..]");
        ST_ty_dump(sb, t->inner);
        break;
    case ST_TY_STRUCT:
    case ST_TY_ENUM:
    case ST_TY_TAG_UNION:
        if (t->decl && t->decl->name.len)
        {
            for (u32 k = 0; k < t->decl->name.len; k++) {
                ST_da_append(sb, t->decl->name.data[k]);
            }
        } else  ST_append_to_builder(sb, "<anon>");
        break;
    case ST_TY_FN:
        ST_append_to_builder(sb, "fn(");
        ST_forrange(0, t->params.count)
        {
            ST_append_to_builder(sb, ", ");
            ST_ty_dump(sb, t->params.items[i]);
        }

        if (t->is_variadic) ST_append_to_builder(sb, t->params.count ? ", .." : ".. ");
        ST_append_to_builder(sb, ")");
        if (t->rets.count)
        {
            ST_append_to_builder(sb, "->");
            ST_forrange(0, t->rets.count)
            {
                if (i) ST_append_to_builder(sb, ", ");
                ST_ty_dump(sb, t->rets.items[i]);
            }
        }
        break;
    case ST_TY_COUNT: ST_assert(0); break;
    }
}

const char *ST_ty_cstr(ST_arena_t *a, ST_ty_t *t)
{
    ST_sb_t sb = {0};
    ST_ty_dump(&sb, t);
    char *o = ST_arena_push(a, sb.count + 1);
    if (sb.count) memcpy(o, sb.items, sb.count);
    o[sb.count] = 0;
    free(sb.items);
    return o;
}
