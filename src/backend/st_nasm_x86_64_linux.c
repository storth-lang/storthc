#include "st_nasm.h"
#include <stdio.h>

static const char *arg_regs[] = { "rdi", "rsi", "rdx", "rcx", "r8", "r9" };
#define ST_N_ARG_REGS ((u32)ST_array_len(arg_regs))

typedef struct
{
    ST_ir_fn_t *fn;
    u32 hidden_ret_off;
} ST_gen_ctx_t;

static i32 ST_slot(ST_ir_inst_t *v)
{
    while (v->repl) v = v->repl;
    return -8 * (i32)(v->id + 1);
}

static void ST_load(FILE *out, const char *reg, ST_ir_inst_t *v)
{
    fprintf(out, "    mov %s, [rbp%+d]\n", reg, ST_slot(v));
}

static u32 ST_call_ret_count(ST_ir_inst_t *call_inst)
{
    if (call_inst->call.callee && call_inst->call.callee->ty)
        return call_inst->call.callee->ty->rets.count;
    return 1;
}

static i32 ST_ret_buf_off(ST_ir_inst_t *call_inst, u32 index)
{
    return (i32)(8 * index) - (i32)call_inst->call.ret_buf_offset;
}

static void ST_mem_load(FILE *out, ST_ty_t *ty)
{
    u32 size = ty && ty->size ? ty->size : 8;
    b8 _signed = ty && ty->kind == ST_TY_INT && ty->is_signed;
    switch (size)
    {
    case 1: fprintf(out, "    %s rax, byte [rcx]\n", _signed ? "movsx" : "movzx"); break;
    case 2: fprintf(out, "    %s rax, word [rcx]\n", _signed ? "movsx" : "movzx"); break;
    case 4: {
        if (_signed) { fprintf(out, "    movsxd rax, dword [rcx]\n"); break; }
        else { fprintf(out, "    mov eax, dword [rcx]\n"); break; }
        break;
    }
    default: fprintf(out, "    mov rax, [rcx]\n"); break;
    }
}

static void ST_mem_store(FILE *out, ST_ty_t *ty)
{
    u32 size = ty && ty->size ? ty->size : 8;
    switch (size)
    {
    case 1: fprintf(out, "    mov [rcx], al\n"); break;
    case 2: fprintf(out, "    mov [rcx], ax\n"); break;
    case 4: fprintf(out, "    mov [rcx], eax\n"); break;
    default: fprintf(out, "    mov [rcx], rax\n"); break;
    }
}

static void ST_generate_strs(FILE *out, ST_ir_module_t *m)
{
    if (!m->strs.count) return;
    fprintf(out, "\nsection .rodata\n");
    ST_forrange(0, m->strs.count)
    {
        ST_string_t s = m->strs.items[i];
        fprintf(out, "str_%u_data: db ", (u32)i);
        for (u32 j = 0; j < s.len; j++)
        {
            fprintf(out, "0x%02x, ", (u8)s.data[j]);
        }
        fprintf(out, "0\n");
        fprintf(out, "align 8\n");
        fprintf(out, "str_%u:\n", (u32)i);
        fprintf(out, "    dq str_%u_data\n", (u32)i);
        fprintf(out, "    dq %u\n", (u32)s.len);
    }
}

static void ST_icmp(FILE *out, ST_ir_inst_t *in, const char *setcc)
{
    ST_load(out, "rax", in->bin.l);
    ST_load(out, "rcx", in->bin.r);
    fprintf(out, "    cmp rax, rcx\n");
    fprintf(out, "    %s al\n", setcc);
    fprintf(out, "    movzx rax, al\n");
}

