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

pub enum __socket_type {
    STREAM := 1,
    DGRAM := 2,
    RAW := 3,
    RDM := 4,
    SEQPACKET := 5,
    DCCP := 6,
    PACKET := 10,
    CLOEXEC := 524288,
    NONBLOCK := 2048,
}

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

pub struct in_addr {
    s_addr: u32;
}

pub struct ip_opts {
    ip_dst: in_addr;
    ip_opts: [40]char;
}

pub struct in_pktinfo {
    ipi_ifindex: i32;
    ipi_spec_dst: in_addr;
    ipi_addr: in_addr;
}

// Todo
pub struct in6_addr {
    // __in6_u: struct #union {
    __in6_u: struct {
        __u6_addr8: [16]u8;
        __u6_addr16: [8]u16;
        __u6_addr32: [4]u32;
    }
}

pub extern in6addr_any: in6_addr;

pub extern in6addr_loopback: in6_addr;

pub struct sockaddr_in {
    sin_family: u16;
    sin_port: u16;
    sin_addr: in_addr;
    sin_zero: [8]u8;
}

pub struct sockaddr_in6 {
    sin6_family: u16;
    sin6_port: u16;
    sin6_flowinfo: u32;
    sin6_addr: in6_addr;
    sin6_scope_id: u32;
}

pub struct ip_mreq {
    imr_multiaddr: in_addr;
    imr_interface: in_addr;
}

pub struct ip_mreqn {
    imr_multiaddr: in_addr;
    imr_address: in_addr;
    imr_ifindex: i32;
}

pub struct ip_mreq_source {
    imr_multiaddr: in_addr;
    imr_interface: in_addr;
    imr_sourceaddr: in_addr;
}

pub struct ipv6_mreq {
    ipv6mr_multiaddr: in6_addr;
    ipv6mr_interface: u32;
}

pub struct group_req {
    gr_interface: u32;
    gr_group: sockaddr_storage;
}

pub struct group_source_req {
    gsr_interface: u32;
    gsr_group: sockaddr_storage;
    gsr_source: sockaddr_storage;
}

pub struct ip_msfilter {
    imsf_multiaddr: in_addr;
    imsf_interface: in_addr;
    imsf_fmode: u32;
    imsf_numsrc: u32;
    imsf_slist: [1]in_addr;
}

pub struct group_filter {
    gf_interface: u32;
    gf_group: sockaddr_storage;
    gf_fmode: u32;
    gf_numsrc: u32;
    gf_slist: [1]sockaddr_storage;
}

pub extern fn ntohl(__netlong: u32) -> u32;

pub extern fn ntohs(__netshort: u16) -> u16;

pub extern fn htonl(__hostlong: u32) -> u32;

pub extern fn htons(__hostshort: u16) -> u16;

pub extern fn bindresvport(__sockfd: i32, __sock_in: *sockaddr_in) -> i32;

pub extern fn bindresvport6(__sockfd: i32, __sock_in: *sockaddr_in6) -> i32;

