#import "Libc";

struct Button {
    title: string;
    on_click: closure();
}

struct Holder {
    ptr: *i32;
    pair: [2]i32;
}

fn apply(f: closure(x: i32) -> i32, v: i32) -> i32 {
    return f(v);
}

fn twice(f: closure&(x: i32) -> i32, v: i32) -> i32 {
    return f(f(v));
}

fn make_adder(k: i32) -> closure(x: i32) -> i32 {
    return fn(x: i32) -> i32 |k| { return x + k; };
}

fn run_all(cbs: []closure(x: i32) -> i32, v: i32) {
    for cb: cbs {
        printf("run_all: %d\n", cb(v));
    }
}

fn fire(b: *Button) {
    b.on_click();
}

fn pair() -> (i32, i32) {
    return 3, 4;
}

fn show(v: $T) {
    k := 5;
    f := fn(x: i32) -> i32 |k, v| { return x + k + cast(i32) v; };
    printf("generic: %d\n", f(1));
}

fn scale(v: $T, by: $T) -> i32 {
    f := fn(x: i32) -> i32 |v, by| {
        w : $T = v;
        return x * cast(i32) (w * by);
    };
    return f(2) + f(3);
}

pub fn main() -> i32 {
    base := 10;
    add := fn(x: i32) -> i32 |base| { return x + base; };
    printf("owning: %d\n", apply(add, 5));
    printf("plain fn: %d\n", apply(fn(x: i32) -> i32 { return x * 3; }, 5));
    printf("literal: %d\n", apply(fn(x: i32) -> i32 |base| { return x - base; }, 5));

    a5 := make_adder(5);
    a7 := make_adder(7);
    printf("returned: %d %d\n", a5(1), a7(1));
    copy := a5;
    printf("copied: %d\n", copy(2));

    n := 0;
    inc := fn(x: i32) -> i32 |&n| { n += 1; return x + n; };
    printf("borrowed: %d\n", twice(inc, 100));
    printf("borrowed n: %d\n", n);
    printf("borrowed from owning: %d\n", twice(add, 1));

    msg := 7;
    btn := Button { title = "ok", on_click = fn() |msg| { printf("clicked %d\n", msg); } };
    btn.on_click();
    fire(&btn);

    arr : [3]closure(x: i32) -> i32;
    for i: 0..3 {
        arr[i] = fn(x: i32) -> i32 |i| { return x * 10 + i; };
    }
    printf("array: %d %d %d\n", arr[0](1), arr[1](1), arr[2](1));
    run_all(arr, 2);

    x := 1;
    y := 2;
    swap := fn() |&x, &y| { x, y = y, x; };
    swap();
    printf("swap: %d %d\n", x, y);
    set := fn() |&x, &y| { x, y = pair(); };
    set();
    printf("multi: %d %d\n", x, y);

    v := 10;
    h := Holder { ptr = &v };
    poke := fn() |h| { h.ptr[0] = 77; };
    poke();
    printf("through pointer: %d\n", v);

    show(10);
    show(cast(i64) 20);
    printf("scale: %d %d\n", scale(3, 4), scale(cast(i64) 2, cast(i64) 5));

    total := 0;
    off := 3;
    fn accumulate(x: $U) |&total, off| { total += cast(i32) x + off; }
    accumulate(1);
    accumulate(cast(i64) 10);
    printf("generic nested: %d\n", total);
    return 0;
}
