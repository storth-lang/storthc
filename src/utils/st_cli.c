#include "st_cli.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define ARENA_BLOCK_SIZE (64 * 1024)
#define ARENA_ALIGN 16

#define ANSI_RESET "\033[0m"
#define ANSI_BOLD "\033[1m"
#define ANSI_GREY "\033[90m"

enum cli_kind_t {
    CLI_INT,
    CLI_BOOL,
    CLI_STRING,
    CLI_STRING_LIST,
    CLI_REST,
};

typedef struct arena_block_t arena_block_t;

struct arena_block_t {
    arena_block_t *next;
    size_t size;
    size_t used;
    unsigned char data[];
};

typedef struct {
    arena_block_t *head;
} arena_t;

static arena_block_t *arena_new_block(size_t min_size) {
    size_t size = min_size > ARENA_BLOCK_SIZE ? min_size : ARENA_BLOCK_SIZE;
    arena_block_t *block = malloc(sizeof(*block) + size);
    if (!block)
        return NULL;

    block->next = NULL;
    block->size = size;
    block->used = 0;
    return block;
}

static void *arena_alloc(arena_t *arena, size_t size) {
    size = (size + (ARENA_ALIGN - 1)) & ~(size_t)(ARENA_ALIGN - 1);

    if (!arena->head || arena->head->used + size > arena->head->size) {
        arena_block_t *block = arena_new_block(size);
        if (!block)
            return NULL;

        block->next = arena->head;
        arena->head = block;
    }

    void *ptr = arena->head->data + arena->head->used;
    arena->head->used += size;
    return ptr;
}

static void arena_free(arena_t *arena) {
    arena_block_t *block = arena->head;
    while (block) {
        arena_block_t *next = block->next;
        free(block);
        block = next;
    }
    arena->head = NULL;
}

#define da_append(arena, arr, item)                                                                \
    do {                                                                                           \
        if ((arr)->count >= (arr)->capacity) {                                                     \
            unsigned int new_cap = ((arr)->capacity <= 0) ? 256 : (arr)->capacity * 2;             \
            void *new_items = arena_alloc((arena), sizeof(*(arr)->items) * new_cap);               \
            if ((arr)->items)                                                                      \
                memcpy(new_items, (arr)->items, sizeof(*(arr)->items) * (arr)->count);             \
            (arr)->items = new_items;                                                              \
            (arr)->capacity = new_cap;                                                             \
        }                                                                                          \
        (arr)->items[(arr)->count++] = item;                                                       \
    } while (0)

typedef struct cli_flag_t cli_flag_t;

struct cli_flag_t {
    const char *name;
    const char *description;
    const char *program;

    char alias;
    bool seen, positional, required;

    void *value;
    cli_kind_t kind;

    cli_flag_t *next;
};

struct command_t {
    cli_t *cli;
    const char *name;

    const char *description;
    const char *alias;

    cli_call_back_t callback;
    command_t *parent;
    command_t *children;
    command_t *next;

    cli_flag_t *flags;
};

struct cli_t {
    const char *program;

    command_t root;
    command_t *active;

    arena_t arena;
};

static void *cli_alloc(cli_t *cli, size_t size) {
    return arena_alloc(&cli->arena, size);
}

typedef bool (*cli_flag_iter_fn)(command_t *cmd, cli_flag_t *flag, void *user);

static bool cli_for_each_flag(command_t *cmd, cli_flag_iter_fn fn, void *user) {
    for (command_t *current = cmd; current; current = current->parent) {
        for (cli_flag_t *flag = current->flags; flag; flag = flag->next) {
            if (!fn(current, flag, user))
                return false;
        }
    }

    return true;
}

struct find_value_ctx {
    const char *name;
    bool positional;
    cli_flag_t *result;
};

static bool cli_find_value_cb(command_t *cmd, cli_flag_t *flag, void *user) {
    (void)cmd;
    struct find_value_ctx *ctx = user;

    if (flag->positional == ctx->positional && strcmp(flag->name, ctx->name) == 0) {
        ctx->result = flag;
        return false;
    }

    return true;
}

