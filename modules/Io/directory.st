#import "Os";

struct Linux {
}

pub trait Directory($T) {
    self = *$T;
    fn read(self: *Self, path : string) -> []string;
}

pub dir := Directory(Linux) {
    fn read(self: *Self, path : string) -> []string {
    }
}
