// Io does not include async or thread related code because io is meant for
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

fn read_all(handle: *void) -> (StringBuilder, bool) {
    buffer : StringBuilder;
    fseek(handle, 0, SEEK_END);
    size := ftell(handle);
    fseek(handle, 0, SEEK_SET);
    if size < 0 {
        log_error("ftell: could not get the size of the file\n\r");
        return buffer, false;
    }
    n := cast(u64) size;
    cap := cast(u32) size;
    buffer.reserve(cap);
    got := fread(buffer.data, 1, n, handle);
    if got != n {
        log_error("fread: Could not read file into String Builder\n\r");
        return buffer, false;
    }
    buffer.count = cap;
    return buffer, true;
}

libc_instance := Libc{};

pub fs := File(Libc) {
    self = &libc_instance;

    fn open(self: *Self, path: string, op_kind: openKind) -> (*void, bool) {
        handle, ok := open_file(path, op_kind);
        return handle, ok;
    }

    fn close(_libc: *Self, handle : *void) -> void {
        if handle != null {
            fclose(handle);
        }
    }

    fn read(self: *Self, path: string) -> (StringBuilder, bool) {
        empty : StringBuilder;
        if path.ptr == null || path.len == 0 {
            log_error("path can not be empty or null\n\r");
            return empty, false;
        }
        handle, ok := open(self, path, openKind.Read);
        if !ok {
            log_error("Could not open {} for reading\n\r", path);
            return empty, false;
        }
        defer close(self, handle);
        content, good := read_all(handle);
        return content, good;
    }

    fn read(self: *Self, path: *char) -> (StringBuilder, bool) {
        as_string := string { ptr = path, len = strlen(path) };
        content, ok := fs.read(as_string);
        return content, ok;
    }

    fn read(self: *Self, handle: *void) -> (StringBuilder, bool) {
        content, ok := read_all(handle);
        return content, ok;
    }

    fn write(self: *Self, path: string, content: string, append := false) -> bool {
        mode := openKind.Write;
        if append then mode = openKind.Append;
        handle, ok := open(self, path, mode);
        if !ok {
            log_error("Could not open {} for writing\n\r", path);
            return false;
        }
        defer close(self, handle);
        n := cast(u64) content.len;
        return fwrite(content.ptr, 1, n, handle) == n;
    }

    fn write(self: *Self, handle: *void, content: string, append := false) -> bool {
        if handle == null then return false;
        n := cast(u64) content.len;
        return fwrite(content.ptr, 1, n, handle) == n;
    }

    fn write(self: *Self, handle: *void, content: []char, append := false) -> bool {
        if handle == null then return false;
        n := cast(u64) content.len;
        return fwrite(content.ptr, 1, n, handle) == n;
    }
};
