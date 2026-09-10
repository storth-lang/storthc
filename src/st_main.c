#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "backend/st_address_sanitizer.h"
#include "backend/st_dwarf.h"
#include "backend/st_nasm.h"
#ifndef _WIN32
#include "backend/st_fasm.h"
#endif
#include "frontend/st_lexer.h"
#include "frontend/st_load.h"
#include "frontend/st_module.h"
#include "frontend/st_parser.h"
#include "frontend/st_semantic.h"
#include "frontend/comptime/st_comptime.h"
#include "frontend/comptime/st_comptime_compile.h"
#include "middle/st_lower.h"
#include "utils/st_cli.h"
#include "utils/st_arena.h"
#include "utils/st_helper.h"
#include "utils/st_process.h"
#include "utils/st_string.h"
#include "utils/platform/st_platform.h"

#ifndef STORTHC_VERSION_MAJOR
#define STORTHC_VERSION_MAJOR 0
#endif
#ifndef STORTHC_VERSION_MINOR
#define STORTHC_VERSION_MINOR 0
#endif
#ifndef STORTHC_VERSION_PATCH
#define STORTHC_VERSION_PATCH 0
#endif
#ifndef STORTHC_GIT_HASH
#define STORTHC_GIT_HASH "unknown"
#endif

#define ST_STR2(x) #x
#define ST_STR(x) ST_STR2(x)
#define STORTHC_VERSION_STRING                                                                 \
    ST_STR(STORTHC_VERSION_MAJOR) "." ST_STR(STORTHC_VERSION_MINOR) "." ST_STR(                 \
        STORTHC_VERSION_PATCH)

#ifndef ST_TIME_START
#define ST_TIME_START() f64 st_time_last_ = ST_get_current_time()
#define ST_TIME_MARK(acc)                                                                      \
    do {                                                                                       \
        f64 st_time_now_ = ST_get_current_time();                                              \
        *(acc) += (st_time_now_ - st_time_last_) * 1000.0;                                     \
        st_time_last_ = st_time_now_;                                                          \
    } while (0)
#endif

#ifdef _WIN32
#define ST_PATH_SEP '\\'
#define ST_EXE_SUFFIX ".exe"
#define ST_CT_LIB_FMT "%s.dll"
#define ST_CT_CACHE_GLOB "storthc-ct-*"
#else
#define ST_PATH_SEP '/'
#define ST_EXE_SUFFIX ""
#define ST_CT_LIB_FMT "lib%s.so"
#define ST_CT_CACHE_GLOB "storthc-ct-*"
#endif

static void st_error(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, ST_COLOR_BOLD "storthc: " ST_COLOR_BOLD_RED "error: " ST_COLOR_RESET);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static void st_print_proc(ST_proc_t *p) {
    fputs("  ", stderr);
    for (u32 i = 0; p->opt.args[i]; i++)
        fprintf(stderr, "%s%s", i ? " " : "", p->opt.args[i]);
    if (!p->exited_normally)
        fputs("  (could not start or did not exit normally)\n", stderr);
    else if (p->exit_code != 0)
        fprintf(stderr, "  (exit code %d)\n", p->exit_code);
    else
        fputc('\n', stderr);
}

static b8 st_run_step(ST_procs_t *procs, const char *what) {
    u32 n = procs->count;
    if (ST_run_processes(procs))
        return 1;
    st_error("%s failed", what);
    for (u32 i = 0; i < n; i++)
        st_print_proc(&procs->items[i]);
    return 0;
}

static void st_print_version(void) {
    fprintf(stdout, "storthc %s (%s)\n", STORTHC_VERSION_STRING, STORTHC_GIT_HASH);
}

typedef enum {
    ST_STAGE_TOKENS,
    ST_STAGE_AST,
    ST_STAGE_IR,
    ST_STAGE_ASM,
    ST_STAGE_OBJ,
    ST_STAGE_EXE,
    ST_STAGE_RUN,
} st_stage_t;

static char *st_abs_path_cstr(ST_arena_t *arena, const char *path) {
    ST_string_t s = ST_abs_path(arena, path);
    char *out = ST_arena_push(arena, s.len + 1);
    memcpy(out, s.data, s.len);
    out[s.len] = 0;
    return out;
}

