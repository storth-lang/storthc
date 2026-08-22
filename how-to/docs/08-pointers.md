# 8 - Pointers

`*T` is a pointer to `T`. `&expr` takes an address:

```rs
buf : [4096]char;
p := &buf[0];
```

Address-of works on a plain variable, a field (`&x.field`), or an array element (`&arr[i]`).

Dereferencing and pointer arithmetic follow ordinary C-like rules: `p + 8` moves by 8 bytes if `p` is `*u8`, or by `8 * sizeof(T)` if `p` is `*T`. Casting a pointer, e.g. treating a `*void` as a `*u8` before doing arithmetic on it, uses `#as` like any other cast:

```rs
base := raw #as *u8;
next := base + offset;
```

No null-safety wrapper, no borrow checker (yet). A pointer is a pointer, same responsibility as C.
