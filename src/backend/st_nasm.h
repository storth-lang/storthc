#ifndef ST_NASM_H
#define ST_NASM_H

#include "../middle/st_ir.h"
#include "../utils/st_diagnostic.h"

b8 ST_nasm_generate(FILE *out, ST_ir_module_t *m, ST_string_t src,
                    ST_string_t file, b8 emit_entry);

#endif
