#import "Libc";

struct Counter {
    hits: i32;
}

fn call_with(f: $F, v: i32) -> i32 {
    return f(v);
}

fn scaled(k: i32, v: i32) -> i32 {
    mul := fn(x: i32) -> i32 |k| { return x * k; };
    return mul(v);
}

pub fn main() -> i32 {
    a := 10;
    b := 3;
    f := fn(x: i32) -> i32 |a, b| { return x * a + b; };
    printf("multi: %d\n", f(2));

    // by value: copied where the lambda is written
    a = 1000;
    printf("copy: %d\n", f(2));

    // by reference: reads and writes the original
    n := 5;
    inc := fn() -> i32 |&n| { n += 1; return n; };
    printf("ref: %d %d\n", inc(), inc());
    printf("outer n: %d\n", n);
    n = 100;
    printf("ref sees outer: %d\n", inc());

    // mix, and a struct captured by reference
    c := Counter { hits = 0 };
    bump := fn(by: i32) |&c, a| { c.hits += by + a; };
    bump(1);
    bump(2);
    printf("struct: %d\n", c.hits);

    // captured parameter, passed to a generic function, nested closures
    printf("param: %d\n", scaled(6, 7));
    printf("generic: %d\n", call_with(f, 3));
    base := 7;
    outer := fn(x: i32) -> i32 |base| {
        inner := fn(y: i32) -> i32 |base| { return y + base; };
        return inner(x) * 2;
    };
    printf("nested: %d\n", outer(1));

    total := 0;
    for i: 0..3 {
        fn(x: i32) |&total, i| { total += x * i; }(10);
    }
    printf("loop: %d\n", total);

    // nested fn with a capture list: 'f := fn ...'
    fn add_total(x: i32) -> i32 |&total| { return total + x; }
    printf("named: %d\n", add_total(1));

    // a lambda can call a nested fn written in the same block
    fn triple(x: i32) -> i32 { return x * 3; }
    printf("sibling: %d\n", fn(x: i32) -> i32 { return triple(x) + 1; }(4));
    return 0;
}
