#include <string.h>

#include "backend/st_nasm.h"
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

static void st_append_ld(ST_procs_t *procs, const char *out_path, const char *obj_path,
                         strings_t *ldflags) {
    ST_proc_opt_t opt = {0};
    u32 n = 0;
    opt.args[n++] = "ld";
    opt.args[n++] = "-o";
    opt.args[n++] = out_path;
    opt.args[n++] = obj_path;
    opt.args[n++] = "--dynamic-linker=/usr/lib64/ld-linux-x86-64.so.2";
    opt.args[n++] = "-lc";
    if (ldflags)
        for (u32 i = 0; i < ldflags->count && n < MAX_ARGS; i++)
            opt.args[n++] = ldflags->items[i];
    ST_append_process_opt(procs, opt);
}

static b8 st_compile(ST_arena_t *arena, const char *path, st_stage_t stage, const char *output,
                     strings_t *ldflags, i32 *run_exit_code) {
    ST_procs_t procs = {0};
    b8 ok = 0;

    ST_string_t file = ST_abs_path(arena, path);
    ST_srcmap_t srcs;
    ST_srcmap_init(arena, &srcs);

    ST_tokens_t tokens;
    if (!ST_load_file(arena, ST_cstr_to_str((char *)path), &srcs, &tokens))
        goto done;
    if (stage == ST_STAGE_TOKENS) {
        ST_dump_token(tokens);
        ok = 1;
        goto done;
    }

    ST_string_t src = ST_srcmap_get(&srcs, file);
    ST_program_t prog = {0};
    if (!ST_parse(arena, tokens, src, file, &srcs, &prog))
        goto done;

    ST_diag_t mod_diag = {.src = src, .file = file, .max_errors = ST_SEMA_MAX_ERRORS};
    if (!ST_modules_process(arena, &prog, file, &srcs, &mod_diag))
        goto done;

    if (stage == ST_STAGE_AST) {
        ST_dump_program(stdout, &prog);
        ok = 1;
        goto done;
    }

    ST_sema_t sema = {0};
    if (!ST_sema_run(arena, &prog, src, file, &srcs, &sema))
        goto done;

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
                if (ldflags) {
                    // Collect '-L<path>' search paths first, so every '-l<name>' can be
                    // tried against each of them (dlopen only searches the standard
                    // system paths and LD_LIBRARY_PATH on its own -- it has no idea
                    // '-L' ever existed, that's purely an 'ld' convention).
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
                        snprintf(libname, sizeof(libname), "lib%s.so", flag + 2);
                        void *h = ST_ct_lib_load(libname); // bare name: standard search paths
                        for (u32 k = 0; !h && k < n_search_paths; k++) {
                            char full_path[512];
                            snprintf(full_path, sizeof(full_path), "%s/%s", search_paths[k],
                                    libname);
                            h = ST_ct_lib_load(full_path);
                        }
                        if (!h)
                            fprintf(stderr,
                                    "storthc: warning: could not dlopen '%s' for "
                                    "'#comptime' extern calls (from '%s')\n",
                                    libname, flag);
                    }
                }
                ST_ct_chunk_t chunk;
                ST_ct_chunk_init(arena, &chunk);
                u32 entry_ip = 0, cerr_line = 0, cerr_col = 0;
                char cerr_msg[256] = {0};
                if (!ST_ct_compile_program(arena, &chunk, &prog, comptime_main, &entry_ip,
                                           &cerr_line, &cerr_col, cerr_msg, sizeof(cerr_msg))) {
                    ST_diag_t diag = {.src = src, .file = file, .max_errors = ST_SEMA_MAX_ERRORS};
                    ST_diag_error(&diag, cerr_line, cerr_col, "%s", cerr_msg);
                    goto done;
                }
                ST_ct_vm_t vm;
                ST_ct_vm_init(&vm);
                ST_ct_val_t result = {0};
                ST_ct_status_t vst = ST_ct_run(&vm, &chunk, &result);
                if (vst != ST_CT_OK) {
                    ST_diag_t diag = {.src = src, .file = file, .max_errors = ST_SEMA_MAX_ERRORS};
                    if (vst == ST_CT_ERR_COMPTIME)
                        ST_diag_error(&diag, vm.err_line, 1, "%s", vm.err_msg);
                    else
                        ST_diag_error(&diag, vm.err_line, 1, "internal: comptime VM error: %s",
                                      vm.err_msg);
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
    if (!ST_lower_program(arena, &prog, &sema, src, file, &mod))
        goto done;

    if (stage == ST_STAGE_IR) {
        ST_ir_dump_module(stdout, &mod);
        ok = 1;
        goto done;
    }

    const char *asm_path = stage == ST_STAGE_ASM ? (output ? output : "test.asm")
                                                  : "/tmp/storthc_build.asm";
    FILE *f = fopen(asm_path, "wb");
    if (!f) {
        fprintf(stderr, "storthc: could not open '%s' for writing\n", asm_path);
        goto done;
    }
    b8 asm_ok = ST_nasm_generate(f, &mod, src, file, 1);
    fclose(f);
    if (!asm_ok)
        goto done;
    if (stage == ST_STAGE_ASM) {
        ok = 1;
        goto done;
    }

    const char *obj_path =
        stage == ST_STAGE_OBJ ? (output ? output : "test.o") : "/tmp/storthc_build.o";
    ST_append_process(&procs, "nasm", "-f", "elf64", asm_path, "-o", obj_path);
    if (!ST_run_processes(&procs))
        goto done;
    if (stage == ST_STAGE_OBJ) {
        ok = 1;
        goto done;
    }

    const char *exe_path = output ? output : "test";
    st_append_ld(&procs, exe_path, obj_path, ldflags);
    if (!ST_run_processes(&procs))
        goto done;
    if (stage == ST_STAGE_EXE) {
        ok = 1;
        goto done;
    }

    ST_append_process(&procs, st_abs_path_cstr(arena, exe_path));
    {
        ST_proc_t *run_proc = &procs.items[procs.count - 1];
        ST_run_process(run_proc);
        if (!run_proc->exited_normally) {
            fprintf(stderr, "storthc: '%s' did not exit normally\n", exe_path);
            goto done;
        }
        ok = 1;
        if (run_exit_code)
            *run_exit_code = run_proc->exit_code;
    }

done:
    ST_free_process(&procs);
    return ok;
}

