# I do not know this bug

- STATUS: OPEN
- PRIORITY: 100
- TAGS: ir, backend

pub fn main() -> i32 {
    x := alloc(256) #as *u8;
    return 0;
}

#import "io";
#import "mem";

```
/home/segfault/Projects/segfault/storthc/foo.st:137:5: error: internal: control flow (switch) isn't lowered yet
 135 |
 136 |
 137 |
     |     ^
/home/segfault/Projects/segfault/storthc/foo.st:165:5: error: internal: control flow (switch) isn't lowered yet
 163 |
 164 |
 165 |
     |     ^

```
