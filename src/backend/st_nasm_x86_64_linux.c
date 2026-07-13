#include "st_nasm.h"
#include <stdio.h>

static const char *arg_regs[] = { "rdi", "rsi", "rdx", "rcx", "r8", "r9" };

static i32 ST_slot(ST_ir_inst_t *v)
{
    while (v->repl) v = v->repl;
    return -8 * (i32)(v->id + 1);
}

static void ST_load(FILE *out, const char *reg, ST_ir_inst_t *v)
{
    fprintf(out, "    mov %s, [rbp%+d]\n", reg, ST_slot(v));
}

static void ST_generate_inst(FILE *out, ST_ir_inst_t *in)
{
    if (in->removed) return;
    switch(in->kind)
    {
    case ST_IR_CONST_INT: {
        fprintf(out, "    mov rax, %ld\n", in->const_int);
    } break;
    case ST_IR_CONST_FLOAT: ST_todo("ST_IR_CONST_FLOAT"); break;
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
    case ST_IR_UDIV: ST_todo("ST_IR_UDIV"); break;
    case ST_IR_SREM: ST_todo("ST_IR_SREM"); break;
    case ST_IR_UREM: ST_todo("ST_IR_UREM"); break;
    case ST_IR_FADD: ST_todo("ST_IR_FADD"); break;
    case ST_IR_FSUB: ST_todo("ST_IR_FSUB"); break;
    case ST_IR_FMUL: ST_todo("ST_IR_FMUL"); break;
    case ST_IR_FDIV: ST_todo("ST_IR_FDIV"); break;
    case ST_IR_NEG: ST_todo("ST_IR_NEG"); break;
    case ST_IR_FNEG: ST_todo("ST_IR_FNEG"); break;
    case ST_IR_AND: ST_todo("ST_IR_AND"); break;
    case ST_IR_OR: ST_todo("ST_IR_OR"); break;
    case ST_IR_XOR: ST_todo("ST_IR_XOR"); break;
    case ST_IR_SHL: ST_todo("ST_IR_SHL"); break;
    case ST_IR_LSHR: ST_todo("ST_IR_LSHR"); break;
    case ST_IR_ASHR: ST_todo("ST_IR_ASHR"); break;
    case ST_IR_NOT: ST_todo("ST_IR_NOT"); break;
    case ST_IR_ICMP_EQ: ST_todo("ST_IR_ICMP_EQ"); break;
    case ST_IR_ICMP_NE: ST_todo("ST_IR_ICMP_NE"); break;
    case ST_IR_ICMP_SLT: {
        ST_load(out, "rax", in->bin.l);
        ST_load(out, "rcx", in->bin.r);
        fprintf(out, "    cmp rax, rcx\n");
        fprintf(out, "    setl al\n");
        fprintf(out, "    movzx rax, al\n");
    } break;
    case ST_IR_ICMP_SLE: ST_todo("ST_IR_ICMP_SLE"); break;
    case ST_IR_ICMP_SGT: ST_todo("ST_IR_ICMP_SGT"); break;
    case ST_IR_ICMP_SGE: ST_todo("ST_IR_ICMP_SGE"); break;
    case ST_IR_ICMP_ULT: ST_todo("ST_IR_ICMP_ULT"); break;
    case ST_IR_ICMP_ULE: ST_todo("ST_IR_ICMP_ULE"); break;
    case ST_IR_ICMP_UGT: ST_todo("ST_IR_ICMP_UGT"); break;
    case ST_IR_ICMP_UGE: ST_todo("ST_IR_ICMP_UGE"); break;
    case ST_IR_FCMP_EQ: ST_todo("ST_IR_FCMP_EQ"); break;
    case ST_IR_FCMP_NE: ST_todo("ST_IR_FCMP_NE"); break;
    case ST_IR_FCMP_LT: ST_todo("ST_IR_FCMP_LT"); break;
    case ST_IR_FCMP_LE: ST_todo("ST_IR_FCMP_LE"); break;
    case ST_IR_FCMP_GT: ST_todo("ST_IR_FCMP_GT"); break;
    case ST_IR_FCMP_GE: ST_todo("ST_IR_FCMP_GE"); break;
    case ST_IR_CAST: ST_todo("ST_IR_CAST"); break;
    case ST_IR_PARAM: {
        fprintf(out, "    mov rax, %s\n", arg_regs[in->params.index]);
    } break;
    case ST_IR_CALL: {
        ST_forrange(0, in->call.args.count)
            ST_load(out, arg_regs[i], in->call.args.items[i]);

        fprintf(out, "    xor eax, ecx\n");
        fprintf(out, "    call " ST_sv_fmt "\n", ST_sv_args(in->call.callee_name));
    }break;
    case ST_IR_CALL_INDIRECT: ST_todo("ST_IR_CALL_INDIRECT"); break;
    case ST_IR_PHI: ST_todo("ST_IR_PHI"); break;
    case ST_IR_COUNT: ST_todo("ST_IR_COUNT"); break;
    default: break;
    }
    if (in->ty && in->ty->kind != ST_TY_VOID) 
    {
        fprintf(out, "    mov [rbp%+d], rax\n", ST_slot(in));
    }
}

static void ST_generate_term(FILE *out, ST_ir_block_t *b)
{
    ST_ir_term_t *t = &b->term;
    switch (t->kind)
    {
    case ST_IR_TERM_RET: {
        if (t->rets.count) ST_load(out, "rax", t->rets.items[0]);
        fprintf(out, "    leave\n");
        fprintf(out, "    ret\n");
    } break;
    case ST_IR_TERM_BR: {
        fprintf(out, "    jmp .bb%u\n", t->t_block->id);
    } break;
    case ST_IR_TERM_NONE: ST_todo("ST_IR_TERM_RET"); break;
    case ST_IR_TERM_COND_BR: {
        ST_load(out, "rax", t->cond);
        fprintf(out, "    test rax, rax\n");
        fprintf(out, "    jnz .bb%u\n", t->t_block->id);
        fprintf(out, "    jmp .bb%u\n", t->f_block->id);
    } break;
    case ST_IR_TERM_UNREACHABLE: ST_todo("ST_IR_TERM_UNREACHABLE"); break;
    }
}

static void ST_generate_fn(FILE *out, ST_ir_fn_t *fn)
{
    u32 frame = (fn->next_value_id * 8 + 15) & ~15u;
    fprintf(out, "\n" ST_sv_fmt":\n", ST_sv_args(fn->name));
    fprintf(out, "    push rbp\n");
    fprintf(out, "    mov rbp, rsp\n");
    fprintf(out, "    sub rsp, %u\n", frame);
    ST_forrange(0, fn->blocks.count)
    {
        ST_ir_block_t *b = fn->blocks.items[i];
        fprintf(out, "    .bb%u\n", b->id);
        for (ST_ir_inst_t *in = b->first; in; in = in->next)
        {
            ST_generate_inst(out, in);
        }
        ST_generate_term(out, b);
    }
}

b8 ST_nasm_generate(FILE *out, ST_ir_module_t *m, ST_string_t src,
                    ST_string_t file, b8 emit_entry)
{
    ST_unused(src);
    ST_unused(file);
    if (out == NULL) out = stdout;
    fprintf(out, "BITS 64\n");
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
