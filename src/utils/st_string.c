#define _XOPEN_SOURCE 700

#include <string.h>
#include "st_string.h"

ST_string_t ST_cstr_to_str(char *s)
{
    return (ST_string_t) {
        .data = (u8*)s,
        .len = strlen(s),
    };
}

void ST_chop_left_n(ST_string_t *sv, u32 n)
{
    if (sv->len < n) n = sv->len;
    sv->data += n;
    sv->len  -= n;
}

void ST_chop_right_n(ST_string_t *sv, u32 n)
{
    if (sv->len < n) n = sv->len;
    sv->len  -= n;
}

ST_string_t ST_trim_by_delim(ST_string_t *sv, char delim)
{
    u32 i = 0;
    while(sv->len > i && sv->data[i] != delim) i++;

    ST_string_t new_sv = {
        .data = sv->data,
        .len =  i,
    };

    ST_chop_left_n(sv, i);
    return  new_sv;
}

b8 ST_read_entire_file(ST_arena_t *arena, ST_string_t *sv, const char *path)
{
    ST_assert(path != NULL);
    FILE *f = fopen(path, "rb");
    if (!f) return 0;

    fseek(f, 0, SEEK_END);
    u32 file_size = (u32)ftell(f);
    fseek(f, 0, SEEK_SET);

    sv->data = ST_arena_push_zeroed(arena, (u64)file_size ? file_size : 1);

    if (file_size && fread((char *)sv->data, 1, file_size, f) != file_size)
    {
        fclose(f);
        return 0;
    }
    sv->len = file_size;
    fclose(f);
    return 1;
}

b8 ST_string_eq(ST_string_t s1, ST_string_t s2)
{
    if (s1.len != s2.len) return 0;
    if (s1.len == 0) return 1;
    return (memcmp(s1.data, s2.data, s1.len) == 0);
}

void ST_append_to_builder(ST_sb_t *sb, const char *item)
{
    while(*item) ST_da_append(sb, (u8)*item++);
}

const char *ST_sb_cstr(ST_sb_t *sb)
{
    ST_da_append(sb, (u8)0);
    sb->count -= 1;
    return (const char *)sb->items;
}

b8 ST_string_eq_cstr(ST_string_t a, const char *b)
{
    return ST_string_eq(a, ST_cstr_to_str((char *)b));
}

ST_string_t ST_abs_path(ST_arena_t *arena, const char *path)
{
    char buf[4096];
    const char *resolved = realpath(path, buf) ? buf : path;
    u32 len = (u32)strlen(resolved);
    ST_string_t sv = {
        .data = ST_arena_push(arena, len),
        .len = len,
    };
    memcpy(sv.data, resolved, len);
    return sv;
}
