#import "Libc";
#import "Array";

pub fn main() -> i32 {
    {
        xs : []u32 = { 1, 2, 3 };
        for x: xs { printf("hello world %zu\n", x); }
    }

    {
        xs : [..]u32;
        for it: 0..5 { array_add(&xs, cast(u32)it * 3); }
        for it, x: xs {
            printf("Index %d => Value %zu\n", it, x);
        }
    }

    return 0;
}
