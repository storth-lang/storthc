fn open(path: string, flags: i32, mode: i32) -> i32 {
    buf : [4096]char;
    for i : 0..path.len {
        buf[i] = path[i];
    }
    buf[path.len] = 0 #as char;
    p := &buf[0];
    return #asm {
        mov rax, 2
        mov rdi, p
        mov rsi, flags
        mov rdx, mode
        syscall
    };
}

fn close(fd: i32) -> i32 {
    return #asm {
        mov rax, 3
        mov rdi, fd
        syscall
    };
}

fn write(fd: i32, p: *char, n: i32) -> i32 {
    return #asm {
        mov rax, 1
        mov rdi, fd
        mov rsi, p
        mov rdx, n
        syscall
    };
}

fn read(fd: i32, p: *char, n: i32) -> i32 {
    return #asm {
        mov rax, 0
        mov rdi, fd
        mov rsi, p
        mov rdx, n
        syscall
    };
}
