// @note: This is the official io interface of the language you can use
// to read files to write files. Log some stuff as well. This io interface
// does not include async or thread related code because io is meant for
// input output so those will be in their own module. Do note for most of our
// io interface we will be using libc as libc is the most stable abi we can
// use. Libc functions will be in their own separate module with in libc.
// We might not need the entire libc functions with in out module so we
// will programatically increase as we go. The naming convention is as follows
// with in the standard library of modules.
// Traits -> Capitalized.
// Functions -> camelCase.

#import "libc";
#import "os";

// @note: Properties for the function open with in the trait File.

pub enum_flag openKind {
    ReadAndWrite,
    Append,
    Read,
    Write,
}

// @note: Properties for the function open with in the trait File.
pub enum_flag stream {
    Stdout,
    Stderr,
}

// @note this should be a feature.
pub trait File($T) {
    self = *$T;
    // Opens a file with a path on the permssion kind of openKind.
    fn open(_libc: *Libc, path: string, op_kind : openKind, buffered : bool = false) -> (*void, bool);
    // Close the file handle created by open.
    fn close(f: *$T, fd: *void) -> void;

    // This function will read the file path into an dynamic array of chars.
    // For any errors encountered with this function, it will return false.
    // usage: content, ok := fs.read(path); Upon failure, ok is guaranteed to be false.
    // If you want to log the error of where it failed at runtime, you can. Just change
    // log_error = true
    fn read(f: *$T, path: string, log_error : bool = false) -> ([..]char, bool);

    // If, instead of a file path, you are reading a file from a handle, while the same
    // logic applies, you just need to provide a valid handle for the file.
    fn read(f: *$T, handle: *void, log_error : bool = false) -> ([..]char, bool);

    // Write accepts the path and the content and makes it so that we auto create the
    // path if it does not exist, however, if does, it will override it instead of appending.
    // To make it so that write appends to the file we just do append = true.
    fn write(f: *$T, path: string, content: string,
             append : bool = false, log_error : bool = false) -> bool;
    // The same function for the above however in this case instead of a file string we give
    // write a handle in this case handle can not be null. It should be coming from `fn open`
    fn write(f: *$T, handle : *void, content: string,
             append : bool = false, log_error : bool = false) -> bool;
}

// Global struct for Libc-funtion implementation. But since all of the libc function and handlers
// come from the extern we just forward declaraing this structure. If you have custom implmentation
// for these sepcific functions just override the default one with libc.
pub struct Libc {}

enum_flag logKind {
    Todo,
    Error,
}

// @note: In the future we should be able to print where in our location we faild so
// equivalent to C __LINE__, __FILE__.
//
// 'msg' is treated as a printf-style format string; 'args' is spread straight into
// libc.printf's own C-ABI variadic tail.
fn die(type: logKind, msg: string, code := #code, args: $T...) -> void {

    if type {
        case logKind.Todo then libc.printf("%s:%d:%d: TODO: ", code.filename, code.line, code.col);
        case logKind.Error then libc.printf("%s:%d:%d: ERROR: ", code.filename, code.line, code.col);
    }

    if msg != "" then libc.printf(msg, args...);
    libc.exit(1);
}
// This will just print todo
pub fn todo(msg : string) -> void {
    die(logKind.Todo, msg);
}

// @bug: Cry why the fuck is the arg packing #code.
pub fn log_error(msg : string,  args: $T...) -> void {
    die(logKind.Error, msg, args...);
}

// TODO: log errors if provided
// open_file returns handle if the file is valid otherwise returns null and false.
fn open_file(path: string, mode: openKind, buffered: bool) -> (*void, bool) {
    if buffered then todo("Not implemented yet");
    f : *void;

    if mode {
        case openKind.ReadAndWrite { f = libc.fopen(path, "w+"); }
        case openKind.Append { f = libc.fopen(path, "a+"); }
        case openKind.Read { f = libc.fopen(path, "rb"); }
        case openKind.Write { f = libc.fopen(path, "wb"); }
    }

    if f == null then goto error;

    return f, true;

    label error;
        return null, false;
}

libc_instance := Libc{};

