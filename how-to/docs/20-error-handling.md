# 20 - Error handling as a pattern

storth has no exceptions. The pattern used throughout the standard library: an `enum_flag` of possible errors, returned alongside the real result, plus a function that turns a variant into a message.

```rs
pub enum_flag ioError {
    None;
    OpenErrorR;
    WriteError;
    ReadError;
}

pub fn getErrorString(err: $T(ioError | memoryError)) -> string {
    if err {
        case ioError.WriteError { return "could not write into file descriptor"; }
        case ioError.ReadError { return "could not read from file descriptor"; }
        default { return "unknown error kind"; }
    }
}
```

Calling code checks against `None`:

```rs
fd, err := openFileDescriptor("aboba.txt");
if err != ioError.None {
    eprint("%\n", getErrorString(err));
    return 1;
}
```

`getErrorString` being `$T(ioError | memoryError)` means one function handles error types from two different modules, as long as both are listed in the constraint (Chapter 15).

## Real system errno

For errors backed by an actual syscall, the last real errno is tracked separately, since `getErrorString`'s message alone doesn't say why a syscall failed:

```rs
if err != ioError.None {
    print("%: %\n", getErrorString(err), errnoString(get_last_errno()));
}
```

`errnoString` maps a positive errno to the same message C's `strerror` would give, only for the errnos actually reachable from what the standard library calls. `last_errno` is a plain global, not thread-safe, same as C's own `errno` before thread-locals existed.
