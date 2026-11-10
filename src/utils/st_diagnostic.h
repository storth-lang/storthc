#ifndef ST_DIAGNOSTIC_H
#define ST_DIAGNOSTIC_H

#include "../utils/st_helper.h"
#include "../utils/st_string.h"

typedef struct {
    ST_string_t file;
    u32 line, col;
} ST_diag_loc_t;

#define ST_DIAG_SEEN_MAX 512
#define ST_DIAG_REC_MAX 32

typedef struct {
    ST_string_t src;
    ST_string_t file;
    u32 n_errors;
    u32 max_errors;
    u64 seen[ST_DIAG_SEEN_MAX];
    u32 n_seen;
    b8 skip_notes;
    ST_diag_loc_t rec[ST_DIAG_REC_MAX];
    u32 n_rec;
    b8 recording;
    const ST_diag_loc_t *supp;
    u32 n_supp;
} ST_diag_t;

void ST_diag_error(ST_diag_t *d, u32 line, u32 col, const char *fmt, ...);
void ST_diag_note(ST_diag_t *d, u32 line, u32 col, const char *fmt, ...);

#endif
