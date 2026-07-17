#ifndef ST_FLAG_H
#define ST_FLAG_H

#include "st_helper.h"
#include "st_arena.h"

typedef struct ST_flag_parser_t ST_flag_parser_t;

typedef enum
{
    ST_FLAG_BOOL,
    ST_FLAG_INT,
    ST_FLAG_STRING,
} ST_flag_kind_t;

ST_flag_parser_t *ST_flag_init(ST_arena_t *arena);

void ST_flag_bool(ST_flag_parser_t *fp, const char *name, const char *description, b8 *value);
void ST_flag_int(ST_flag_parser_t *fp, const char *name, const char *description, i32 *value);
void ST_flag_string(ST_flag_parser_t *fp, const char *name, const char *description, char **value);

void ST_flag_positional(ST_flag_parser_t *fp, const char *name, const char *description, char **value, b8 required);
void ST_flag_alias(ST_flag_parser_t *fp, const char *long_name, char short_name);

b8 ST_flag_parse(ST_flag_parser_t *fp, int argc, char **argv);
void ST_flag_usage(ST_flag_parser_t *fp);
void ST_flag_dump(ST_flag_parser_t *fp);

#endif // ST_FLAG_H
