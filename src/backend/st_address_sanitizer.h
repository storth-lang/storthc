#ifndef ST_ADDRESS_SANITIZER_H
#define ST_ADDRESS_SANITIZER_H

#include <stdio.h>

#include "../frontend/st_ast.h"
#include "../utils/st_arena.h"
#include "../utils/st_srcmap.h"

b8 ST_asan_instrument(ST_arena_t *arena, ST_program_t *prog, ST_srcmap_t *srcs);
void ST_asan_write_runtime_asm(FILE *out, b8 use_fasm);

#endif
