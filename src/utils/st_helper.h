#ifndef ST_HELPER_H
#define ST_HELPER_H

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>

#define ST_COLOR_BOLD      "\x1b[1m"
#define ST_COLOR_RED       "\x1b[31m"
#define ST_COLOR_BLUE      "\x1b[34m"
#define ST_COLOR_CYAN      "\x1b[36m"
#define ST_COLOR_YELLOW    "\x1b[33m"
#define ST_COLOR_RESET     "\x1b[0m"
#define ST_COLOR_DIM       "\x1b[2m"
#define ST_COLOR_WHITE     "\x1b[97m"
#define ST_COLOR_GREY      "\x1b[90m"
#define ST_COLOR_BOLD_RED  "\x1b[1m\x1b[31m"


typedef uint8_t u8;
typedef int8_t i8;
typedef uint16_t u16;
typedef int16_t i16;
typedef uint32_t u32;
typedef int32_t i32;
typedef uint64_t u64;
typedef int64_t i64;
typedef i8 b8;
typedef i64 b64;
typedef float f32;
typedef double f64;

#define ST_array_len(arr) (sizeof(arr) / sizeof(arr[0]))

#define ST_da_append(arr, item)                                         \
    do {                                                                \
        if ((arr)->count >= (arr)->capacity)                            \
        {                                                               \
            (arr)->capacity = ((arr)->capacity <= 0) ?                  \
                256 : (arr)->capacity * 2;                              \
            (arr)->items = realloc((arr)->items, (sizeof(*(arr)->items) \
                                                  * (arr)->capacity));  \
        }                                                               \
        (arr)->items[(arr)->count++] = item;                            \
    } while(0)

#define ST_foreach(it, arr) \
    for (u32 i = 0; i < (arr)->count; i++) \
        for (void *it = (void *)(arr)->items[i]; it; it = 0)

#define ST_forrange(m, n) \
    for (u32 i = (m); i < (n); i++)

#define ST_assert(expr)                                         \
    do {                                                        \
        if (!(expr)) {                                          \
            fprintf(stderr, "%s:%d:1: Assertion failed: %s\n",  \
                    __FILE__, __LINE__, #expr);                 \
            abort();                                            \
        }                                                       \
    } while (0)

#define ST_unused(item) (void)(item)

#define ST_todo(msg)                            \
    do {                                        \
        fprintf(stderr, "TODO: %s\n", msg);     \
        abort();                                \
} while(0)

#define ST_MIN(a, b) ((a) < (b)) ? (a) : (b)
#define ST_MAX(a, b) ((a) > (b)) ? (a) : (b)

#endif
