#import "Libc";

fn show(fmt: string, args: ...) -> void {
    printf("%s n=%ld\n", fmt.ptr, args.len);
    i : i64 = 0;
    while i < args.len {
        a := args[i];
        printf("  size=%d\n", a.info.size);
        i += 1;
    }
}

fn outer(args: ...) -> void {
    show("outer", ...args);
}

pub fn main() -> i32 {
    show("a", 4, "x", 2.5);
    show("none");
    outer(1, 2, 3);
    return 0;
}
