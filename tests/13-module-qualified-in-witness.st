#import "Os";

// the witness methods are named like the Os functions they call:
// 'Os.write' must reach the module function, not the witness method.
trait Sink($T) {
    fn make(fd: i32) -> Self;
    fn write(self: *Self, data: string) -> i64;
    fn close(self: *Self) -> void;
}

struct Fd {
    fd: i32;
}

sink := Sink(Fd) {
    fn make(fd: i32) -> Self {
        return Self { fd };
    }

    fn write(self: *Self, data: string) -> i64 {
        return Os.write(self.fd, data.ptr, cast(u64)data.len);
    }

    fn close(self: *Self) -> void {
        if self.fd > 2 then Os.close(self.fd);
    }
};

pub fn main() -> i32 {
    f := sink.make(1);
    n := sink.write(&f, "written through Os.write\n");
    sink.close(&f);
    return cast(i32)(n - 25);
}
