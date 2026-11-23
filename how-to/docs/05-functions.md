# 5 - Functions

Declared with `fn`, named, parameters in parens, return type after `->`:

```rs
fn foo() -> u32 {
    return 2000;
}
```

Parameters:

```rs
fn foo(x: u32) -> void {
    print("%\n", x);
}
```

## pub

A function is only visible outside the file it's declared in if marked `pub`. `main` always needs to be `pub`, storthc invokes it from outside your file.

## Multiple return values

```rs
fn foo() -> (u32, string) {
    return 22, "Aboba";
}
```

Calling and binding both results:

```rs
n, s := foo();
```

## Default parameter values

```rs
fn openFileDescriptor(path: string, mode := openMode.readAndWrite) -> (i32, ioError) {
    ...
}
```

`mode` can be omitted at the call site, `openFileDescriptor("aboba.txt")` uses the default.

## Lambdas

A function without a name, written where an expression goes:

```rs
dbl := fn(x: i32) -> i32 { return x * 2; };
print("%\n", dbl(21));
```

Parameter types are required. A lambda is a plain function pointer, so it can be stored in a variable, a struct field or an array, passed to a function taking a `fn(...)`, returned, or called right where it is written: `fn(x: i32) -> i32 { return x * 3; }(3)`.

To use variables of the function the lambda is written in, list them between `|` bars after the signature. `x` copies the variable where the lambda is written, `&x` captures it by reference so the body can modify the original:

```rs
base := 10;
count := 0;
add := fn(x: i32) -> i32 |base, &count| {
    count += 1;       // changes the outer count
    return x + base;  // base is a read only copy
};
base = 99;
print("%\n", add(5)); // 15
```

A capture can also be a value written right in the list, `name: type = value` (or `name := value` to take the type from the value). It is copied into the closure like a by-value capture, evaluated where the lambda is written, and can be mixed with ordinary captures. A bare `|` ends the list, so put bitwise or in parentheses:

```rs
f := fn(x: i32) -> i32 |a: i32 = 1, b: i32 = 2| {
    return x * a * b;
};
g := fn(x: i32) -> i32 |scale := base * 2, &total| { total += x; return x * scale; };
```

Nothing is captured unless it is listed. A copy is read only: assigning to `base` above is an error, write `|&base|` to modify the original. A `&x` capture points into the function that wrote the lambda, so don't let the lambda outlive it.

A lambda with a capture list is a closure, an ordinary struct value, not a function pointer. Call it, copy it, pass it to a generic (`$T`) parameter or to a function parameter written `f: fn(x: i32) -> i32`, but it can't be assigned to a `fn(x: i32) -> i32` variable or struct field. The same list works on a function written inside another function, `fn add(x: i32) -> i32 |base| { ... }`, which is shorthand for `add := fn(x: i32) -> i32 |base| { ... };` (so it can't call itself). A lambda can't be generic or have default parameter values.

To write a closure's type, use `closure(x: i32) -> i32`. It holds the function and up to 64 bytes of captured values, so it can be returned, copied and stored in struct fields and arrays. A plain `fn(..)` value converts to it too, so a parameter of this type accepts either:

```rs
fn make_adder(k: i32) -> closure(x: i32) -> i32 {
    return fn(x: i32) -> i32 |k| { return x + k; };
}

fn apply(f: closure(x: i32) -> i32, v: i32) -> i32 {
    return f(v);
}
```

A function parameter written as a plain `fn(..)` type accepts a function, a lambda or a closure, so most code never has to spell `closure`:

```rs
fn apply(f: fn(x: i32) -> i32, v: i32) -> i32 {
    return f(v);
}

apply(double, 4);                                  // a named function
apply(fn(x: i32) -> i32 { return x + 1; }, 4);     // a lambda
apply(fn(x: i32) -> i32 |k| { return x + k; }, 4); // a closure
```

Such a parameter is read as `closure&(x: i32) -> i32`, so inside the function you can call it and pass it on to other `fn(..)` parameters, but not store it in a `fn(..)` field, return it as a `fn(..)` or give it to a C function. Declare the parameter as `closure(x: i32) -> i32` when you need to keep it. Signatures that use a generic parameter (`fn(x: $T) -> $T`), `#comptime` functions and methods inside a trait implementation keep a plain function pointer.

A closure with a `&x` capture points into the frame that wrote it. It converts only to the borrowing form `closure&(x: i32) -> i32`, and the compiler rejects returning it, storing it through a pointer, a slice or a global, or keeping it in a `static`. Calling a `closure` needs a variable, a field or an array element, not a call result: store it first. A closure type returns at most one value.

A capture list works inside generic functions, and on a nested generic function. It can't be used in a `#comptime` scope yet.

## Calling

Ordinary call syntax, no special cases: `foo(x, y)`.
