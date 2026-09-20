#define _POSIX_C_SOURCE 200809L

#include "../st_process.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>
#include <sys/resource.h>

b8 ST_run_process(ST_proc_t *proc) {
    fflush(NULL);
    pid_t id = fork();
    if (id < 0)
        return 0;
    if (id == 0) {
        if (proc->opt.out)
            dup2(fileno(proc->opt.out), STDOUT_FILENO);
        
        char *const *original_args = (char *const *)proc->opt.args;
        int arg_count = 0;
        while (original_args[arg_count] != NULL)
            arg_count++;
        char **new_args = malloc((arg_count + 3) * sizeof(char *));
        if (new_args == NULL)
            _exit(127);
        new_args[0] = "stdbuf";
        new_args[1] = "-o0";
        for (int i = 0; i < arg_count; i++)
            new_args[i + 2] = original_args[i];
        new_args[arg_count + 2] = NULL;
        execvp(new_args[0], new_args);
        _exit(127);
    }
    proc->id = (void *)(intptr_t)id;
    if (proc->opt.async)
        return 1;
    return ST_wait_process(proc);
}

b8 ST_wait_process(ST_proc_t *proc) {
    pid_t id = (pid_t)(intptr_t)proc->id;
    if (id <= 0)
        return 1;
    int status = 0;
    pid_t r = waitpid(id, &status, 0);
    proc->id = (void *)-1;
    if (r < 0)
        return 0;
    proc->exited_normally = WIFEXITED(status);
    proc->exit_code = proc->exited_normally ? WEXITSTATUS(status) : -1;
    return proc->exited_normally && proc->exit_code == 0;
}
