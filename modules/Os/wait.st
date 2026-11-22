
pub _SYS_WAIT_H :: 1;
pub _BITS_SIGNUM_GENERIC_H :: 1;
pub SIG_ERR :: -1;
pub SIG_DFL :: 0;
pub SIG_IGN :: 1;
pub SIGINT :: 2;
pub SIGILL :: 4;
pub SIGABRT :: 6;
pub SIGFPE :: 8;
pub SIGSEGV :: 11;
pub SIGTERM :: 15;
pub SIGHUP :: 1;
pub SIGQUIT :: 3;
pub SIGTRAP :: 5;
pub SIGKILL :: 9;
pub SIGPIPE :: 13;
pub SIGALRM :: 14;
pub SIGIOT :: 6;
pub _BITS_SIGNUM_ARCH_H :: 1;
pub SIGSTKFLT :: 16;
pub SIGPWR :: 30;
pub SIGBUS :: 7;
pub SIGSYS :: 31;
pub SIGURG :: 23;
pub SIGSTOP :: 19;
pub SIGTSTP :: 20;
pub SIGCONT :: 18;
pub SIGCHLD :: 17;
pub SIGTTIN :: 21;
pub SIGTTOU :: 22;
pub SIGPOLL :: 29;
pub SIGXFSZ :: 25;
pub SIGXCPU :: 24;
pub SIGVTALRM :: 26;
pub SIGPROF :: 27;
pub SIGUSR1 :: 10;
pub SIGUSR2 :: 12;
pub SIGWINCH :: 28;
pub SIGIO :: 29;
pub SIGCLD :: 17;
pub _BITS_SIGINFO_ARCH_H :: 1;
pub _BITS_SIGINFO_CONSTS_H :: 1;
pub _BITS_SIGEVENT_CONSTS_H :: 1;
pub _BITS_SIGACTION_H :: 1;
pub SA_NOCLDSTOP :: 1;
pub SA_NOCLDWAIT :: 2;
pub SA_SIGINFO :: 4;
pub SA_ONSTACK :: 134217728;
pub SA_RESTART :: 268435456;
pub SA_NODEFER :: 1073741824;
pub SA_RESETHAND :: cast(u32) 2147483648;
pub SA_INTERRUPT :: 536870912;
pub SA_NOMASK :: 1073741824;
pub SA_ONESHOT :: cast(u32) 2147483648;
pub SA_STACK :: 134217728;
pub SIG_BLOCK :: 0;
pub SIG_UNBLOCK :: 1;
pub SIG_SETMASK :: 2;
pub _BITS_SIGCONTEXT_H :: 1;
pub FP_XSTATE_MAGIC1 :: 1179670611;
pub FP_XSTATE_MAGIC2 :: 1179670597;
pub _SYS_UCONTEXT_H :: 1;
pub _BITS_SIGSTACK_H :: 1;
pub MINSIGSTKSZ :: 2048;
pub SIGSTKSZ :: 8192;
pub _BITS_SS_FLAGS_H :: 1;
pub _BITS_SIGTHREAD_H :: 1;
pub WNOHANG :: 1;
pub WUNTRACED :: 2;
pub WSTOPPED :: 2;
pub WEXITED :: 4;
pub WCONTINUED :: 8;
pub WNOWAIT :: 16777216;
pub WAIT_ANY :: -1;
pub WAIT_MYPGRP :: 0;
pub SI_ASYNCNL :: -60;
pub SI_DETHREAD :: -7;
pub SI_TKILL :: -6;
pub SI_SIGIO :: -5;
pub SI_ASYNCIO :: -4;
pub SI_MESGQ :: -3;
pub SI_TIMER :: -2;
pub SI_QUEUE :: -1;
pub SI_USER :: 0;
pub SI_KERNEL :: 128;
pub ILL_ILLOPC :: 1;
pub ILL_ILLOPN :: 2;
pub ILL_ILLADR :: 3;
pub ILL_ILLTRP :: 4;
pub ILL_PRVOPC :: 5;
pub ILL_PRVREG :: 6;
pub ILL_COPROC :: 7;
pub ILL_BADSTK :: 8;
pub ILL_BADIADDR :: 9;
pub FPE_INTDIV :: 1;
pub FPE_INTOVF :: 2;
pub FPE_FLTDIV :: 3;
pub FPE_FLTOVF :: 4;
pub FPE_FLTUND :: 5;
pub FPE_FLTRES :: 6;
pub FPE_FLTINV :: 7;
pub FPE_FLTSUB :: 8;
pub FPE_FLTUNK :: 14;
pub FPE_CONDTRAP :: 15;
pub SEGV_MAPERR :: 1;
pub SEGV_ACCERR :: 2;
pub SEGV_BNDERR :: 3;
pub SEGV_PKUERR :: 4;
pub SEGV_ACCADI :: 5;
pub SEGV_ADIDERR :: 6;
pub SEGV_ADIPERR :: 7;
pub SEGV_MTEAERR :: 8;
pub SEGV_MTESERR :: 9;
pub SEGV_CPERR :: 10;
pub BUS_ADRALN :: 1;
pub BUS_ADRERR :: 2;
pub BUS_OBJERR :: 3;
pub BUS_MCEERR_AR :: 4;
pub BUS_MCEERR_AO :: 5;
pub CLD_EXITED :: 1;
pub CLD_KILLED :: 2;
pub CLD_DUMPED :: 3;
pub CLD_TRAPPED :: 4;
pub CLD_STOPPED :: 5;
pub CLD_CONTINUED :: 6;
pub POLL_IN :: 1;
pub POLL_OUT :: 2;
pub POLL_MSG :: 3;
pub POLL_ERR :: 4;
pub POLL_PRI :: 5;
pub POLL_HUP :: 6;
pub SIGEV_SIGNAL :: 0;
pub SIGEV_NONE :: 1;
pub SIGEV_THREAD :: 2;
pub SIGEV_THREAD_ID :: 4;
pub SS_ONSTACK :: 1;
pub SS_DISABLE :: 2;

