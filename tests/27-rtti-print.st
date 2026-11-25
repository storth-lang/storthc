enum_flag Perm { Read, Write, Exec }
enum Color : u8 { Red, Green, Blue }
union Opt { None, Some($T) }
struct Node { v: i32; next: *Node; }
struct Inner { c: char; ok: bool; col: Color; }
struct Outer { name: string; in: Inner; tags: [2]u8; o: Opt(i32); a: any; f: f64; neg: i16; }

pub fn main() -> i32 {
    n : Node;
    n.v = 5;
    o : Outer;
    o.name = "box";
    o.in.c = 'z';
    o.in.ok = true;
    o.in.col = Color.Blue;
    o.tags[0] = 9; o.tags[1] = 255;
    o.o = Opt(i32).Some(42);
    o.a = "inside";
    o.f = 1.5;
    o.neg = -7;
    println("{}", o);
    println("{}", n);
    p : Perm = Perm.Read | Perm.Exec;
    println("{} {}", p, Color.Green);
    sl : []i32 = [4, 5, 6];
    println("{} {}", sl, "plain");
    x : any = 3.25;
    println("{} {} {}", x, true, 'q');
    e : Opt(i32) = Opt(i32).None;
    println("{}", e);
    return 0;
}

#import "Error";
