# Iterating over a pointer of slice/array

- STATUS: CLOSED
- PRIORITY: 80
- TAGS: parser, semantic

```
fn foo(arr: *[]i32) {
    for *a : arr {
        print("%\n", arr);
    }
}

pub fn main() -> i32 {
    xs := [22, 33, 44];
    foo(&xs);
    return 0;
}

#import "io";
```

This should be a semantic + parser fix
