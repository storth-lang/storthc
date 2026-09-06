#ifndef ST_DWARF_H
#define ST_DWARF_H

#include <stdio.h>

#include "../frontend/st_types.h"
#include "../utils/st_arena.h"
#include "../utils/st_string.h"

typedef struct {
    ST_string_t label;
    u32 file_idx;
    u32 line;
    u32 col;
    u32 fn_idx;
} ST_dbg_row_t;

typedef struct {
    ST_string_t name;
    ST_ty_t *ty;
    i32 frame_off;
    b8 is_param;
} ST_dbg_var_t;

typedef struct {
    ST_string_t name;
    ST_string_t start_label;
    ST_string_t end_label;
    u32 decl_file_idx;
    u32 decl_line;
    ST_dbg_var_t *vars;
    u32 n_vars, vars_cap;
} ST_dbg_fn_t;

typedef struct {
    ST_string_t *files;
    u32 n_files, files_cap;
    ST_dbg_row_t *rows;
    u32 n_rows, rows_cap;
    ST_dbg_fn_t *fns;
    u32 n_fns, fns_cap;
} ST_dbg_info_t;

u32 ST_dbg_intern_file(ST_arena_t *arena, ST_dbg_info_t *info, ST_string_t file);

u32 ST_dbg_add_fn(ST_arena_t *arena, ST_dbg_info_t *info, ST_string_t name,
                  ST_string_t start_label, u32 decl_file_idx, u32 decl_line);
void ST_dbg_set_fn_end_label(ST_dbg_info_t *info, u32 fn_idx, ST_string_t end_label);
u32 ST_dbg_add_row(ST_arena_t *arena, ST_dbg_info_t *info, ST_string_t label, u32 file_idx,
                   u32 line, u32 col, u32 fn_idx);

void ST_dbg_add_var(ST_arena_t *arena, ST_dbg_info_t *info, u32 fn_idx, ST_string_t name,
                    ST_ty_t *ty, i32 frame_off, b8 is_param);

void ST_dwarf_emit(FILE *out, ST_dbg_info_t *info, const char *comp_dir);
void ST_dwarf_emit_fasm_asan_lines(FILE *out, ST_dbg_info_t *info);

#endif
