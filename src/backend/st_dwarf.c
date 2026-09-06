#include "st_dwarf.h"

#include <string.h>

#include "../utils/st_arena.h"

static void ST_dbg_fns_reserve(ST_arena_t *arena, ST_dbg_info_t *info) {
    if (info->n_fns < info->fns_cap)
        return;
    u32 new_cap = info->fns_cap ? info->fns_cap * 2 : 8;
    ST_dbg_fn_t *new_fns = ST_arena_push(arena, sizeof(*new_fns) * new_cap);
    if (info->n_fns)
        memcpy(new_fns, info->fns, sizeof(*new_fns) * info->n_fns);
    info->fns = new_fns;
    info->fns_cap = new_cap;
}

static void ST_dbg_rows_reserve(ST_arena_t *arena, ST_dbg_info_t *info) {
    if (info->n_rows < info->rows_cap)
        return;
    u32 new_cap = info->rows_cap ? info->rows_cap * 2 : 64;
    ST_dbg_row_t *new_rows = ST_arena_push(arena, sizeof(*new_rows) * new_cap);
    if (info->n_rows)
        memcpy(new_rows, info->rows, sizeof(*new_rows) * info->n_rows);
    info->rows = new_rows;
    info->rows_cap = new_cap;
}

static void ST_dbg_files_reserve(ST_arena_t *arena, ST_dbg_info_t *info) {
    if (info->n_files < info->files_cap)
        return;
    u32 new_cap = info->files_cap ? info->files_cap * 2 : 8;
    ST_string_t *new_files = ST_arena_push(arena, sizeof(*new_files) * new_cap);
    if (info->n_files)
        memcpy(new_files, info->files, sizeof(*new_files) * info->n_files);
    info->files = new_files;
    info->files_cap = new_cap;
}

u32 ST_dbg_add_fn(ST_arena_t *arena, ST_dbg_info_t *info, ST_string_t name,
                  ST_string_t start_label, u32 decl_file_idx, u32 decl_line) {
    ST_dbg_fns_reserve(arena, info);
    info->fns[info->n_fns].name = name;
    info->fns[info->n_fns].start_label = start_label;
    info->fns[info->n_fns].end_label = (ST_string_t){0};
    info->fns[info->n_fns].decl_file_idx = decl_file_idx;
    info->fns[info->n_fns].decl_line = decl_line;
    return info->n_fns++;
}

void ST_dbg_set_fn_end_label(ST_dbg_info_t *info, u32 fn_idx, ST_string_t end_label) {
    if (fn_idx < info->n_fns)
        info->fns[fn_idx].end_label = end_label;
}

u32 ST_dbg_add_row(ST_arena_t *arena, ST_dbg_info_t *info, ST_string_t label, u32 file_idx,
                   u32 line, u32 col, u32 fn_idx) {
    ST_dbg_rows_reserve(arena, info);
    info->rows[info->n_rows].label = label;
    info->rows[info->n_rows].file_idx = file_idx;
    info->rows[info->n_rows].line = line;
    info->rows[info->n_rows].col = col;
    info->rows[info->n_rows].fn_idx = fn_idx;
    return info->n_rows++;
}

static void ST_dbg_vars_reserve(ST_arena_t *arena, ST_dbg_fn_t *fn) {
    if (fn->n_vars < fn->vars_cap)
        return;
    u32 new_cap = fn->vars_cap ? fn->vars_cap * 2 : 8;
    ST_dbg_var_t *new_vars = ST_arena_push(arena, sizeof(*new_vars) * new_cap);
    if (fn->n_vars)
        memcpy(new_vars, fn->vars, sizeof(*new_vars) * fn->n_vars);
    fn->vars = new_vars;
    fn->vars_cap = new_cap;
}

void ST_dbg_add_var(ST_arena_t *arena, ST_dbg_info_t *info, u32 fn_idx, ST_string_t name,
                    ST_ty_t *ty, i32 frame_off, b8 is_param) {
    if (fn_idx >= info->n_fns || !name.len)
        return;
    ST_dbg_fn_t *fn = &info->fns[fn_idx];
    ST_dbg_vars_reserve(arena, fn);
    fn->vars[fn->n_vars].name = name;
    fn->vars[fn->n_vars].ty = ty;
    fn->vars[fn->n_vars].frame_off = frame_off;
    fn->vars[fn->n_vars].is_param = is_param;
    fn->n_vars++;
}

