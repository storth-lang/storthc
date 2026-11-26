pub fn main() -> i32 {
    content, ok := dir.read("modules");
    if !ok then return -1;
    println("{}", content);
    return 0;
}

#import "String";
#import "Logger";
#import "Io";
