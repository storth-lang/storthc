#include "st_process.h"
#include <stdio.h>

void ST_set_proc_arena(ST_arena_t *arena, ST_proc_t *proc) {
    ST_assert(arena != NULL);
    if (!proc->arena)
        proc->arena = arena;
}

void ST_procs_init(ST_arena_t *arena, ST_procs_t *procs) {
    ST_assert(arena != NULL);
    procs->arena = arena;
    procs->items = NULL;
    procs->count = 0;
    procs->capacity = 0;
}

void ST_append_process_opt(ST_procs_t *procs, ST_proc_opt_t opt) {
    ST_assert(procs->arena != NULL);
    ST_proc_t p = {0};
    p.opt = opt;
    p.id = (void *)-1;
    p.arena = procs->arena;
    ST_da_append_arena(procs->arena, procs, p);
}

b8 ST_run_processes(ST_procs_t *procs) {
    b8 ok = 1;

    for (u32 i = 0; i < procs->count; i++) {
        if (!ST_run_process(&procs->items[i]))
            ok = 0;
    }

    for (u32 i = 0; i < procs->count; i++) {
        if (!ST_wait_process(&procs->items[i]))
            ok = 0;
    }

    procs->count = 0;
    return ok;
}