static void ST_generate_inst(FILE *out, ST_gen_ctx_t *ctx, ST_ir_inst_t *in)
{
    _Static_assert(ST_IR_COUNT == 50, "IR count exceeded");
    if (in->removed) return;
    switch(in->kind)
    {
    case ST_IR_CONST_INT: {
        fprintf(out, "    mov rax, %ld\n", in->const_int);
    } break;
    case ST_IR_CONST_FLOAT: ST_todo("ST_IR_CONST_FLOAT"); break;
    case ST_IR_CONST_STRING: {
        fprintf(out, "    lea rax, [rel str_%u]\n", in->str_index);
    } break;
    case ST_IR_ADD: {
        ST_load(out, "rax", in->bin.l);
        ST_load(out, "rcx", in->bin.r);
        fprintf(out, "    add rax, rcx\n");
    } break;
    case ST_IR_SUB: {
        ST_load(out, "rax", in->bin.l);
        ST_load(out, "rcx", in->bin.r);
        fprintf(out, "    sub rax, rcx\n");
    } break;
    case ST_IR_MUL:  {
        ST_load(out, "rax", in->bin.l);
        ST_load(out, "rcx", in->bin.r);
        fprintf(out, "    imul rax, rcx\n");
    } break;
    case ST_IR_SDIV: ST_todo("ST_IR_SDIV"); break;
    case ST_IR_EXTRACT_OP: {
        ST_ir_inst_t *agg = in->extract.agg;
        u32 rc = (agg->kind == ST_IR_CALL) ? ST_call_ret_count(agg) : 0;
        if (rc >= 2) fprintf(out, "    mov rax, [rbp%+d]\n",
                             ST_ret_buf_off(agg, in->extract.index));
        else if (in->extract.index == 0) ST_load(out, "rax", agg);
        else ST_todo("extract index >0 from a non multi-return value");
    } break;
    case ST_IR_UDIV: ST_todo("ST_IR_UDIV"); break;
    case ST_IR_SREM: ST_todo("ST_IR_SREM"); break;
    case ST_IR_UREM: ST_todo("ST_IR_UREM"); break;
    case ST_IR_FADD: ST_todo("ST_IR_FADD"); break;
    case ST_IR_FSUB: ST_todo("ST_IR_FSUB"); break;
    case ST_IR_FMUL: ST_todo("ST_IR_FMUL"); break;
    case ST_IR_FDIV: ST_todo("ST_IR_FDIV"); break;
    case ST_IR_NEG: ST_todo("ST_IR_NEG"); break;
    case ST_IR_FNEG: ST_todo("ST_IR_FNEG"); break;
    case ST_IR_AND: {
        ST_load(out, "rax", in->bin.l);
        ST_load(out, "rbx", in->bin.r);
        fprintf(out, "    test rax, rax\n");
        fprintf(out, "    setne al\n");
        fprintf(out, "    movzx eax, al\n");

        fprintf(out, "    test rbx, rbx\n");
        fprintf(out, "    setne cl\n");
        fprintf(out, "    movzx ecx, cl\n");

        fprintf(out, "    and eax, ecx\n");
    } break;
    case ST_IR_OR: {
        ST_load(out, "rax", in->bin.l);
        ST_load(out, "rbx", in->bin.r);

        fprintf(out, "    test rax, rax\n");
        fprintf(out, "    setne al\n");
        fprintf(out, "    movzx eax, al\n");

        fprintf(out, "    test rbx, rbx\n");
        fprintf(out, "    setne cl\n");
        fprintf(out, "    movzx ecx, cl\n");

        fprintf(out, "    or eax, ecx\n");
    } break;
    case ST_IR_XOR: ST_todo("ST_IR_XOR"); break;
    case ST_IR_SHL: ST_todo("ST_IR_SHL"); break;
    case ST_IR_LSHR: ST_todo("ST_IR_LSHR"); break;
    case ST_IR_ASHR: ST_todo("ST_IR_ASHR"); break;
    case ST_IR_NOT: {
        ST_load(out, "rax", in->unary.v);
        fprintf(out, "    test rax, rax\n");
        fprintf(out, "    sete al\n");
        fprintf(out, "    movzx eax, al\n");
    } break;
    case ST_IR_ICMP_EQ: ST_icmp(out, in, "sete"); break;
    case ST_IR_ICMP_NE: ST_icmp(out, in, "setne"); break;
    case ST_IR_ICMP_SLT: ST_icmp(out, in, "setl"); break;
    case ST_IR_ICMP_SLE: ST_icmp(out, in, "setle"); break;
    case ST_IR_ICMP_SGT: ST_icmp(out, in, "setg"); break;
    case ST_IR_ICMP_SGE: ST_icmp(out, in, "setge"); break;
    case ST_IR_ICMP_ULT: ST_icmp(out, in, "setb"); break;
    case ST_IR_ICMP_ULE: ST_icmp(out, in, "setbe"); break;
    case ST_IR_ICMP_UGT: ST_icmp(out, in, "seta"); break;
    case ST_IR_ICMP_UGE: ST_icmp(out, in, "setae"); break;
    case ST_IR_FCMP_EQ: ST_todo("ST_IR_FCMP_EQ"); break;
    case ST_IR_FCMP_NE: ST_todo("ST_IR_FCMP_NE"); break;
    case ST_IR_FCMP_LT: ST_todo("ST_IR_FCMP_LT"); break;
    case ST_IR_FCMP_LE: ST_todo("ST_IR_FCMP_LE"); break;
    case ST_IR_FCMP_GT: ST_todo("ST_IR_FCMP_GT"); break;
    case ST_IR_FCMP_GE: ST_todo("ST_IR_FCMP_GE"); break;
    case ST_IR_CAST: ST_todo("ST_IR_CAST"); break;
    case ST_IR_PARAM: {
        u32 shift = ctx->hidden_ret_off ? 1 : 0;
        u32 idx = in->params.index + shift;
        if (idx >= ST_N_ARG_REGS) ST_todo("too many parameters");
        fprintf(out, "    mov rax, %s\n", arg_regs[idx]);
    } break;
    case ST_IR_CALL: {
        u32 rc = ST_call_ret_count(in);
        if (rc > 2)
        {
            u32 n = in->call.args.count;
            if (n + 1 > ST_N_ARG_REGS) ST_todo("too many arguments alongside hidden return pointer");
            fprintf(out, "    lea rdi, [rbp%+d]\n", ST_ret_buf_off(in, 0));
            ST_forrange(0, n) ST_load(out, arg_regs[i + 1], in->call.args.items[i]);
        }
        else
        {
            ST_forrange(0, in->call.args.count) ST_load(out, arg_regs[i], in->call.args.items[i]);
        }

        fprintf(out, "    xor eax, eax\n");
        fprintf(out, "    call " ST_sv_fmt "\n", ST_sv_args(in->call.callee_name));

        if (rc == 2)
        {
            fprintf(out, "    mov [rbp%+d], rax\n", ST_ret_buf_off(in, 0));
            fprintf(out, "    mov [rbp%+d], rdx\n", ST_ret_buf_off(in, 1));
            fprintf(out, "    mov rax, [rbp%+d]\n", ST_ret_buf_off(in, 0));
        }
        else if (rc > 2) fprintf(out, "    mov rax, [rbp%+d]\n", ST_ret_buf_off(in, 0));
    }break;
    case ST_IR_CALL_INDIRECT: ST_todo("ST_IR_CALL_INDIRECT"); break;
    case ST_IR_PHI: return;
    case ST_IR_COUNT: ST_todo("ST_IR_COUNT"); break;
    case ST_IR_ALLOCA: {
        fprintf(out, "    lea rax, [rbp-%u]\n", in->alloca_.frame_off);
    } break;
    case ST_IR_LOAD: {
        ST_load(out, "rcx", in->load.addr);
        ST_mem_load(out, in->ty);
    } break;
    case ST_IR_STORE: {
        ST_load(out, "rax", in->store.v);
        ST_load(out, "rcx", in->store.addr);
        ST_mem_store(out, in->ty);
    } break;
    case ST_IR_ADDR: {
        ST_load(out, "rax", in->addr.base);
        if (in->addr.index)
        {
            ST_load(out, "rcx", in->addr.index);
            u32 _scale = in->addr.scale ? in->addr.scale : 1;
            if (_scale == 1 || _scale == 2 || _scale == 4 || _scale == 8) {
                fprintf(out, "    lea rax, [rax + rcx*%u]\n", _scale);
            } else
            {
                fprintf(out, "    imul rcx, %u\n", _scale);
                fprintf(out, "    add rax, rcx\n");
            }
        }
        if (in->addr.offset) fprintf(out, "    lea rax, [rax + %d]\n", in->addr.offset);
    } break;
    case ST_IR_GLOBAL_ADDR: ST_todo("ST_IR_GLOBAL_ADDR"); break;

    default: break;
    }
    if (in->ty && in->ty->kind != ST_TY_VOID)
    {
        fprintf(out, "    mov [rbp%+d], rax\n", ST_slot(in));
    }
}

