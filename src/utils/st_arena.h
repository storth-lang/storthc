#ifndef ST_ARENA_H
#define ST_ARENA_H

#include <string.h>

#include "./st_helper.h"

typedef struct ST_arena_chunk_t ST_arena_chunk_t;

struct ST_arena_chunk_t
{
    ST_arena_chunk_t *next;
    u64 pos;
    u64 cap;
    u8 data[];
};

typedef struct
{
    ST_arena_chunk_t *head;
    ST_arena_chunk_t *cur;
} ST_arena_t;

ST_arena_t *ST_arena_alloc();
void ST_arena_free(ST_arena_t *arena);

void *ST_arena_push(ST_arena_t *arena, u64 size);
void *ST_arena_push_zeroed(ST_arena_t *arena, u64 size);
void ST_arena_pop(ST_arena_t *arena, u64 pos);

// da_append for lists that live inside arena-owned structures (AST nodes).
// Grows into the arena; abandoned blocks are reclaimed by ST_arena_free.
#define ST_da_append_arena(a, arr, item)                                \
    do {                                                                \
        if ((arr)->count >= (arr)->capacity)                            \
        {                                                               \
            u32 ST_new_cap_ = (arr)->capacity ? (arr)->capacity * 2 : 8;\
            void *ST_new_items_ = ST_arena_push((a),                    \
                sizeof(*(arr)->items) * ST_new_cap_);                   \
            if ((arr)->count)                                           \
                memcpy(ST_new_items_, (arr)->items,                     \
                       sizeof(*(arr)->items) * (arr)->count);           \
            (arr)->items = ST_new_items_;                               \
            (arr)->capacity = ST_new_cap_;                              \
        }                                                               \
        (arr)->items[(arr)->count++] = item;                            \
    } while(0)

#endif
