# 3 - Variables, types, and inference

## Declaring a variable

`:=` infers the type from the value:

```rs
x := 20; // untyped integer literal, defaults to i32
```

`:` gives an explicit type:

```rs
x: u64 = 20;
y: i8; // no initializer, zero-filled, never garbage
```

## Integer types

`i8 i16 i32 i64`, `u8 u16 u32 u64`. Width is always spelled out. There's no plain `int`.

## Floats

`f32 f64`.

## Other primitives

`bool`, `char`, `void`, `string`, `any`.

`string` is not a C-style pointer. It's a fat value: a `ptr: *char` and a `len: i64`, both accessible as fields:

```rs
s := "hello";
n := s.len; // 5
p := s.ptr; // *char
```

## No implicit conversion

Assigning or returning a value of a different numeric type needs `#as`, see Chapter 1. This applies uniformly, there's no special case for "small enough to fit" conversions.

## Constants

`::` declares a constant:

```rs
STDOUT :: 1;
```

If the right-hand side is a type instead of a value, `::` participates in a type alias via `using` (Chapter 13), not a constant.
