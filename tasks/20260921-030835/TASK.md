# array length not working

- STATUS: OPEN
- PRIORITY: 100
- TAGS: frontend, middle, ir

Array returning is not working. Plus doing sizeof($T) is also not working.
One more thing is that doing [N]. from a var is not working even though it should be working.

```
pub fn slice(array: []$T, start: i32, end: i32) -> []$T {
    len := end - start;
    dest : [len]$T;
    memcpy(&dest[0], array.ptr + start, cast(u64)len * sizeof($T));
    return dest;
}

pub fn main() -> i8 {
    xs := [22, 33, 44, 55, 66, 55];
    f := slice(xs, 2, cast(i32)xs.len);

    for x: xs {
        libc.printf("%d\n", x);
    }
    return 0;
}

#import "libc";
#import "array";


```

and since I overloaded it it can not know which one I am referering. So this needs to be fixed as well.
```
pub fn slice(array: []$T, start: i32, end: i32) -> []$T;
pub fn slice(array: *[]$T, start: i32, end: i32) -> void;
```
