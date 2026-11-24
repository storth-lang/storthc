#import "Libc";

struct H { items: []string; }

fn mk() -> []string {
    r : []string;
    return r;
}

pub fn main() -> i32 {
    h : H;
    h.items = ["p", "q", "r"];
    printf("%s %ld\n", h.items[2].ptr, h.items.len);
    s := ["a", "b"];
    t := ["c"];
    s = t;
    printf("%s %ld\n", s[0].ptr, s.len);
    s = [];
    printf("%ld\n", s.len);
    return 0;
}
