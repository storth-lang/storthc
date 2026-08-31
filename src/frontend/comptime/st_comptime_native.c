#define _POSIX_C_SOURCE 200809L

#include "st_comptime_native.h"

#include "../../backend/st_address_sanitizer.h"
#include "../../backend/st_dwarf.h"
#include "../../backend/st_nasm_x86_64_linux.h"
#include "../../middle/st_lower.h"
#include "../../utils/st_process.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static u64 ST_ct_fnv1a(const char *data, size_t len) {
    u64 h = 1469598103934665603ull;
    for (size_t i = 0; i < len; i++) {
        h ^= (u8)data[i];
        h *= 1099511628211ull;
    }
    return h;
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
    FILE *mem = open_memstream(&buf, &len);
    ST_dbg_info_t dbg = {0};
    b8 ok = ST_nasm_generate(mem, &ir, pctx->src, pctx->file, 0, NULL);
    if (ok) {
        ST_asan_write_runtime_asm(mem, 0);
        ST_dwarf_emit(mem, &dbg, ".");
    }
    fclose(mem);
    if (!ok) {
        free(buf);
        snprintf(err_msg, err_msg_cap, "comptime: codegen failed for this module");
        return 0;
    }

    u64 h = ST_ct_fnv1a(buf, len);
    char s_path[256], o_path[256];
    snprintf(s_path, sizeof(s_path), "/tmp/storthc-ct-%016llx.s", (unsigned long long)h);
    snprintf(o_path, sizeof(o_path), "/tmp/storthc-ct-%016llx.o", (unsigned long long)h);
    snprintf(pctx->native_so_path, sizeof(pctx->native_so_path), "/tmp/storthc-ct-%016llx.so",
             (unsigned long long)h);

    if (access(pctx->native_so_path, F_OK) != 0) {
        FILE *f = fopen(s_path, "wb");
        if (!f) {
            free(buf);
            snprintf(err_msg, err_msg_cap, "comptime: could not write %s", s_path);
            return 0;
        }
        fwrite(buf, 1, len, f);
        fclose(f);
        free(buf);

        ST_procs_t asm_procs = {0};
        ST_append_process(&asm_procs, "nasm", "-f", "elf64", "-O1", s_path, "-o", o_path);
        b8 asm_ok = ST_run_processes(&asm_procs);
        ST_free_process(&asm_procs);
        if (!asm_ok) {
            snprintf(err_msg, err_msg_cap, "comptime: nasm failed assembling this module");
            return 0;
        }

        ST_procs_t link_procs = {0};
        ST_append_process(&link_procs, "ld", "-shared", "-z", "noexecstack", "-z", "notext",
                          "-o", pctx->native_so_path, o_path, "-lc");
        b8 link_ok = ST_run_processes(&link_procs);
        ST_free_process(&link_procs);
        if (!link_ok) {
            snprintf(err_msg, err_msg_cap, "comptime: ld failed linking this module");
            return 0;
        }
    } else {
        free(buf);
    }

    void *handle = dlopen(pctx->native_so_path, RTLD_NOW | RTLD_GLOBAL);
    if (!handle) {
        snprintf(err_msg, err_msg_cap, "comptime: built %s but couldn't dlopen it: %s",
                 pctx->native_so_path, dlerror());
        return 0;
    }
    dlclose(handle);

    pctx->native_so_ok = 1;
    return 1;
}
