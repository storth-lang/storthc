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
    if (sv->len < n) sv->len = n;
    sv->data += n;
    sv->len  -= n;
}

void ST_chop_right_n(ST_string_t *sv, u32 n)
{
    if (sv->len < n) sv->len = n;
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

b32 ST_read_entire_file(ST_string_t *sv, const char *path)
{
    ST_assert(path != NULL);
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    u32 file_size = (u32)ftell(f);
    fseek(f, 0, SEEK_SET);

    sv->data = malloc(sizeof(*sv->data) * file_size);
    ST_assert(sv->data != NULL);    

    if (fread((char *)sv->data, 1, file_size, f) <= 0) return -1;
    sv->len = file_size;
    return 0;
}

void ST_free_string(ST_string_t *sv)
{
    if (sv->data)
    {
        free(sv->data);
        sv->len = 0;
    }
}
