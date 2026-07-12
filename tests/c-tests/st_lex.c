#include "./utils/st_types.h"
#include "./utils/st_string.h"
#include "./frontend/st_lexer.h"
#include "./frontend/st_ast.h"
#include "./frontend/st_parser.h"
#include "./frontend/st_semantic.h"

char *source_code =
    "fn main() -> i32 {\n"
    "    return 0;"
    "}";

int main(int argc, char **argv)
{
    ST_string_t sv = ST_cstr_to_str(source_code);
    ST_arena_t *arena = ST_arena_alloc();
    ST_tokens_t tokens = ST_lex(arena, sv, ST_abs_path(arena, NULL));
    if (!tokens.ok)
    {
        free(tokens.items);
        ST_arena_free(arena);
        return 1;
    }

    ST_program_t prog = {0};
    ST_string_t abs = ST_abs_path(arena, NULL);
    b8 ok = ST_parse(arena, tokens, sv, abs, &prog);
    ST_sema_t s = {0};
    if (ok) ok = ST_sema_run(arena, &prog, sv, abs, &s);
    if (ok) ST_dump_program(stdout, &prog);

    free(tokens.items);
    ST_arena_free(arena);
    return ok ? 0 : 1;
}