static ST_ir_inst_t *ST_phi_incoming(ST_ir_inst_t *in, ST_ir_block_t *from)
{
    ST_forrange(0, in->phi.preds.count)
    {
        if (in->phi.preds.items[i] == from) return in->phi.values.items[i];
    }
    return NULL;
}

static void ST_generate_phi_copies(FILE *out, ST_ir_block_t *from, ST_ir_block_t *to)
{
    for (ST_ir_inst_t *inst = to->first; inst; inst = inst->next)
    {
        if (inst->removed || inst->kind != ST_IR_PHI) continue;

        ST_ir_inst_t *v = ST_phi_incoming(inst, from);
        if (!v) continue;
        ST_load(out, "rax", v);
        fprintf(out, "push rax\n");
    }

    for (ST_ir_inst_t *inst = to->last; inst; inst = inst->prev)
    {
        if (inst->removed || inst->kind != ST_IR_PHI) continue;
        if (!ST_phi_incoming(inst, from)) continue;
        fprintf(out, "pop rax\n");
        fprintf(out, "mov [rbp%+d], rax\n", ST_slot(inst));
    }
}

static b8 ST_edge_has_phi(ST_ir_block_t *to)
{
    for (ST_ir_inst_t *inst = to->first; inst; inst = inst->next)
        if (!inst->removed && inst->kind == ST_IR_PHI) return 1;
    return 0;
}

