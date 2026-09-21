# disable fasm extern definition twice

- STATUS: OPEN
- PRIORITY: 100
- TAGS: backend

```
extern fn printf(fmt: *char, ...) -> i32;
extern fn printf(fmt: *char, ...) -> i32;

pub fn main() -> i8 {
    printf("Hello world\n");
    return 0;
}

```

This is a valid code in the nasm backend however in the fasm code this is invalid.