static cli_flag_t *cli_find_flag_value(command_t *cmd, const char *name) {
    struct find_value_ctx ctx = {
        .name = name,
        .positional = true,
        .result = NULL,
    };

    cli_for_each_flag(cmd, cli_find_value_cb, &ctx);

    if (ctx.result)
        return ctx.result;

    ctx.positional = false;
    cli_for_each_flag(cmd, cli_find_value_cb, &ctx);

    return ctx.result;
}

struct find_named_ctx {
    const char *name;
    char alias;
    bool by_alias;
    cli_flag_t *result;
};

static bool cli_find_named_cb(command_t *cmd, cli_flag_t *flag, void *user) {
    (void)cmd;
    struct find_named_ctx *ctx = user;

    if (ctx->by_alias) {
        if (flag->alias == ctx->alias) {
            ctx->result = flag;
            return false;
        }
    } else if (!flag->positional && strcmp(flag->name, ctx->name) == 0) {
        ctx->result = flag;
        return false;
    }

    return true;
}

static cli_flag_t *cli_find_flag(command_t *cmd, const char *name) {
    struct find_named_ctx ctx = {.name = name};
    cli_for_each_flag(cmd, cli_find_named_cb, &ctx);
    return ctx.result;
}

static cli_flag_t *cli_find_flag_alias(command_t *cmd, char alias) {
    struct find_named_ctx ctx = {.by_alias = true, .alias = alias};
    cli_for_each_flag(cmd, cli_find_named_cb, &ctx);
    return ctx.result;
}

static bool cli_next_position_cb(command_t *cmd, cli_flag_t *flag, void *user) {
    (void)cmd;
    cli_flag_t **out = user;
    if (flag->positional && !flag->seen) {
        *out = flag;
        return false;
    }
    return true;
}

static cli_flag_t *cli_goto_next_position(command_t *cmd) {
    cli_flag_t *result = NULL;
    cli_for_each_flag(cmd, cli_next_position_cb, &result);
    return result;
}

static bool cli_find_rest_cb(command_t *cmd, cli_flag_t *flag, void *user) {
    (void)cmd;
    cli_flag_t **out = user;
    if (flag->kind == CLI_REST) {
        *out = flag;
        return false;
    }
    return true;
}

static cli_flag_t *cli_find_rest(command_t *cmd) {
    cli_flag_t *result = NULL;
    cli_for_each_flag(cmd, cli_find_rest_cb, &result);
    return result;
}

static bool cli_check_required_cb(command_t *cmd, cli_flag_t *flag, void *user) {
    (void)cmd;
    cli_flag_t **out = user;
    if (!flag->seen && flag->positional && flag->required) {
        *out = flag;
        return false;
    }
    return true;
}

static cli_status_t cli_check_required(command_t *cmd) {
    cli_flag_t *missing = NULL;
    cli_for_each_flag(cmd, cli_check_required_cb, &missing);

    if (missing) {
        fprintf(stderr, "%s: missing required argument <%s>\n", cmd->cli->program, missing->name);
        return CLI_ERROR;
    }
    return CLI_OK;
}

static cli_flag_t *cli_add_flag(command_t *cmd, cli_kind_t kind, const char *name,
                                const char *description, void *value, bool required,
                                bool positional) {
    cli_flag_t *flag = cli_alloc(cmd->cli, sizeof(*flag));
    if (!flag)
        return NULL;

    flag->name = name;
    flag->description = description;
    flag->value = value;
    flag->kind = kind;

    flag->alias = 0;
    flag->seen = false;
    flag->positional = positional;
    flag->required = required;

    flag->next = cmd->flags;
    cmd->flags = flag;

    return flag;
}