static char *st_temp_path(ST_arena_t *arena, const char *name) {
#ifdef _WIN32
    char dir[MAX_PATH];
    DWORD n = GetTempPathA(MAX_PATH, dir);
    if (n == 0 || n >= MAX_PATH) {
        dir[0] = '.';
        dir[1] = '\\';
        dir[2] = 0;
        n = 2;
    }
#else
    const char *dir = "/tmp/";
    u32 n = 5;
#endif
    u32 name_len = (u32)strlen(name);
    char *out = ST_arena_push(arena, n + name_len + 1);
    memcpy(out, dir, n);
    memcpy(out + n, name, name_len + 1);
    return out;
}

static char *st_default_exe_name(ST_arena_t *arena, const char *path) {
    const char *base = path;
    for (const char *p = path; *p; p++)
        if (*p == '/' || *p == '\\')
            base = p + 1;
    u32 len = (u32)strlen(base);
    const char *dot = NULL;
    for (u32 i = 0; i < len; i++)
        if (base[i] == '.')
            dot = base + i;
    u32 out_len = dot ? (u32)(dot - base) : len;
    if (out_len == 0)
        out_len = len;
    u32 suffix_len = (u32)strlen(ST_EXE_SUFFIX);
    char *out = ST_arena_push(arena, out_len + suffix_len + 1);
    memcpy(out, base, out_len);
    memcpy(out + out_len, ST_EXE_SUFFIX, suffix_len + 1);
    return out;
}

static void st_append_assemble(ST_procs_t *procs, const char *asm_path, const char *obj_path,
                               b8 use_fasm) {
#ifdef _WIN32
    ST_unused(use_fasm);
    ST_append_process(procs, "nasm", "-f", "win64", "-O1", asm_path, "-o", obj_path);
#else
    if (use_fasm)
        ST_append_process(procs, "fasm", asm_path, obj_path);
    else
        ST_append_process(procs, "nasm", "-f", "elf64", "-O1", asm_path, "-o", obj_path);
#endif
}

#ifdef _WIN32
// @note: translates GCC/ld-style linker flags into what MSVC's link.exe
// expects, since '-lraylib' and '-Lpath' are what people habitually type
// and what the Linux side of ST_append_link already accepts as-is.
// '-lname' -> 'name.lib' (a bare '-lname.lib' or '-lname.dll' is passed
// through as just 'name.lib'/'name.dll' unchanged, in case the caller
// already spelled out the extension). '-Lpath' -> '/LIBPATH:path'.
// Anything else (already a bare '.lib' filename, or an MSVC-style flag
// like '/LIBPATH:...' or '/NODEFAULTLIB') passes through untouched.
static const char *st_translate_win_ldflag(ST_arena_t *arena, const char *flag) {
    size_t len = strlen(flag);
    if (len > 2 && flag[0] == '-' && flag[1] == 'l') {
        const char *name = flag + 2;
        size_t name_len = strlen(name);
        b8 has_ext = name_len > 4 && (strcmp(name + name_len - 4, ".lib") == 0 ||
                                      strcmp(name + name_len - 4, ".dll") == 0);
        size_t total = name_len + (has_ext ? 0 : 4);
        char *out = ST_arena_push(arena, total + 1);
        memcpy(out, name, name_len);
        if (!has_ext)
            memcpy(out + name_len, ".lib", 4);
        out[total] = 0;
        return out;
    }
    if (len > 2 && flag[0] == '-' && flag[1] == 'L') {
        const char *path = flag + 2;
        size_t path_len = strlen(path);
        size_t total = 9 + path_len; // "/LIBPATH:"
        char *out = ST_arena_push(arena, total + 1);
        memcpy(out, "/LIBPATH:", 9);
        memcpy(out + 9, path, path_len + 1);
        return out;
    }
    return flag;
}
#endif

