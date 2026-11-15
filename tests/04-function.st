#import "Libc";
#import "Allocator";

pub fn find_name_reverse(list: []string, want: string) -> (bool, string) {
    fn reverse(name: string) -> string {
        rev : string;
        rev.ptr = mem.allocate(cast(u32)name.len).unwrap();
        for i: name.len-1..=0 {
            *(rev.ptr + rev.len) = name[i]; 
            rev.len += 1;
        }
        return rev;
    }
    found := false;
    rev : string;
    for str: list {
        if str == want {
            rev = reverse(str);
            found = !found;
        }
    }

    return found, rev;
}

pub fn main() -> i32 {
    ok, rev := find_name_reverse(["Foo", "Bar", "Urmom"], "Foo");
    if !ok then return 1;

    printf("Found %.*s\n", cast(i32)rev.len, rev.ptr);
    return 0;
}