#define ST_DBG_MAX_TYS 256

typedef struct {
    ST_ty_t *items[ST_DBG_MAX_TYS];
    u32 count;
} ST_dbg_tys_t;

static u32 ST_dbg_ty_lookup(ST_dbg_tys_t *tys, ST_ty_t *ty) {
    for (u32 i = 0; i < tys->count; i++)
        if (tys->items[i] == ty)
            return i;
    return (u32)-1;
}

static u32 ST_dbg_collect_ty(ST_dbg_tys_t *tys, ST_ty_t *ty) {
    if (!ty || tys->count >= ST_DBG_MAX_TYS)
        return (u32)-1;
    u32 existing = ST_dbg_ty_lookup(tys, ty);
    if (existing != (u32)-1)
        return existing;

    u32 idx = tys->count++;
    tys->items[idx] = ty;

    if (ty->kind == ST_TY_PTR) {
        ST_dbg_collect_ty(tys, ty->inner);
    } else if (ty->kind == ST_TY_STRUCT) {
        for (u32 i = 0; i < ty->fields.count; i++)
            ST_dbg_collect_ty(tys, ty->fields.items[i].ty);
    } else if (ty->kind == ST_TY_SLICE || ty->kind == ST_TY_DYN_ARRAY) {
        ST_dbg_collect_ty(tys, ty->inner);
    }

    return idx;
}

static void ST_dwarf_emit_shared_tys(FILE *out) {
    fputs("Ldbgty_u8:\n", out);
    fputs("    db 0x03\n", out);
    fputs("    db \"u8\", 0\n", out);
    fputs("    db 0x07\n", out);
    fputs("    db 1\n", out);

    fputs("Ldbgty_u8ptr:\n", out);
    fputs("    db 0x04\n", out);
    fputs("    dd Ldbgty_u8 - Ldbg_info\n", out);
    fputs("    db 8\n", out);

    fputs("Ldbgty_len64:\n", out);
    fputs("    db 0x03\n", out);
    fputs("    db \"u64\", 0\n", out);
    fputs("    db 0x07\n", out);
    fputs("    db 8\n", out);
}

static void ST_dwarf_emit_slice_like(FILE *out, ST_dbg_tys_t *tys, u32 idx, ST_ty_t *inner) {
    u32 inner_idx = inner ? ST_dbg_ty_lookup(tys, inner) : (u32)-1;
    fprintf(out, "Ldbgty_%u_p:\n", idx);
    fputs("    db 0x04\n", out);
    if (inner_idx != (u32)-1)
        fprintf(out, "    dd Ldbgty_%u - Ldbg_info\n", inner_idx);
    else
        fputs("    dd Ldbgty_u8 - Ldbg_info\n", out);
    fputs("    db 8\n", out);

    fprintf(out, "Ldbgty_%u:\n", idx);
    fputs("    db 0x05\n", out);
    fputs("    dd 16\n", out);
    fputs("    db 0x06\n", out);
    fputs("    db \"data\", 0\n", out);
    fprintf(out, "    dd Ldbgty_%u_p - Ldbg_info\n", idx);
    fputs("    dd 0\n", out);
    fputs("    db 0x06\n", out);
    fputs("    db \"len\", 0\n", out);
    fputs("    dd Ldbgty_len64 - Ldbg_info\n", out);
    fputs("    dd 8\n", out);
    fputs("    db 0x00\n", out);
}

