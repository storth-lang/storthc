pub fn reserve(arr: *[..]$T, size : i32 = 256) -> void {
    if arr.capacity == 0 then arr.capacity = cast(i64) size;
    else then arr.capacity *= 2;

    arr.items = realloc(arr.items, cast(i64) type_info(arr.items).size *
                        cast(i64) arr.capacity);
}

pub fn append(arr: *[..]$T, item: $T) -> void {
    if arr.count >= arr.capacity then reserve(arr);
    arr.items[arr.count] = item;
    arr.count += 1;
}

pub fn reset(arr: *[..]$T) {
    free(arr.items);
    arr.count = 0;
    arr.capacity = 0;
}

pub fn slice(array: []$T, start: i32, end: i32) -> []$T {
    new_array: []$T;

    for i: start..end {
        new_array[i] = array[i];
    }

    return new_array;
}

pub fn slice(array: *[]$T, start: i32, end: i32) -> void {
    if start > end then return;
    array.ptr += end - start;
    array.len = end - start;
}

pub fn shift(array: *[]$T) -> void {
    slice(array, 0, 1);
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

// TODO: @bug: This is a clear bug as in it does not preserve the array pointer
// state making so that we can not create a copy..
pub fn insert(arr: *[..]$T, index: i32, value: $T) {
    copy := *arr;
    reset(arr);
    for i:0..index {
        temp := copy[i];
        append(arr, temp);
    }
    append(arr, value);

    for i:index..copy.count {
        temp := copy[i];
        append(arr, temp);
    }
}

#import "libc";