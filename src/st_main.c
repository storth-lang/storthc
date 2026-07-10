#include "./utils/st_types.h"
#include "./utils/st_string.h"
#include "./frontend/st_lexer.h"
#include "./frontend/st_ast.h"
#include "./frontend/st_parser.h"
#include "./frontend/st_semantic.h"

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "main.st";
    ST_string_t sv = {0};
    ST_arena_t *arena = ST_arena_alloc();
    if (!ST_read_entire_file(arena, &sv, path))
    {
        fprintf(stderr, "error: could not read '%s'\n", path);
        return 1;
    }

    ST_tokens_t tokens = ST_lex(arena, sv, ST_abs_path(arena, path));
    if (!tokens.ok)
    {
        free(tokens.items);
        ST_arena_free(arena);
        return 1;
    }

    ST_program_t prog = {0};
    ST_string_t abs = ST_abs_path(arena, path);
    b8 ok = ST_parse(arena, tokens, sv, abs, &prog);
    if (ok) ok = ST_sema_run(arena, &prog, sv, abs);
    if (ok) ST_dump_program(stdout, &prog);

    free(tokens.items);
    ST_arena_free(arena);
    return ok ? 0 : 1;
}
