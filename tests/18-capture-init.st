#import "Libc";

// A capture can be a value written in the list: 'name: type = value', 'name := value' or 'name = value'.

fn apply(f: fn(x: i32) -> i32, v: i32) -> i32 {
    return f(v);
}

fn keep(f: fn(x: i32) -> i32, v: i32) -> i32 {
    return f(v);
}

pub fn main() -> i32 {
    base := 5;
    n := 0;

    f := fn(x: i32) -> i32 |a: i32 = 1, b: i32 = 2| {
        return x * a * b;
    };
    printf("f: %d\n", f(10));

    // inferred types, mixed with a capture of an existing variable
    g := fn(x: i32) -> i32 |k := 3, m = 4, base| {
        return x + k + m + base;
    };
    printf("g: %d\n", g(1));

    // the value is an expression evaluated where the lambda is written; '&n' still captures by reference
    h := fn(x: i32) -> i32 |scale: i32 = base * 2 + 1, &n| {
        n += 1; return x * scale;
    };

    printf("h: %d\n", h(2));
    printf("n: %d\n", n);

    printf("apply: %d\n", apply(fn(x: i32) -> i32 |off: i32 = 100| { return x + off; }, 1));
    printf("keep: %d\n", keep(fn(x: i32) -> i32 |off: i32 = 7| { return x + off; }, 1));

    outer := fn(x: i32) -> i32 |a: i32 = 2| {
        inner := fn(y: i32) -> i32 |a, c: i32 = 10| { return y * a + c; };
        return inner(x);
    };
    printf("outer: %d\n", outer(4));

    // a bare '|' ends the list, so bitwise or needs parentheses
    bits := fn(x: i32) -> i32 |mask: i32 = (0xF0 | 0x0F), sh: i32 = (1 | 2)| { return (x & mask) << sh; };
    printf("bits: %d\n", bits(1));
    return 0;
}