static void st_append_link(ST_arena_t *arena, ST_procs_t *procs, const char *out_path,
                           const char *obj_path, strings_t *ldflags) {
    ST_proc_opt_t opt = {0};
    u32 n = 0;
#ifdef _WIN32
    u32 out_len = (u32)strlen(out_path);
    char *out_flag = ST_arena_push(arena, out_len + 6);
    memcpy(out_flag, "/out:", 5);
    memcpy(out_flag + 5, out_path, out_len + 1);
    opt.args[n++] = "link";
    opt.args[n++] = "/nologo";
    opt.args[n++] = "/subsystem:console";
    opt.args[n++] = out_flag;
    opt.args[n++] = obj_path;
    opt.args[n++] = "kernel32.lib";
    opt.args[n++] = "user32.lib";
    opt.args[n++] = "gdi32.lib";
    opt.args[n++] = "msvcrt.lib";
    opt.args[n++] = "legacy_stdio_definitions.lib";
#else
    ST_unused(arena);
    opt.args[n++] = "ld";
    opt.args[n++] = "-o";
    opt.args[n++] = out_path;
    opt.args[n++] = obj_path;
    opt.args[n++] = "--dynamic-linker=/usr/lib64/ld-linux-x86-64.so.2";
    opt.args[n++] = "-lc";
#endif
    if (ldflags)
        for (u32 i = 0; i < ldflags->count && n < MAX_ARGS; i++)
#ifdef _WIN32
            opt.args[n++] = st_translate_win_ldflag(arena, ldflags->items[i]);
#else
            opt.args[n++] = ldflags->items[i];
#endif
    ST_append_process_opt(procs, opt);
}

static b8 st_generate_asm(FILE *f, ST_ir_module_t *mod, ST_string_t src, ST_string_t file,
                          b8 want_debug, b8 use_fasm, ST_dbg_info_t *dbg) {
#ifdef _WIN32
    ST_unused(use_fasm);
    ST_unused(dbg);
    return ST_nasm_generate(f, mod, src, file, 1, want_debug);
#else
    if (use_fasm)
        return ST_fasm_generate(f, mod, src, file, 1, want_debug ? dbg : NULL);
    return ST_nasm_generate(f, mod, src, file, 1, want_debug);
#endif
}

#ifndef _WIN32
static void st_emit_debug(FILE *f, ST_dbg_info_t *dbg, b8 use_fasm) {
    if (use_fasm)
        ST_dwarf_emit_fasm_asan_lines(f, dbg);
    else
        ST_dwarf_emit(f, dbg, ".");
}
#endif

static void st_print_summary(double t_frontend, double t_middle, double t_backend_write,
                             double t_assemble, double t_link) {
    double total = t_frontend + t_middle + t_backend_write + t_assemble + t_link;
    fprintf(stderr, ST_COLOR_BOLD ST_COLOR_CYAN "summary\n" ST_COLOR_RESET);
    fprintf(stderr, "  " ST_COLOR_CYAN "frontend" ST_COLOR_RESET "   " ST_COLOR_WHITE "%8.2f ms\n" ST_COLOR_RESET,
            t_frontend);
    fprintf(stderr, "  " ST_COLOR_CYAN "middle" ST_COLOR_RESET "     " ST_COLOR_WHITE "%8.2f ms\n" ST_COLOR_RESET,
            t_middle);
    fprintf(stderr, "  " ST_COLOR_CYAN "backend" ST_COLOR_RESET "    " ST_COLOR_WHITE "%8.2f ms\n" ST_COLOR_RESET,
            t_backend_write);
    fprintf(stderr, "  " ST_COLOR_CYAN "assemble" ST_COLOR_RESET "   " ST_COLOR_WHITE "%8.2f ms\n" ST_COLOR_RESET,
            t_assemble);
    fprintf(stderr, "  " ST_COLOR_CYAN "link" ST_COLOR_RESET "       " ST_COLOR_WHITE "%8.2f ms\n" ST_COLOR_RESET,
            t_link);
    fprintf(stderr, "  " ST_COLOR_BOLD ST_COLOR_YELLOW "total" ST_COLOR_RESET "      " ST_COLOR_BOLD
            ST_COLOR_YELLOW "%8.2f ms\n" ST_COLOR_RESET, total);
}

