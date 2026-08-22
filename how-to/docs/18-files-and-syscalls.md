# 18 - Working with files

`unix.st` (part of the `io` module) wraps four real Linux syscalls: `open`, `close`, `read`, `write`. All four are built on `#asm` (Chapter 17), no libc involved.

## Opening

```rs
fd, err := openFileDescriptor("hello.txt", openMode.writeOnly);
if err != ioError.None {
    eprint("%\n", getErrorString(err));
    return 1;
}
defer closeFileDescriptor(fd);
```

`openMode` is `readOnly`, `writeOnly`, or `readAndWrite` (the default). `writeOnly` creates and truncates the file. `readAndWrite` creates if missing but doesn't truncate.

## Writing

```rs
written, err := writeToFileDescriptor(fd, "Hello, file!\n");
```

Takes a `string` directly, no manual nul-termination needed, `write(2)` only needs a pointer and a byte count, which a string already carries.

## Reading

```rs
fd, err := openFileDescriptor("hello.txt", openMode.readOnly);
content, err := readFromFileDescriptor(fd);
```

Returns a `string`. Internally grows a heap buffer (`mem.st`, Chapter 19) and keeps reading until the syscall returns 0 (true end of file), so this reads the whole file regardless of size, not just one syscall's worth.

The returned string's `.ptr` is a real heap allocation. You own it:

```rs
heap_free(content.ptr #as *void);
```

Nothing frees it automatically.
