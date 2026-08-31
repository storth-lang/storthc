pub fn arrayReserve(arr: *[..]$T, capacity : i64 = 256) {
    if arr.capacity == 0 then arr.capacity = capacity;
    else { arr.capacity = arr.capacity  * 2; }
}

pub fn arrayPush(arr: *[..]$T, item: $T) {
    if arr.count >= arr.capacity {
        arrayReserve(arr);
        new_bytes, _ := realloc(arr.items, arr.capacity);
        arr.items = new_bytes;
    }

    arr.items[arr.count] = item;
    arr.count = arr.count + 1;
}

pub fn arrayRemoveUnordered(arr: *[..]$T, index: i32) {
    arr.count -= 1;
    arr.items[index] = arr.items[arr.count];
}

pub fn shift(args: *[]$T) {
    if args.len <= 0 then return;
    args.ptr = args.ptr + 1;
    args.len -= 1;
}

#import "mem";
