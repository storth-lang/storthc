pub struct timespec {
    tv_sec: i64;
    tv_nsec: i64;
}

pub struct __sigset_t {
    __val: [16]u64;
}

pub struct iovec {
    iov_base: *void;
    iov_len: u64;
}

pub struct timeval {
    tv_sec: i64;
    tv_usec: i64;
}

pub struct fd_set {
    __fds_bits: [16]i64;
}

pub extern fn select(__nfds: i32, __readfds: *fd_set, __writefds: *fd_set, __exceptfds: *fd_set, __timeout: *timeval) -> i32;

pub extern fn pselect(__nfds: i32, __readfds: *fd_set, __writefds: *fd_set, __exceptfds: *fd_set, __timeout: *timespec, __sigmask: *__sigset_t) -> i32;

pub struct sockaddr {
    sa_family: u16;
    sa_data: [14]char;
}

pub struct sockaddr_storage {
    ss_family: u16;
    __ss_padding: [118]char;
    __ss_align: u64;
}

pub struct msghdr {
    msg_name: *void;
    msg_namelen: u32;
    msg_iov: *iovec;
    msg_iovlen: u64;
    msg_control: *void;
    msg_controllen: u64;
    msg_flags: i32;
}

pub struct cmsghdr {
    cmsg_len: u64;
    cmsg_level: i32;
    cmsg_type: i32;
    // flexible array member '__cmsg_data' omitted
}

pub extern fn __cmsg_nxthdr(__mhdr: *msghdr, __cmsg: *cmsghdr) -> *cmsghdr;

pub struct __kernel_fd_set {
    fds_bits: [16]u64;
}

pub using __kernel_sighandler_t *fn(a0: i32) -> void;

pub struct __kernel_fsid_t {
    val: [2]i32;
}

pub struct linger {
    l_onoff: i32;
    l_linger: i32;
}

pub extern fn socket(__domain: i32, __type: i32, __protocol: i32) -> i32;

pub extern fn socketpair(__domain: i32, __type: i32, __protocol: i32, __fds: *i32) -> i32;

pub extern fn bind(__fd: i32, __addr: *sockaddr, __len: u32) -> i32;

pub extern fn getsockname(__fd: i32, __addr: *sockaddr, __len: *u32) -> i32;

pub extern fn connect(__fd: i32, __addr: *sockaddr, __len: u32) -> i32;

pub extern fn getpeername(__fd: i32, __addr: *sockaddr, __len: *u32) -> i32;

pub extern fn send(__fd: i32, __buf: *void, __n: u64, __flags: i32) -> i64;

pub extern fn recv(__fd: i32, __buf: *void, __n: u64, __flags: i32) -> i64;

pub extern fn sendto(__fd: i32, __buf: *void, __n: u64, __flags: i32, __addr: *sockaddr, __addr_len: u32) -> i64;

pub extern fn recvfrom(__fd: i32, __buf: *void, __n: u64, __flags: i32, __addr: *sockaddr, __addr_len: *u32) -> i64;

pub extern fn sendmsg(__fd: i32, __message: *msghdr, __flags: i32) -> i64;

pub extern fn recvmsg(__fd: i32, __message: *msghdr, __flags: i32) -> i64;

pub extern fn getsockopt(__fd: i32, __level: i32, __optname: i32, __optval: *void, __optlen: *u32) -> i32;

pub extern fn setsockopt(__fd: i32, __level: i32, __optname: i32, __optval: *void, __optlen: u32) -> i32;

pub extern fn listen(__fd: i32, __n: i32) -> i32;

pub extern fn accept(__fd: i32, __addr: *sockaddr, __addr_len: *u32) -> i32;

pub extern fn shutdown(__fd: i32, __how: i32) -> i32;

pub extern fn sockatmark(__fd: i32) -> i32;

pub extern fn isfdtype(__fd: i32, __fdtype: i32) -> i32;

