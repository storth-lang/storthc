#include <string.h>
#include <math.h>

#include "st_ht.h"

#define ST_MAX_LVL (sizeof(u32) * 8)
#define ST_MIN_LVL (sizeof(u32))

#define ST_TOMBSTONE ((ST_ht_generic_t *)1)
#define ST_DELTA 0.05

typedef struct
{
    u32 start;
    u32 size;
} ST_level_t;

static u32 ST_build_levels_(u32 cap, ST_level_t lvls[ST_MAX_LVL])
{
    u32 n = 0;
    u32 rem = cap;
    u32 start = 0;
    if (cap == 0) return 0;

    while (rem > 0 && n < ST_MAX_LVL - 1)
    {
        u32 size = rem - (rem / 2);
        lvls[n].start = start;
        lvls[n].size = size;

        start += size;
        rem   -= size;

        n++;
        if (size <= ST_MAX_LVL || rem <= ST_MIN_LVL)
        {
            if (rem > 0)
            {
                lvls[n - 1].size += rem;
                rem = 0;
            }
            break;
        }
    }
    return n;
}

void ST_hash_bytes(ST_ht_t *ht, u32 bytes)
{
    u32 capacity;
    if (!ht) return;
    capacity = bytes < 8 ? 8 : bytes;
    ht->slots = calloc(capacity, sizeof(*ht->slots));
    ht->capacity = capacity;
    ht->count = 0;
}

static u64 ST_fnv1a(const void *data, u32 len, u64 seed)
{
    const u8* p = (const u8*)data;
    u64 h = 1469598103934665603ULL ^ seed;
    for (u32 i = 0; i < len; ++i)
    {
        h ^= (u64)p[i];
        h *= 1099511628211ULL;
    }

    return h;
}

static b32 ST_key_equal(const ST_ht_generic_t *key1, const ST_ht_generic_t *key2)
{
    if (key1->size != key2->size) return 0;
    if (key1->size == 0) return 1;
    return (memcmp(key1->tag, key2->tag, key1->size) == 0);
}

static u32 ST_probe_slot(const ST_ht_generic_t *key, u32 level, u32 probe, u32 level_size)
{
    u64 seed = ((u64)level<< 32) ^ (u64)((probe + 1) * 0x9E3779B97F4A7C15ULL);
    u64 h;
    if (level_size == 0) return 0;
    h = ST_fnv1a(key->tag, key->size, seed);
    h ^= (h >> 33);
    return (u32)(h % (u64)level_size);
}


static i32 ST_slot_free(ST_ht_generic_t *key)
{
    return key == NULL || key == ST_TOMBSTONE;
}

static u32 ST_probe_budget(f64 eps)
{
    const f64 c = 4.0;
    f64 e = eps > 1e-9 ? eps : 1e-9;
    f64 log2_inv_eps = log2(1.0 / e);
    f64 log2_inv_delta = log2(1.0 / ST_DELTA);

    f64 f = c * (log2_inv_eps < log2_inv_delta ? log2_inv_eps : log2_inv_delta);
    if (f < 1.0) f = 1.0;

    return (u32)(f + 0.999);
}

static u32 ST_find_slot(ST_ht_t *ht, const ST_ht_generic_t *key)
{
    ST_level_t levels[ST_MAX_LVL];
    u32 nlevels = ST_build_levels_(ht->capacity, levels);
    for (u32 i = 0; i < nlevels; i++)
    {
        for (u32 j = 0; j < levels[i].size; j++)
        {
            u32 slot = levels[i].start + ST_probe_slot(key, i, j, levels[i].size);
            ST_ht_generic_t *k = ht->slots[slot].key;
            if (k == NULL) break;
            if (k == ST_TOMBSTONE) continue;
            if (ST_key_equal(k, key)) return slot;
        }
    }
    return ht->capacity; // This is to denote not found.
}

static void ST_ht_grow(ST_ht_t *ht)
{
    u32 old_cap = ht->capacity;
    ST_ht_slots_t *old_slots = ht->slots;
    u32 new_cap = old_cap < 8 ? 8 : old_cap;
    ST_unused(new_cap);

    if (old_slots)
    {
        for (u32 i = 0; i < old_cap; i++)
        {
            if (old_slots[i].key != NULL && old_slots[i].key != ST_TOMBSTONE)
            {
                ST_ht_generic_t v = *old_slots[i].value;
                free(old_slots[i].value);
                ST_ht_set(ht, old_slots[i].key, v);
            }
        }
        free(old_slots);
    }
}

