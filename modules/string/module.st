// str_from_raw is an intrinsic function to make it so that we accept a pointer
// *char and a size and construct stroth version of string.
pub using StringBuilder [..]char;


pub fn slice(str: *string, end: i64, begin: i64 = 0) {
    if (str.len > end) {
        str.ptr = str.ptr + begin;
        str.len = end - begin;
    }
}

pub fn builderToString(sb: StringBuilder) -> string {
    return str_from_raw(sb.items, sb.count);
}

pub fn trim(str: string, delim: char) -> (string, string) {
    i: i64 = 0;
    while i < str.len && str[i] != delim {
        i++;
    }
    new_str := str_from_raw(str.ptr, i);
    if (i < str.len) {
        old_str := str_from_raw(str.ptr + i + 1, str.len - i - 1);
        return new_str, old_str;
    }
    old_str := str_from_raw(str.ptr + str.len, 0);
    return new_str, old_str;
}

pub fn free_builder(sb: *StringBuilder) -> void {
    free(sb.items);
    sb.count = 0;
    sb.capacity = 0;
}

#import "mem";
