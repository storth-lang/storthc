using FILE *void;
struct _G_fpos_t {
    __pos: i64;
    __state: __mbstate_t;
}

struct __mbstate_t {
    __count: i32;
    __value: [4]u8;
}

using va_list *void;

using off_t i64;

using ssize_t i64;

using fpos_t _G_fpos_t;

using cookie_io_functions_t *void;

pub extern fn remove(__filename: *char) -> i32;
pub extern fn rename(__old: *char, __new: *char) -> i32;
pub extern fn renameat(__oldfd: i32, __old: *char, __newfd: i32, __new: *char) -> i32;
pub extern fn fclose(__stream: *FILE) -> i32;
pub extern fn tmpfile() -> *FILE;
pub extern fn tmpnam(a0: *char) -> *char;
pub extern fn tmpnam_r(__s: *char) -> *char;
pub extern fn tempnam(__dir: *char, __pfx: *char) -> *char;
pub extern fn fflush(__stream: *FILE) -> i32;
pub extern fn fflush_unlocked(__stream: *FILE) -> i32;
pub extern fn fopen(__filename: *char, __modes: *char) -> *FILE;
pub extern fn freopen(__filename: *char, __modes: *char, __stream: *FILE) -> *FILE;
pub extern fn fdopen(__fd: i32, __modes: *char) -> *FILE;
pub extern fn fopencookie(__magic_cookie: *void, __modes: *char, __io_funcs: cookie_io_functions_t) -> *FILE;
pub extern fn fmemopen(__s: *void, __len: u64, __modes: *char) -> *FILE;
pub extern fn open_memstream(__bufloc: **char, __sizeloc: *u64) -> *FILE;
pub extern fn setbuf(__stream: *FILE, __buf: *char) -> void;
pub extern fn setvbuf(__stream: *FILE, __buf: *char, __modes: i32, __n: u64) -> i32;
pub extern fn setbuffer(__stream: *FILE, __buf: *char, __size: u64) -> void;
pub extern fn setlinebuf(__stream: *FILE) -> void;
pub extern fn fprintf(__stream: *FILE, __format: *char, ...) -> i32;
pub extern fn printf(__format: *char, ...) -> i32;
pub extern fn sprintf(__s: *char, __format: *char, ...) -> i32;
pub extern fn vfprintf(__s: *FILE, __format: *char, __arg: **void) -> i32;
pub extern fn vprintf(__format: *char, __arg: **void) -> i32;
pub extern fn vsprintf(__s: *char, __format: *char, __arg: **void) -> i32;
pub extern fn snprintf(__s: *char, __maxlen: u64, __format: *char, ...) -> i32;
pub extern fn vsnprintf(__s: *char, __maxlen: u64, __format: *char, __arg: **void) -> i32;
pub extern fn vasprintf(__ptr: **char, __f: *char, __arg: **void) -> i32;
pub extern fn __asprintf(__ptr: **char, __fmt: *char, ...) -> i32;
pub extern fn asprintf(__ptr: **char, __fmt: *char, ...) -> i32;
pub extern fn vdprintf(__fd: i32, __fmt: *char, __arg: **void) -> i32;
pub extern fn dprintf(__fd: i32, __fmt: *char, ...) -> i32;
pub extern fn fscanf(__stream: *FILE, __format: *char, ...) -> i32;
pub extern fn scanf(__format: *char, ...) -> i32;
pub extern fn sscanf(__s: *char, __format: *char, ...) -> i32;
pub extern fn fscanf(__stream: *FILE, __format: *char, ...) -> i32;
pub extern fn scanf(__format: *char, ...) -> i32;
pub extern fn sscanf(__s: *char, __format: *char, ...) -> i32;
pub extern fn vfscanf(__s: *FILE, __format: *char, __arg: **void) -> i32;
pub extern fn vscanf(__format: *char, __arg: **void) -> i32;
pub extern fn vsscanf(__s: *char, __format: *char, __arg: **void) -> i32;
pub extern fn vfscanf(__s: *FILE, __format: *char, __arg: **void) -> i32;
pub extern fn vscanf(__format: *char, __arg: **void) -> i32;
pub extern fn vsscanf(__s: *char, __format: *char, __arg: **void) -> i32;
pub extern fn fgetc(__stream: *FILE) -> i32;
pub extern fn getc(__stream: *FILE) -> i32;
pub extern fn getchar() -> i32;
pub extern fn getc_unlocked(__stream: *FILE) -> i32;
pub extern fn getchar_unlocked() -> i32;
pub extern fn fgetc_unlocked(__stream: *FILE) -> i32;
pub extern fn fputc(__c: i32, __stream: *FILE) -> i32;
pub extern fn putc(__c: i32, __stream: *FILE) -> i32;
pub extern fn putchar(__c: i32) -> i32;
pub extern fn fputc_unlocked(__c: i32, __stream: *FILE) -> i32;
pub extern fn putc_unlocked(__c: i32, __stream: *FILE) -> i32;
pub extern fn putchar_unlocked(__c: i32) -> i32;
pub extern fn getw(__stream: *FILE) -> i32;
pub extern fn putw(__w: i32, __stream: *FILE) -> i32;
pub extern fn fgets(__s: *char, __n: i32, __stream: *FILE) -> *char;
pub extern fn __getdelim(__lineptr: **char, __n: *u64, __delimiter: i32, __stream: *FILE) -> i64;
pub extern fn getdelim(__lineptr: **char, __n: *u64, __delimiter: i32, __stream: *FILE) -> i64;
pub extern fn getline(__lineptr: **char, __n: *u64, __stream: *FILE) -> i64;
pub extern fn fputs(__s: *char, __stream: *FILE) -> i32;
pub extern fn puts(__s: *char) -> i32;
pub extern fn ungetc(__c: i32, __stream: *FILE) -> i32;
pub extern fn fread(__ptr: *void, __size: u64, __n: u64, __stream: *FILE) -> u64;
pub extern fn fwrite(__ptr: *void, __size: u64, __n: u64, __s: *FILE) -> void;
pub extern fn fread_unlocked(__ptr: *void, __size: u64, __n: u64, __stream: *FILE) -> u64;
pub extern fn fwrite_unlocked(__ptr: *void, __size: u64, __n: u64, __stream: *FILE) -> u64;
pub extern fn fseek(__stream: *FILE, __off: i64, __whence: i32) -> i32;
pub extern fn ftell(__stream: *FILE) -> i64;
pub extern fn rewind(__stream: *FILE) -> void;
pub extern fn fseeko(__stream: *FILE, __off: i64, __whence: i32) -> i32;
pub extern fn ftello(__stream: *FILE) -> i64;
pub extern fn fgetpos(__stream: *FILE, __pos: *fpos_t) -> i32;
pub extern fn fsetpos(__stream: *FILE, __pos: *fpos_t) -> i32;
pub extern fn clearerr(__stream: *FILE) -> void;
pub extern fn feof(__stream: *FILE) -> i32;
pub extern fn ferror(__stream: *FILE) -> i32;
pub extern fn clearerr_unlocked(__stream: *FILE) -> void;
pub extern fn feof_unlocked(__stream: *FILE) -> i32;
pub extern fn ferror_unlocked(__stream: *FILE) -> i32;
pub extern fn perror(__s: *char) -> void;
pub extern fn fileno(__stream: *FILE) -> i32;
pub extern fn fileno_unlocked(__stream: *FILE) -> i32;
pub extern fn pclose(__stream: *FILE) -> i32;
pub extern fn popen(__command: *char, __modes: *char) -> *FILE;
pub extern fn ctermid(__s: *char) -> *char;
pub extern fn flockfile(__stream: *FILE) -> void;
pub extern fn ftrylockfile(__stream: *FILE) -> i32;
pub extern fn funlockfile(__stream: *FILE) -> void;
pub extern fn __uflow(a0: *FILE) -> i32;
pub extern fn __overflow(a0: *FILE, a1: i32) -> i32;