static u32 st_count_free(ST_ht_t *ht, ST_level_t lvl)
{
    u32 free_count = 0;
    for (u32 i = 0; i < lvl.size; i++)
    {
        if (ST_slot_free(ht->slots[lvl.start + i].key)) free_count++;
    }
    return free_count;
}

static b32 st_try_place(ST_ht_t *ht, ST_level_t lvl, ST_ht_generic_t *key, ST_ht_generic_t value, u32 level_index, u32 budget)
{
    u32 j;
    if (budget > lvl.size) budget = lvl.size;

    for (j = 0; j < budget; j++)
    {
        u32 slot = lvl.start + ST_probe_slot(key, level_index, j, lvl.size);
        if (ST_slot_free(ht->slots[slot].key))
        {
            ST_ht_generic_t *stored = malloc(sizeof(*stored));
            if (!stored) return 0;
            *stored = value;
            ht->slots[slot].key = key;
            ht->slots[slot].value = stored;
            ht->count++;
            return 1;
        }
    }
    return 0;
}


static u32 ST_count_free(ST_ht_t *ht, ST_level_t lvl)
{
    u32 free_count = 0;
    for (u32 i = 0; i < lvl.size; i++)
    {
        if (ST_slot_free(ht->slots[lvl.start + i].key)) free_count++;
    }
    return free_count;
}


void ST_ht_set(ST_ht_t *ht, ST_ht_generic_t *key, ST_ht_generic_t value)
{
    ST_level_t levels[ST_MAX_LVL];

    if (!ht || !key) return;
    if (!ht->slots || ht->capacity == 0) ST_hash_bytes(ht, 8);

    u32 existing = ST_find_slot(ht, key);
    if (existing < ht->capacity)
    {
        *ht->slots[existing].value = value;
        return;
    }
    if ((f64)(ht->count + 1) > (f64)ht->capacity * (1.0 - ST_DELTA))  ST_ht_grow(ht);

    u32 nlevels = ST_build_levels_(ht->capacity, levels);

    for (u32 i = 0; i < nlevels; i++)
    {
        int has_next = (i + 1 < nlevels);
        u32 free1 = ST_count_free(ht, levels[i]);
        f64 eps1 = (f64)free1 / (f64)levels[i].size;
        f64 eps2 = has_next ? (f64)st_count_free(ht, levels[i + 1]) / (f64)levels[i + 1].size : 0.0;

        if (has_next && eps1 > ST_DELTA / 2.0 && eps2 > 0.25)
        {
            u32 budget = ST_probe_budget(eps1);
            if (st_try_place(ht, levels[i], key, value, i, budget)) return;
            if (st_try_place(ht, levels[i + 1], key, value, i + 1, levels[i + 1].size)) return;
            continue;
        }
        else if (!has_next || eps1 <= ST_DELTA / 2.0)
        {
            if (has_next)
            {
                if (st_try_place(ht, levels[i + 1], key, value, i + 1, levels[i + 1].size)) return;
            }
            else
            {
                if (st_try_place(ht, levels[i], key, value, i, levels[i].size)) return;
            }
            continue;
        }
        else
        {
            if (st_try_place(ht, levels[i], key, value, i, levels[i].size)) return;
            continue;
        }
    }

    ST_ht_grow(ht);
    ST_ht_set(ht, key, value);
}

ST_ht_generic_t ST_ht_get(ST_ht_t *ht, ST_ht_generic_t key)
{
    ST_ht_generic_t not_found = {0};

    if (!ht || !ht->slots) return not_found;
    u32 slot = ST_find_slot(ht, &key);
    if (slot < ht->capacity) return *ht->slots[slot].value;

    return not_found;
}

ST_ht_generic_t ST_ht_delete(ST_ht_t *ht, ST_ht_generic_t key)
{
    ST_ht_generic_t removed = {0};
    if (!ht || !ht->slots) return removed;
    u32 slot = ST_find_slot(ht, &key);
    if (slot < ht->capacity)
    {
        removed = *ht->slots[slot].value;
        free(ht->slots[slot].value);
        ht->slots[slot].value = ST_TOMBSTONE;
        ht->count--;
    }

    return removed;
}

void ST_ht_free(ST_ht_t *ht)
{
    if (!ht) return;
    if (ht->slots)
    {
        for (u32 i = 0; i < ht->capacity; i++)
        {
            if (ht->slots[i].value) free(ht->slots[i].value);
        }
        free(ht->slots);
    }
    ht->slots = NULL;
    ht->count = 0;
    ht->capacity = 0;
}
