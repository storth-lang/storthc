#include "st_abi.h"

struct ST_abi_t {
    ST_abi_kind_t kind;
};

static const ST_abi_t ST_abi_table[ST_ABI_COUNT] = {
    [ST_ABI_SYSV_X86_64] = {ST_ABI_SYSV_X86_64},
    [ST_ABI_WIN64] = {ST_ABI_WIN64},
};

const ST_abi_t *ST_abi_get(ST_abi_kind_t kind) {
    return &ST_abi_table[kind];
}

const ST_abi_t *ST_abi_host(void) {
#ifdef _WIN32
    return ST_abi_get(ST_ABI_WIN64);
#else
    return ST_abi_get(ST_ABI_SYSV_X86_64);
#endif
}

u32 ST_abi_part_count(ST_ty_t *ty) {
    return (ty->size + 7) / 8;
}

static b8 ST_abi_part_is_sse(ST_ty_t *ty, u32 part) {
    u32 lo = part * 8, hi = lo + 8;
    if (ty->kind == ST_TY_ARRAY) {
        if (!ST_ty_is_float(ty->inner))
            return 0;
        return lo < ty->size;
    }
    b8 saw_field = 0, all_float = 1;
    for (u32 i = 0; i < ty->fields.count; i++) {
        ST_ty_field_t *f = &ty->fields.items[i];
        u32 fend = f->offset + f->ty->size;
        if (fend <= lo || f->offset >= hi)
            continue;
        saw_field = 1;
        if (!ST_ty_is_float(f->ty))
            all_float = 0;
    }
    return saw_field && all_float;
}

ST_abi_part_kind_t ST_abi_part_kind(ST_ty_t *ty, u32 part) {
    return ST_abi_part_is_sse(ty, part) ? ST_ABI_PART_F64 : ST_ABI_PART_I64;
}

static b8 ST_abi_win64_is_scalar_size(u32 size) {
    return size == 1 || size == 2 || size == 4 || size == 8;
}

static b8 ST_abi_win64_is_single_float(ST_ty_t *ty) {
    if (ty->kind == ST_TY_ARRAY)
        return ty->count == 1 && ST_ty_is_float(ty->inner) && ty->inner->size == ty->size;
    if (ty->fields.count != 1)
        return 0;
    ST_ty_t *fty = ty->fields.items[0].ty;
    return ST_ty_is_float(fty) && fty->size == ty->size;
}

static ST_abi_arg_t ST_abi_classify_win64_extern(ST_ty_t *ty) {
    ST_abi_arg_t a = {0};
    if (!ST_abi_win64_is_scalar_size(ty->size)) {
        a.pass = ST_ABI_PASS_POINTER;
        return a;
    }
    a.pass = ST_ABI_PASS_PARTS;
    a.n_parts = 1;
    if (ST_abi_win64_is_single_float(ty))
        a.kinds[0] = ty->size == 4 ? ST_ABI_PART_F32 : ST_ABI_PART_F64;
    else
        a.kinds[0] = ST_ABI_PART_I64;
    return a;
}

ST_abi_arg_t ST_abi_classify_arg(const ST_abi_t *abi, ST_ty_t *ty, b8 is_extern) {
    if (abi->kind == ST_ABI_WIN64 && is_extern)
        return ST_abi_classify_win64_extern(ty);

    ST_abi_arg_t a = {0};
    if (ty->size > 16) {
        a.pass = is_extern ? ST_ABI_PASS_MEMORY : ST_ABI_PASS_POINTER;
        return a;
    }
    a.pass = ST_ABI_PASS_PARTS;
    a.n_parts = ST_abi_part_count(ty);
    for (u32 k = 0; k < a.n_parts && k < 2; k++)
        a.kinds[k] = ST_abi_part_kind(ty, k);
    return a;
}

u32 ST_abi_ret_slots(const ST_tys_t *tys) {
    u32 total = 0;
    for (u32 i = 0; i < tys->count; i++) {
        ST_ty_t *t = tys->items[i];
        total += t ? (t->size + 7u) / 8u : 1;
    }
    return total;
}

b8 ST_abi_ret_is_indirect(u32 slots) {
    return slots > 2;
}
