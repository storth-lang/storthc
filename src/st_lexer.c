#include "st_lexer.h"

ST_string_t ST_token_kind_to_string(ST_token_kind_t kind)
{
#if (ST_TCOUNT > 10)
    #error "ST_TCOUNT is exceeded"
#endif
    switch(kind)
    {
    case ST_TINT:       return ST_cstr_to_str("ST_TINT");
    case ST_TFLOAT:     return ST_cstr_to_str("ST_TFLOAT");
    case ST_TSTRING:    return ST_cstr_to_str("ST_TSTRING");
    case ST_TCHAR:      return ST_cstr_to_str("ST_TCHAR");
    case ST_TIDENT:     return ST_cstr_to_str("ST_TIDENT");
    case ST_TTYPE:      return ST_cstr_to_str("ST_TTYPE");
    case ST_TKEYWORD:   return ST_cstr_to_str("ST_TKEYWORD");
    case ST_TSYMBOL:    return ST_cstr_to_str("ST_TSYMBOL");
    case ST_TDOCCOMENT: return ST_cstr_to_str("ST_TDOCCOMENT");
    default: ST_assert(1);
    }

    return (ST_string_t) {
        .data = NULL,
        .len = 0,
    };
}

b32 ST_iswhitespace(ST_lexer_t *l)
{
    ST_unused(l);
    return 0;
}
