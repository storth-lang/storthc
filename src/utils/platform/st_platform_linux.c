#define _POSIX_C_SOURCE 200809L

#include <dlfcn.h>
#include <glob.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "st_platform.h"

b8 ST_glob(const char *path, ST_glob_t *st_glob) {
    st_glob->gl_pathv = NULL;
    st_glob->gl_pathc = 0;

    glob_t g;
    memset(&g, 0, sizeof(g));

    int ret = glob(path, 0, NULL, &g);
    if (ret != 0) {
        globfree(&g);
        return 0;
    }

    if (g.gl_pathc == 0) {
        globfree(&g);
        return 0;
    }

    st_glob->gl_pathv = malloc(g.gl_pathc * sizeof(*st_glob->gl_pathv));
    if (!st_glob->gl_pathv) {
        globfree(&g);
        return 0;
    }

    for (size_t i = 0; i < g.gl_pathc; i++) {
        st_glob->gl_pathv[i] = strdup(g.gl_pathv[i]);

        if (!st_glob->gl_pathv[i]) {
            for (size_t j = 0; j < i; j++)
                free((void *)st_glob->gl_pathv[j]);

            free((void *)st_glob->gl_pathv);
            st_glob->gl_pathv = NULL;
            st_glob->gl_pathc = 0;

            globfree(&g);
            return 0;
        }

        st_glob->gl_pathc++;
    }

    globfree(&g);
    return 1;
}

void ST_glob_free(ST_glob_t *st_glob) {
    for (u32 i = 0; i < st_glob->gl_pathc; i++)
        free((void *)st_glob->gl_pathv[i]);

    free((void *)st_glob->gl_pathv);

    st_glob->gl_pathv = NULL;
    st_glob->gl_pathc = 0;
}

f64 ST_get_current_time(void) {
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0.0;

    return (f64)ts.tv_sec + (f64)ts.tv_nsec / 1000000000.0;
}

b8 ST_access_file(const char *path) {
    return (b8)access(path, F_OK);
}

b8 ST_path_is_absolute(const char *path) {
    return path && path[0] == '/';
}

void *ST_load_dynlib(const char *dll_path) {
    return dlopen(dll_path, RTLD_LAZY);
}

void *ST_load_dynlib_fn(void *dyn_handle, const char *dll_fn_name) {
    return dlsym(dyn_handle, dll_fn_name);
}

void *ST_close_dll(void *handle) {
    if (handle)
        dlclose(handle);

    return NULL;
}