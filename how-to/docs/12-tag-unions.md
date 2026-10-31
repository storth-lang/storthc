# 12 - Tag unions

`tag_union` declares a type that holds exactly one of several named variants. A variant either carries a payload, written `Variant(Type)`, or carries nothing.

```rs
pub tag_union Shape {
    Circle(f64),
    Square(f64),
    Empty,
}
```

Variants are separated by commas. A tag union can be generic: the parameters are collected from the payloads, or listed explicitly as `Option($T)`.

```rs
tag_union Option {
    Some($T),
    None,
}
```

## Constructing

`Type.Variant(payload)` builds a value, and a variant without a payload is written `Type.Variant`. For a generic tag union the arguments come from the payload, or from the type the value is going to.

```rs
fn find(xs: []u8, want: u8) -> Option(u8) {
    for x: xs {
        if want == x then return Option.Some(want);
    }
    return Option.None;
}
```

When the destination type is known, the type name can be left out: `.Some(x)`, `.None`, an enum value like `.Green`, or a struct literal `.{ x = 1, y = 2 }`. The destination can be a typed declaration, an assignment, a return value, a struct field, or an argument of a non-generic function. A leading-dot value anywhere else (for example `x := .None`) is an error, since there is nothing to infer from.

```rs
fn find(xs: []u8, want: u8) -> Option(u8) {
    for x: xs {
        if want == x then return .Some(want);
    }
    return .None;
}
```

The generic arguments can also be written out: `Option(u8).Some(7)` and `Option(u8).None`. These work anywhere, with or without a destination type.

When there is no destination type, name the type with its arguments: `Option(u8).Some(7)` and `Option(u8).None` both work.

A tag union value is only ever built this way. A brace literal such as `Shape{ Circle, 5 }` is an error.

A zero-initialized tag union is its first variant, so declare the empty variant first (`None, Some($T)`) when you want an uninitialized value to mean "none".

## Matching

Use `case` inside an `if` (Chapter 6, Chapter 11). `case Variant(name)` binds the payload to `name` for the body of the case, and `case Variant(_)` matches without binding. A variant without a payload is matched as `case Variant`. The matched expression can be a variable, a pointer to a tag union, or a call.

```rs
if find(xs, 2) {
    case Some(v) { printf("%d\n", v); }
    case None { printf("none\n"); }
}
```

The payload can only be read through a pattern, there is no `value.Variant` access. The tag itself is available as `value.kind` and can be compared against `Type.Variant`.

## impl blocks

```rs
impl Option {
    fn unwrap_or(self: Self, d: $T) -> $T {
        if self {
            case Some(v) { return v; }
            case None { return d; }
        }
        return d;
    }
}
```

An `impl` block lives in the same file as its struct or tag union. Each method is an ordinary function named `Type__method` whose first parameter is `self`; `x.method(args)` is rewritten to `Type__method(x, args)`. `Self` stands for the type being implemented in signatures and bodies. `impl Option` takes the generic parameters from the type, `impl Option($T)` states them. A method that takes `self` by pointer can be called on a variable, on a pointer, or on a call result.

## Where tag unions can live

A tag union can be a struct field, an array or slice element, and the payload of another tag union. A payload can be a scalar, struct, string, slice, array or another tag union.

A zero-initialized tag union (a local declared without a value, or a struct field left unset) is the first variant with a zeroed payload, so declare `None` first in an option-like type: `tag_union Option { None, Some($T) }`.