// Public file_system interface
pub fs := File(Libc) {
    self = &libc_instance;

    // Creates a file if it does not exist. However if it does it just repoens.
    fn open(libc: *Libc, path: string, op_kind : openKind, buffered := false) -> (*void, bool) {
        handle, ok := open_file(path, op_kind, buffered);
        return handle, ok;
    }

    // Close the handle that we got from the 'open.'
    fn close(_libc: *Libc, handle : *void) {
        if handle != null {
            libc.fclose(handle);
        }
    }

    // Read will the entire file into an array of chars.
    fn read(libc: *Libc, path: string, log_err := false) -> ([..]char, bool) {
        buffer : [..]char;
        if &path[0] == null || path == "" {
            if log_err then log_error("path can not be empty or null\n\r");
            goto error;
        }

        handle := libc.fopen(&path[0], "rb");
        if handle == null {
            if log_err then log_error("Could not open %s for reading\n\r", path);
            goto error;
        }
        defer libc.fclose(handle);

        libc.fseek(handle, 0, libc.SEEK_END);
        size := libc.ftell(handle);
        libc.fseek(handle, 0, libc.SEEK_SET);

        if size < 0 {
            if log_err then log_error("libc.ftell: File %s has no content(it is just empty)\n\r", path);
            goto error;
        }

        buffer.count = cast(i64) size;
        buffer.items = libc.realloc(buffer.items,
                       cast(u64) type_info(buffer.items).size * cast(u64) buffer.count);

        if buffer.items == null {
            if log_err then log_error("libc.realloc: Could not reallocate memory for dynamic array");
            goto error;
        }

        if libc.fread(buffer.items, 1, cast(u64) buffer.count, handle) < 0 {
            if log_err then log_error("libc.fread: Could not read file into dynamic array");
            goto error;
        }

        return buffer, true;

        label error;
             return buffer, false;
    }

    fn read(_f: *Libc, handle: *void, log_err := false) -> ([..]char, bool) {
        buffer : [..]char;
        if handle == null {
            if log_err then log_error("file handler can not be null\n\r");
            goto error;
        }

        libc.fseek(handle, 0, libc.SEEK_END);
        size := libc.ftell(handle);
        libc.fseek(handle, 0, libc.SEEK_SET);
        if size < 0 {
            if log_err then log_error("libc.ftell: %p is an empty file\n\r", handle);
            goto error;
        }

        buffer.count = cast(i64) size;
        buffer.items = libc.realloc(buffer.items,
                       cast(u64)type_info(buffer.items).size * cast(u64) buffer.count);

        if buffer.items == null {
            if log_err then log_error("libc.realloc: Could not reallocate memory for dynamic array\n\r");
            goto error;
        }

        if libc.fread(buffer.items, 1, cast(u64) buffer.count, handle) < 0 {
            if log_err then log_error("libc.fread: Could not read file into dynamic array");
            goto error;
        }

        return buffer, true;

        label error;
             return buffer, false;

    }

    fn write(f: *Libc, path: string, content: string,
             append : bool = false, log_error : bool = false) -> bool {
        if &path[0] == null then return false;
        handle := libc.fopen(&path[0], "wb");
        if handle == null then return false;
        defer libc.fclose(handle);

        if libc.fwrite(&content[0], 1, cast(u64) content.len, handle) < 0 then return false;
        return true;
    }

    fn write(f: *Libc, handle: *void, content: string,
             append : bool = false, log_error : bool = false) -> bool {
        if handle == null then return false;
        if libc.fwrite(&content[0], 1, cast(u64)content.len, handle) < 0 then return false;
        return true;
    }

};

pub fn create_directory_if_not_exist(dir_path: string, perm: u32) -> bool {
    stats: stat;
    if stat(dir_path, &stats) == 0 {
        S_IFMT : u32 : 0xF000;
        S_IDIR : u32 : 0x4000;
        // @note: 0040000 octal (0x4000) is the bitmask flag for a directory
        // 0170000 octal (0xF000) is the file type mask ()
        file_type := stats.st_mode & S_IFMT;
        return file_type == S_IDIR;
    }

    if mkdir(dir_path, perm) == 0 then return true;
    return false;
}

#import "libc";
#import "os";
