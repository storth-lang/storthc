#ifndef ST_HT_H
#define ST_HT_H

// NOTE: Paper used for this hash table: https://arxiv.org/pdf/2501.02305
#include "st_helper.h"
#include "st_arena.h"

typedef struct
{
    void *tag;
    u32 size;
} ST_ht_generic_t;

typedef struct
{
    ST_ht_generic_t *key;
    ST_ht_generic_t *value;
} ST_ht_slots_t;

typedef struct
{
    ST_arena_t *arena;
    ST_ht_slots_t *slots;
    u32 count;
    u32 capacity;
} ST_ht_t;

void ST_ht_init(ST_arena_t *arena, ST_ht_t *ht, u32 init_cap);
void ST_ht_set(ST_ht_t *ht, ST_ht_generic_t *key, ST_ht_generic_t value);

ST_ht_generic_t ST_ht_get(ST_ht_t *ht, ST_ht_generic_t key);
ST_ht_generic_t ST_ht_delete(ST_ht_t *ht, ST_ht_generic_t key);

#endif
