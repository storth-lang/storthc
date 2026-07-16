#include "utils/st_helper.h"
#include "utils/st_arena.h"
#include "utils/st_string.h"
#include "frontend/st_lexer.h"
#include "frontend/st_parser.h"
#include "frontend/st_semantic.h"
#include "middle/st_lower.h"
#include "backend/st_nasm.h"

static void ST_usage(const char *prog)
{
    fprintf(stderr, "usage: %s [--emit-asm] [--dump-ir] [--dump-tokens] [--dump-ast] <file.st>\n", prog);
}

int main(int argc, char **argv)
{
    const char *path = NULL;
    b8 dump_tokens = 0, dump_ast = 0, dump_ir = 0, emit_asm = 0;

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--dump-tokens") == 0) dump_tokens = 1;
        else if (strcmp(argv[i], "--dump-ast") == 0) dump_ast = 1;
        else if (strcmp(argv[i], "--dump-ir") == 0) dump_ir = 1;
        else if (strcmp(argv[i], "--emit-asm") == 0) emit_asm = 1;
        else if (argv[i][0] == '-')
        {
            ST_usage(argv[0]);
            return 1;
        }
        else path = argv[i];
    }
    if (!path)
    {
        ST_usage(argv[0]);
        return 1;
    }

    ST_arena_t *arena = ST_arena_alloc();
    ST_string_t file = ST_abs_path(arena, path);

    ST_string_t src = {0};
    if (!ST_read_entire_file(arena, &src, path))
    {
        fprintf(stderr, "error: could not read '%s'\n", path);
        ST_arena_free(arena);
        return 1;
    }

    int rc = 1;

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
    if (emit_asm) if (!ST_nasm_generate(stdout, &mod, src, file, 1)) goto done;

done:
    ST_arena_free(arena);
    return rc;
}
