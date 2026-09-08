# Use , instead of | and add ! for parametrization

- STATUS: OPEN
- PRIORITY: 100
- TAGS: generics, semantic

Current syntax
```rs
fn foo(x: $T(char | *char)) -> void {
}
```

New syntax
```rs
fn foo(x: $T(char, *char)) -> void {
}
```

```rs
fn foo(x: $T(!char)) -> void {
}
```
