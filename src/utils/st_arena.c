#include <stdlib.h>
#include <string.h>

#include "st_arena.h"

#define ST_DEFAULT_CHUNK_SIZE 4096
#define ST_is_allign(x, y) ((x & (y - 1)) == 0)

static u64 ST_allign_mem(u64 pos, u64 size)
{
    if (ST_is_allign(pos, size)) return pos;
    return ((pos + (size - 1)) & ~(size - 1));
}

static ST_arena_chunk_t *ST_arena_chunk_alloc(u64 capacity)
{
    ST_arena_chunk_t *chunk = calloc(1, sizeof(*chunk) + capacity);
    ST_assert(chunk != NULL);
    chunk->next = NULL;
    chunk->pos = 0;
    chunk->cap = capacity;
    return chunk;
}

ST_arena_t *ST_arena_alloc(void)
{
    ST_arena_t *arena = malloc(sizeof(*arena));
    arena->head = ST_arena_chunk_alloc(ST_DEFAULT_CHUNK_SIZE);
    arena->cur = arena->head;
    return arena;
}

void ST_arena_free(ST_arena_t *arena)
{
    if (arena)
    {
        ST_arena_chunk_t *chunk = arena->head;
        while(chunk)
        {
            ST_arena_chunk_t *next = chunk->next;
            free(chunk);
            chunk = next;
        }
    }
    free(arena);
}

void *ST_arena_push(ST_arena_t *arena, u64 size)
{
    ST_assert(arena != NULL);
    ST_arena_chunk_t *chunk = arena->cur;
    u64 pos = ST_allign_mem(chunk->pos, size);
    if (pos + size > chunk->cap)
    {
        u64 cap = ST_MAX(size, (u64)ST_DEFAULT_CHUNK_SIZE);
        ST_arena_chunk_t *next = ST_arena_chunk_alloc(cap);
        chunk->next = next;
        arena->cur = next;
        chunk = next;
        pos = 0;
    }

    chunk->pos = pos + size;
    return chunk->data + pos;
}

void *ST_arena_push_zeroed(ST_arena_t *arena, u64 size)
{
    void *pushed = ST_arena_push(arena, size);
    memset(pushed, 0, size);
    return pushed;
}

void ST_arena_pop(ST_arena_t *arena, u64 pos)
{
    pos = ST_MIN(arena->cur->pos, pos);
    arena->cur->pos -= pos;
}

#undef ST_DEFAULT_CHUNK_SIZE
#undef ST_IS_ALLIGN
