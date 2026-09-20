pub struct div_t {
    quot: i32;
    rem: i32;
}

pub struct ldiv_t {
    quot: i64;
    rem: i64;
}

pub struct lldiv_t {
    quot: i64;
    rem: i64;
}

pub extern fn __ctype_get_mb_cur_max() -> u64;
pub extern fn atof(__nptr: *char) -> f64;
pub extern fn atoi(__nptr: *char) -> i32;
pub extern fn atol(__nptr: *char) -> i64;
pub extern fn atoll(__nptr: *char) -> i64;
pub extern fn strtod(__nptr: *char, __endptr: **char) -> f64;
pub extern fn strtof(__nptr: *char, __endptr: **char) -> f32;
pub extern fn strtold(__nptr: *char, __endptr: **char) -> f128;
pub extern fn strtol(__nptr: *char, __endptr: **char, __base: i32) -> i64;
pub extern fn strtoul(__nptr: *char, __endptr: **char, __base: i32) -> u64;
pub extern fn strtoq(__nptr: *char, __endptr: **char, __base: i32) -> i64;
pub extern fn strtouq(__nptr: *char, __endptr: **char, __base: i32) -> u64;
pub extern fn strtoll(__nptr: *char, __endptr: **char, __base: i32) -> i64;
pub extern fn strtoull(__nptr: *char, __endptr: **char, __base: i32) -> u64;
pub extern fn l64a(__n: i64) -> *char;
pub extern fn a64l(__s: *char) -> i64;
pub extern fn random() -> i64;
pub extern fn srandom(__seed: u32) -> void;
pub extern fn initstate(__seed: u32, __statebuf: *char, __statelen: u64) -> *char;
pub extern fn setstate(__statebuf: *char) -> *char;
pub struct random_data {
    fptr: *i32;
    rptr: *i32;
    state: *i32;
    rand_type: i32;
    rand_deg: i32;
    rand_sep: i32;
    end_ptr: *i32;
}

pub extern fn random_r(__buf: *random_data, __result: *i32) -> i32;
pub extern fn srandom_r(__seed: u32, __buf: *random_data) -> i32;
pub extern fn initstate_r(__seed: u32, __statebuf: *char, __statelen: u64, __buf: *random_data) -> i32;
pub extern fn setstate_r(__statebuf: *char, __buf: *random_data) -> i32;
pub extern fn rand() -> i32;
pub extern fn srand(__seed: u32) -> void;
pub extern fn rand_r(__seed: *u32) -> i32;
pub extern fn drand48() -> f64;
pub extern fn erand48(__xsubi: *u16) -> f64;
pub extern fn lrand48() -> i64;
pub extern fn nrand48(__xsubi: *u16) -> i64;
pub extern fn mrand48() -> i64;
pub extern fn jrand48(__xsubi: *u16) -> i64;
pub extern fn srand48(__seedval: i64) -> void;
pub extern fn seed48(__seed16v: *u16) -> *u16;
pub extern fn lcong48(__param: *u16) -> void;
pub struct drand48_data {
    __x: [3]u16;
    __old_x: [3]u16;
    __c: u16;
    __init: u16;
    __a: u64;
}

