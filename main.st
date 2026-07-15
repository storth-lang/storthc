struct Foo {
    x : i64;
    y : i64;
}

fn add(f: *i64) -> void {
    *f = 20;
    *f += 20;
}

fn main() -> i64 {
    a : i64;
    add(&a);
    return 0;
}
