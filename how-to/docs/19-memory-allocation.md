# 19 - Memory allocation

storth has no garbage collector. `mem.st` provides a real heap allocator, backed by `mmap`/`mprotect` through `#asm` (Chapter 17), not a wrapper around libc's `malloc`.

## Allocating

```rs
buf, err := heap_alloc(65536);
if err != Error.None {
    ...
}
```

Returns a `*void` and `mem`'s own `Error` enum, `ReserveFailed`, `ProtectFailed`, `CommitFailed`, `OutOfBounds`, distinct from `io`'s `ioError`.

## Freeing

```rs
heap_free(buf);
```

Nothing happens automatically. Every `heap_alloc` needs a matching `heap_free` once you're done with it, same discipline as C.

## Growing an allocation

```rs
new_buf, err := reallocate(buf, new_size);
```

Copies existing content into the new, larger (or smaller) allocation. This is how `readFromFileDescriptor` (Chapter 18) reads a file of any size: start with one chunk, double via `reallocate` whenever the buffer fills, stop at true end of file.

## Copying

```rs
mem_copy(dst, src, n);
```

## Allocate and initialize together

```rs
p, err := heap_new(SomeStruct { field: 1 });
```
