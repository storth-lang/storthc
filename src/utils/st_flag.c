#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "st_flag.h"

typedef struct
{
    const char *name;
    const char *desc;
    void *value;
    ST_flag_kind_t kind;
    char alias;
    b8 pos;
    b8 required;
    b8 seen;
} ST_flag_t;

struct ST_flag_parser_t
{
    ST_flag_t *items;
    u32 count, capacity;
    ST_arena_t *arena;
    const char *prog;
};

ST_flag_parser_t *ST_flag_init(ST_arena_t *arena)
{
    ST_flag_parser_t *fp = ST_arena_push_zeroed(arena, sizeof(*fp));
    fp->arena = arena;
    return fp;
}

static ST_flag_t *ST_flag_push(ST_flag_parser_t *fp, ST_flag_kind_t kind,
                               const char *name, const char *desc,
                               void *value, b8 pos, b8 required)
{
    ST_assert(fp != NULL);
    ST_assert(name != NULL);
    ST_assert(value != NULL);

    ST_flag_t flag = {0};
    flag.name = name;
    flag.desc = desc;
    flag.value = value;
    flag.kind = kind;
    flag.pos = pos;
    flag.required = required;

    ST_da_append_arena(fp->arena, fp, flag);
    return &fp->items[fp->count - 1];
}

void ST_flag_bool(ST_flag_parser_t *fp, const char *name, const char *description, b8 *value)
{
    ST_flag_push(fp, ST_FLAG_BOOL, name, description, value, 0, 0);
}

void ST_flag_int(ST_flag_parser_t *fp, const char *name, const char *description, i32 *value)
{
    ST_flag_push(fp, ST_FLAG_INT, name, description, value, 0, 0);
}

void ST_flag_string(ST_flag_parser_t *fp, const char *name, const char *description, char **value)
{
    ST_flag_push(fp, ST_FLAG_STRING, name, description, value, 0, 0);
}

void ST_flag_positional(ST_flag_parser_t *fp, const char *name, const char *description, char **value, b8 required)
{
    ST_flag_push(fp, ST_FLAG_STRING, name, description, value, 1, required);
}

void ST_flag_alias(ST_flag_parser_t *fp, const char *long_name, char short_name)
{
    ST_forrange(0, fp->count)
    {
        ST_flag_t *flag = &fp->items[i];
        if (!flag->pos && strcmp(flag->name, long_name) == 0)
        {
            flag->alias = short_name;
            return;
        }
    }
    ST_assert(0 && "ST_flag_alias: no such flag");
}

static ST_flag_t *ST_flag_find_long(ST_flag_parser_t *fp, const char *name, u64 len)
{
    ST_forrange(0, fp->count)
    {
        ST_flag_t *flag = &fp->items[i];
        if (!flag->pos && strlen(flag->name) == len
            && strncmp(flag->name, name, len) == 0)
            return flag;
    }
    return NULL;
}

static ST_flag_t *ST_flag_find_short(ST_flag_parser_t *fp, char alias)
{
    ST_forrange(0, fp->count)
    {
        ST_flag_t *flag = &fp->items[i];
        if (!flag->pos && flag->alias == alias) return flag;
    }
    return NULL;
}

static ST_flag_t *ST_flag_next_positional(ST_flag_parser_t *fp)
{
    ST_forrange(0, fp->count)
    {
        ST_flag_t *flag = &fp->items[i];
        if (flag->pos && !flag->seen) return flag;
    }
    return NULL;
}

static b8 ST_flag_parse_int(const char *s, i32 *out)
{
    char *end = NULL;
    long v = strtol(s, &end, 0);
    if (end == s || *end != '\0') return 0;
    *out = (i32)v;
    return 1;
}

static b8 ST_flag_parse_bool(const char *s, b8 *out)
{
    if (strcmp(s, "1") == 0 || strcmp(s, "true") == 0)  { *out = 1; return 1; }
    if (strcmp(s, "0") == 0 || strcmp(s, "false") == 0) { *out = 0; return 1; }
    return 0;
}

static b8 ST_flag_set(ST_flag_parser_t *fp, ST_flag_t *flag,
                      const char *value, int *i, int argc, char **argv)
{
    if (flag->kind == ST_FLAG_BOOL)
    {
        if (!value) *(b8 *)flag->value = 1;
        else if (!ST_flag_parse_bool(value, (b8 *)flag->value))
        {
            fprintf(stderr, "%s: invalid value '%s' for --%s (expected 0/1/true/false)\n",
                    fp->prog, value, flag->name);
            return 0;
        }
        flag->seen = 1;
        return 1;
    }

    if (!value)
    {
        if (*i + 1 >= argc)
        {
            fprintf(stderr, "%s: missing value for --%s\n", fp->prog, flag->name);
            return 0;
        }
        value = argv[++*i];
    }

    switch (flag->kind)
    {
    case ST_FLAG_INT:
        if (!ST_flag_parse_int(value, (i32 *)flag->value))
        {
            fprintf(stderr, "%s: invalid integer '%s' for --%s\n",
                    fp->prog, value, flag->name);
            return 0;
        }
        break;
    case ST_FLAG_STRING:
        *(char **)flag->value = (char *)value;
        break;
    case ST_FLAG_BOOL:
        break;
    }

    flag->seen = 1;
    return 1;
}

