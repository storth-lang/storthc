# 10 - Structs

```rs
pub struct Decl {
    handle: i64;
}
```

Construction is a struct literal, field name to value:

```rs
d := Decl { handle: raw };
```

Field access with `.`:

```rs
h := d.handle;
```

Structs are ordinary value types. No inheritance, no methods attached to the struct itself, functions that operate on a struct are just ordinary functions taking it as a parameter.