static void ST_generate_term(FILE *out, ST_gen_ctx_t *ctx, ST_ir_block_t *b)
{
    ST_ir_term_t *t = &b->term;
    switch (t->kind)
    {
    case ST_IR_TERM_RET: {
        if (t->rets.count > 2)
        {
            fprintf(out, "    mov r10, [rbp%+d]\n", -(i32)ctx->hidden_ret_off);
            ST_forrange(0, t->rets.count)
            {
                ST_load(out, "rax", t->rets.items[i]);
                fprintf(out, "    mov [r10+%u], rax\n", 8u * i);
            }
            fprintf(out, "    mov rax, r10\n");
        }
        else
        {
            if (t->rets.count >= 1) ST_load(out, "rax", t->rets.items[0]);
            if (t->rets.count >= 2) ST_load(out, "rdx", t->rets.items[1]);
        }
        fprintf(out, "    leave\n");
        fprintf(out, "    ret\n");
    } break;
    case ST_IR_TERM_BR: {
        ST_generate_phi_copies(out, b, t->t_block);
        fprintf(out, "    jmp .bb%u\n", t->t_block->id);
    } break;
    case ST_IR_TERM_NONE: ST_todo("ST_IR_TERM_RET"); break;
    case ST_IR_TERM_COND_BR: {
        ST_load(out, "rax", t->cond);
        fprintf(out, "    test rax, rax\n");
        if (ST_edge_has_phi(t->f_block))
        {
            fprintf(out, "    jz .bb%u_edge_f\n", b->id);
            ST_generate_phi_copies(out, b, t->t_block);
            fprintf(out, "    jmp .bb%u\n", t->t_block->id);
            fprintf(out, "    .bb%u_edge_f:\n", b->id);
            ST_generate_phi_copies(out, b, t->f_block);
            fprintf(out, "    jmp .bb%u\n", t->f_block->id);

        }
        else
        {
            fprintf(out, "    jz .bb%u\n", t->f_block->id);
            ST_generate_phi_copies(out, b, t->t_block);
            fprintf(out, "    jmp .bb%u\n", t->t_block->id);
        }
    } break;
    case ST_IR_TERM_UNREACHABLE: {
        fprintf(out, "    ud2\n");
    } break;
    }
}

