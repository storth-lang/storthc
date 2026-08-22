# 13 - Type aliasing

`using` creates an alias for a type. No `=`, just the name and the type:

```rs
using Bytes []u8;
```

After this, `Bytes` and `[]u8` are the same type, interchangeable everywhere.

`using` is its own keyword, separate from `::` (Chapter 4). `::` declares a constant, always a value. `using` declares a type alias, always a type. They don't overlap or share syntax, each one only ever does its own job.

Aliasing is transparent, not a distinct nominal type. A function expecting `[]u8` accepts a `Bytes` and vice versa, there's no type-safety boundary between them.
