
pub _DIRENT_H :: 1;
pub _DIRENT_MATCHES_DIRENT64 :: 1;
pub PTHREAD_STACK_MIN :: 16384;
pub MAXNAMLEN :: 255;
pub DT_UNKNOWN :: 0;
pub DT_FIFO :: 1;
pub DT_CHR :: 2;
pub DT_DIR :: 4;
pub DT_BLK :: 6;
pub DT_REG :: 8;
pub DT_LNK :: 10;
pub DT_SOCK :: 12;
pub DT_WHT :: 14;

pub using DIR *void;
pub struct dirent {
    d_ino: u64;
    d_off: i64;
    d_reclen: u16;
    d_type: u8;
    d_name: [256]char;
}

pub extern fn closedir(__dirp: *DIR) -> i32;

pub extern fn opendir(__name: *char) -> *DIR;

pub extern fn fdopendir(__fd: i32) -> *DIR;

pub extern fn readdir(__dirp: *DIR) -> *dirent;

pub extern fn readdir_r(__dirp: *DIR, __entry: *dirent, __result: **dirent) -> i32;

pub extern fn rewinddir(__dirp: *DIR) -> void;

pub extern fn seekdir(__dirp: *DIR, __pos: i64) -> void;

pub extern fn telldir(__dirp: *DIR) -> i64;

pub extern fn dirfd(__dirp: *DIR) -> i32;

pub extern fn scandir(__dir: *char, __namelist: ***dirent, __selector: *fn(a0: *dirent) -> i32, __cmp: *fn(a0: **dirent, a1: **dirent) -> i32) -> i32;

pub extern fn alphasort(__e1: **dirent, __e2: **dirent) -> i32;

pub extern fn getdirentries(__fd: i32, __buf: *char, __nbytes: u64, __basep: *i64) -> i64;

