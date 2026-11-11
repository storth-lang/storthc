pub fn array_reserve(arr: *[..]$T, size : i32 = 256) -> void {
    if arr.capacity == 0 { arr.capacity = cast(i64) size; }
    else { arr.capacity *= 2; }
    arr.items = mem.reallocate(arr.items, cast(u32) arr.capacity).unwrap();
}

pub fn array_add(arr: *[..]$T, item: $T) -> void {
    if arr.count >= arr.capacity then array_reserve(arr);
    arr.items[arr.count] = item;
    arr.count += 1;
}

pub fn array_reset(arr: *[..]$T) {
    mem.deallocate(arr.items);
    arr.items = null;
    arr.count = 0;
    arr.capacity = 0;
}

pub fn slice(array: []$T, start: i32, end: i32) -> []$T {
    result : []$T;
    if start < 0 || start > end || cast(i64) end > array.len then return result;

    len := end - start;
    bytes := cast(u32) sizeof(array[0]) * cast(u32) len;
    result.ptr = cast(*type_of(result[0]))mem.allocate(bytes).unwrap();
    if result.ptr == null then return result;

    mem.copy(result.ptr, array.ptr + start, bytes);
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

pub fn peek(array: []$T) -> $T {
    if array.len > 0 then return array[array.len - 1];
    return cast($T)null;
}

pub fn to_slice(array: [..]$T) -> []$T {
    vals : []$T;
    vals.ptr = array.items;
    vals.len = array.count;
    return vals;
}

#import "Allocator";
