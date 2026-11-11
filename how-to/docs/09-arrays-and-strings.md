# 9 - Arrays and strings

## Fixed-size arrays

```rs
buf : [4096]char;
buf[0] = 'a';
```

Size is part of the type. `&buf[0]` gets a pointer to the first element (Chapter 8).

## Strings

`string` is `{ptr: *char, len: i64}`, not a nul-terminated C string:

```rs
s := "hello";
n := s.len;
p := s.ptr;
```

Because it's not nul-terminated, handing a string to something that expects a real C string (a syscall, see Chapter 18) means building a nul-terminated copy yourself first:

```rs
buf : [4096]char;
i := 0;
while i < path.len {
    buf[i] = path.ptr[i];
    i = i + 1;
}
buf[path.len] = 0 #as char;
```

## str_from_raw

Builds a real string from a raw pointer and a length, the reverse direction:

```rs
content := str_from_raw(ptr, n);
```

## Slices

`[]T` is `{ptr: *T, len: i64}`, a view over memory that someone else owns. A fixed array converts to a slice of the same element type wherever a slice is expected.

```rs
fn sum(xs: []i32) -> i32 {
    total := 0;
    for x: xs { total += x; }
    return total;
}

a : [8]i32;
sum(a);
```

### Sub-slices

`base[lo..hi]` is a slice over the elements from `lo` up to, but not including, `hi`. Either bound may be left out: a missing lower bound is `0`, and a missing upper bound is the length of `base`.

```rs
head := a[0..3];
tail := a[3..];
first_n := a[..n];
whole := a[..];
```

`base` can be a fixed array, a slice, a dynamic array, a string or a pointer. Slicing a string gives a string, everything else gives `[]T`. A pointer has no length, so `p[lo..]` is an error and `p[lo..hi]` is the only form.

```rs
word := "hello world"[6..];
rest := p[2..n];
```

The result is a temporary `{ptr, len}` header. Nothing is copied: the elements are still the original ones, so writing through `head[0]` changes `a[0]`. Like any other value it can be stored, passed, returned, and asked for `.len` and `.ptr`. It can also be written as `&a[lo..hi]`, which is a pointer to that temporary header and is only valid inside the function that created it. The slice expression itself cannot be assigned to.

Bounds are checked. With constant bounds the compiler reports the error, otherwise the program stops at run time when `lo > hi` or `hi > len`.

```rs
a : [4]i32;
x := a[1..9];
```
