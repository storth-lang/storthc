# 11 - Enums and enum_flag

```rs
pub enum Foo {
}
```

```rs
pub enum_flag ioError {
    None;
    OpenErrorC;
    OpenErrorR;
    WriteError;
    ReadError;
}
```

`enum` is a plain enumeration. `enum_flag` is meant for bit-flag-style values, both are declared the same way, `enum_flag` communicates intent rather than changing the syntax.

Variant access with `.`:

```rs
if err != ioError.None { ... }
```

Compared for equality like any other value. Matched with `case` inside an `if` (Chapter 6) when there are several variants to handle:

```rs
if err {
    case ioError.WriteError { ... }
    case ioError.ReadError { ... }
    default { ... }
}
```

## Backing type

An enum or enum_flag is stored as `u32` by default. Pick another integer type with `: type`:

```rs
enum Color { Red; Green; Blue; }          // u32, 4 bytes
enum Small : u8 { A; B := 200; }          // 1 byte
enum_flag Perm : i64 { Read; Write; }     // 8 bytes
```

A signed backing type such as `i32` is sign-extended when loaded.
