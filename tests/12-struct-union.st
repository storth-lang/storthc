#import "Libc";

struct Value #union {
    i: i32;
    l: i64;
    f: f64;
    b: [3]u8;
}

struct Tagged {
    tag: u8;
    v: struct #union {
        a: u8;
        b: u32;
    };
}

struct Pair #union #pack {
    a: u8;
    b: [5]u8;
}

pub fn main() -> i32 {
    x : Value;
    x.l = 0x1122334455667788;
    printf("sizeof(Value)=%d\n", sizeof(Value));
    printf("sizeof(Tagged)=%d\n", sizeof(Tagged));
    printf("sizeof(Pair)=%d\n", sizeof(Pair));
    printf("i=%x\n", x.i);
    printf("b0=%x\n", x.b[0]);

    y := Value { f = 1.5 };
    printf("f=%f\n", y.f);

    t : Tagged;
    t.v.b = 0xAABBCCDD;
    printf("a=%x\n", t.v.a);
    return 0;
}
