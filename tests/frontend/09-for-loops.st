pub fn main() -> i32 {
    i := 20;

    while i < 20 {
        defer i++;
        print("Hello world %\n", i);
    }
    return 0;
}

#import "io";