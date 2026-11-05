#ifndef ST_ABI_H
#define ST_ABI_H

#include "../frontend/st_types.h"
#include "../utils/st_helper.h"

typedef struct ST_abi_t ST_abi_t;

typedef enum {
    ST_ABI_SYSV_X86_64,
    ST_ABI_WIN64,
    ST_ABI_COUNT,
} ST_abi_kind_t;

typedef enum {
    ST_ABI_PART_I64,
    ST_ABI_PART_F32,
    ST_ABI_PART_F64,
} ST_abi_part_kind_t;

typedef enum {
    ST_ABI_PASS_PARTS,
    ST_ABI_PASS_POINTER,
    ST_ABI_PASS_MEMORY,
} ST_abi_pass_t;

typedef struct {
    ST_abi_pass_t pass;
    u32 n_parts;
    ST_abi_part_kind_t kinds[2];
} ST_abi_arg_t;

const ST_abi_t *ST_abi_get(ST_abi_kind_t kind);

const ST_abi_t *ST_abi_host(void);

ST_abi_arg_t ST_abi_classify_arg(const ST_abi_t *abi, ST_ty_t *ty, b8 is_extern);

u32 ST_abi_part_count(ST_ty_t *ty);

ST_abi_part_kind_t ST_abi_part_kind(ST_ty_t *ty, u32 part);

u32 ST_abi_ret_slots(const ST_tys_t *tys);

b8 ST_abi_ret_is_indirect(u32 slots);

#endif
