// Linux important syscall:
// reference: https://www.man7.org/linux/man-pages/man2/syscalls.2.html
// TODO: Add sys_stat so that we can have more errors and also setting
// behavior in files.

pub PROT_NONE     :: #comptime 0;
pub PROT_READ     :: #comptime 1;
pub PROT_WRITE    :: #comptime 2;
pub MAP_PRIVATE   :: #comptime 2;
pub MAP_ANON      :: #comptime 32;
pub MADV_DONTNEED :: #comptime 4;
pub PAGE          :: #comptime 4096;

pub struct timespec {
    tv_sec: i64;
    tv_nsec: i64;
}

pub fn sys_time(n: u64, a1: u64, a2: *timespec) -> i64 {
    ret: i64 = 0;
    #asm {
        mov rax, n
        mov rdi, a1
        mov rsi, a2
        syscall
        mov [&ret], rax
    };
    return ret;
}

pub fn sys_mmap(addr: *void, length: i64, prot: i32, flags: i32, fd: i32, offset: i64) -> *void {
    MMAP :: 9;

	x := #asm {
		mov rax, MMAP
		mov rdi, addr
		mov rsi, length
		mov edx, prot
		mov r10d, flags
		mov r8d, fd
		mov r9, offset
		syscall
	};
	return x #as *void;
}

pub fn sys_mprotect(addr: *void, length: i64, prot: i32) -> i64 {
    MPROTECT :: 10;

	return #asm {
		mov rax, MPROTECT
		mov rdi, addr
		mov rsi, length
		mov edx, prot
		syscall
	};
}

pub fn sys_munmap(addr: *void, length: i64) -> i64 {
    MUNMAP ::11;

	return #asm {
		mov rax, MUNMAP
		mov rdi, addr
		mov rsi, length
		syscall
	};
}

pub fn sys_madvise(addr: *void, length: i64, advice: i32) -> i64 {
    MADVISE :: 28;

	return #asm {
		mov rax, MADVISE
		mov rdi, addr
		mov rsi, length
		mov edx, advice
		syscall
	};
}

pub fn sys_open(path: string, flags: i32, mode: i32) -> i32 {
    buf : [4096]char;
    for i : 0..path.len {
        buf[i] = path[i];
    }
    buf[path.len] = 0 #as char;
    p := &buf[0];
    OPEN :: 2;

    return #asm {
        mov rax, OPEN
        mov rdi, p
        mov rsi, flags
        mov rdx, mode
        syscall
    };
}

pub fn sys_close(fd: i32) -> i32 {
    CLOSE :: 3;

    return #asm {
        mov rax, CLOSE
        mov rdi, fd
        syscall
    };
}

pub fn sys_write(fd: i32, p: *char, n: i32) -> i32 {
    return #asm {
        mov rax, 1
        mov rdi, fd
        mov rsi, p
        mov rdx, n
        syscall
    };
}

pub fn sys_read(fd: i32, p: *char, n: i32) -> i32 {
    READ :: 0;

    return #asm {
        mov rax, READ
        mov rdi, fd
        mov rsi, p
        mov rdx, n
        syscall
    };
}

pub fn sys_mkdir(path: string, permission: i32) -> i32 {
    MKDIR :: 83;

    fn decimal_to_octal_mask(decimal_input: i32) -> i32 {
        octal_mask := 0;
        base := 1;

        while decimal_input > 0 {
            last_digit := decimal_input % 10;
            decimal_input = decimal_input / 10;

            octal_mask += last_digit * base;
            base *= 8;
        }

        return octal_mask;
    }

    permission = decimal_to_octal_mask(permission);

    return #asm {
        mov rax, MKDIR
        mov rdi, path
        mov rsi, permission
        syscall
    };
}

pub fn sys_execve(path: string, args: []string, env: []string) -> i32 {
    EXECVE :: 59;

    fn strs_bytes(strs: []string) -> u64 {
        total : u64;
        for i: 0..strs.len {
            total = total + strs[i].len #as u64 + 1;
        }
        return total;
    }

    path_size := path.len + 1;
    argv_size := (args.len + 1) * 8;
    args_bytes := strs_bytes(args);
    envp_size := (env.len + 1) * 8;
    env_bytes := strs_bytes(env);

    total : u64 = (path_size #as u64 + argv_size #as u64 + args_bytes #as u64 + envp_size #as u64 + env_bytes #as u64 + 15) & ~15;

    base := #asm {
        sub rsp, total
        mov rax, rsp
    } #as *char;

    c_path := base;
    for i: 0..path.len {
        c_path[i] = path.ptr[i];
    }
    c_path[path.len] = '\0';

    argv := (base + path_size) #as **char;
    args_area := base + path_size + argv_size;
    off : u64;
    for i: 0..args.len {
        dst := args_area + off;
        for j: 0..args[i].len {
            dst[j] = args[i].ptr[j];
        }
        dst[args[i].len] = '\0';
        argv[i] = dst;
        off = off +  args[i].len #as u64 + 1;
    }
    argv[args.len] = null;

    envp := (base + path_size + argv_size + args_bytes) #as **char;
    env_area := base + path_size + argv_size + args_bytes + envp_size;
    off = 0;
    for i: 0..env.len {
        dst := env_area + off;
        for j: 0..env[i].len {
            dst[j] = env[i].ptr[j];
        }
        dst[env[i].len] = '\0';
        envp[i] = dst;
        off = off + env[i].len #as u64 + 1;
    }
    envp[env.len] = null;

    return #asm {
        mov rax, EXECVE
        mov rdi, c_path
        mov rsi, argv
        mov rdx, envp
        syscall
    };
}

pub fn sys_fork() -> i32 {
    FORK :: 57;
    return #asm {
        mov rax, FORK
        syscall
    };
}

pub fn sys_pipe(fds: *i32) -> i32 {
    PIPE :: 22;
    return #asm {
        mov rax, PIPE
        mov rdi, fds
        syscall
    };
}

pub fn sys_dup2(oldfd: i32, newfd: i32) -> i32 {
    DUP2 :: 33;
    return #asm {
        mov rax, DUP2
        mov rdi, oldfd
        mov rsi, newfd
        syscall
    };
}

pub fn sys_wait4(pid: i32, status: *i32, options: i32, rusage: *void) -> i32 {
    WAIT4 :: 61;
    return #asm {
        mov rax, WAIT4
        mov rdi, pid
        mov rsi, status
        mov edx, options
        mov r10, rusage
        syscall
    };
}

pub fn sys_exit(code: i32) {
    EXIT :: 60;
    #asm {
        mov rax, EXIT
        mov rdi, code
        syscall
    }
}

pub fn sys_rename(old:string, new:string) -> i32 {
    RENAME :: 82;

    return #asm {
        mov rax, RENAME
        mov rdi, old
        mov rsi, new
    };
}