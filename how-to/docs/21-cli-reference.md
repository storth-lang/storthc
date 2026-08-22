# 21 - storthc CLI reference

## Commands

| Command | Effect |
|---|---|
| `run <source>` | build and execute, exit code is the process's exit code |
| `build asm <source>` | emit NASM assembly |
| `build obj <source>` | assemble to an object file |
| `build exe <source>` | link a final executable, don't run it |
| `dump tokens <source>` | print the lexer's token stream |
| `dump ast <source>` | print the parsed and resolved AST |
| `dump ir <source>` | print the lowered SSA IR |

## Flags

`-o <path>` / `--output <path>`, sets the output path for `build asm`/`build obj`/`build exe`/`run`. Defaults: `test.asm`, `test.o`, `test`, `test` respectively.

A lone `-` followed by more arguments passes everything after it straight to `ld`, for `build exe` and `run`:

```
storthc run main.st - -lm
```

## Help

`-h`, `--help`, or `help` after any command or subcommand prints that command's own usage. Running a parent command (`build`, `dump`) with no subcommand also prints that command's usage, not the root one.

## Exit codes

`0` on success. `1` on a compile error, a missing subcommand, or no arguments at all. For `run` specifically, the compiled program's own exit code is returned instead.
