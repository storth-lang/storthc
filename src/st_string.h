#ifndef ST_STRING_H
#define ST_STRING_H

#include "st_types.h"

typedef struct
{
    u8 *data;
    u32 len;
} ST_string_t;

ST_string_t ST_cstr_to_str(char *s);
char *ST_cstr_from_str(ST_string_t *sv);

void ST_chop_left_n(ST_string_t *sv, u32 n);
void ST_chop_right_n(ST_string_t *sv, u32 n);

ST_string_t ST_trim_by_delim(ST_string_t *sv, char delim);

b32 ST_read_entire_file(ST_string_t *sv, const char *path);
void ST_free_string(ST_string_t *sv);

#define ST_sv_fmt "%.*s"
#define ST_sv_args(sv) (int)(sv).len, (char *)(sv).data

#endif
