#import "Error";

enum Color { Red; Green := 5; Blue; }
enum_flag Perm : u8 { Read, Write }

fn describe(c: Color) -> string {
    return enum_name(c);
}

pub fn main() -> i32 {
    c := Color.Green;
    println("{} {} {}", 
            enum_name(Color.Red),
            enum_name(c),
            describe(.Blue));
    p := Perm.Write;
    println("{} {}", enum_name(p), enum_name(p).len);
    return 0;
}
