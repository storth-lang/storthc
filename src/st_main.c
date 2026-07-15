// cc -I../src -o 
#include "frontend/st_ast.h"
#include "frontend/st_types.h"
#include "frontend/st_semantic.h"
#include "middle/st_lower.h"
#include "utils/st_types.h"

static ST_expr_t *mk_int(ST_arena_t *a, ST_ty_ctx_t *tys, i64 v)
{
    ST_expr_t *e = ST_expr_new(a, ST_EX_INT, 0, 0);
    e->ival = v;
    e->ty = ST_ty_prim(tys, ST_ti32);
    return e;
}

static ST_expr_t *mk_ident(ST_arena_t *a, ST_string_t name, ST_ty_t *ty)
{
    ST_expr_t *e = ST_expr_new(a, ST_EX_IDENT, 0, 0);
    e->name = name;
    e->ty = ty;
    return e;
}

int main(void)
{
    ST_arena_t *arena = ST_arena_alloc();
    ST_ty_ctx_t tys;
    ST_ty_ctx_init(&tys, arena);
    ST_ty_t *i32 = ST_ty_prim(&tys, ST_ti32);

    // fn add(a: i32, b: i32) -> i32 { x := a + b; return x }
    ST_decl_t *add = ST_decl_new(arena, ST_DE_FN, 1, 1);
    add->name = ST_cstr_to_str("add");

    ST_tyexpr_t *te_i32_a = ST_tyexpr_new(arena, ST_TE_NAME, 1, 1);
    te_i32_a->name = ST_cstr_to_str("i32");
    ST_tyexpr_t *te_i32_b = ST_tyexpr_new(arena, ST_TE_NAME, 1, 1);
    te_i32_b->name = ST_cstr_to_str("i32");
    ST_tyexpr_t *te_i32_ret = ST_tyexpr_new(arena, ST_TE_NAME, 1, 1);
    te_i32_ret->name = ST_cstr_to_str("i32");

    ST_param_t pa = { .name = ST_cstr_to_str("a"), .te = te_i32_a };
    ST_param_t pb = { .name = ST_cstr_to_str("b"), .te = te_i32_b };
    ST_da_append_arena(arena, &add->fn.sig.params, pa);
    ST_da_append_arena(arena, &add->fn.sig.params, pb);
    ST_da_append_arena(arena, &add->fn.sig.rets, te_i32_ret);
    add->fn.sig.has_ret_ann = 1;

    ST_expr_t *a_id = mk_ident(arena, ST_cstr_to_str("a"), i32);
    ST_expr_t *b_id = mk_ident(arena, ST_cstr_to_str("b"), i32);
    ST_expr_t *a_plus_b = ST_expr_new(arena, ST_EX_BINARY, 2, 1);
    a_plus_b->bin.op = ST_cstr_to_str("+");
    a_plus_b->bin.l = a_id;
    a_plus_b->bin.r = b_id;
    a_plus_b->ty = i32;

    ST_stmt_t *x_decl = ST_stmt_new(arena, ST_ST_DECL, 2, 1);
    x_decl->decl.name = ST_cstr_to_str("x");
    x_decl->decl.init = a_plus_b;
    ST_da_append_arena(arena, &add->fn.body, x_decl);

    ST_expr_t *x_id_for_ret = mk_ident(arena, ST_cstr_to_str("x"), i32);
    ST_stmt_t *ret_x = ST_stmt_new(arena, ST_ST_RETURN, 3, 1);
    ST_da_append_arena(arena, &ret_x->ret.values, x_id_for_ret);
    ST_da_append_arena(arena, &add->fn.body, ret_x);

    // fn main() -> i32 { r := add(1, 2); return r }
    ST_decl_t *main_fn = ST_decl_new(arena, ST_DE_FN, 1, 5);
    main_fn->name = ST_cstr_to_str("main");
    ST_tyexpr_t *te_i32_main_ret = ST_tyexpr_new(arena, ST_TE_NAME, 5, 1);
    te_i32_main_ret->name = ST_cstr_to_str("i32");
    ST_da_append_arena(arena, &main_fn->fn.sig.rets, te_i32_main_ret);
    main_fn->fn.sig.has_ret_ann = 1;

    ST_expr_t *callee = mk_ident(arena, ST_cstr_to_str("add"), NULL);
    ST_expr_t *call = ST_expr_new(arena, ST_EX_CALL, 6, 1);
    call->call.callee = callee;
    ST_arg_t arg1 = { .value = mk_int(arena, &tys, 1) };
    ST_arg_t arg2 = { .value = mk_int(arena, &tys, 2) };
    ST_da_append_arena(arena, &call->call.args, arg1);
    ST_da_append_arena(arena, &call->call.args, arg2);
    call->ty = i32;

    ST_stmt_t *r_decl = ST_stmt_new(arena, ST_ST_DECL, 6, 1);
    r_decl->decl.name = ST_cstr_to_str("r");
    r_decl->decl.init = call;
    ST_da_append_arena(arena, &main_fn->fn.body, r_decl);

    ST_expr_t *r_id_for_ret = mk_ident(arena, ST_cstr_to_str("r"), i32);
    ST_stmt_t *ret_r = ST_stmt_new(arena, ST_ST_RETURN, 7, 1);
    ST_da_append_arena(arena, &ret_r->ret.values, r_id_for_ret);
    ST_da_append_arena(arena, &main_fn->fn.body, ret_r);

    ST_program_t prog = {0};
    prog.file = ST_cstr_to_str("test.st");
    ST_da_append_arena(arena, &prog.decls, add);
    ST_da_append_arena(arena, &prog.decls, main_fn);

    ST_sema_t sema = {0};
    sema.tys = tys;

    ST_ir_module_t mod = {0};
    ST_string_t src = ST_cstr_to_str("");
    b8 ok = ST_lower_program(arena, &prog, &sema, src, prog.file, &mod);

    ST_ir_dump_module(stdout, &mod);
    fprintf(stderr, "\nlower ok = %d\n", ok);

    ST_arena_free(arena);
    return ok ? 0 : 1;
}
