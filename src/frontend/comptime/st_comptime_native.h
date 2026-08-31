#ifndef ST_COMPTIME_NATIVE_H
#define ST_COMPTIME_NATIVE_H

#include "st_comptime_compile.h"

b8 ST_ct_ensure_native_module(ST_ct_prog_ctx_t *pctx, char *err_msg, u32 err_msg_cap);

#endif