pub using rusage *void;
pub struct sigval #union {
    sival_int: i32;
    sival_ptr: *void;
}

pub enum idtype_t {
    ALL := 0,
    PID := 1,
    PGID := 2,
    PIDFD := 3,
}

pub struct __sigset_t {
    __val: [16]u64;
}

pub struct timespec {
    tv_sec: i64;
    tv_nsec: i64;
}

pub struct siginfo_t {
    si_signo: i32;
    si_errno: i32;
    si_code: i32;
    __pad0: i32;
    _sifields: struct #union {
        _pad: [28]i32;
        _kill: struct {
            si_pid: i32;
            si_uid: u32;
        }
        _timer: struct {
            si_tid: i32;
            si_overrun: i32;
            si_sigval: __sigval_t;
        }
        _rt: struct {
            si_pid: i32;
            si_uid: u32;
            si_sigval: __sigval_t;
        }
        _sigchld: struct {
            si_pid: i32;
            si_uid: u32;
            si_status: i32;
            si_utime: i64;
            si_stime: i64;
        }
        _sigfault: struct {
            si_addr: *void;
            si_addr_lsb: i16;
            _bounds: struct #union {
                _addr_bnd: struct {
                    _lower: *void;
                    _upper: *void;
                }
                _pkey: u32;
            }
        }
        _sigpoll: struct {
            si_band: i64;
            si_fd: i32;
        }
        _sigsys: struct {
            _call_addr: *void;
            _syscall: i32;
            _arch: u32;
        }
    }
}

pub struct sigstack {
    ss_sp: *void;
    ss_onstack: i32;
}

pub struct stack_t {
    ss_sp: *void;
    ss_flags: i32;
    ss_size: u64;
}

pub using __sighandler_t *fn(a0: i32) -> void;

pub extern fn __sysv_signal(__sig: i32, __handler: *fn(a0: i32) -> void) -> *fn(a0: i32) -> void;

pub extern fn signal(__sig: i32, __handler: *fn(a0: i32) -> void) -> *fn(a0: i32) -> void;

pub extern fn kill(__pid: i32, __sig: i32) -> i32;

pub extern fn killpg(__pgrp: i32, __sig: i32) -> i32;

pub extern fn raise(__sig: i32) -> i32;

pub extern fn ssignal(__sig: i32, __handler: *fn(a0: i32) -> void) -> *fn(a0: i32) -> void;

pub extern fn gsignal(__sig: i32) -> i32;

pub extern fn psignal(__sig: i32, __s: *char) -> void;

pub extern fn psiginfo(__pinfo: *siginfo_t, __s: *char) -> void;

pub extern fn sigblock(__mask: i32) -> i32;

pub extern fn sigsetmask(__mask: i32) -> i32;

pub extern fn siggetmask() -> i32;

pub using sig_t *fn(a0: i32) -> void;

pub extern fn sigemptyset(__set: *sigset_t) -> i32;

pub extern fn sigfillset(__set: *sigset_t) -> i32;

pub extern fn sigaddset(__set: *sigset_t, __signo: i32) -> i32;

pub extern fn sigdelset(__set: *sigset_t, __signo: i32) -> i32;

pub extern fn sigismember(__set: *sigset_t, __signo: i32) -> i32;

pub struct sigaction {
    __sigaction_handler: struct #union {
        sa_handler: *fn(a0: i32) -> void;
        sa_sigaction: *fn(a0: i32, a1: *siginfo_t, a2: *void) -> void;
    }
    sa_mask: __sigset_t;
    sa_flags: i32;
    sa_restorer: *fn() -> void;
}

pub extern fn sigprocmask(__how: i32, __set: *sigset_t, __oset: *sigset_t) -> i32;

pub extern fn sigsuspend(__set: *sigset_t) -> i32;

