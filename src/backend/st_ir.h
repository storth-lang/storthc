#ifndef ST_IR_H
#define ST_IR_H

#include "../st_types.h"
#include "../st_string.h"
#include "../st_lexer.h"

typedef struct
{
    ST_string_t name;
    ST_type_t type;
    u32 offset;
} ST_struct_field_t;

typedef struct
{
    ST_struct_field_t *items;
    u32 count;
    u32 capacity;
} ST_struct_fields_t;

typedef struct
{
    ST_generic_t items;
    ST_string_t name;
} ST_variant_t;

typedef struct
{
    ST_variant_t *items;
    u32 count;
    u32 capacity;
} ST_variants_t;

typedef struct
{
    ST_string_t name;
    ST_struct_fields_t fields;
    u32 size;
    b32 is_pub;
    b32 is_flag;
    b32 is_tag_union;
    ST_variants_t variant;
} ST_struct_decl_t;

#endif
