INIT_CAPACITY :: 256;

pub fn array_push(arr: *[..]$T, item: $T) {
    if arr.count >= arr.capacity {
        if arr.capacity == 0 then arr.capacity = INIT_CAPACITY;
        else { arr.capacity *= 2; }

        new_bytes, _ := reallocate(arr.items, arr.capacity);
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
