pub struct Process {
    exitCode: i32;
    capturedStdout: string;
    capturedStderr: string;
}

pub fn runCommandProcess(command: []string, capture_stdout := true, capture_stderr := false) -> (Process, bool) {
    fn readAllFromFd(fd: i32) -> string {
        sb : StringBuilder;
        chunk : [4096]char;
        while true {
            n := read(fd, &chunk[0], 4096);
            if n <= 0 then break;
            for i: 0..n {
                arrayPush(&sb, chunk[i]);
            }
        }
        return builderToString(sb);
    }
    out: Process;
    out.exitCode = -1;
    out.capturedStdout = "";
    out.capturedStderr = "";
    out_pipe : [2]i32;
    err_pipe : [2]i32;
    if capture_stdout {
        if sys_pipe(&out_pipe[0]) < 0 then return out, false;
    }
    if capture_stderr {
        if sys_pipe(&err_pipe[0]) < 0 then return out, false;
    }
    pid := sys_fork();
    if pid < 0 then return out, false;
    if pid == 0 {
        O_WRONLY :: 1;
        if capture_stdout {
            sys_dup2(out_pipe[1], 1);
            close(out_pipe[0]);
            close(out_pipe[1]);
        } else {
            devnull_out := open("/dev/null", O_WRONLY, 0);
            sys_dup2(devnull_out, 1);
            close(devnull_out);
        }
        if capture_stderr {
            sys_dup2(err_pipe[1], 2);
            close(err_pipe[0]);
            close(err_pipe[1]);
        } else {
            devnull_err := open("/dev/null", O_WRONLY, 0);
            sys_dup2(devnull_err, 2);
            close(devnull_err);
        }
        env : []string = [];
        path_buf : [4096]char;
        path := concatinate(path_buf, "/bin/", command[0]);
        execve(path, command, env);
        sys_exit(127);
    }
    if capture_stdout {
        close(out_pipe[1]);
        out.capturedStdout = readAllFromFd(out_pipe[0]);
        close(out_pipe[0]);
    }
    if capture_stderr {
        close(err_pipe[1]);
        out.capturedStderr = readAllFromFd(err_pipe[0]);
        close(err_pipe[0]);
    }
    status : i32;
    sys_wait4(pid, &status, 0, null);
    if (status & 0x7f) == 0 {
        out.exitCode = (status >> 8) & 0xff;
    }
    return out, true;
}
#import "os";
#import "mem";
#import "string";
#import "array";
