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
    b8 dump_tokens = 0, dump_ast = 0, dump_ir = 0, emit_asm = 0;
    char *path = NULL;

    ST_arena_t *arena = ST_arena_alloc();
    ST_flag_parser_t *fp = ST_flag_init(arena);
    ST_procs_t procs = {0};
    int rc = 1;

    ST_flag_bool(fp, "dump_tokens", "Dump the tokens of storth source code", &dump_tokens);
    ST_flag_bool(fp, "dump_ast", "Dump the ast of storth source code", &dump_ast);
    ST_flag_bool(fp, "dump_ir", "Print out the intermediate representation of storth source code with its custom IR", &dump_ir);
    ST_flag_bool(fp, "emit-asm", "Emit the assembly instruction that is generated from the IR(nasm)", &emit_asm);
    ST_flag_alias(fp, "emit-asm", 's');
    ST_flag_positional(fp, "input", "Path to the storth source file to compile", &path, 1);

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

    const char *asm_path = "test.asm";
    FILE *f = fopen(asm_path, "wb");
    if (emit_asm)
    {
        if (!ST_nasm_generate(f, &mod, src, file, 1))
        {
            fclose(f);
            goto done;
        }
    }
    fclose(f);

    ST_append_process(&procs, "nasm", "-f", "elf64", asm_path);
    if (!ST_run_processes(&procs)) goto done;
    ST_append_process(&procs, "ld", "-o", "test", "test.o");
    if (!ST_run_processes(&procs)) goto done;

done:
    ST_free_process(&procs);
    ST_arena_free(arena);
    return rc;
}
