#ifndef ST_TYPES_H
#define ST_TYPES_H

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>

typedef uint8_t u8;
typedef int8_t i8;
typedef uint16_t u16;
typedef int16_t i16;
typedef uint32_t u32;
typedef int32_t i32;
typedef uint64_t u64;
typedef int64_t i64;
typedef i32 b32;
typedef i64 b64;
typedef float f32;
typedef double f64;

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
        for (void *it = (void *)(arr)->items[i]; ; it = 0)

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

#endif
