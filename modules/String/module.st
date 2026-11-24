pub fn toString(arr: [..]char) -> string {
    return string {
        ptr = &arr.items[0],
        len = arr.count,
    };
}

pub fn toString(arr: []char) -> string {
    return string {
        ptr = arr.ptr,
        len = arr.len,
    };
}


pub fn toString(arr: *char) -> string {
    return string {
        ptr = arr,
        len = strlen(arr),
    };
}

pub fn toCString(arr: string) -> *char {
    return cstr(arr);
}

pub fn contains(s: string, c: char) -> bool {
    for idk, character: s {
        if character == c then return true;
    }
    return false;
}

pub fn ends_with(buf: []char, str: string) -> bool {
    str_len := str.len - 1;
    buf_len := buf.len - 1;
    while str_len > 0 and buf_len > 0{
        if buf[buf_len] == str[str_len]  {
            buf_len -= 1;
            str_len -= 1;
            continue;
        } else { return false; }
    }
    
    return true;
}

pub fn chop_left(xs: string, n: i64) -> (string, string) {
    if n > xs.len { n = xs.len; }
    new_str := string { ptr = xs.ptr, len = n };
    old_str := string { ptr = xs.ptr + n, len = xs.len - n };
    return new_str, old_str;
}

pub fn chop_left(xs: *string, n: i64) -> string {
    if n > xs.len { n = xs.len; }
    new_str := string { ptr = xs.ptr, len = n };
    xs.ptr = xs.ptr + n;
    xs.len -= n;
    return new_str;
}

pub fn chop_by_delimiter(str: string, c: char) -> (string, string) {
    i : i64 = 0;
    while str.len > i and str.ptr[i] != c {
        i += 1;
    }

    new_str, old_str := chop_left(str, i);
    chop_left(&old_str, 1);
    return new_str, old_str;
}

pub fn chop_by_delimiter(str: *string, c: char) -> string {
    i : i64 = 0;
    while str.len > i and str.ptr[i] != c {
        i += 1;
    }

    new_str := chop_left(str, i);
    chop_left(str, 1);
    return new_str;
}

#load "StringBuilder.st";

#import "Libc";
#import "Allocator";
