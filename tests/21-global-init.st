#import "Libc";

struct Entry { name: string; code: i32; }

kw : []string = ["fn", "struct", "enum"];
codes : [3]i32 = [10, 20, 30];
msg : string = "hello";
ents : [2]Entry = [{"a", 1}, {"bb", 2}];
fl : [2]f64 = [1.5, -2.25];

pub fn main() -> i32 {
    printf("%d %d %s %s\n", codes[0], codes[2], msg.ptr, kw[2].ptr);
    printf("%ld %ld %s %d\n", kw.len, msg.len, ents[1].name.ptr, ents[1].code);
    printf("%f %f\n", fl[0], fl[1]);
    return 0;
}
