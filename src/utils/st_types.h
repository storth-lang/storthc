#ifndef ST_PRIMITIVE_H
#define ST_PRIMITIVE_H


#define ST_KEYWORD_LIST                    \
    ST_KEYWORD(ST_kfn, "fn")               \
    ST_KEYWORD(ST_kstruct, "struct")       \
    ST_KEYWORD(ST_kenum, "enum")           \
    ST_KEYWORD(ST_kextern, "extern")       \
    ST_KEYWORD(ST_kpub, "pub")             \
    ST_KEYWORD(ST_knull, "null")           \
    ST_KEYWORD(ST_kif, "if")               \
    ST_KEYWORD(ST_kelse, "else")           \
    ST_KEYWORD(ST_kwhile, "while")         \
    ST_KEYWORD(ST_kfor, "for")             \
    ST_KEYWORD(ST_kcast, "cast")           \
    ST_KEYWORD(ST_ktype_of, "type_of")     \
    ST_KEYWORD(ST_ktype_info, "type_info") \
    ST_KEYWORD(ST_ksizeof, "sizeof")       \
    ST_KEYWORD(ST_kand, "and")             \
    ST_KEYWORD(ST_kor, "or")               \
    ST_KEYWORD(ST_knot, "not")             \
    ST_KEYWORD(ST_kdefault, "default")     \
    ST_KEYWORD(ST_kenum_flag, "enum_flag") \
    ST_KEYWORD(ST_ktag, "tag_union")       \
    ST_KEYWORD(ST_kcase, "case")           \
    ST_KEYWORD(ST_kreturn, "return")       \
    ST_KEYWORD(ST_kusing, "using")         \
    ST_KEYWORD(ST_kbreak, "break")         \
    ST_KEYWORD(ST_kcontinue, "continue")   \
    ST_KEYWORD(ST_kdefer, "defer")         \
    ST_KEYWORD(ST_ktrue, "true")           \
    ST_KEYWORD(ST_kfalse, "false")         \
    ST_KEYWORD(ST_kconst, "const")         \
    ST_KEYWORD(ST_kstatic, "static")       \
    ST_KEYWORD(ST_klabel, "label")         \
    ST_KEYWORD(ST_kgodown, "godown")       \

typedef enum {
#define ST_KEYWORD(e, s) e,
    ST_KEYWORD_LIST
#undef ST_KEYWORD
    ST_KEYWORD_COUNT,
} ST_keyword_t;

#define ST_TYPE_LIST          \
    ST_TYPE(ST_ti8, "i8")     \
    ST_TYPE(ST_ti16, "i16")   \
    ST_TYPE(ST_ti32, "i32")   \
    ST_TYPE(ST_ti64, "i64")   \
    ST_TYPE(ST_tu8, "u8")     \
    ST_TYPE(ST_tu16, "u16")   \
    ST_TYPE(ST_tu32, "u32")   \
    ST_TYPE(ST_tu64, "u64")   \
    ST_TYPE(ST_tf32, "f32")   \
    ST_TYPE(ST_tf64, "f64")   \
    ST_TYPE(ST_tf128, "f128") \
    ST_TYPE(ST_tchar, "char") \
    ST_TYPE(ST_tvoid, "void") \
    ST_TYPE(ST_tany, "any")   \
    ST_TYPE(ST_tbool, "bool") \
    ST_TYPE(ST_tstring, "string") \

typedef enum {
#define ST_TYPE(e, s) e,
    ST_TYPE_LIST
#undef ST_TYPE
    ST_TYPE_COUNT,
} ST_type_t;

extern const char *ST_type_names[ST_TYPE_COUNT];
extern const char *ST_keyword_names[ST_KEYWORD_COUNT];

#endif
