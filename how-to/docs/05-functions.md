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

## Calling

Ordinary call syntax, no special cases: `foo(x, y)`.
