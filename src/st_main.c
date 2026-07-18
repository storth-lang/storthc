#include "utils/st_helper.h"
#include "utils/st_arena.h"
#include "utils/st_string.h"
#include "utils/st_process.h"
#include "utils/st_flag.h"
#include "frontend/st_lexer.h"
#include "frontend/st_parser.h"
#include "frontend/st_semantic.h"
#include "middle/st_lower.h"
#include "backend/st_nasm.h"

int main(int argc, char **argv)
{
    b8 dump_tokens = 0;
    b8 dump_ast = 0;
    b8 dump_ir = 0;
    b8 emit_asm = 0;
    b8 build_exe = 0;
    b8 emit_obj = 0;
    b8 run_exe = 0;
    char *path = NULL;

    ST_arena_t *arena = ST_arena_alloc();
    ST_flag_parser_t *fp = ST_flag_init(arena);
    ST_procs_t procs = {0};
    b8 rc = 1;

    ST_flag_bool(fp, "dump-tokens", "Dump the tokens of storth"
                 " source code", &dump_tokens);
    ST_flag_bool(fp, "dump-ast", "Dump the ast of storth source code", &dump_ast);
    ST_flag_bool(fp, "emit-ir", "Print out the intermediate representation "
                 "of storth source code.", &dump_ir);
    ST_flag_bool(fp, "emit-asm", "Emit the assembly instruction that "
                 "is generated from the intermediate representation", &emit_asm);
    ST_flag_bool(fp, "build", "Generate the final executable from the assembly.",
                 &build_exe);
    ST_flag_bool(fp, "run", "Generate the executable and run the "
                 "generated executable.", &run_exe);
    ST_flag_bool(fp, "emit-obj", "Emit the object files"
                 "generated from the assembly", &emit_obj);

    ST_flag_alias(fp, "emit-asm", 's');
    ST_flag_alias(fp, "emit-obj", 'c');
    ST_flag_alias(fp, "build", 'b');
    ST_flag_alias(fp, "run", 'r');

    ST_flag_positional(fp, "input", "Path to the storth source file "
                       "to compile", &path, 1);

    const char *asm_path = "test.asm";
    const char *obj_path = "test.o";

    const char *exe_path = "test";
    ST_string_t exe_path_s = ST_abs_path(arena, exe_path);
    exe_path = (const char *)exe_path_s.data;

    if (build_exe || run_exe) asm_path = "/tmp/test.asm";
    if (build_exe || run_exe) obj_path = "/tmp/test.o";
    FILE *f = fopen(asm_path, "wb");

    if (!ST_flag_parse(fp, argc, argv))
    {
        ST_flag_usage(fp);
        goto done;
    }

    ST_string_t file = ST_abs_path(arena, path);
    ST_string_t src = {0};
    if (!ST_read_entire_file(arena, &src, path))
    {
        fprintf(stderr, "error: could not read '%s'\n", path);
        goto done;
    }

    ST_tokens_t tokens = ST_lex(arena, src, file);
    if (!tokens.ok) goto done;
    if (dump_tokens) ST_dump_token(tokens);

    ST_program_t prog = {0};
    if (!ST_parse(arena, tokens, src, file, &prog)) goto done;
    if (dump_ast) ST_dump_program(stdout, &prog);

    ST_sema_t sema = {0};
    if (!ST_sema_run(arena, &prog, src, file, &sema)) goto done;

    ST_ir_module_t mod = {0};
    rc = ST_lower_program(arena, &prog, &sema, src, file, &mod) ? 0 : 1;
    if (dump_ir) ST_ir_dump_module(stdout, &mod);
    if (rc != 0) goto done;

    if (emit_asm) if (!ST_nasm_generate(f, &mod, src, file, 1)) goto done;

    if (emit_obj || build_exe || run_exe)
    {
        if (!ST_nasm_generate(f, &mod, src, file, 1)) goto done;
        ST_append_process(&procs, "nasm", "-f", "elf64", asm_path);
        if (!ST_run_processes(&procs)) goto done;
    }

    if (build_exe || run_exe)
    {
        ST_append_process(&procs, "ld", "-o", "test", obj_path,
                          "--dynamic-linker=/usr/lib64/ld-linux-x86-64.so.2",
                          "-lc");
        if (!ST_run_processes(&procs)) goto done;
        if (run_exe)
        {
            ST_append_process(&procs, exe_path);
            if (!ST_run_processes(&procs)) goto done;
        }
    }

done:
    fclose(f);
    ST_free_process(&procs);
    ST_arena_free(arena);
    return rc;
}