static u32 ST_layout_fn(ST_ir_fn_t *fn, ST_gen_ctx_t *ctx)
{
    u32 cur = fn->next_value_id * 8u;

    ctx->fn = fn;
    ctx->hidden_ret_off = 0;
    if (fn->ty && fn->ty->rets.count > 2)
    {
        cur += 8;
        ctx->hidden_ret_off = cur;
    }

    ST_forrange(0, fn->blocks.count)
    {
        ST_ir_block_t *b = fn->blocks.items[i];
        for (ST_ir_inst_t *in = b->first; in; in = in->next)
        {
            if (in->removed) continue;
            if (in->kind == ST_IR_ALLOCA)
            {
                u32 align = in->alloca_.align ? in->alloca_.align : 8;
                u32 size  = in->alloca_.size  ? in->alloca_.size  : 8;
                cur = (cur + align - 1) & ~(align - 1);
                cur += size;
                in->alloca_.frame_off = cur;
            }
            else if (in->kind == ST_IR_CALL)
            {
                u32 rc = ST_call_ret_count(in);
                if (rc >= 2)
                {
                    cur = (cur + 7u) & ~7u;
                    cur += rc * 8u;
                    in->call.ret_buf_offset = cur;
                }
            }
        }
    }
    return cur;
}

static void ST_generate_fn(FILE *out, ST_ir_fn_t *fn)
{
    ST_gen_ctx_t ctx;
    u32 extra = ST_layout_fn(fn, &ctx);
    u32 frame = (extra + 15) & ~15u;
    fprintf(out, "\n" ST_sv_fmt":\n", ST_sv_args(fn->name));
    fprintf(out, "    push rbp\n");
    fprintf(out, "    mov rbp, rsp\n");
    fprintf(out, "    sub rsp, %u\n", frame);
    if (ctx.hidden_ret_off) fprintf(out, "    mov [rbp%+d], rdi\n",
                                    -(i32)ctx.hidden_ret_off);
    ST_forrange(0, fn->blocks.count)
    {
        ST_ir_block_t *b = fn->blocks.items[i];
        fprintf(out, "    .bb%u:\n", b->id);
        for (ST_ir_inst_t *in = b->first; in; in = in->next)
        {
            ST_generate_inst(out, &ctx, in);
        }
        ST_generate_term(out, &ctx, b);
    }
}

b8 ST_nasm_generate(FILE *out, ST_ir_module_t *m, ST_string_t src,
                    ST_string_t file, b8 emit_entry)
{
    ST_unused(src);
    ST_unused(file);
    if (out == NULL) out = stdout;
    fprintf(out, "BITS 64\n");
    ST_generate_strs(out, m);
    fprintf(out, "section .text\n");
    ST_forrange(0, m->fns.count)
    {
        ST_ir_fn_t *fn = m->fns.items[i];
        if (fn->is_extern)  { fprintf(out, "extern " ST_sv_fmt "\n", ST_sv_args(fn->name)); continue; }
        if (fn->is_pub)  { fprintf(out, "global " ST_sv_fmt "\n", ST_sv_args(fn->name)); }
        ST_generate_fn(out, fn);
    }
    if (emit_entry) {
        fprintf(out, "\nglobal _start\n");
        fprintf(out, "\n_start:\n");
        fprintf(out, "      call main\n");
        fprintf(out, "      mov rdi, rax\n");
        fprintf(out, "      mov rax, 60\n");
        fprintf(out, "      syscall\n");
    }
    return 1;
}
