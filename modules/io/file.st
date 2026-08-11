enum_flag File_Mode {
    READ := 1,
    WRITE;
}

fn open_fd(x: string, mode: File_Mode) -> i32 {
    return #asm {
        mov rax, 2
        mov rdi, x
        mov rsi, mode
        mov rdx, 0
        syscall
    };
}

pub fn read_entire_file(path: string) -> (string, bool) {
    fd := open_fd(path);
    if fd < 0 { return null, false; }
    content_cstr := #asm {
        mov rax, 0
        mov rdi, fd
        syscall
    };
    content := string_from_raw(content_cstr, strlen(content_cstr));
    return content, true;
}
