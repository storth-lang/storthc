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

`$T...` collects any number of arguments of any types into a pack. A pack is processed
recursively, the way C++ variadic templates are: peel the first element off, handle it, and
recurse on the rest. A plain function with the same name is the base case:

```rs
fn sum() -> i64 {
    return 0;
}

fn sum(first: $T, rest: $U...) -> i64 {
    return cast(i64) first + sum(...rest);
}

sum(1, 2, 3);    // 6, instantiates sum(i32,i32,i32) -> sum(i32,i32) -> sum(i32) -> sum()
```

Each call site gets its own compiled version, sized and typed for whatever arguments were actually passed. There's no runtime array or boxing involved, this is fully resolved at compile time. A pack may be empty, so `sum(1)` calls the `sum()` base case from inside the one-element instance.

When a plain function and generic templates share a name, the plain function is used if it accepts the call, otherwise the matching template is. That is what makes the zero-argument base case work.

A pack can also be forwarded whole: `other(...args)`, and `args.len` is its length. There is no `for` over a pack, so `for a: args` is a compile error that points you to recursion instead.

### Spreading arrays into a pack

`...xs` in a call expands to one argument per element, so a variadic function can be fed from an array:

```rs
xs := [1, 2, 3];
ys: [4]i32 = {4, 5, 6, 7};

sum(...xs);          // sum(xs[0], xs[1], xs[2])
sum(...ys);
sum(...[10, 20]);    // the literal's elements
```

The length has to be known at compile time, because every call site is compiled for exactly that many arguments. That works for:

- a fixed array `[N]T`, or a field that is one
- a local slice initialised from a `[a, b, c]` literal (until it is reassigned)
- a `[a, b, c]` literal written in the call
- the enclosing function's own `$T...` pack

Spreading a slice or dynamic array whose length is only known at runtime is a compile error.
