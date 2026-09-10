#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include "st_comptime_native.h"

#include "../../backend/st_address_sanitizer.h"
#include "../../backend/st_dwarf.h"
#include "../../backend/st_nasm.h"
#include "../../middle/st_lower.h"
#include "../../utils/st_process.h"
#include "../../utils/platform/st_platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#define ST_CT_MODULE_EXT "dll"
#else
#define ST_CT_MODULE_EXT "so"
#endif

static u64 ST_ct_fnv1a(const char *data, size_t len) {
    u64 h = 1469598103934665603ull;
    for (size_t i = 0; i < len; i++) {
        h ^= (u8)data[i];
        h *= 1099511628211ull;
    }
    return h;
}

static u32 st_ct_temp_dir(char *out, u32 cap) {
#ifdef _WIN32
    DWORD n = GetTempPathA(cap, out);
    if (n == 0 || n >= cap) {
        out[0] = '.';
        out[1] = '\\';
        out[2] = 0;
        return 2;
    }
    return (u32)n;
#else
    ST_unused(cap);
    memcpy(out, "/tmp/", 6);
    return 5;
#endif
}

static b8 st_ct_generate_asm(ST_ct_prog_ctx_t *pctx, ST_ir_module_t *ir, char **out_buf,
                             size_t *out_len) {
#ifndef _WIN32
    ST_dbg_info_t dbg = {0};
#endif
#ifdef _WIN32
    char dir[MAX_PATH], tmp[MAX_PATH];
    st_ct_temp_dir(dir, sizeof(dir));
    if (!GetTempFileNameA(dir, "stc", 0, tmp))
        return 0;
    FILE *mem = fopen(tmp, "w+b");
    if (!mem)
        return 0;
#else
    char *mbuf = NULL;
    size_t mlen = 0;
    FILE *mem = open_memstream(&mbuf, &mlen);
    if (!mem)
        return 0;
#endif

    b8 ok = ST_nasm_generate(mem, ir, pctx->src, pctx->file, 0, 0);
#ifndef _WIN32
    if (ok) {
        ST_asan_write_runtime_asm(mem, 0);
        ST_dwarf_emit(mem, &dbg, ".");
    }
#endif

#ifdef _WIN32
    if (ok) {
        fflush(mem);
        long end = ftell(mem);
        ok = end >= 0;
        if (ok) {
            rewind(mem);
            *out_len = (size_t)end;
            *out_buf = ST_arena_push(pctx->arena, (u64)*out_len + 1);
            ok = fread(*out_buf, 1, *out_len, mem) == *out_len;
            (*out_buf)[*out_len] = 0;
        }
    }
    fclose(mem);
    remove(tmp);
#else
    fclose(mem);
    if (ok) {
        *out_len = mlen;
        *out_buf = ST_arena_push(pctx->arena, (u64)mlen + 1);
        memcpy(*out_buf, mbuf, mlen);
        (*out_buf)[mlen] = 0;
    }
    free(mbuf);
#endif
    return ok;
}

static void st_ct_append_assemble(ST_procs_t *procs, const char *s_path, const char *o_path) {
#ifdef _WIN32
    ST_append_process(procs, "nasm", "-f", "win64", "-O1", s_path, "-o", o_path);
#else
    ST_append_process(procs, "nasm", "-f", "elf64", "-O1", s_path, "-o", o_path);
#endif
}

static void st_ct_append_link(ST_arena_t *arena, ST_procs_t *procs, const char *out_path,
                              const char *o_path) {
#ifdef _WIN32
    u32 out_len = (u32)strlen(out_path);
    char *out_flag = ST_arena_push(arena, out_len + 6);
    memcpy(out_flag, "/out:", 5);
    memcpy(out_flag + 5, out_path, out_len + 1);
    ST_append_process(procs, "link", "/nologo", "/dll", "/noentry", out_flag, o_path,
                      "kernel32.lib", "msvcrt.lib", "legacy_stdio_definitions.lib");
#else
    ST_unused(arena);
    ST_append_process(procs, "ld", "-shared", "-z", "noexecstack", "-z", "notext", "-o",
                      out_path, o_path, "-lc");
#endif
}

b8 ST_ct_ensure_native_module(ST_ct_prog_ctx_t *pctx, char *err_msg, u32 err_msg_cap) {
    if (pctx->native_so_built)
        return pctx->native_so_ok;
    pctx->native_so_built = 1;
    pctx->native_so_ok = 0;

    ST_ir_module_t ir = {0};
    if (!ST_lower_program(pctx->arena, pctx->prog, pctx->sema, pctx->src, pctx->file, &ir)) {
        snprintf(err_msg, err_msg_cap,
                 "comptime: this module has errors outside of #comptime, can't build it for "
                 "a native call");
        return 0;
    }

    char *buf = NULL;
    size_t len = 0;
    if (!st_ct_generate_asm(pctx, &ir, &buf, &len)) {
        snprintf(err_msg, err_msg_cap, "comptime: codegen failed for this module");
        return 0;
    }

    u64 h = ST_ct_fnv1a(buf, len);
    char dir[256];
    st_ct_temp_dir(dir, sizeof(dir));
    char s_path[512], o_path[512];
    snprintf(s_path, sizeof(s_path), "%sstorthc-ct-%016llx.s", dir, (unsigned long long)h);
    snprintf(o_path, sizeof(o_path), "%sstorthc-ct-%016llx.o", dir, (unsigned long long)h);
    snprintf(pctx->native_so_path, sizeof(pctx->native_so_path),
             "%sstorthc-ct-%016llx." ST_CT_MODULE_EXT, dir, (unsigned long long)h);

    if (ST_access_file(pctx->native_so_path) != 0) {
        FILE *f = fopen(s_path, "wb");
        if (!f) {
            snprintf(err_msg, err_msg_cap, "comptime: could not write %s", s_path);
            return 0;
        }
        fwrite(buf, 1, len, f);
        fclose(f);

        ST_procs_t procs;
        ST_procs_init(pctx->arena, &procs);

        st_ct_append_assemble(&procs, s_path, o_path);
        if (!ST_run_processes(&procs)) {
            snprintf(err_msg, err_msg_cap, "comptime: nasm failed assembling this module");
            return 0;
        }

        st_ct_append_link(pctx->arena, &procs, pctx->native_so_path, o_path);
        if (!ST_run_processes(&procs)) {
            snprintf(err_msg, err_msg_cap, "comptime: linker failed linking this module");
            return 0;
        }
    }

    void *handle = ST_load_dynlib(pctx->native_so_path);
    if (!handle) {
        snprintf(err_msg, err_msg_cap,
                 "comptime: built %s but couldn't dynamically load and open it",
                 pctx->native_so_path);
        return 0;
    }
    ST_close_dll(handle);

    pctx->native_so_ok = 1;
    return 1;
}
