// does not include async or thread related code because io is meant for
// input output so those will be in their own module. Do note for most of our
// io interface we will be using libc as libc is the most stable abi we can
// use. Libc functions will be in their own separate module.
// We might not need the entire libc functions with in out module so we
// will programatically increase as we go. The naming convention is as follows
// with in the standard library of modules.
// Traits -> Capitalized.
// Functions -> camelCase.

#import "Libc";
#import "Allocator";
#import "String";

// @note: Properties for the function open with in the trait File.

pub enum_flag openKind {
    Append,
    Read,
    Write,
}

// @note this should be a feature.
pub trait File($T) {
    self = *$T;
    // Opens a file with a path on the permssion kind of openKind.
    fn open(self: *Self, path: string, op_kind: openKind) -> (*void, bool);
    // Close the file handle created by open.
    fn close(f: *Self, fd: *void) -> void;

    // This function will read the file path into an dynamic array of chars.
    // For any errors encountered with this function, it will return false.
    // usage: content, ok := fs.read(path); Upon failure, ok is guaranteed to be false.
    // If you want to log the error of where it failed at runtime, you can. Just change
    // log_error = true
    fn read(self: *Self, path: string) -> (StringBuilder, bool);
    fn read(self: *Self, path: *char) -> (StringBuilder, bool);

    // If, instead of a file path, you are reading a file from a handle, while the same
    // logic applies, you just need to provide a valid handle for the file.
    fn read(self: *Self, handle: *void) -> (StringBuilder, bool);

    // Write accepts the path and the content and makes it so that we auto create the
    // path if it does not exist, however, if does, it will override it instead of appending.
    // To make it so that write appends to the file we just do append = true.
    fn write(self: *Self, path: string, content: string, append := false) -> bool;
    // The same function for the above however in this case instead of a file string we give
    // write a handle in this case handle can not be null. It should be coming from `fn open`
    fn write(self: *Self, handle : *void, content: string, append := false) -> bool;
    fn write(self: *Self, handle : *void, content: []char, append := false) -> bool;
}

// Global struct for Libc-funtion implementation. But since all of the libc function and handlers
// come from the extern we just forward declaraing this structure. If you have custom implmentation
// for these sepcific functions just override the default one with
pub struct Libc {}

enum_flag logKind {
    Todo,
    Error,
}

// @note: In the future we should be able to print where in our location we faild so
// equivalent to C __LINE__, __FILE__.
//
// 'msg' is treated as a printf-style format string; 'args' is spread straight into
// printf's own C-ABI variadic tail.
fn die(type: logKind, msg: string, code := #code, args: $T...) -> noreturn {

    if type {
        case logKind.Todo then printf("%s:%d:%d: TODO: ", code.filename, code.line, code.col);
        case logKind.Error then printf("%s:%d:%d: ERROR: ", code.filename, code.line, code.col);
    }

    if msg != "" then printf(msg, ...args);
    exit(1);
}
// This will just print todo
pub fn todo(msg : string) -> void {
    die(logKind.Todo, msg);
}

// @bug: Cry why the fuck is the arg packing #code.
pub fn log_error(msg : string,  args: $T...) -> void {
    die(logKind.Error, msg, ...args);
}

// TODO: log errors if provided
// open_file returns handle if the file is valid otherwise returns null and false.
fn open_file(path: string, mode: openKind) -> (*void, bool) {
    f : *void;
    mod := "rb";

    if mode {
        case .Append { mod = "a+"; }
        case .Read { mod = "rb"; }
        case .Write { mod = "wb"; }
    }
    
    f = fopen(path, mod);
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
    fn open(self: *Self, path: string, op_kind: openKind) -> (*void, bool) {
        handle, ok := open_file(path, op_kind);
        return handle, ok;
    }

    // Close the handle that we got from the 'open.'
    fn close(_libc: *Self, handle : *void) {
        if handle != null {
            fclose(handle);
        }
    }

    // Read will the entire file into an array of chars.
    fn read(self: *Self, path: string) -> (StringBuilder, bool) {
        if path == null || path.count <= 0 {
            log_error("path can not be empty or null\n\r");
            goto error;
        }

        handle, ok := self.open(path, .Read);
        if  !ok {
            log_error("Could not open %s for reading\n\r", path);
            goto error;
        }
        defer self.close(handle);

        fseek(handle, 0, SEEK_END);
        size := ftell(handle);
        fseek(handle, 0, SEEK_SET);

        if size < 0 {
            log_error("ftell: File %s has no content(it is just empty)\n\r", path);
            goto error;
        }


        buffer.reserve(cast(u32)size);

        if fread(buffer.data, 1, cast(u64) buffer.count, handle) < 0 {
            log_error("fread: Could not read file into String Builder");
            goto error;
        }

        return buffer, true;

        label error;
             return buffer, false;
    }

    fn read(self: *Self, path: *char) -> (StringBuilder, bool) {
        buffer : StringBuilder;
        handle, ok := self.open(path, .Read);
        if !ok {
            log_error("Could not open %s for reading\n\r", path);
            goto error;
        }

        fseek(handle, 0, SEEK_END);
        size := ftell(handle);
        fseek(handle, 0, SEEK_SET);
        if size < 0 {
            log_error("ftell: %p is an empty file\n\r", handle);
            goto error;
        }

        buffer.reserve(cast(u32) size);

        if fread(buffer.items, 1, cast(u64) buffer.count, handle) < 0 {
            log_error("fread: Could not read file into String Builder");
            goto error;
        }

        return buffer, true;

        label error;
             return buffer, false;

    }

    fn write(self: *Self, handle : *void, content: string, append := false) -> bool {
        handle, ok := self.open(path, .Write);
        if !ok {
            log_error("Could not open %s for reading\n\r", path);
            return false;
        }
        defer fclose(handle);

        if fwrite(content, 1, cast(u64) content.len, handle) < 0 then return false;
        return true;
    }

    fn write(f: *Self, handle: *void, content: string,
             append : bool = false, log_error : bool = false) -> bool {
        if handle == null then return false;
        if fwrite(&content[0], 1, cast(u64)content.len, handle) < 0 then return false;
        return true;
    }

};
