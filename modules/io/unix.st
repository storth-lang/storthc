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

pub enum_flag openMode {
    readOnly;
    writeOnly;
    readAndWrite;
}

fn flags_for_mode(mode: openMode) -> i32 {
    if mode == openMode.readOnly then return 0;
    if mode == openMode.writeOnly then return 577;
    return 66;
}

pub fn openFileDescriptor(path: string, mode := openMode.readAndWrite) -> (i32, ioError) {
    flags := flags_for_mode(mode);
    fd := open(path, flags, 420);
    if fd < 0 {
        set_last_errno(fd);
        if mode == openMode.readOnly then return 0, ioError.OpenErrorR;
        if mode == openMode.writeOnly then return 0, ioError.OpenErrorW;
        return 0, ioError.OpenErrorRW;
    }
    return fd, ioError.None;
}

pub fn closeFileDescriptor(fd: i32) -> void {
    if fd > 0 then close(fd);
}

pub fn writeToFileDescriptor(fd: i32, data: string) -> (i64, ioError) {
    p := data.ptr;
    n := data.len;
    written := #asm {
        mov rax, 1
        mov rdi, fd
        mov rsi, p
        mov rdx, n
        syscall
    };
    if written < 0 {
        set_last_errno(written);
        return 0, ioError.WriteError;
    }
    return written #as i64, ioError.None;
}

pub fn readFromFileDescriptor(fd: i32) -> (string, ioError) {
    cap : i64 = 65536;
    buf, merr := heap_alloc(cap);
    if merr != Error.None {
        return "", ioError.ReadError;
    }

    total := 0;
    while true {
        base := buf #as *u8;
        p := base + total;
        remaining := cap #as i32 - total;
        n := #asm {
            mov rax, 0
            mov rdi, fd
            mov rsi, p
            mov rdx, remaining
            syscall
        };
        if n < 0 {
            set_last_errno(n);
            heap_free(buf);
            return "", ioError.ReadError;
        }
        if n == 0 then break;
        total += n;
        if total #as i64 == cap {
            new_cap := cap * 2;
            new_buf, merr2 := reallocate(buf, new_cap);
            if merr2 != Error.None {
                heap_free(buf);
                return "", ioError.ReadError;
            }
            buf = new_buf;
            cap = new_cap;
        }
    }

    return str_from_raw(buf #as *char, total), ioError.None;
}

pub fn readEntireFile(path: string) -> (StringBuilder, ioError) {
    empty : StringBuilder;

    fd, ferr := openFileDescriptor(path, openMode.readOnly);
    if ferr != ioError.None {
        print("open failed: path=% ferr=% fd=%\n", path, ferr, fd);
        return empty, ferr;
    }

    content, rerr := readFromFileDescriptor(fd);
    closeFileDescriptor(fd);
    if rerr != ioError.None {
        print("read failed: path=% rerr=%\n", path, rerr);
        return empty, rerr;
    }

    sb : StringBuilder;
    sb.items = content.ptr;
    sb.count = content.len;
    sb.capacity = content.len;
    return sb, ioError.None;
}


#import "error";
#import "mem";
#import "string";
