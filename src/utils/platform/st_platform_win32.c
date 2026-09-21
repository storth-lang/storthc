#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <io.h>
#include <stdlib.h>
#include <string.h>

#include "st_platform.h"

b8 ST_glob(const char *path, ST_glob_t *glob) {
    glob->gl_pathv = NULL;
    glob->gl_pathc = 0;

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(path, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return 0;

    const char *s1 = strrchr(path, '/');
    const char *s2 = strrchr(path, '\\');
    const char *last = s1 > s2 ? s1 : s2;
    size_t dir_len = last ? (size_t)(last - path + 1) : 0;

    u32 cap = 0;
    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0)
            continue;
        if (glob->gl_pathc >= cap) {
            cap = cap ? cap * 2 : 16;
            glob->gl_pathv = realloc((void *)glob->gl_pathv, cap * sizeof(*glob->gl_pathv));
        }
        size_t name_len = strlen(fd.cFileName);
        char *full = malloc(dir_len + name_len + 1);
        memcpy(full, path, dir_len);
        memcpy(full + dir_len, fd.cFileName, name_len + 1);
        glob->gl_pathv[glob->gl_pathc++] = full;
    } while (FindNextFileA(h, &fd));

    FindClose(h);
    return 1;
}

void ST_glob_free(ST_glob_t *glob) {
    for (u32 i = 0; i < glob->gl_pathc; i++)
        free((void *)glob->gl_pathv[i]);
    free((void *)glob->gl_pathv);
    glob->gl_pathv = NULL;
    glob->gl_pathc = 0;
}

f64 ST_get_current_time(void) {
    static LARGE_INTEGER freq;
    LARGE_INTEGER now;
    if (freq.QuadPart == 0)
        QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    return (f64)now.QuadPart / (f64)freq.QuadPart;
}

b8 ST_access_file(const char *path) {
    return (b8)_access(path, 0);
}

void *ST_load_dynlib(const char *dll_path) {
    return (void *)LoadLibraryA(dll_path);
}

void *ST_load_dynlib_fn(void *dyn_handle, const char *dll_fn_name) {
    return (void *)(uintptr_t)GetProcAddress((HMODULE)dyn_handle, dll_fn_name);
}

void *ST_close_dll(void *handle) {
    if (handle)
        FreeLibrary((HMODULE)handle);
    return NULL;
}
