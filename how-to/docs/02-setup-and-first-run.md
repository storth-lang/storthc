# 2 - Setup and running your first program

storth's compiler binary is `storthc`. It has three top-level commands: `run`, `build`, `dump`.

## run

Compiles and immediately executes:

```
storthc run main.st
```

`main.st` needs a `pub fn main() -> i32`. Its return value becomes the process exit code.

`-o <path>` sets the output executable's location. Anything after a lone `-` gets passed straight through to the linker (`ld`):

```
storthc run main.st - -lm -lX11
```

## build

Three subcommands, each a different stage:

```
storthc build asm main.st -o out.asm   # NASM assembly
storthc build obj main.st -o out.o     # object file
storthc build exe main.st -o out       # linked executable, not run
```

Running `storthc build` with no subcommand prints `build`'s own usage, listing `asm`/`obj`/`exe`. Same for `dump`.

## dump

Inspects an intermediate compilation stage without producing an executable:

```
storthc dump tokens main.st   # lexer output
storthc dump ast main.st      # parsed and resolved AST
storthc dump ir main.st       # lowered SSA IR
```

Useful for understanding what the compiler actually saw, especially when something doesn't compile the way you expected.

## Help

`-h`, `--help`, or `help` after any command shows that command's own usage, not the root one.