cli_t *cli_init(const char *name, const char *desc) {
    cli_t *cli = malloc(sizeof(*cli));
    if (!cli)
        return NULL;

    cli->root.cli = cli;
    cli->root.name = name ? name : "";
    cli->root.description = desc ? desc : "Application";
    cli->root.alias = NULL;
    cli->root.callback = NULL;
    cli->root.parent = NULL;
    cli->root.children = NULL;
    cli->root.next = NULL;
    cli->root.flags = NULL;

    cli->active = &cli->root;
    cli->arena.head = NULL;
    return cli;
}

command_t *cli_root(cli_t *cli) {
    return &cli->root;
}

command_t *cli_command(command_t *parent, const char *name, const char *desc) {
    command_t *cmd = cli_alloc(parent->cli, sizeof(*cmd));
    if (!cmd)
        return NULL;

    cmd->cli = parent->cli;
    cmd->parent = parent;

    cmd->name = name;
    cmd->description = desc;
    cmd->alias = NULL;
    cmd->callback = NULL;
    cmd->children = NULL;
    cmd->flags = NULL;
    cmd->next = parent->children;

    parent->children = cmd;
    return cmd;
}

void cli_command_alias(command_t *cmd, const char *alias) {
    cmd->alias = alias;
}

void cli_command_callback(command_t *cmd, cli_call_back_t callback) {
    cmd->callback = callback;
}

void cli_create_flag_bool(command_t *cmd, const char *name, const char *description, bool *value) {
    cli_add_flag(cmd, CLI_BOOL, name, description, value, 0, 0);
}

void cli_create_flag_int(command_t *cmd, const char *name, const char *description, int *value) {
    cli_add_flag(cmd, CLI_INT, name, description, value, 0, 0);
}

void cli_create_flag_string(command_t *cmd, const char *name, const char *description,
                            char **value) {
    cli_add_flag(cmd, CLI_STRING, name, description, value, 0, 0);
}

void cli_create_flag_strings(command_t *cmd, const char *name, const char *description,
                             strings_t *value) {
    cli_add_flag(cmd, CLI_STRING_LIST, name, description, value, 0, 0);
}

void cli_positional(command_t *cmd, const char *name, const char *description, char **value,
                    bool required) {
    cli_add_flag(cmd, CLI_STRING, name, description, value, required, true);
}

void cli_rest(command_t *cmd, const char *description, strings_t *value) {
    cli_add_flag(cmd, CLI_REST, "__rest__", description, value, false, false);
}

void cli_alias(command_t *cmd, const char *name, char alias) {
    for (cli_flag_t *flag = cmd->flags; flag; flag = flag->next) {
        if (strcmp(flag->name, name) == 0) {
            flag->alias = alias;
            return;
        }
    }
    return;
}

static command_t *cli_find_command(command_t *cmd, const char *name) {
    for (command_t *child = cmd->children; child; child = child->next) {
        if (strcmp(child->name, name) == 0)
            return child;

        if (child->alias && strcmp(child->alias, name) == 0)
            return child;
    }

    return NULL;
}

static bool cli_parse_int(const char *s, int *out) {
    char *end = NULL;
    long value;
    value = strtol(s, &end, 0);
    if (end == s || *end != 0)
        return false;

    *out = (int)value;
    return true;
}

bool cli_get_bool(command_t *cmd, const char *name) {
    cli_flag_t *flag = cli_find_flag_value(cmd, name);

    assert(flag);
    assert(flag->kind == CLI_BOOL);

    return *(bool *)flag->value;
}

int cli_get_int(command_t *cmd, const char *name) {
    cli_flag_t *flag = cli_find_flag_value(cmd, name);

    assert(flag);
    assert(flag->kind == CLI_INT);

    return *(int *)flag->value;
}

char *cli_get_string(command_t *cmd, const char *name) {
    cli_flag_t *flag = cli_find_flag_value(cmd, name);

    assert(flag);
    assert(flag->kind == CLI_STRING);

    return *(char **)flag->value;
}

strings_t *cli_get_strings(command_t *cmd, const char *name) {
    cli_flag_t *flag = cli_find_flag_value(cmd, name);

    assert(flag);
    assert(flag->kind == CLI_STRING_LIST);

    return (strings_t *)flag->value;
}

