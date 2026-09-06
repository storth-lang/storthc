# float with more than 2 return does not work

- STATUS: OPEN
- PRIORITY: 100
- TAGS: asm, backend

```
fn bar() -> (f32, f32, f32, f32)  {
    return 22.0, 33.0, 22.0, 33.0;
}

pub fn main() -> i32 {
    a, b, f3, f4 := bar();

    print("% %\n", f3, f4);
    print("% %\n", a, b);

    return 0;
}

#import "io";
```

Both a and b retain the last stack frame.