static void ST_dwarf_emit_types(FILE *out, ST_dbg_tys_t *tys) {
    ST_dwarf_emit_shared_tys(out);

    for (u32 i = 0; i < tys->count; i++) {
        ST_ty_t *ty = tys->items[i];
        switch (ty->kind) {
            case ST_TY_BOOL:
            case ST_TY_CHAR:
            case ST_TY_FLOAT:
            case ST_TY_INT: {
                char name[16];
                u32 sz = ty->size ? ty->size : (ty->kind == ST_TY_FLOAT ? 8u : 4u);
                u8 enc;
                if (ty->kind == ST_TY_BOOL) {
                    snprintf(name, sizeof(name), "bool");
                    enc = 2;
                } else if (ty->kind == ST_TY_CHAR) {
                    snprintf(name, sizeof(name), "char");
                    enc = 8;
                } else if (ty->kind == ST_TY_FLOAT) {
                    snprintf(name, sizeof(name), "f%u", sz * 8);
                    enc = 4;
                } else {
                    snprintf(name, sizeof(name), "%c%u", ty->is_signed ? 'i' : 'u', sz * 8);
                    enc = ty->is_signed ? 5 : 7;
                }
                fprintf(out, "Ldbgty_%u:\n", i);
                fputs("    db 0x03\n", out);
                fprintf(out, "    db \"%s\", 0\n", name);
                fprintf(out, "    db %u\n", enc);
                fprintf(out, "    db %u\n", sz);
            } break;
            case ST_TY_PTR: {
                u32 inner_idx = ty->inner ? ST_dbg_ty_lookup(tys, ty->inner) : (u32)-1;
                fprintf(out, "Ldbgty_%u:\n", i);
                fputs("    db 0x04\n", out);
                if (inner_idx != (u32)-1)
                    fprintf(out, "    dd Ldbgty_%u - Ldbg_info\n", inner_idx);
                else
                    fputs("    dd Ldbgty_u8 - Ldbg_info\n", out);
                fputs("    db 8\n", out);
            } break;
            case ST_TY_STRUCT: {
                fprintf(out, "Ldbgty_%u:\n", i);
                fputs("    db 0x05\n", out);
                fprintf(out, "    dd %u\n", ty->size);
                for (u32 fi = 0; fi < ty->fields.count; fi++) {
                    ST_ty_field_t *f = &ty->fields.items[fi];
                    u32 fidx = ST_dbg_ty_lookup(tys, f->ty);
                    fputs("    db 0x06\n", out);
                    fprintf(out, "    db \"%.*s\", 0\n", (int)f->name.len, f->name.data);
                    if (fidx != (u32)-1)
                        fprintf(out, "    dd Ldbgty_%u - Ldbg_info\n", fidx);
                    else
                        fputs("    dd Ldbgty_u8 - Ldbg_info\n", out);
                    fprintf(out, "    dd %u\n", f->offset);
                }
                fputs("    db 0x00\n", out);
            } break;
            case ST_TY_SLICE:
            case ST_TY_DYN_ARRAY: {
                ST_dwarf_emit_slice_like(out, tys, i, ty->inner);
            } break;
            case ST_TY_STRING: {
                fprintf(out, "Ldbgty_%u:\n", i);
                fputs("    db 0x05\n", out);
                fputs("    dd 16\n", out);
                fputs("    db 0x06\n", out);
                fputs("    db \"data\", 0\n", out);
                fputs("    dd Ldbgty_u8ptr - Ldbg_info\n", out);
                fputs("    dd 0\n", out);
                fputs("    db 0x06\n", out);
                fputs("    db \"len\", 0\n", out);
                fputs("    dd Ldbgty_len64 - Ldbg_info\n", out);
                fputs("    dd 8\n", out);
                fputs("    db 0x00\n", out);
            } break;
            default: {
                u32 sz = ty->size ? ty->size : 8;
                fprintf(out, "Ldbgty_%u:\n", i);
                fputs("    db 0x03\n", out);
                fputs("    db \"?\", 0\n", out);
                fputs("    db 0x07\n", out);
                fprintf(out, "    db %u\n", sz);
            } break;
        }
    }
}

static void ST_dwarf_write_sleb128(FILE *out, i64 v);

static u32 ST_sleb128_len(i64 v) {
    u32 n = 0;
    b8 more = 1;
    while (more) {
        u8 b = (u8)(v & 0x7f);
        v >>= 7;
        b8 sign_bit_set = (b & 0x40) != 0;
        if ((v == 0 && !sign_bit_set) || (v == -1 && sign_bit_set))
            more = 0;
        n++;
    }
    return n;
}

static void ST_dwarf_write_fbreg_loc(FILE *out, i32 offset) {
    u32 len = 1 + ST_sleb128_len((i64)offset);
    fprintf(out, "    db %u\n", len);
    fputs("    db 0x91\n", out);
    ST_dwarf_write_sleb128(out, (i64)offset);
}

