#ifndef ST_FASM_H
#define ST_FASM_H

#include "../middle/st_ir.h"
#include "../utils/st_diagnostic.h"
#include "st_dwarf.h"

b8 ST_fasm_generate(FILE *out, ST_ir_module_t *m, ST_string_t src, ST_string_t file,
                    b8 emit_entry, ST_dbg_info_t *dbg);

#endif