int main(int argc, char **argv) {
    ST_arena_t *arena = ST_arena_alloc();
    cli_t *cli = cli_init("storthc", "The Storth compiler");
    command_t *root = cli_root(cli);

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

    char *src_asm = NULL, *out_asm = NULL;
    cli_positional(build_asm, "source", "Storth source file", &src_asm, true);
    cli_create_flag_string(build_asm, "output", "Output .asm path", &out_asm);
    cli_alias(build_asm, "output", 'o');

    char *src_obj = NULL, *out_obj = NULL;
    cli_positional(build_obj, "source", "Storth source file", &src_obj, true);
    cli_create_flag_string(build_obj, "output", "Output object file path", &out_obj);
    cli_alias(build_obj, "output", 'o');

    char *src_exe = NULL, *out_exe = NULL;
    strings_t exe_ldflags = {0};
    cli_positional(build_exe, "source", "Storth source file", &src_exe, true);
    cli_create_flag_string(build_exe, "output", "Output executable path", &out_exe);
    cli_alias(build_exe, "output", 'o');
    cli_rest(build_exe, "Extra flags passed through to 'ld' (after a lone '-')", &exe_ldflags);

    command_t *run = cli_command(root, "run", "Build an executable and run it");
    char *src_run = NULL, *out_run = NULL;
    strings_t run_ldflags = {0};
    cli_positional(run, "source", "Storth source file", &src_run, true);
    cli_create_flag_string(run, "output", "Output executable path", &out_run);
    cli_alias(run, "output", 'o');
    cli_rest(run, "Extra flags passed through to 'ld' (after a lone '-')", &run_ldflags);

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
    if (active == dump_tokens)
        ok = st_compile(arena, src_tokens, ST_STAGE_TOKENS, NULL, NULL, NULL);
    else if (active == dump_ast)
        ok = st_compile(arena, src_ast, ST_STAGE_AST, NULL, NULL, NULL);
    else if (active == dump_ir)
        ok = st_compile(arena, src_ir, ST_STAGE_IR, NULL, NULL, NULL);
    else if (active == build_asm)
        ok = st_compile(arena, src_asm, ST_STAGE_ASM, out_asm, NULL, NULL);
    else if (active == build_obj)
        ok = st_compile(arena, src_obj, ST_STAGE_OBJ, out_obj, NULL, NULL);
    else if (active == build_exe)
        ok = st_compile(arena, src_exe, ST_STAGE_EXE, out_exe, &exe_ldflags, NULL);
    else if (active == run)
        ok = st_compile(arena, src_run, ST_STAGE_RUN, out_run, &run_ldflags, &run_exit_code);
    else
        cli_usage(cli);

done:
    cli_destroy(cli);
    ST_arena_free(arena);
    if (ok && active == run)
        return run_exit_code;
    return ok ? 0 : 1;
}
