#ifndef ST_PLATFORM_H
#define ST_PLATFORM_H

#include "../st_helper.h"

typedef struct {
    const char **gl_pathv;
    u32 gl_pathc;
} ST_glob_t;

b8 ST_glob(const char *path, ST_glob_t *glob);
void ST_glob_free(ST_glob_t *glob);

f64 ST_get_current_time();
b8 ST_access_file(const char *path);

void *ST_load_dynlib(const char *dll_path);
void *ST_load_dynlib_fn(void *dyn_handle, const char *dll_fn_name);
void *ST_close_dll(void *handle);

b8 ST_path_is_absolute(const char *path);
char ST_path_separator(void);

#endif // ST_PLATFORM_H
