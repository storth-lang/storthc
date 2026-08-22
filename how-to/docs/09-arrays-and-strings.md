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
