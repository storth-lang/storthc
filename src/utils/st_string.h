#ifndef ST_STRING_H
#define ST_STRING_H

#include "st_helper.h"
#include "st_arena.h"

#define ST_sv_fmt "%.*s"
#define ST_sv_args(sv) (int)(sv).len, (char *)(sv).data

typedef struct
{
    u8 *data;
    u32 len;
} ST_string_t;

typedef struct
{
    u8 *items;
    u32 count, capacity;
} ST_sb_t;

ST_string_t ST_cstr_to_str(char *s);
char *ST_cstr_from_str(ST_string_t *sv);

void ST_chop_left_n(ST_string_t *sv, u32 n);
void ST_chop_right_n(ST_string_t *sv, u32 n);

ST_string_t ST_trim_by_delim(ST_string_t *sv, char delim);
b8 ST_string_eq(ST_string_t s1, ST_string_t s2);
b8 ST_string_eq_cstr(ST_string_t a, const char *b);

b8 ST_read_entire_file(ST_arena_t *arena, ST_string_t *sv, const char *path);
ST_string_t ST_abs_path(ST_arena_t *arena, const char *path);
void ST_append_to_builder(ST_sb_t *sb, const char *item);
const char *ST_sb_cstr(ST_sb_t *sb);

#endif