static void st_ct_load_ldflag_libs(strings_t *ldflags) {
    const char *search_paths[64];
    u32 n_search_paths = 0;
    ST_forrange(0, ldflags->count) {
        const char *flag = ldflags->items[i];
        if (flag[0] == '-' && flag[1] == 'L' && flag[2] != '\0' &&
            n_search_paths < ST_array_len(search_paths))
            search_paths[n_search_paths++] = flag + 2;
    }
    ST_forrange(0, ldflags->count) {
        const char *flag = ldflags->items[i];
        if (!(flag[0] == '-' && flag[1] == 'l' && flag[2] != '\0'))
            continue;
        char libname[300];
        snprintf(libname, sizeof(libname), ST_CT_LIB_FMT, flag + 2);
        void *h = ST_ct_lib_load(libname);
        for (u32 k = 0; !h && k < n_search_paths; k++) {
            char full_path[512];
            snprintf(full_path, sizeof(full_path), "%s%c%s", search_paths[k], ST_PATH_SEP,
                     libname);
            h = ST_ct_lib_load(full_path);
        }
        if (!h)
            fprintf(stderr,
                    "storthc: warning: could not load '%s' for "
                    "'#comptime' extern calls (from '%s')\n",
                    libname, flag);
    }
}

static b8 st_compile(ST_arena_t *arena, const char *path, st_stage_t stage, const char *output,
                     strings_t *ldflags, i32 *run_exit_code, b8 no_debug, b8 use_fasm) {
    ST_procs_t procs;
    ST_procs_init(arena, &procs);
    b8 ok = 0;
    ST_TIME_START();
    double t_frontend = 0, t_middle = 0, t_backend_write = 0, t_assemble = 0, t_link = 0;
    b8 summary_printed = 0;

    b8 want_debug = !no_debug;

    ST_string_t file = ST_abs_path(arena, path);
    ST_srcmap_t srcs;
    ST_srcmap_init(arena, &srcs);

    ST_tokens_t tokens;
    if (!ST_load_file(arena, ST_cstr_to_str((char *)path), &srcs, &tokens)) {
        st_error("could not load '%s'", path);
        goto done;
    }
    ST_TIME_MARK(&t_frontend);
    if (stage == ST_STAGE_TOKENS) {
        ST_dump_token(tokens);
        ok = 1;
        goto done;
    }

    ST_string_t src = ST_srcmap_get(&srcs, file);
    ST_program_t prog = {0};
    if (!ST_parse(arena, tokens, src, file, &srcs, &prog)) {
        st_error("parsing '%s' failed", path);
        goto done;
    }
    ST_TIME_MARK(&t_frontend);

    ST_diag_t mod_diag = {.src = src, .file = file, .max_errors = ST_SEMA_MAX_ERRORS};
    if (!ST_modules_process(arena, &prog, file, &srcs, &mod_diag)) {
        st_error("module resolution for '%s' failed", path);
        goto done;
    }
    ST_TIME_MARK(&t_frontend);

    {
        ST_decl_t *fasm_const = ST_decl_new(arena, ST_DE_CONST, 0, 0);
        fasm_const->name = ST_cstr_to_str("FASM");
        fasm_const->is_pub = 1;
        ST_expr_t *fasm_val = ST_expr_new(arena, ST_EX_BOOL, 0, 0);
        fasm_val->ival = use_fasm ? 1 : 0;
        fasm_const->const_.value = fasm_val;
        ST_da_append_arena(arena, &prog.decls, fasm_const);
    }

#ifndef _WIN32
    if (!ST_asan_instrument(arena, &prog, &srcs)) {
        st_error("asan instrumentation failed");
        goto done;
    }
#endif
    ST_TIME_MARK(&t_frontend);

    if (stage == ST_STAGE_AST) {
        ST_dump_program(stdout, &prog);
        ok = 1;
        goto done;
    }

    ST_sema_t sema = {0};
    if (!ST_sema_run(arena, &prog, src, file, &srcs, &sema)) {
        st_error("semantic analysis for '%s' failed", path);
        goto done;
    }
    ST_TIME_MARK(&t_frontend);

    {
        ST_decl_t *comptime_main = NULL;
        ST_forrange(0, prog.decls.count) {
            ST_decl_t *d = prog.decls.items[i];
            if (d && d->kind == ST_DE_FN && d->fn.sig.is_comptime &&
                ST_string_eq_cstr(d->name, "main"))
                comptime_main = d;
        }

        if (comptime_main) {
            if (stage == ST_STAGE_ASM || stage == ST_STAGE_OBJ || stage == ST_STAGE_EXE) {
                fprintf(stderr,
                        "storthc: 'main' is marked '#comptime' -- it has no object code to "
                        "emit, only 'dump ir', 'dump ast', or 'run' are meaningful for it\n");
                goto done;
            }
            if (stage == ST_STAGE_RUN) {
                if (ldflags)
                    st_ct_load_ldflag_libs(ldflags);
                ST_ct_chunk_t chunk;
                ST_ct_chunk_init(arena, &chunk);
                u32 entry_ip = 0, cerr_line = 0, cerr_col = 0;
                ST_string_t cerr_file = file;
                char cerr_msg[256] = {0};
                if (!ST_ct_compile_program(arena, &chunk, &prog, &sema, src, file, comptime_main,
                                           &entry_ip, &cerr_line, &cerr_col, &cerr_file, cerr_msg,
                                           sizeof(cerr_msg))) {
                    ST_string_t err_src = cerr_file.len ? ST_srcmap_get(&srcs, cerr_file) : src;
                    ST_diag_t diag = {.src = err_src.len ? err_src : src,
                                      .file = cerr_file.len ? cerr_file : file,
                                      .max_errors = ST_SEMA_MAX_ERRORS};
                    ST_diag_error(&diag, cerr_line, cerr_col, "%s", cerr_msg);
                    goto done;
                }
                ST_ct_vm_t *vm = ST_arena_push(arena, sizeof(*vm));
                ST_ct_vm_init(vm);
                ST_ct_val_t result = {0};
                ST_ct_status_t vst = ST_ct_run(vm, &chunk, &result);
                if (vst != ST_CT_OK) {
                    ST_string_t vfile = ST_ct_chunk_file_at(&chunk, vm->err_ip);
                    if (!vfile.len)
                        vfile = file;
                    ST_string_t vsrc = ST_srcmap_get(&srcs, vfile);
                    ST_diag_t diag = {.src = vsrc.len ? vsrc : src, .file = vfile,
                                      .max_errors = ST_SEMA_MAX_ERRORS};
                    if (vst == ST_CT_ERR_COMPTIME)
                        ST_diag_error(&diag, vm->err_line, 1, "%s", vm->err_msg);
                    else
                        ST_diag_error(&diag, vm->err_line, 1, "internal: comptime VM error: %s",
                                      vm->err_msg);
                    goto done;
                }
                ok = 1;
                if (run_exit_code)
                    *run_exit_code = result.kind == ST_CT_INT ? (i32)result.i : 0;
                goto done;
            }
        }
    }

    ST_ir_module_t mod = {0};
    if (!ST_lower_program(arena, &prog, &sema, src, file, &mod)) {
        st_error("lowering '%s' to IR failed", path);
        goto done;
    }
    ST_TIME_MARK(&t_middle);

    if (stage == ST_STAGE_IR) {
        ST_ir_dump_module(stdout, &mod);
        ok = 1;
        goto done;
    }

    const char *asm_path = stage == ST_STAGE_ASM ? (output ? output : "test.asm")
                                                  : st_temp_path(arena, "storthc_build.asm");
    ST_dbg_info_t dbg = {0};
    FILE *f = fopen(asm_path, "wb");
    if (!f) {
        st_error("could not open '%s' for writing", asm_path);
        goto done;
    }

    b8 asm_ok = st_generate_asm(f, &mod, src, file, want_debug, use_fasm, &dbg);
    ST_TIME_MARK(&t_backend_write);
#ifndef _WIN32
    if (asm_ok) {
        ST_asan_write_runtime_asm(f, use_fasm);
        ST_TIME_MARK(&t_backend_write);
        st_emit_debug(f, &dbg, use_fasm);
        ST_TIME_MARK(&t_backend_write);
    }
#endif
    fclose(f);
    if (!asm_ok) {
        st_error("code generation failed (asm target was '%s')", asm_path);
        goto done;
    }
    if (stage == ST_STAGE_ASM) {
        ok = 1;
        goto done;
    }

    const char *obj_path = stage == ST_STAGE_OBJ ? (output ? output : "test.o")
                                                  : st_temp_path(arena, "storthc_build.o");

    st_append_assemble(&procs, asm_path, obj_path, use_fasm);
    if (!st_run_step(&procs, "assembling"))
        goto done;
    ST_TIME_MARK(&t_assemble);
    if (stage == ST_STAGE_OBJ) {
        ok = 1;
        goto done;
    }

    const char *exe_path = output ? output : st_default_exe_name(arena, path);
    st_append_link(arena, &procs, exe_path, obj_path, ldflags);
    if (!st_run_step(&procs, "linking"))
        goto done;
    ST_TIME_MARK(&t_link);
    if (stage == ST_STAGE_EXE) {
        ok = 1;
        goto done;
    }

    st_print_summary(t_frontend, t_middle, t_backend_write, t_assemble, t_link);
    summary_printed = 1;

    ST_append_process(&procs, st_abs_path_cstr(arena, exe_path));
    {
        ST_proc_t *run_proc = &procs.items[procs.count - 1];
        if (!ST_run_process(run_proc) && !run_proc->exited_normally) {
            st_error("could not run '%s'", exe_path);
            st_print_proc(run_proc);
            goto done;
        }
        ok = 1;
        if (run_exit_code)
            *run_exit_code = run_proc->exit_code;
    }

done:
    if (!summary_printed && ok)
        st_print_summary(t_frontend, t_middle, t_backend_write, t_assemble, t_link);
    return ok;
}

