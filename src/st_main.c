#include "st_types.h"
#include "st_string.h"
#include "st_lexer.h"

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "main.st";
    ST_string_t sv = {0};
    ST_arena_t *arena = ST_arena_alloc();
    if (ST_read_entire_file(arena, &sv, path) < 0)
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

    ST_dump_token(tokens);

    free(tokens.items);
    ST_arena_free(arena);
    return 0;
}
