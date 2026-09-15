pub fn toString(arr: [..]char) -> string {
    return string {
        ptr = &arr.items[0],
        len = arr.count,
    };
}

pub fn toString(arr: []char) -> string {
    return string {
        ptr = &arr[0],
        len = arr.len,
    };
}


pub fn toString(arr: *char) -> string {
    return string {
        ptr = arr,
        len = strlen(arr),
    };
}


pub fn chop_by_delim(str: string, delim : char) -> (string, string) {
    i : i64 = 0;
    while i < str.len && str[i] != delim {
        i++;
    }

    new_string := string {
        ptr = str.ptr,
        len = i,
    };

    skip : i64 = 0;
    if i < str.len then skip = 1;

    rest := string {
        ptr = str.ptr + i + skip,
        len = str.len - i - skip,
    };

    return new_string, rest;
}

#import "libc";
