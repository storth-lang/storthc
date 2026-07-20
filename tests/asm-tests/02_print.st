fn main() -> i64 {
    x := 20;
    inc(&x);
    printf("%d\n", x);
    return 0;
}

fn inc(x: *i64) -> void {
    *x += 1;
}

extern fn printf(fmt: *char, ...) -> i64;
