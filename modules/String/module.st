#load "StringBuilder.st";
#import "Libc";

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

