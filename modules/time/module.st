pub fn currentTimeMonotonic() -> u64 {
    SYS_clock_gettime :: 228;
    CLOCK_MONOTONIC   :: 1;
    ts: timespec;
    rc := sys_time(SYS_clock_gettime #as u64, CLOCK_MONOTONIC #as u64, &ts);
    if rc < 0 then return 0;
    return (ts.tv_sec #as u64) * 1000000000 +
           (ts.tv_nsec #as u64);
}

pub fn timeToMillisecond(x: u64) -> f64 {
    return (x #as f64 / 1000000.0);
}

// The format for getCurrentDate is yr -> month -> data
// Resource: https://en.wikipedia.org/wiki/Unix_time
pub fn getCurrentDate() -> string {
    return "";
}

#import "os";