pub extern fn drand48_r(__buffer: *drand48_data, __result: *f64) -> i32;
pub extern fn erand48_r(__xsubi: *u16, __buffer: *drand48_data, __result: *f64) -> i32;
pub extern fn lrand48_r(__buffer: *drand48_data, __result: *i64) -> i32;
pub extern fn nrand48_r(__xsubi: *u16, __buffer: *drand48_data, __result: *i64) -> i32;
pub extern fn mrand48_r(__buffer: *drand48_data, __result: *i64) -> i32;
pub extern fn jrand48_r(__xsubi: *u16, __buffer: *drand48_data, __result: *i64) -> i32;
pub extern fn srand48_r(__seedval: i64, __buffer: *drand48_data) -> i32;
pub extern fn seed48_r(__seed16v: *u16, __buffer: *drand48_data) -> i32;
pub extern fn lcong48_r(__param: *u16, __buffer: *drand48_data) -> i32;
pub extern fn arc4random() -> u32;
pub extern fn arc4random_buf(__buf: *void, __size: u64) -> void;
pub extern fn arc4random_uniform(__upper_bound: u32) -> u32;
pub extern fn malloc(__size: u64) -> *void;
pub extern fn calloc(__nmemb: u64, __size: u64) -> *void;
pub extern fn realloc(__ptr: *void, __size: u64) -> *void;
pub extern fn free(__ptr: *void) -> void;
pub extern fn reallocarray(__ptr: *void, __nmemb: u64, __size: u64) -> *void;
pub extern fn valloc(__size: u64) -> *void;
pub extern fn posix_memalign(__memptr: **void, __alignment: u64, __size: u64) -> i32;
pub extern fn aligned_alloc(__alignment: u64, __size: u64) -> *void;
pub extern fn abort() -> void;
pub extern fn atexit(__func: *fn() -> void) -> i32;
pub extern fn at_quick_exit(__func: *fn() -> void) -> i32;
pub extern fn on_exit(__func: *fn(a0: i32, a1: *void) -> void, __arg: *void) -> i32;
pub extern fn exit(__status: i32) -> void;
pub extern fn quick_exit(__status: i32) -> void;
pub extern fn _Exit(__status: i32) -> void;
pub extern fn getenv(__name: *char) -> *char;
pub extern fn putenv(__string: *char) -> i32;
pub extern fn setenv(__name: *char, __value: *char, __replace: i32) -> i32;
pub extern fn unsetenv(__name: *char) -> i32;
pub extern fn clearenv() -> i32;
pub extern fn mktemp(__template: *char) -> *char;
pub extern fn mkstemp(__template: *char) -> i32;
pub extern fn mkstemps(__template: *char, __suffixlen: i32) -> i32;
pub extern fn mkdtemp(__template: *char) -> *char;
pub extern fn system(__command: *char) -> i32;
pub extern fn realpath(__name: *char, __resolved: *char) -> *char;
pub using __compar_fn_t *fn(a0: *void, a1: *void) -> i32;
pub extern fn bsearch(__key: *void, __base: *void, __nmemb: u64, __size: u64, __compar: *fn(a0: *void, a1: *void) -> i32) -> *void;
pub extern fn qsort(__base: *void, __nmemb: u64, __size: u64, __compar: *fn(a0: *void, a1: *void) -> i32) -> void;
pub extern fn abs(__x: i32) -> i32;
pub extern fn labs(__x: i64) -> i64;
pub extern fn llabs(__x: i64) -> i64;
// pub extern fn div(__numer: i32, __denom: i32) -> div_t;
pub extern fn ldiv(__numer: i64, __denom: i64) -> ldiv_t;
pub extern fn lldiv(__numer: i64, __denom: i64) -> lldiv_t;
pub extern fn ecvt(__value: f64, __ndigit: i32, __decpt: *i32, __sign: *i32) -> *char;
pub extern fn fcvt(__value: f64, __ndigit: i32, __decpt: *i32, __sign: *i32) -> *char;
pub extern fn gcvt(__value: f64, __ndigit: i32, __buf: *char) -> *char;
pub extern fn qecvt(__value: f128, __ndigit: i32, __decpt: *i32, __sign: *i32) -> *char;
pub extern fn qfcvt(__value: f128, __ndigit: i32, __decpt: *i32, __sign: *i32) -> *char;
pub extern fn qgcvt(__value: f128, __ndigit: i32, __buf: *char) -> *char;
pub extern fn ecvt_r(__value: f64, __ndigit: i32, __decpt: *i32, __sign: *i32, __buf: *char, __len: u64) -> i32;
pub extern fn fcvt_r(__value: f64, __ndigit: i32, __decpt: *i32, __sign: *i32, __buf: *char, __len: u64) -> i32;
pub extern fn qecvt_r(__value: f128, __ndigit: i32, __decpt: *i32, __sign: *i32, __buf: *char, __len: u64) ->i32;
pub extern fn qfcvt_r(__value: f128, __ndigit: i32, __decpt: *i32, __sign: *i32, __buf: *char, __len: u64) ->i32;
pub extern fn mblen(__s: *char, __n: u64) -> i32;
pub extern fn mbtowc(__pwc: *i32, __s: *char, __n: u64) -> i32;
pub extern fn wctomb(__s: *char, __wchar: i32) -> i32;
pub extern fn mbstowcs(__pwcs: *i32, __s: *char, __n: u64) -> u64;
pub extern fn wcstombs(__s: *char, __pwcs: *i32, __n: u64) -> u64;
pub extern fn rpmatch(__response: *char) -> i32;
pub extern fn getsubopt(__optionp: **char, __tokens: **char, __valuep: **char) -> i32;
pub extern fn getloadavg(__loadavg: *f64, __nelem: i32) -> i32;