static void ST_dwarf_emit_var_die(FILE *out, ST_dbg_tys_t *tys, ST_dbg_var_t *v) {
    fprintf(out, "    db 0x%02x\n", v->is_param ? 0x07 : 0x08);
    fprintf(out, "    db \"%.*s\", 0\n", (int)v->name.len, v->name.data);
    u32 idx = ST_dbg_ty_lookup(tys, v->ty);
    if (idx != (u32)-1)
        fprintf(out, "    dd Ldbgty_%u - Ldbg_info\n", idx);
    else
        fputs("    dd Ldbgty_u8 - Ldbg_info\n", out);
    ST_dwarf_write_fbreg_loc(out, v->frame_off);
}

u32 ST_dbg_intern_file(ST_arena_t *arena, ST_dbg_info_t *info, ST_string_t file) {
    for (u32 i = 0; i < info->n_files; i++)
        if (ST_string_eq(info->files[i], file))
            return i;

    ST_dbg_files_reserve(arena, info);
    info->files[info->n_files] = file;
    return info->n_files++;
}

static void ST_dwarf_write_uleb128(FILE *out, u64 v) {
    fputs("    db ", out);
    b8 first = 1;
    do {
        u8 b = (u8)(v & 0x7f);
        v >>= 7;
        if (v != 0)
            b |= 0x80;
        if (!first)
            fputs(", ", out);
        fprintf(out, "0x%02x", b);
        first = 0;
    } while (v != 0);
    fputc('\n', out);
}

static void ST_dwarf_write_sleb128(FILE *out, i64 v) {
    fputs("    db ", out);
    b8 first = 1;
    b8 more = 1;
    while (more) {
        u8 b = (u8)(v & 0x7f);
        v >>= 7;
        b8 sign_bit_set = (b & 0x40) != 0;
        if ((v == 0 && !sign_bit_set) || (v == -1 && sign_bit_set))
            more = 0;
        else
            b |= 0x80;
        if (!first)
            fputs(", ", out);
        fprintf(out, "0x%02x", b);
        first = 0;
    }
    fputc('\n', out);
}

static b8 ST_path_is_absolute(ST_string_t s) {
    return s.len > 0 && s.data[0] == '/';
}

static void ST_dwarf_write_str_label(FILE *out, ST_string_t s) {

    fputs("    db ", out);
    for (u32 i = 0; i < s.len; i++) {
        if (i)
            fputs(", ", out);
        fprintf(out, "0x%02x", (u8)s.data[i]);
    }
    if (s.len)
        fputs(", ", out);
    fputs("0x00\n", out);
}