pub extern fn sigaction(__sig: i32, __act: *sigaction, __oact: *sigaction) -> i32;

pub extern fn sigpending(__set: *sigset_t) -> i32;

pub extern fn sigwait(__set: *sigset_t, __sig: *i32) -> i32;

pub extern fn sigwaitinfo(__set: *sigset_t, __info: *siginfo_t) -> i32;

pub extern fn sigtimedwait(__set: *sigset_t, __info: *siginfo_t, __timeout: *timespec) -> i32;

pub extern fn sigqueue(__pid: i32, __sig: i32, __val: sigval) -> i32;

pub struct _fpx_sw_bytes {
    magic1: u32;
    extended_size: u32;
    xstate_bv: u64;
    xstate_size: u32;
    __glibc_reserved1: [7]u32;
}

pub struct _fpreg {
    significand: [4]u16;
    exponent: u16;
}

pub struct _fpxreg {
    significand: [4]u16;
    exponent: u16;
    __glibc_reserved1: [3]u16;
}

pub struct _xmmreg {
    element: [4]u32;
}

pub struct _fpstate {
    cwd: u16;
    swd: u16;
    ftw: u16;
    fop: u16;
    rip: u64;
    rdp: u64;
    mxcsr: u32;
    mxcr_mask: u32;
    _st: [8]_fpxreg;
    _xmm: [16]_xmmreg;
    __glibc_reserved1: [24]u32;
}

pub struct sigcontext {
    r8: u64;
    r9: u64;
    r10: u64;
    r11: u64;
    r12: u64;
    r13: u64;
    r14: u64;
    r15: u64;
    rdi: u64;
    rsi: u64;
    rbp: u64;
    rbx: u64;
    rdx: u64;
    rax: u64;
    rcx: u64;
    rsp: u64;
    rip: u64;
    eflags: u64;
    cs: u16;
    gs: u16;
    fs: u16;
    __pad0: u16;
    err: u64;
    trapno: u64;
    oldmask: u64;
    cr2: u64;
    _anon0: struct #union {
        fpstate: *_fpstate;
        __fpstate_word: u64;
    }
    __reserved1: [8]u64;
}

pub struct _xsave_hdr {
    xstate_bv: u64;
    __glibc_reserved1: [2]u64;
    __glibc_reserved2: [5]u64;
}

pub struct _ymmh_state {
    ymmh_space: [64]u32;
}

pub struct _xstate {
    fpstate: _fpstate;
    xstate_hdr: _xsave_hdr;
    ymmh: _ymmh_state;
}

pub extern fn sigreturn(__scp: *sigcontext) -> i32;

pub struct _libc_fpxreg {
    significand: [4]u16;
    exponent: u16;
    __glibc_reserved1: [3]u16;
}

pub struct _libc_xmmreg {
    element: [4]u32;
}

pub struct _libc_fpstate {
    cwd: u16;
    swd: u16;
    ftw: u16;
    fop: u16;
    rip: u64;
    rdp: u64;
    mxcsr: u32;
    mxcr_mask: u32;
    _st: [8]_libc_fpxreg;
    _xmm: [16]_libc_xmmreg;
    __glibc_reserved1: [24]u32;
}

pub using fpregset_t *_libc_fpstate;

pub struct mcontext_t {
    gregs: [23]i64;
    fpregs: *_libc_fpstate;
    __reserved1: [8]u64;
}

pub struct ucontext_t {
    uc_flags: u64;
    uc_link: *ucontext_t;
    uc_stack: stack_t;
    uc_mcontext: mcontext_t;
    uc_sigmask: sigset_t;
    __fpregs_mem: _libc_fpstate;
    __ssp: [4]u64;
}

pub extern fn siginterrupt(__sig: i32, __interrupt: i32) -> i32;

pub extern fn sigaltstack(__ss: *stack_t, __oss: *stack_t) -> i32;

pub extern fn sigstack(__ss: *sigstack, __oss: *sigstack) -> i32;

pub struct __atomic_wide_counter #union {
    __value64: u64;
    __value32: struct {
        __low: u32;
        __high: u32;
    }
}

pub extern fn pthread_sigmask(__how: i32, __newmask: *__sigset_t, __oldmask: *__sigset_t) -> i32;

pub extern fn pthread_kill(__threadid: u64, __signo: i32) -> i32;

pub extern fn __libc_current_sigrtmin() -> i32;

pub extern fn __libc_current_sigrtmax() -> i32;

pub extern fn wait(__stat_loc: *i32) -> i32;

pub extern fn waitpid(__pid: i32, __stat_loc: *i32, __options: i32) -> i32;

pub extern fn waitid(__idtype: idtype_t, __id: u32, __infop: *siginfo_t, __options: i32) -> i32;

pub extern fn wait3(__stat_loc: *i32, __options: i32, __usage: *rusage) -> i32;

pub extern fn wait4(__pid: i32, __stat_loc: *i32, __options: i32, __usage: *rusage) -> i32;

