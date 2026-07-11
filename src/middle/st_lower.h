#ifndef ST_LOWER_H
#define ST_LOWER_H

#include "st_ir.h"
#include "../frontend/st_semantic.h"

b8 ST_lower_program(ST_arena_t *arena, ST_program_t *p,
                    ST_sema_t *se, ST_string_t src, ST_string_t file,
                    ST_ir_module_t *out);

#endif