void ST_dwarf_emit(FILE *out, ST_dbg_info_t *info, const char *comp_dir) {

    fputs("section .debug_abbrev progbits noalloc noexec nowrite align=1\n", out);
    fputs("Ldbg_abbrev:\n", out);
    fputs("    db 0x01, 0x11, 0x01\n", out);
    fputs("    db 0x25, 0x08\n", out);
    fputs("    db 0x13, 0x05\n", out);
    fputs("    db 0x03, 0x08\n", out);
    fputs("    db 0x1b, 0x08\n", out);
    fputs("    db 0x11, 0x01\n", out);
    fputs("    db 0x12, 0x01\n", out);
    fputs("    db 0x10, 0x17\n", out);
    fputs("    db 0x00, 0x00\n", out);
    fputs("    db 0x02, 0x2e, 0x01\n", out);
    fputs("    db 0x03, 0x08\n", out);
    fputs("    db 0x11, 0x01\n", out);
    fputs("    db 0x12, 0x01\n", out);
    fputs("    db 0x40, 0x18\n", out);
    fputs("    db 0x3f, 0x0c\n", out);
    fputs("    db 0x3a, 0x0b\n", out);
    fputs("    db 0x3b, 0x06\n", out);
    fputs("    db 0x00, 0x00\n", out);
    fputs("    db 0x03, 0x24, 0x00\n", out);
    fputs("    db 0x03, 0x08\n", out);
    fputs("    db 0x3e, 0x0b\n", out);
    fputs("    db 0x0b, 0x0b\n", out);
    fputs("    db 0x00, 0x00\n", out);
    fputs("    db 0x04, 0x0f, 0x00\n", out);
    fputs("    db 0x49, 0x13\n", out);
    fputs("    db 0x0b, 0x0b\n", out);
    fputs("    db 0x00, 0x00\n", out);
    fputs("    db 0x05, 0x13, 0x01\n", out);
    fputs("    db 0x0b, 0x06\n", out);
    fputs("    db 0x00, 0x00\n", out);
    fputs("    db 0x06, 0x0d, 0x00\n", out);
    fputs("    db 0x03, 0x08\n", out);
    fputs("    db 0x49, 0x13\n", out);
    fputs("    db 0x38, 0x06\n", out);
    fputs("    db 0x00, 0x00\n", out);
    fputs("    db 0x07, 0x05, 0x00\n", out);
    fputs("    db 0x03, 0x08\n", out);
    fputs("    db 0x49, 0x13\n", out);
    fputs("    db 0x02, 0x0a\n", out);
    fputs("    db 0x00, 0x00\n", out);
    fputs("    db 0x08, 0x34, 0x00\n", out);
    fputs("    db 0x03, 0x08\n", out);
    fputs("    db 0x49, 0x13\n", out);
    fputs("    db 0x02, 0x0a\n", out);
    fputs("    db 0x00, 0x00\n", out);
    fputs("    db 0x00\n", out);
    fputc('\n', out);

    fputs("section .debug_info progbits noalloc noexec nowrite align=1\n", out);
    fputs("Ldbg_info:\n", out);
    fputs("    dd Ldbg_info_end - Ldbg_info_v\n", out);
    fputs("Ldbg_info_v:\n", out);
    fputs("    dw 5\n", out);
    fputs("    db 0x01\n", out);
    fputs("    db 8\n", out);
    fputs("    dd 0\n", out);

    fputs("    db 0x01\n", out);
    fputs("    db \"storthc DWARF5\", 0\n", out);
    fputs("    dw 0x0002\n", out);
    ST_string_t cu_name = info->n_files ? info->files[0] : ST_cstr_to_str((char *)"<unknown>");
    ST_dwarf_write_str_label(out, cu_name);
    fprintf(out, "    db \"%s\", 0\n", comp_dir);
    if (info->n_fns) {
        fprintf(out, "    dq %.*s\n", (int)info->fns[0].start_label.len, info->fns[0].start_label.data);
        fprintf(out, "    dq %.*s\n", (int)info->fns[info->n_fns - 1].end_label.len,
                info->fns[info->n_fns - 1].end_label.data);
    } else {
        fputs("    dq 0\n    dq 0\n", out);
    }
    fputs("    dd 0\n", out);

    ST_dbg_tys_t tys = {0};
    for (u32 fi = 0; fi < info->n_fns; fi++)
        for (u32 vi = 0; vi < info->fns[fi].n_vars; vi++)
            ST_dbg_collect_ty(&tys, info->fns[fi].vars[vi].ty);
    ST_dwarf_emit_types(out, &tys);

    for (u32 i = 0; i < info->n_fns; i++) {
        ST_dbg_fn_t *f = &info->fns[i];
        fputs("    db 0x02\n", out);
        ST_dwarf_write_str_label(out, f->name);
        fprintf(out, "    dq %.*s\n", (int)f->start_label.len, f->start_label.data);
        fprintf(out, "    dq %.*s\n", (int)f->end_label.len, f->end_label.data);
        fputs("    db 1, 0x56\n", out);
        fputs("    db 0x01\n", out);
        fprintf(out, "    db %u\n", f->decl_file_idx);
        fprintf(out, "    dd %u\n", f->decl_line);
        for (u32 vi = 0; vi < f->n_vars; vi++)
            ST_dwarf_emit_var_die(out, &tys, &f->vars[vi]);
        fputs("    db 0x00\n", out);
    }
    fputs("    db 0x00\n", out);
    fputs("Ldbg_info_end:\n\n", out);

    fputs("section .debug_line progbits noalloc noexec nowrite align=1\n", out);
    fputs("Ldbg_line:\n", out);
    fputs("    dd Ldbg_line_end - Ldbg_line_v\n", out);
    fputs("Ldbg_line_v:\n", out);
    fputs("    dw 5\n", out);
    fputs("    db 8\n", out);
    fputs("    db 0\n", out);
    fputs("    dd Ldbg_line_prog - Ldbg_line_hdr\n", out);
    fputs("Ldbg_line_hdr:\n", out);
    fputs("    db 1\n", out);
    fputs("    db 1\n", out);
    fputs("    db 1\n", out);
    fputs("    db 0xfb\n", out);
    fputs("    db 14\n", out);
    fputs("    db 13\n", out);
    fputs("    db 0,1,1,1,1,0,0,0,1,0,0,1\n", out);
    fputs("    db 1\n", out);
    fputs("    db 0x01, 0x08\n", out);
    fputs("    db 2\n", out);
    fprintf(out, "    db \"%s\", 0\n", comp_dir);
    fputs("    db 0\n", out);
    fputs("    db 2\n", out);
    fputs("    db 0x01, 0x08\n", out);
    fputs("    db 0x02, 0x0f\n", out);
    ST_dwarf_write_uleb128(out, info->n_files);
    for (u32 i = 0; i < info->n_files; i++) {
        ST_dwarf_write_str_label(out, info->files[i]);
        fputs(ST_path_is_absolute(info->files[i]) ? "    db 1\n" : "    db 0\n", out);
    }
    fputs("Ldbg_line_prog:\n", out);

    for (u32 fi = 0; fi < info->n_fns; fi++) {
        ST_dbg_fn_t *f = &info->fns[fi];
        i64 cur_line = 1;
        i64 cur_file = -1;

        fputs("    db 0x04\n", out);
        ST_dwarf_write_uleb128(out, f->decl_file_idx);
        cur_file = (i64)f->decl_file_idx;
        fputs("    db 0x00\n", out);
        ST_dwarf_write_uleb128(out, 9);
        fputs("    db 0x02\n", out);
        fprintf(out, "    dq %.*s\n", (int)f->start_label.len, f->start_label.data);
        fputs("    db 0x03\n", out);
        ST_dwarf_write_sleb128(out, (i64)f->decl_line - cur_line);
        cur_line = (i64)f->decl_line;
        fputs("    db 0x01\n", out);

        for (u32 ri = 0; ri < info->n_rows; ri++) {
            ST_dbg_row_t *r = &info->rows[ri];
            if (r->fn_idx != fi)
                continue;
            if ((i64)r->file_idx != cur_file) {
                fputs("    db 0x04\n", out);
                ST_dwarf_write_uleb128(out, r->file_idx);
                cur_file = (i64)r->file_idx;
            }
            fputs("    db 0x00\n", out);
            ST_dwarf_write_uleb128(out, 9);
            fputs("    db 0x02\n", out);
            fprintf(out, "    dq %.*s\n", (int)r->label.len, r->label.data);
            fputs("    db 0x03\n", out);
            ST_dwarf_write_sleb128(out, (i64)r->line - cur_line);
            cur_line = (i64)r->line;
            fputs("    db 0x01\n", out);
        }

        fputs("    db 0x00\n", out);
        ST_dwarf_write_uleb128(out, 9);
        fputs("    db 0x02\n", out);
        fprintf(out, "    dq %.*s\n", (int)f->end_label.len, f->end_label.data);
        fputs("    db 0x00\n", out);
        ST_dwarf_write_uleb128(out, 1);
        fputs("    db 0x01\n", out);
    }
    fputs("Ldbg_line_end:\n\n", out);

    fputs("section .asan_lines progbits alloc noexec nowrite align=8\n", out);

    for (u32 i = 0; i < info->n_files; i++) {
        ST_string_t file = info->files[i];
        fprintf(out, "__asan_dbg_filestr_%u: db ", i);
        if (file.len) {
            for (u32 k = 0; k < file.len; k++) {
                if (k)
                    fputs(", ", out);
                fprintf(out, "0x%02x", (u8)file.data[k]);
            }
            fputc('\n', out);
        } else {
            fputs("0\n", out);
        }
    }

    fputs("__asan_dbg_row_count: dq ", out);
    fprintf(out, "%u\n", info->n_rows);
    fputs("__asan_dbg_rows:\n", out);
    for (u32 ri = 0; ri < info->n_rows; ri++) {
        ST_dbg_row_t *r = &info->rows[ri];
        u32 flen = r->file_idx < info->n_files ? info->files[r->file_idx].len : 0;
        fprintf(out, "    dq %.*s\n", (int)r->label.len, r->label.data);
        fprintf(out, "    dq __asan_dbg_filestr_%u\n", r->file_idx);
        fprintf(out, "    dq %u\n", flen);
        fprintf(out, "    dq %u\n", r->line);
        fprintf(out, "    dq %u\n", r->col);
        fprintf(out, "    dq %u\n", r->fn_idx);
    }

    for (u32 fi = 0; fi < info->n_fns; fi++) {
        ST_string_t name = info->fns[fi].name;
        fprintf(out, "__asan_dbg_fnstr_%u: db ", fi);
        if (name.len) {
            for (u32 k = 0; k < name.len; k++) {
                if (k)
                    fputs(", ", out);
                fprintf(out, "0x%02x", (u8)name.data[k]);
            }
            fputc('\n', out);
        } else {
            fputs("0\n", out);
        }
    }
    fputs("__asan_dbg_fn_count: dq ", out);
    fprintf(out, "%u\n", info->n_fns);
    fputs("__asan_dbg_fn_names:\n", out);
    for (u32 fi = 0; fi < info->n_fns; fi++) {
        fprintf(out, "    dq __asan_dbg_fnstr_%u\n", fi);
        fprintf(out, "    dq %u\n", info->fns[fi].name.len);
        fprintf(out, "    dq %.*s\n", (int)info->fns[fi].end_label.len,
                info->fns[fi].end_label.data);
    }
}

