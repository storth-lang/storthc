#ifndef ST_DIAGNOSTIC_H
#define ST_DIAGNOSTIC_H

#include "../utils/st_helper.h"
#include "../utils/st_string.h"

typedef struct
{
    ST_string_t src;
    ST_string_t file;
    u32 n_errors;
    u32 max_errors;
} ST_diag_t;

void ST_diag_error(ST_diag_t *d, u32 line, u32 col, const char *fmt, ...);
void ST_diag_note(ST_diag_t *d, u32 line, u32 col, const char *fmt, ...);

#endif
