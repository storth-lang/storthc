# 6 - Control flow

## if / else

Ordinary branching:

```rs
if fd > 0 then close(fd); // single statement form
if fd > 0 {
    close(fd);
} else {
    print("already closed\n");
}
```

`else if` chains work as expected.

## if as multi-way dispatch

`if` reused with `case`/`default` inside acts as a match instead of a plain boolean branch:

```rs
if err {
    case memoryError.ReserveFailed { return "failed to reserve address space (mmap)"; }
    case ioError.WriteError { return "could not write into file descriptor"; }
    default { return "unknown error kind"; }
}
```

There's no separate `switch` keyword, this is the same `if` doing both jobs, distinguished by whether the body has `case` labels.

## while

```rs
i := 0;
while i < 10 {
    i = i + 1;
}
```

## for

Over a numeric range:

```rs
for i: 0..10 { ... }
```

Over an array, slice, or string (character by character):

```rs
for i: 0..path.len {
    buf[i] = path.ptr[i];
}
```

Over a `$T...` variadic pack:

```rs
for a: args {
    total += a;
}
```

Pack and struct-field (`#fields(...)`) iteration always run at compile time, since there's no runtime way to walk a heterogeneous pack. You don't need to mark this with `#`, the compiler already knows from what you're iterating.

## return, break, continue, defer

`return` exits a function, optionally with value(s) (Chapter 5). `break`/`continue` work on the innermost `while`/`for`. `defer` schedules a statement for when the current scope exits, in reverse order for multiple `defer`s:

```rs
defer closeFileDescriptor(fd);
```
