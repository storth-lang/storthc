# 1 - What is storth, and why

storth is a small, C-like systems language. No OOP, no functional machinery on top. If you know C, the shape is familiar.

Influences: Moscow ML (type inference, `:=`), Jai (compile-time execution: `#if`, `#comptime`, generics), C (syntax baseline, no garbage collector, you manage memory yourself, see Chapter 19).

## No macros

No token pasting, no text substitution before parsing. Anything that needs to happen "early" runs as real storth code at compile time instead: `#if`, generics, `#comptime` functions.

## No existing compiler backend

storth's compiler doesn't use LLVM. It generates x86-64 NASM directly.

One precision worth stating: storth's own generated code never calls libc functions, every syscall goes through `#asm` and a raw `syscall` instruction (Chapters 17-19). But the final executable is still a normal dynamically-linked ELF binary: it requests the system's dynamic linker and links against libc at the `ld` step. No LLVM and no libc calls in storth's own logic, but not a fully freestanding binary either.

## Strict typing, no implicit casts

Every cast is explicit, via `#as`. This doesn't compile:

```rs
pub fn foo() -> u32 {
    x: u32 = 20;
    y := 44; // untyped, defaults to i32
    return x + y; // mixing u32 and i32, not automatic
}
```

Fix: `return x + y #as u32;`. A literal can be cast directly (`return 20 #as u32;`) without an intermediate variable.

The tradeoff: more casts to write, in exchange for never wondering whether a conversion happened silently.

## What it's for

storth is young and small. This guide stays scoped to what's actually built. The stated goals: an OS, a DAW, a game. All three need real low-level control, which is why there's no GC and why `#asm`/raw syscalls are core, not a corner case.

## A first look

```rs
pub fn main() -> i32 {
    print("Hello world\n");
    return 0;
}

#import "io";
```

`main` is `pub`, every top-level function you want reachable from outside its own file needs to be. `main`'s return type is `i32`, spelled out, not `int`. `print` is an ordinary function from `#import "io"`, not a keyword. Declaration order doesn't matter, the import works fine placed after the code that uses it.