char *cli_get_positional(command_t *cmd, const char *name) {
    cli_flag_t *flag = cli_find_flag_value(cmd, name);

    assert(flag);
    assert(flag->positional);

    return *(char **)flag->value;
}

static void cli_run_callback(command_t *cmd) {
    if (cmd->callback)
        cmd->callback(cmd);
}

static cli_status_t cli_set_flag(cli_t *cli, command_t *cmd, cli_flag_t *flag, const char *value,
                                 int *index, int argc, char **argv) {
    switch (flag->kind) {
        case CLI_BOOL:
            if (!value)
                *(bool *)flag->value = 1;

            else {
                if (strcmp(value, "true") == 0 || strcmp(value, "1") == 0)
                    *(bool *)flag->value = 1;
                else if (strcmp(value, "false") == 0 || strcmp(value, "0") == 0)
                    *(bool *)flag->value = 0;
                else {
                    fprintf(stderr, "%s: invalid boolean value '%s'\n", cli->program, value);
                    return CLI_ERROR;
                }
            }
            break;

        case CLI_INT:
            if (!value) {
                if (*index + 1 >= argc) {
                    fprintf(stderr, "%s: missing value for --%s\n", cli->program, flag->name);
                    return CLI_ERROR;
                }
                value = argv[++*index];
            }
            if (!cli_parse_int(value, (int *)flag->value)) {
                fprintf(stderr, "%s: invalid integer '%s'\n", cli->program, value);
                return CLI_ERROR;
            }
            break;

        case CLI_STRING:
            if (!value) {
                if (*index + 1 >= argc) {
                    fprintf(stderr, "%s: missing value for --%s\n", cli->program, flag->name);
                    return CLI_ERROR;
                }
                value = argv[++*index];
            }
            *(char **)flag->value = (char *)value;
            break;

        case CLI_STRING_LIST: {
            strings_t *list = flag->value;

            while (*index + 1 < argc) {
                char *next = argv[*index + 1];
                if (next[0] == '-') {
                    if (cli_find_flag(cmd, next + 2))
                        break;
                    if (next[1] && cli_find_flag_alias(cmd, next[1]))
                        break;
                }
                da_append(&cli->arena, list, next);
                (*index)++;
            }

            if (list->count == 0) {
                fprintf(stderr, "%s: missing value for --%s\n", cli->program, flag->name);
                return CLI_ERROR;
            }

            break;
        }

        case CLI_REST:
            break;
    }

    flag->seen = 1;
    return CLI_OK;
}

static cli_status_t cli_parse_commands(command_t *cmd, int argc, char **argv, int *cursor) {
    while (*cursor < argc) {
        char *arg = argv[*cursor];
        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0 || strcmp(arg, "help") == 0) {
            cli_command_usage(cmd);
            return CLI_HELP;
        }

        command_t *child = cli_find_command(cmd, arg);
        if (child) {
            cmd = child;
            cmd->cli->active = cmd;
            (*cursor)++;
            continue;
        }

        if (arg[0] == '-' && arg[1] == '-') {
            cli_flag_t *flag = cli_find_flag(cmd, arg + 2);
            if (!flag) {
                fprintf(stderr, "%s: unknown option '%s'\n", cmd->cli->program, arg);
                return CLI_ERROR;
            }

            cli_status_t status = cli_set_flag(cmd->cli, cmd, flag, NULL, cursor, argc, argv);
            if (status != CLI_OK)
                return status;

            (*cursor)++;
            continue;
        }
        if (arg[0] == '-' && arg[1] && !arg[2]) {
            cli_flag_t *flag = cli_find_flag_alias(cmd, arg[1]);
            if (!flag) {
                fprintf(stderr, "%s: unknown option '%s'\n", cmd->cli->program, arg);
                return CLI_ERROR;
            }

            cli_status_t status = cli_set_flag(cmd->cli, cmd, flag, NULL, cursor, argc, argv);
            if (status != CLI_OK)
                return status;

            (*cursor)++;
            continue;
        }

        cli_flag_t *pos = cli_goto_next_position(cmd);
        if (pos) {
            *(char **)pos->value = arg;
            pos->seen = 1;
            (*cursor)++;
            continue;
        }

        cli_flag_t *rest = cli_find_rest(cmd);
        if (rest && strcmp(arg, "-") == 0) {
            strings_t *list = rest->value;
            (*cursor)++; // consume the '-' sentinel itself, not part of the list
            while (*cursor < argc) {
                da_append(&cmd->cli->arena, list, argv[*cursor]);
                (*cursor)++;
            }
            rest->seen = 1;
            continue;
        }

        fprintf(stderr, "%s: unexpected argument '%s'\n", cmd->cli->program, arg);
        return CLI_ERROR;
    }

    return cli_check_required(cmd);
}

