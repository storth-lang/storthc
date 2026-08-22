# 17 - Inline assembly (#asm)

storth doesn't call libc from its own generated code. Every syscall goes through `#asm`, a raw `syscall` instruction directly. This isn't a niche escape hatch, `unix.st` and `mem.st` are built entirely on it.

## Two forms

As a statement:

```rs
fn close(fd: i32) -> i32 {
    return #asm {
        mov rax, 3
        mov rdi, fd
        syscall
    };
}
```

As an expression in `return` position, the block's result (whatever ends up in `rax`) becomes the value.

## How values get in

Plain identifiers, parameters and locals, resolve to their real value by name:

```rs
mov rdi, fd
mov rsi, flags
```

Literal integers work directly: `mov rax, 1`.

a field-access expression as the operand (`mov rsi, data.ptr`), or a named `::` constant referenced by name. The reliable pattern for both is binding to a plain local first, then referencing that local:

```rs
p := data.ptr;
n := data.len;
... mov rsi, p ...
```

## A real gotcha

A local declared from a field read (`n := data.len;`) can silently get too narrow a type if that field's own declared type is wrong somewhere upstream, truncating the value on the way into its stack slot. `#asm` then does a full-width load and reproduces the truncation plus whatever garbage was already on the stack. If a value referenced inside `#asm` looks subtly wrong, not zero, not obviously broken, just off, check that the local's type actually matches the full width of where the value came from.

## A complete example

`unix.st`'s real `open`:

```rs
fn open(path: string, flags: i32, mode: i32) -> i32 {
    buf : [4096]char;
    i := 0;
    while i < path.len {
        buf[i] = path.ptr[i];
        i = i + 1;
    }
    buf[path.len] = 0 #as char;

    p := &buf[0];
    return #asm {
        mov rax, 2
        mov rdi, p
        mov rsi, flags
        mov rdx, mode
        syscall
    };
}
```

Only the last few lines are raw assembly. Everything that can be ordinary storth, is.