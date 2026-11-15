#import "Libc";

pub fn main() -> i32 {
    x :: 2;

    printf("Hello world %d\n", x);
    str, cond := "Aboba", false;

    if !cond then printf("%.*s", cast(i32)str.len, str.ptr);

    return 0;
}