static void st_clean_comptime_cache(ST_arena_t *arena) {
    ST_glob_t g = {0};
    if (ST_glob(st_temp_path(arena, ST_CT_CACHE_GLOB), &g)) {
        for (u32 i = 0; i < g.gl_pathc; i++)
            remove(g.gl_pathv[i]);
        ST_glob_free(&g);
    }
}

int main(int argc, char **argv) {
    if (argc == 2 && (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-version") == 0)) {
        st_print_version();
        return 0;
    }

    ST_arena_t *arena = ST_arena_alloc();
    st_clean_comptime_cache(arena);

    cli_t *cli = cli_init("storthc", "The Storth compiler");
    command_t *root = cli_root(cli);

    command_t *version = cli_command(root, "version", "Print version information");

    command_t *dump = cli_command(root, "dump", "Dump an intermediate stage of compilation");
    command_t *dump_tokens = cli_command(dump, "tokens", "Dump the lexer's token stream");
    command_t *dump_ast = cli_command(dump, "ast", "Dump the parsed and resolved AST");
    command_t *dump_ir = cli_command(dump, "ir", "Dump the lowered SSA IR");

    char *src_tokens = NULL, *src_ast = NULL, *src_ir = NULL;
    cli_positional(dump_tokens, "source", "Storth source file", &src_tokens, true);
    cli_positional(dump_ast, "source", "Storth source file", &src_ast, true);
    cli_positional(dump_ir, "source", "Storth source file", &src_ir, true);

    command_t *build =
        cli_command(root, "build", "Build source into assembly, an object file, or an executable");
    command_t *build_asm = cli_command(build, "asm", "Emit NASM assembly (default: test.asm)");
    command_t *build_obj = cli_command(build, "obj", "Assemble to an object file (default: test.o)");
    command_t *build_exe = cli_command(build, "exe", "Link a final executable (default: test)");

    bool no_debug = false, use_fasm = false;
    const char *nodebug_desc =
        "Skip per-instruction debug-info collection during codegen (faster "
        "assembly, but a crash reports a raw address instead of a "
        "symbolicated file:line)";
#ifndef _WIN32
    const char *fasm_desc = "Use the FASM backend instead of nasm";
#endif

    char *src_asm = NULL, *out_asm = NULL;
    cli_positional(build_asm, "source", "Storth source file", &src_asm, true);
    cli_create_flag_string(build_asm, "output", "Output .asm path", &out_asm);
    cli_alias(build_asm, "output", 'o');
    cli_create_flag_bool(build_asm, "nodebug", nodebug_desc, &no_debug);
#ifndef _WIN32
    cli_create_flag_bool(build_asm, "fasm", fasm_desc, &use_fasm);
#endif

    char *src_obj = NULL, *out_obj = NULL;
    cli_positional(build_obj, "source", "Storth source file", &src_obj, true);
    cli_create_flag_string(build_obj, "output", "Output object file path", &out_obj);
    cli_alias(build_obj, "output", 'o');
    cli_create_flag_bool(build_obj, "nodebug", nodebug_desc, &no_debug);
#ifndef _WIN32
    cli_create_flag_bool(build_obj, "fasm", fasm_desc, &use_fasm);
#endif

    char *src_exe = NULL, *out_exe = NULL;
    strings_t exe_ldflags = {0};
    cli_positional(build_exe, "source", "Storth source file", &src_exe, true);
    cli_create_flag_string(build_exe, "output", "Output executable path", &out_exe);
    cli_alias(build_exe, "output", 'o');
    cli_create_flag_bool(build_exe, "nodebug", nodebug_desc, &no_debug);
#ifndef _WIN32
    cli_create_flag_bool(build_exe, "fasm", fasm_desc, &use_fasm);
#endif
    cli_rest(build_exe, "Extra flags passed through to the linker (after a lone '-')",
             &exe_ldflags);

    command_t *run = cli_command(root, "run", "Build an executable and run it");
    char *src_run = NULL, *out_run = NULL;
    strings_t run_ldflags = {0};
    cli_positional(run, "source", "Storth source file", &src_run, true);
    cli_create_flag_string(run, "output", "Output executable path", &out_run);
    cli_alias(run, "output", 'o');
    cli_create_flag_bool(run, "nodebug", nodebug_desc, &no_debug);
#ifndef _WIN32
    cli_create_flag_bool(run, "fasm", fasm_desc, &use_fasm);
#endif
    cli_rest(run, "Extra flags passed through to the linker (after a lone '-')", &run_ldflags);

    cli_status_t status = cli_parse(cli, argc, argv);

    b8 ok = 0;
    i32 run_exit_code = 0;
    command_t *active = NULL;
    if (argc == 1) {
        cli_usage(cli);
        goto done;
    }
    if (status == CLI_HELP) {
        ok = 1;
        goto done;
    }
    if (status == CLI_ERROR)
        goto done;

    active = cli_active_command(cli);
    if (active == version) {
        st_print_version();
        ok = 1;
    } else if (active == dump_tokens)
        ok = st_compile(arena, src_tokens, ST_STAGE_TOKENS, NULL, NULL, NULL, 0, 0);
    else if (active == dump_ast)
        ok = st_compile(arena, src_ast, ST_STAGE_AST, NULL, NULL, NULL, 0, 0);
    else if (active == dump_ir)
        ok = st_compile(arena, src_ir, ST_STAGE_IR, NULL, NULL, NULL, 0, 0);
    else if (active == build_asm)
        ok = st_compile(arena, src_asm, ST_STAGE_ASM, out_asm, NULL, NULL, no_debug, use_fasm);
    else if (active == build_obj)
        ok = st_compile(arena, src_obj, ST_STAGE_OBJ, out_obj, NULL, NULL, no_debug, use_fasm);
    else if (active == build_exe)
        ok = st_compile(arena, src_exe, ST_STAGE_EXE, out_exe, &exe_ldflags, NULL, no_debug,
                        use_fasm);
    else if (active == run)
        ok = st_compile(arena, src_run, ST_STAGE_RUN, out_run, &run_ldflags, &run_exit_code,
                        no_debug, use_fasm);
    else if (active)
        cli_command_usage(active);
    else
        cli_usage(cli);

done:
    cli_destroy(cli);
    ST_arena_free(arena);
    if (ok && active == run)
        return run_exit_code;
    return ok ? 0 : 1;
}
