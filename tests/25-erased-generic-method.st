#import "Libc";
#import "Allocator";

struct Holder {
    allocator : Allocator;
    data : *char;
}

fn grow(h: *Holder, n: u32) {
    h.data = h.allocator.reallocate(h.data, n).unwrap();
}

pub fn main() -> i32 {
    h := Holder { mem, null };
    grow(&h, 16);
    h.data[0] = cast(char)65;
    h.data[1] = cast(char)0;
    printf("%s\n", h.data);
    return 0;
}
