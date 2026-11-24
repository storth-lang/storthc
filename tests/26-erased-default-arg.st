#import "Libc";
#import "Allocator";

struct Buf {
    data: *char;
    allocator : Allocator;
}

fn init(allocator: Allocator = mem) -> Buf {
    return Buf { null, allocator };
}

fn grow(self: *Buf, n: u32) {
    self.data = self.allocator.reallocate(self.data, n).unwrap();
}

pub fn main() -> i32 {
    b := init();
    grow(&b, 8);
    b.data[0] = cast(char)79;
    b.data[1] = cast(char)75;
    b.data[2] = cast(char)0;
    printf("%s\n", b.data);
    return 0;
}
