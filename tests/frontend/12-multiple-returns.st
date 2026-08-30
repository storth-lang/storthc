pub fn foo() -> (string, string) {
    return "Hello", "World"
}

pub fn main() -> i32 {
    f, _ := foo();
    print("%\n", f);
    return 0;
}

#import "io";