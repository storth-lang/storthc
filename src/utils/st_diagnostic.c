#include <stdarg.h>

#include "st_diagnostic.h"

static ST_string_t ST_diag_src_line(ST_string_t src, u32 line)
{
    u32 cur = 1, start = 0;
    ST_forrange(0, src.len)
    {
        if (cur == line)
        {
            start = i;
            u32 end = i;
            while (end < src.len && src.data[end] != '\n') end++;
            return (ST_string_t){ .data = src.data + start, .len = end - start };
        }
        if (src.data[i] == '\n') cur++;
    }
    return (ST_string_t){0};
}

static void ST_diag_snippet(ST_diag_t *d, u32 line, u32 col, const char *caret_col)
{
    for (u32 l = line > 2 ? line - 2 : 1; l <= line; l++)
    {
        ST_string_t text = ST_diag_src_line(d->src, l);
        if (l == line)
        {
            fprintf(stderr, "%s%4u |" ST_COLOR_RESET " ", caret_col, l);
            fprintf(stderr, "%s" ST_sv_fmt ST_COLOR_RESET "\n",
                    caret_col, ST_sv_args(text));
            fprintf(stderr, ST_COLOR_BOLD ST_COLOR_BLUE "     |" ST_COLOR_RESET " ");
            ST_forrange(0, col > 0 ? col - 1 : 0) fputc(' ', stderr);
            fprintf(stderr, ST_COLOR_CYAN "^" ST_COLOR_RESET "\n");
        }
        else
        {
            fprintf(stderr, ST_COLOR_DIM ST_COLOR_BLUE "%4u |" ST_COLOR_RESET " ", l);
            fprintf(stderr, ST_COLOR_WHITE ST_sv_fmt ST_COLOR_RESET "\n",
                    ST_sv_args(text));
        }
    }
}

void ST_diag_error(ST_diag_t *d, u32 line, u32 col, const char *fmt, ...)
{
    d->n_errors++;
    if (d->max_errors && d->n_errors > d->max_errors) return;
    if (d->max_errors && d->n_errors == d->max_errors)
    {
        fprintf(stderr, ST_COLOR_BOLD "too many errors, stopping\n" ST_COLOR_RESET);
        return;
    }
    fprintf(stderr, ST_COLOR_BOLD ST_sv_fmt ":%u:%u: " ST_COLOR_BOLD_RED "error: "
            ST_COLOR_RESET ST_COLOR_BOLD, ST_sv_args(d->file), line, col);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, ST_COLOR_RESET "\n");
    ST_diag_snippet(d, line, col, ST_COLOR_BOLD_RED);
}

void ST_diag_note(ST_diag_t *d, u32 line, u32 col, const char *fmt, ...)
{
    if (d->max_errors && d->n_errors >= d->max_errors) return;
    fprintf(stderr, ST_COLOR_BOLD ST_sv_fmt ":%u:%u: " ST_COLOR_CYAN "note: "
            ST_COLOR_RESET ST_COLOR_BOLD, ST_sv_args(d->file), line, col);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, ST_COLOR_RESET "\n");
    ST_diag_snippet(d, line, col, ST_COLOR_DIM ST_COLOR_CYAN);
}
