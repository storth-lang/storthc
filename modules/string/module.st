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

pub fn toCString(arr: string) -> *char {
    return cstr(arr);
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

pub fn slice_string(str: *string, begin: i32, end: i32) -> void {
    if begin > end || str.ptr == null then return;
    str.ptr = str.ptr + begin;
    str.len = cast(i64)(end -  begin);
}

pub fn concatinate(str1: string, str2: string) -> string {
    total_len := str1.len + str2.len;
    writable_ptr := cast(*char)malloc(cast(u64)(total_len + 1));
    if writable_ptr == null then return string { ptr = cast(*char)null, len = 0 };

    memcpy(writable_ptr, str1.ptr, cast(u64)str1.len);
    memcpy(writable_ptr + str1.len, str2.ptr, cast(u64)str2.len);

    writable_ptr[total_len] = cast(char)0;

    return string {
        ptr = writable_ptr,
        len = total_len,
    };
}

pub fn contains(s: string, c: char) -> bool {
    for ca: s {
        if ca == c then return true;
    }
    return false;
}

pub fn get_basename(str: string) -> string {
    i := str.len - 1;
    while i >= 0 {
        if str[i] == '/' {
            return string {
                ptr = str.ptr + i + 1,
                len = str.len - i - 1,
            };
        }
        i--;
    }
    return str;
}


#import "libc";
