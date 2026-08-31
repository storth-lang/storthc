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

// pub fn openFileDescriptor(path: string, mode := openMode.readAndWrite) -> (i32, ioError);
// Wrapper around 'sys_open'(os module) for us to have language error code instead of errno
// libc error message and till we need all the error messages just keep things minimal.
pub fn openFileDescriptor(path: string, mode := openMode.readAndWrite) -> (i32, ioError) {
    flags := flags_for_mode(mode);
    fd := sys_open(path, flags, 420);
    if fd < 0 {
        if mode == openMode.readOnly then return 0, ioError.OpenErrorR;
        if mode == openMode.writeOnly then return 0, ioError.OpenErrorW;
        return 0, ioError.OpenErrorRW;
    }
    return fd, ioError.None;
}

// pub fn closeFileDescriptor(fd: i32) -> void;
// Wrapper around 'sys_close'(os module) just for consistency and also we check if the fd > 0.
// So it will not run unless the fd exists getting rid of us handleing error message.
pub fn closeFileDescriptor(fd: i32) -> void {
    if fd > 0 then sys_close(fd);
}

// pub fn writeToFileDescriptor(fd: i32, data: string) -> (i64, ioError);
// Wrapper around 'sys_write'(os module) for the same reason as openFileDescriptor.
// This will accept a string in normal write syscall we will be acceptin till what count.
// In most cases here since string is sized there is no reason for us to pass the length of the string.
// as a parameter. However if the buffer is a pointer it is recommended to just create a new string
// using str_from_raw instead of directly interacting with the syscall unless you are a chad dev.
pub fn writeToFileDescriptor(fd: i32, data: string) -> (i64, ioError) {
    p := data.ptr;
    n := data.len;
    written := sys_write(fd, p, n #as i32);
    if written < 0 {
        return 0, ioError.WriteError;
    }
    return written #as i64, ioError.None;
}

// pub fn readFromFileDescriptor(fd: i32) -> (string, ioError);
// This will read some amount of content from the file descriptor and return us a string.
// The string that will be returned here is on the heap do note that. This is a much more rigourous
// function that 'sys_read'(os module). Because sys_read does not do any memory allocation here we infact
// have memory allocation.
pub fn readFromFileDescriptor(fd: i32) -> (string, ioError) {
    cap : i64 = 65536;
    buf, merr := alloc(cap);
    if merr != memoryError.None then return "", ioError.ReadError;

    total := 0;
    while true {
        base := buf #as *u8;
        p := base + total;
        remaining := cap #as i32 - total;
        n := sys_read(fd, p #as *char, remaining);
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

// pub fn readEntireFile(path: string) -> (string, bool) {
// This is a wrapper combining both openFIledescriptor, readFromfiledescriptor and
// closeFiledescriptor and directly returning us the string and loggin the error message.
// In most cases we just want to read a file we do not always want to prepare the 'file descriptor'
pub fn readEntireFile(path: string) -> (string, bool) {
    fd, ferr := openFileDescriptor(path, openMode.readOnly);
    if ferr != ioError.None {
        eprint("open failed: path=% ferr=% fd=%\n", path, ferr, fd);
        return "", false;
    }

    content, rerr := readFromFileDescriptor(fd);
    closeFileDescriptor(fd);
    if rerr != ioError.None {
        eprint("read failed: path=% rerr=%\n", path, getErrorString(rerr));
        return content, false;
    }

    return content, true;
}

// pub fn writeEntireFile(path : string, str: string) -> bool;
// This is a wrapper combining both openFIledescriptor, readFromfiledescriptor and
// closeFiledescriptor and directly writing to the fd.
pub fn writeEntireFile(path : string, str: string) -> bool {
    fd, ferr := openFileDescriptor(path, openMode.writeOnly);
    if ferr != ioError.None {
        eprint("open failed: path=% ferr=% fd=%\n", path, ferr, fd);
        return false;
    }
    errno, err := writeToFileDescriptor(fd, str);
    closeFileDescriptor(fd);
    if err != ioError.None {
        eprint("read failed: path=% rerr=%\n", path, getErrorString(err));
        return false;
    }

    return true;
}

// pub fn renameFile(file1: string, file2: string) -> bool;
// This will rename an existing 'file1' to 'file2'. So if
// you have a file renameFile(foo, bar); This obviously will not check if 'foo' is a
// directory or not.
pub fn renameFile(file1: string, file2: string) -> bool {
    err := sys_rename(file1, file2);
    if err != 0 then {
        return false;
    }

    return true;
}

// pub fn createDirectoryIfNotExist(dir_name: string) -> bool;
// This function will create a directory if it does not exist.
pub fn createDirectoryIfNotExist(dir_name: string) -> bool {
    err := sys_mkdir(dir_name, 0644);
    if err == 17 {
        eprint("Directory % already exists\n", dir_name);
        return false;
    }

    return true;
}