void ST_dwarf_emit_fasm_asan_lines(FILE *out, ST_dbg_info_t *info) {
    fputs("section '.asan_lines'\n", out);
    fputs("align 8\n", out);

    for (u32 i = 0; i < info->n_files; i++) {
        ST_string_t file = info->files[i];
        fprintf(out, "__asan_dbg_filestr_%u: db ", i);
        if (file.len) {
            for (u32 k = 0; k < file.len; k++) {
                if (k)
                    fputs(", ", out);
                fprintf(out, "0x%02x", (u8)file.data[k]);
            }
            fputc('\n', out);
        } else {
            fputs("0\n", out);
        }
    }

    fputs("__asan_dbg_row_count: dq ", out);
    fprintf(out, "%u\n", info->n_rows);
    fputs("__asan_dbg_rows:\n", out);
    for (u32 ri = 0; ri < info->n_rows; ri++) {
        ST_dbg_row_t *r = &info->rows[ri];
        u32 flen = r->file_idx < info->n_files ? info->files[r->file_idx].len : 0;
        fprintf(out, "    dq %.*s\n", (int)r->label.len, r->label.data);
        fprintf(out, "    dq __asan_dbg_filestr_%u\n", r->file_idx);
        fprintf(out, "    dq %u\n", flen);
        fprintf(out, "    dq %u\n", r->line);
        fprintf(out, "    dq %u\n", r->col);
        fprintf(out, "    dq %u\n", r->fn_idx);
    }

    for (u32 fi = 0; fi < info->n_fns; fi++) {
        ST_string_t name = info->fns[fi].name;
        fprintf(out, "__asan_dbg_fnstr_%u: db ", fi);
        if (name.len) {
            for (u32 k = 0; k < name.len; k++) {
                if (k)
                    fputs(", ", out);
                fprintf(out, "0x%02x", (u8)name.data[k]);
            }
            fputc('\n', out);
        } else {
            fputs("0\n", out);
        }
    }
    fputs("__asan_dbg_fn_count: dq ", out);
    fprintf(out, "%u\n", info->n_fns);
    fputs("__asan_dbg_fn_names:\n", out);
    for (u32 fi = 0; fi < info->n_fns; fi++) {
        fprintf(out, "    dq __asan_dbg_fnstr_%u\n", fi);
        fprintf(out, "    dq %u\n", info->fns[fi].name.len);
        fprintf(out, "    dq %.*s\n", (int)info->fns[fi].end_label.len,
                info->fns[fi].end_label.data);
    }
}