static void cli_print_flag(cli_flag_t *flag) {
    bool grey = !flag->required;

    printf("  ");
    if (grey)
        printf(ANSI_GREY);

    if (flag->alias)
        printf("-%c,  ", flag->alias);

    printf("--%s", flag->name);
    switch (flag->kind) {
        case CLI_INT:
            printf(" <int>");
            break;
        case CLI_STRING:
            printf(" <string>");
            break;
        case CLI_STRING_LIST:
            printf(" <strings...>");
            break;
        default:
            break;
    }
    if (flag->description)
        printf("\n             %s", flag->description);

    if (grey)
        printf(ANSI_RESET);
    putchar('\n');
}

static void cli_print_command_path(command_t *cmd) {
    if (!cmd)
        return;

    cli_print_command_path(cmd->parent);

    if (cmd->name[0])
        printf(" %s", cmd->name);
}

static void cli_print_command(command_t *cmd) {
    printf("Usage: %s", cmd->cli->program);

    cli_print_command_path(cmd);
    printf(" [options]");

    for (command_t *current = cmd; current; current = current->parent) {
        for (cli_flag_t *flag = current->flags; flag; flag = flag->next) {
            if (!flag->positional)
                continue;

            if (flag->required)
                printf("  <%s>", flag->name);
            else
                printf(ANSI_GREY "  [%s]" ANSI_RESET, flag->name);
        }
    }

    putchar('\n');
    if (cmd->description)
        printf("\n%s\n", cmd->description);

    bool has_flags = false;
    for (command_t *current = cmd; current; current = current->parent) {
        for (cli_flag_t *flag = current->flags; flag; flag = flag->next) {
            if (!flag->positional) {
                has_flags = true;
                break;
            }
        }
        if (has_flags)
            break;
    }

    if (has_flags) {
        puts("\noptions:");
        for (command_t *current = cmd; current; current = current->parent) {
            for (cli_flag_t *flag = current->flags; flag; flag = flag->next) {
                if (flag->positional)
                    continue;
                cli_print_flag(flag);
            }
        }
    }

    if (cmd->children) {
        puts("\ncommands:");
        for (command_t *c = cmd->children; c; c = c->next)
            printf("  " ANSI_BOLD "%-16s" ANSI_RESET " %s\n", c->name,
                   c->description ? c->description : "");
    }
}

void cli_usage(cli_t *cli) {
    cli_print_command(&cli->root);
}

void cli_command_usage(command_t *cmd) {
    cli_print_command(cmd);
}

command_t *cli_active_command(cli_t *cli) {
    return cli->active;
}

void cli_destroy(cli_t *cli) {
    if (!cli)
        return;

    arena_free(&cli->arena);
    free(cli);
}

cli_status_t cli_parse(cli_t *cli, int argc, char **argv) {
    assert(cli != NULL);

    cli->program = argv[0];
    cli->active = &cli->root;

    int cursor = 1;
    cli_status_t status = cli_parse_commands(&cli->root, argc, argv, &cursor);
    if (status != CLI_OK)
        return status;

    if (cli->active)
        cli_run_callback(cli->active);

    return CLI_OK;
}
