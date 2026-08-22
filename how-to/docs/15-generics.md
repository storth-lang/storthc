# 15 - Generics

## Unconstrained

`$T` accepts any type, a separate specialized version of the function is compiled per call site:

```rs
fn print_value(fd: i32, v: $V) {
    ...
}
```

## Constrained

`$T(A | B)` restricts the generic to one of a listed set of types:

```rs
pub fn getErrorString(err: $T(ioError | memoryError)) -> string {
    ...
}
```

Calling it with anything other than `ioError` or `memoryError` is a compile error naming the constraint that wasn't satisfied.

## Variadic packs

`$T...` collects any number of arguments of any types into a pack:

```rs
fn sum(args: $T...) -> i64 {
    total := 0;
    for a: args {
        total += a;
    }
    return total;
}
```

Each call site gets its own compiled version, sized and typed for whatever arguments were actually passed. There's no runtime array or boxing involved, this is fully resolved at compile time.

The two-binding form gives you the index alongside the value:

```rs
for a, i: args {
    print("arg %: %\n", i, a);
}
```

No `#` needed, a pack can only be walked at compile time (there's no uniform runtime representation for a heterogeneous argument list), so the compiler treats it that way regardless of whether you write `#for` or plain `for`.
