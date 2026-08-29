#import "error";
#import "mem";
#import "string";
#import "io";
#import "os";

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
    written := write(fd, p, n #as i32);
    if written < 0 {
        return 0, ioError.WriteError;
    }
    return written #as i64, ioError.None;
}

pub fn readFromFileDescriptor(fd: i32) -> (string, ioError) {
    cap : i64 = 65536;
    buf, merr := alloc(cap);
    if merr != memoryError.None then return "", ioError.ReadError;

    total := 0;
    while true {
        base := buf #as *u8;
        p := base + total;
        remaining := cap #as i32 - total;
        n := read(fd, p #as *char, remaining);
        if n < 0 {
            free(buf);
            return "", ioError.ReadError;
        }
        if n == 0 then break;
        total += n;
        if total #as i64 == cap {
            new_cap := cap * 2;
            new_buf, merr2 := realloc(buf, new_cap);
            if merr2 != memoryError.None {
                free(buf);
                return "", ioError.ReadError;
            }
            buf = new_buf;
            cap = new_cap;
        }
    }

    return str_from_raw(buf #as *char, total), ioError.None;
}

pub fn readEntireFile(path: string) -> (string, bool) {
    fd, ferr := openFileDescriptor(path, openMode.readOnly);
    if ferr != ioError.None {
        print("open failed: path=% ferr=% fd=%\n", path, ferr, fd);
        return "", false;
    }

    content, rerr := readFromFileDescriptor(fd);
    closeFileDescriptor(fd);
    if rerr != ioError.None {
        print("read failed: path=% rerr=%\n", path, getErrorString(rerr));
        return content, false;
    }

    return content, true;
}