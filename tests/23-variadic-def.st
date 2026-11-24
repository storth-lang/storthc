#import "Libc";

fn show(fmt: string, args: ...) -> i32 {
    return printf(fmt.ptr, ...args);
}

fn count(tag: i32, rest: ...) -> i32 {
    return tag;
}

pub fn main() -> i32 {
    show("hi %d %s %f\n", 4, "x".ptr, 2.5);
    show("plain\n");
    printf("%d\n", count(7, 1, "a", 3.0));
    return 0;
}
