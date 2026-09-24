pub fn array_reserve(arr: *[..]$T, size : i32 = 256) -> void {
    if arr.capacity == 0 then arr.capacity = cast(i64) size;
    else then arr.capacity *= 2;

    arr.items = realloc(arr.items, cast(u64) sizeof($T) * cast(u64) arr.capacity);
}

pub fn array_add(arr: *[..]$T, item: $T) -> void {
    if arr.count >= arr.capacity then array_reserve(arr);
    arr.items[arr.count] = item;
    arr.count += 1;
}

pub fn array_reset(arr: *[..]$T) {
    free(arr.items);
    arr.items = null;
    arr.count = 0;
    arr.capacity = 0;
}

pub fn slice(array: []$T, start: i32, end: i32) -> []$T {
    result : []$T;
    if start < 0 || start > end || cast(i64) end > array.len then return result;

    len := end - start;
    bytes := cast(u64) sizeof($T) * cast(u64) len;
    result.ptr = malloc(bytes);
    if result.ptr == null then return result;

    memcpy(result.ptr, array.ptr + start, bytes);
    result.len = cast(i64) len;
    return result;
}

pub fn slice(array: *[]$T, start: i32, end: i32) -> void {
    if start < 0 || start > end || array.ptr == null then return;
    if cast(i64) end > array.len then return;
    array.ptr = array.ptr + start;
    array.len = cast(i64) (end - start);
}

pub fn shift(array: *[]$T) -> void {
    if array.len <= 0 then return;
    slice(array, 1, cast(i32) array.len);
}

pub fn reverse(arr: []$T) {
    start : i64 = 0;
    end := arr.len - 1;
    while start < end {
        temp := arr[start];
        arr[start] = arr[end];
        arr[end] = temp;
        start += 1;
        end -= 1;
    }
}

pub fn array_insert(arr: *[..]$T, index: i32, value: $T) {
    if index < 0 || cast(i64) index > arr.count then return;
    if arr.count >= arr.capacity then array_reserve(arr);

    i := arr.count;
    while i > cast(i64) index {
        arr.items[i] = arr.items[i - 1];
        i -= 1;
    }
    arr.items[index] = value;
    arr.count += 1;
}

#import "libc";
