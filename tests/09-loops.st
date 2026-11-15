#import "Libc";

pub fn main() -> i32 {
    xs : [10]i64;
    for it, &x: xs {
        *x = it * 2;
    }
    for x: xs {
        printf("%d\n", x);
    }

    return 0;
}
