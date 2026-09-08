# #if with end and else not supported

- STATUS: OPEN
- PRIORITY: 80
- TAGS: feature

```
pub fn main() -> i32 {
#if 0
    print("Hello");
#else if 1
    print("World");
#endif
}
```
