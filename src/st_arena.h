#ifndef ST_ARENA_H
#define ST_ARENA_H

#include "st_types.h"

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

#endif
