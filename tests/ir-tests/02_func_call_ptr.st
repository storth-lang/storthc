fn add(x: *i64) -> void {
    *x += 20;
}

fn main() -> i64 {
    x : i64;
    add(&x);
    return 0;
}
