pub fn array_reserve(arr: *[..]$T, capacity : i64 = 256) {
    if arr.capacity == 0 then arr.capacity = capacity;
    else { arr.capacity *= 2; }
}

pub fn array_push(arr: *[..]$T, item: $T) {
    if arr.count >= arr.capacity {
        array_reserve(arr);
        new_bytes, _ := realloc(arr.items, arr.capacity);
        arr.items = new_bytes;
    }

    arr.items[arr.count] = item;
    arr.count += 1;
}

pub fn array_remove_unordered(arr: *[..]$T, index: i32) {
    arr.count -= 1;
    arr.items[index] = arr.items[arr.count];
}

#import "mem";