b8 ST_flag_parse(ST_flag_parser_t *fp, int argc, char **argv)
{
    ST_assert(fp != NULL);
    fp->prog = argv[0];

    b8 only_positional = 0;
    for (int i = 1; i < argc; i++)
    {
        char *arg = argv[i];

        if (!only_positional && strcmp(arg, "--") == 0)
        {
            only_positional = 1;
        }
        else if (!only_positional && arg[0] == '-' && arg[1] == '-' && arg[2])
        {
            char *name = arg + 2;
            char *eq = strchr(name, '=');
            u64 len = eq ? (u64)(eq - name) : strlen(name);

            ST_flag_t *flag = ST_flag_find_long(fp, name, len);
            if (!flag)
            {
                fprintf(stderr, "%s: unknown flag '--%.*s'\n", fp->prog, (int)len, name);
                return 0;
            }
            if (!ST_flag_set(fp, flag, eq ? eq + 1 : NULL, &i, argc, argv)) return 0;
        }
        else if (!only_positional && arg[0] == '-' && arg[1] && !arg[2])
        {
            ST_flag_t *flag = ST_flag_find_short(fp, arg[1]);
            if (!flag)
            {
                fprintf(stderr, "%s: unknown flag '-%c'\n", fp->prog, arg[1]);
                return 0;
            }
            if (!ST_flag_set(fp, flag, NULL, &i, argc, argv)) return 0;
        }
        else
        {
            ST_flag_t *flag = ST_flag_next_positional(fp);
            if (!flag)
            {
                fprintf(stderr, "%s: unexpected argument '%s'\n", fp->prog, arg);
                return 0;
            }
            *(char **)flag->value = arg;
            flag->seen = 1;
        }
    }

    ST_forrange(0, fp->count)
    {
        ST_flag_t *flag = &fp->items[i];
        if (flag->pos && flag->required && !flag->seen)
        {
            fprintf(stderr, "%s: missing required argument <%s>\n",
                    fp->prog ? fp->prog : "?", flag->name);
            return 0;
        }
    }
    return 1;
}

static const char *ST_flag_value_hint(ST_flag_kind_t kind)
{
    switch (kind)
    {
    case ST_FLAG_BOOL:   return "";
    case ST_FLAG_INT:    return " <int>";
    case ST_FLAG_STRING: return " <string>";
    }
    return "";
}

void ST_flag_usage(ST_flag_parser_t *fp)
{
    fprintf(stderr, "usage: %s [options]", fp->prog ? fp->prog : "prog");
    ST_forrange(0, fp->count)
    {
        ST_flag_t *flag = &fp->items[i];
        if (!flag->pos) continue;
        if (flag->required) fprintf(stderr, " <%s>", flag->name);
        else                fprintf(stderr, " [%s]", flag->name);
    }
    fprintf(stderr, "\n" ST_COLOR_BOLD "options:" ST_COLOR_RESET "\n");

    ST_forrange(0, fp->count)
    {
        ST_flag_t *flag = &fp->items[i];
        if (flag->pos) continue;

        char left[64];
        if (flag->alias)
            snprintf(left, sizeof(left), "-%c, --%s%s",
                     flag->alias, flag->name, ST_flag_value_hint(flag->kind));
        else
            snprintf(left, sizeof(left), "    --%s%s",
                     flag->name, ST_flag_value_hint(flag->kind));

        fprintf(stderr, "  %-28s %s\n", left, flag->desc ? flag->desc : "");
    }
}

void ST_flag_dump(ST_flag_parser_t *fp)
{
    ST_forrange(0, fp->count)
    {
        ST_flag_t *flag = &fp->items[i];
        fprintf(stderr, "%-16s pos=%d seen=%d ", flag->name, flag->pos, flag->seen);
        switch (flag->kind)
        {
        case ST_FLAG_BOOL:
            fprintf(stderr, "bool=%d\n", *(b8 *)flag->value);
            break;
        case ST_FLAG_INT:
            fprintf(stderr, "int=%d\n", *(i32 *)flag->value);
            break;
        case ST_FLAG_STRING:
            fprintf(stderr, "string=%s\n",
                    *(char **)flag->value ? *(char **)flag->value : "(null)");
            break;
        }
    }
}
