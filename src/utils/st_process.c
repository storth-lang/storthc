#define _POSIX_C_SOURCE 200809L

#include "st_process.h"
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

// This will append all the process options I have set into procs structure
// which will be used in the actual run process.
void ST_append_process_opt(ST_procs_t *procs, ST_proc_opt_t opt) {
    ST_proc_t p = {0};
    p.opt = opt;
    p.id = -1;
    ST_da_append(procs, p);
}

// This functions run the process.
b8 ST_run_process(ST_proc_t *proc) {
    fflush(NULL);
    pid_t id = fork();
    if (id < 0)
        return 0;
    if (id == 0) {
        if (proc->opt.out)
            dup2(fileno(proc->opt.out), STDOUT_FILENO);

        // This is a trick to make stdout flush I did not disable buffering.
        // This is for when passing -r to run the command which obv is not
        // buffered so it does not auto show me stuff printed to stdout.
        char *const *original_args = (char *const *)proc->opt.args;
        int arg_count = 0;
        while (original_args[arg_count] != NULL)
            arg_count++;
        char **new_args = malloc((arg_count + 3) * sizeof(char *));
        if (new_args == NULL) {
            _exit(127);
        }
        new_args[0] = "stdbuf";
        new_args[1] = "-o0";

        for (int i = 0; i < arg_count; i++) {
            new_args[i + 2] = original_args[i];
        }

        new_args[arg_count + 2] = NULL;
        execvp(new_args[0], new_args);
        _exit(127);
    }
    proc->id = id;
    if (proc->opt.async)
        return 1;
    return ST_wait_process(proc);
}

b8 ST_wait_process(ST_proc_t *proc) {
    if (proc->id <= 0)
        return 1;
    int status = 0;
    pid_t r = waitpid(proc->id, &status, 0);
    proc->id = -1;
    if (r < 0)
        return 0;
    proc->exited_normally = WIFEXITED(status) ? 1 : 0;
    proc->exit_code = proc->exited_normally ? WEXITSTATUS(status) : -1;
    return proc->exited_normally && proc->exit_code == 0;
}

// TODO make customizable with reset or no reset.
b8 ST_run_processes(ST_procs_t *procs) {
    b8 ok = 1;
    for (u32 i = 0; i < procs->count; i++) {
        if (!ST_run_process(&procs->items[i]))
            ok = 0;
    }

    for (u32 i = 0; i < procs->count; i++) {
        if (!ST_wait_process(&procs->items[i]))
            ok = 0;
    }

    procs->count = 0;
    return ok;
}

void ST_free_process(ST_procs_t *procs) {
    free(procs->items);
    procs->count = 0;
    procs->capacity = 0;
}
