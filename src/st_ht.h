#ifndef ST_HT_H
#define ST_HT_H

// NOTE: Paper used for this hash table: https://arxiv.org/pdf/2501.02305
#include "st_types.h"


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
    ST_ht_slots_t *slots;
    u32 count;
    u32 capacity;
} ST_ht_t;

void ST_ht_set(ST_ht_t *ht, ST_ht_generic_t *key, ST_ht_generic_t value);
ST_ht_generic_t ST_ht_get(ST_ht_t *ht, ST_ht_generic_t key);
ST_ht_generic_t ST_ht_delete(ST_ht_t *ht, ST_ht_generic_t key);

void ST_hash_bytes(ST_ht_t *ht, u32 bytes);
void ST_ht_free(ST_ht_t *ht);

ST_ht_generic_t *ST_ht_make_key(u32 cap);

#endif
