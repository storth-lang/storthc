#import "Libc";

pub union Option {
    None;
    Some($T);
}

pub impl Option {
    pub fn unwrap_or(self: Self, val: $T) -> $T {
        if self {
            case Some(v) { return v; }
            case None    { return val; }
        }
    }

    pub fn map(self: Self, f: fn(x: $T) -> $T) -> Self {
        if self {
            case Some(v) { return .Some(f(v)); }
            case None    { return .None; }
        }
    }
}

fn half(a: i32) -> Option(i32) {
    if a % 2 == 0 { return .Some(a / 2); }
    return .None;
}

pub fn main() -> i32 {
    a := half(10).map(fn(x: i32) -> i32 |k: i32 = 3| { return x * k; });
    printf("a: %d\n", a.unwrap_or(0));

    b := half(7).map(fn(x: i32) -> i32 |k: i32 = 3| { return x * k; });
    printf("b: %d\n", b.unwrap_or(-1));

    return 0;
}