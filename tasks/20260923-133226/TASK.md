# Initalizing a struct from a module is not valid I have to first declare type.

- STATUS: OPEN
- PRIORITY: 100
- TAGS: frontend, module

```

#import "raylib";
WHITE :: Color{0xFF, 0xFF, 0xFF, 0xFF};

fn foo() -> i32 {
    DrawText(..., WHITE);
}
```
