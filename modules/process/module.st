pub struct Process {
    exitCode: i32;
    capturedStdout: string;
    capturedStderr: string;
}

pub fn runCommandProcess(command: []string, capture_stdout := true, capture_stderr := false) -> (Process, bool) {
    fn readAllFromFd(fd: i32) ->  string {
        sb : StringBuilder;
        chunk : [4096]char;
        while true {
            n := sys_read(fd, &chunk[0], 4096);
            if n <= 0 then break;
            for i: 0..n {
                arrayPush(&sb, chunk[i]);
            }
        }
        return builderToString(sb);
    }

    fn redirect_to_dev_null_and_close_file_descriptor(dup2_number := 1) -> void {
        O_WRONLY :: 1;
        out := sys_open("/dev/null", O_WRONLY, 0);
        sys_dup2(out, dup2_number);
        sys_close(out);
    }

    out: Process = { -1, "", ""};
    out_pipe : [2]i32;
    err_pipe : [2]i32;
    if capture_stdout {
        if sys_pipe(&out_pipe[0]) < 0 then return out, false;
    }
    if capture_stderr {
        if sys_pipe(&err_pipe[0]) < 0 then return out, false;
    }

    fn dup_and_close(out: []i32, n := 1) -> void {
        sys_dup2(out[n], n);
        sys_close(out[n - 1]);
        sys_close(out[n]);
    }

    pid := sys_fork();
    if pid < 0 then return out, false;

    if pid == 0 {
        if capture_stdout then dup_and_close(out_pipe);
        else then redirect_to_dev_null_and_close_file_descriptor();
        if capture_stderr then dup_and_close(err_pipe, 2);
        else then redirect_to_dev_null_and_close_file_descriptor(2);

        env : []string = [];
        path_buf : [4096]char;
        has_slash := false;
        for i: 0..command[0].len {
            if command[0][i] == '/' {
                has_slash = true;
                break;
            }
        }
        if has_slash {
            r := sys_execve(command[0], command, env);
            eprint("execve failed: %\n", r);
        } else {
            prefixes := ["/usr/bin/", "/usr/local/bin/", "/bin/"];
            for i: 0..prefixes.len {
                path := concatinate(path_buf, prefixes[i], command[0]);
                r := sys_execve(path, command, env);
                eprint("execve(%) failed: %\n", path, r);
            }
        }
        sys_exit(127);
    }

    fn close_pipe(pipe: []i32, out: *string) {
        sys_close(pipe[1]);
        *out = readAllFromFd(pipe[0]);
        sys_close(pipe[0]);
    }

    if capture_stdout then close_pipe(out_pipe, &out.capturedStdout);
    if capture_stderr then close_pipe(err_pipe, &out.capturedStderr);

    status : i32;
    sys_wait4(pid, &status, 0, null);
    if (status & 0x7f) == 0 then out.exitCode = (status >> 8) & 0xff;

    return out, true;
}

#import "os";
#import "io";
#import "mem";
#import "string";
#import "array";
