#define _POSIX_C_SOURCE 200809L

#include <unistd.h>
#include <stdio.h>
#include <sys/wait.h>
#include "st_process.h"

void ST_append_process_opt(ST_procs_t *procs, ST_proc_opt_t opt)
{
    ST_proc_t p = {0};
    p.opt = opt;
    p.id = -1;
    ST_da_append(procs, p);
}

b8 ST_run_process(ST_proc_t *proc)
{
    fflush(NULL);
    pid_t id = fork();
    if (id < 0) return 0;
    if (id == 0)
    {
        if (proc->opt.out)
        {
            dup2(fileno(proc->opt.out), STDOUT_FILENO);
        }
        execvp(proc->opt.args[0], (char *const *)proc->opt.args);
        _exit(127);
    }
    proc->id = id;
    if (proc->opt.async) return 1;
    return ST_wait_process(proc);
}

b8 ST_wait_process(ST_proc_t *proc)
{
    if (proc->id <= 0) return 1;
    int status = 0;
    pid_t r = waitpid(proc->id, &status, 0);
    proc->id = -1;
    if (r < 0) return 0;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

// Todo make customizable with reset or no reset.
b8 ST_run_processes(ST_procs_t *procs)
{
    b8 ok = 1;
    for (u32 i = 0; i < procs->count; i++)
    {
        if (!ST_run_process(&procs->items[i])) ok = 0;
    }

    for (u32 i = 0; i < procs->count; i++)
    {
        if (!ST_wait_process(&procs->items[i])) ok = 0;
    }

    procs->count = 0;
    return ok;
}

void ST_free_process(ST_procs_t *procs)
{
    free(procs->items);
    procs->count = 0;
    procs->capacity = 0;
}
