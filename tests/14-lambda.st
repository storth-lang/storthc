#import "Libc";

struct Op {
    name: string;
    run: fn(a: i32, b: i32) -> i32;
}

fn apply(f: fn(x: i32) -> i32, v: i32) -> i32 {
    return f(v);
}

fn twice(f: fn(x: i32) -> i32, v: i32) -> i32 {
    return f(f(v));
}

fn make_adder() -> fn(x: i32) -> i32 {
    return fn(x: i32) -> i32 { return x + 100; };
}

fn generic_show(v: $T) {
    f := fn(x: i32) -> i32 { return x + 1; };
    printf("generic: %d\n", f(cast(i32) v));
}

fn outer() -> i32 {
    fn helper(x: i32) -> i32 { return x * 7; }
    g := fn(x: i32) -> i32 { return x - 1; };
    return helper(g(4));
}

pub fn main() -> i32 {
    // assigned to a variable and called
    dbl := fn(x: i32) -> i32 { return x * 2; };
    printf("dbl: %d\n", dbl(21));

    // passed as an argument
    printf("apply: %d\n", apply(fn(x: i32) -> i32 { return x + 1; }, 4));
    printf("twice: %d\n", twice(dbl, 5));

    // called right where it's written
    printf("immediate: %d\n", fn(x: i32) -> i32 { return x * 3; }(3));

    // in a struct field and in an array
    op := Op{ name = "sub", run = fn(a: i32, b: i32) -> i32 { return a - b; } };
    printf("struct: %d\n", op.run(10, 3));
    fns : [2]fn(x: i32) -> i32;
    fns[0] = fn(x: i32) -> i32 { return x + 10; };
    fns[1] = fn(x: i32) -> i32 { return x + 20; };
    printf("array: %d %d\n", fns[0](1), fns[1](1));

    // returned from a function, nested inside another lambda
    add100 := make_adder();
    printf("returned: %d\n", add100(1));
    nest := fn(x: i32) -> i32 {
        inner := fn(y: i32) -> i32 { return y * y; };
        return inner(x) + 1;
    };
    printf("nested: %d\n", nest(5));

    // void lambda, and one in a defer
    hello := fn() { printf("hello from a lambda\n"); };
    hello();
    defer fn() { printf("deferred lambda\n"); }();

    // inside a nested fn and a generic function
    printf("outer: %d\n", outer());
    generic_show(1);
    generic_show(cast(i64) 2);
    return 0;
}
