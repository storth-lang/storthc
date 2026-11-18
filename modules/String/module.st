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
