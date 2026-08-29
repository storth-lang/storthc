struct timespec {
    tv_sec: i64;
    tv_nsec: i64;
}

fn time_syscall(n: u64, a1: u64, a2: *timespec) -> i64 {
    ret: i64 = 0;
    #asm {
        mov rax, n
        mov rdi, a1
        mov rsi, a2
        syscall
        mov [&ret], rax
    };
    return ret;
}

pub fn currentTimeMonotonic() -> u64 {
    SYS_clock_gettime :: 228;
    CLOCK_MONOTONIC   :: 1;
    ts: timespec;
    rc := time_syscall(SYS_clock_gettime #as u64, CLOCK_MONOTONIC #as u64, &ts);
    if rc < 0 then return 0;
    return (ts.tv_sec #as u64) * 1000000000 +
           (ts.tv_nsec #as u64);
}

pub fn timeToMillisecond(x: u64) -> f64 {
    return (x #as f64 / 1000000.0);
}