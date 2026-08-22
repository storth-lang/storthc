# 7 - Keywords reference

## Declarations

| Keyword | Use |
|---|---|
| `fn` | function |
| `pub` | visible outside this file |
| `struct` | struct type |
| `enum` | enum type |
| `enum_flag` | enum meant for bit flags |
| `tag_union` | tagged union, one of several named variants, some with a payload |
| `using` | `using Name = <type>;`, a type alias |
| `extern` | `extern fn` / `extern var`, a symbol implemented outside storth |

## Control flow

| Keyword | Use |
|---|---|
| `if` / `else` | branching, or (with `case`) multi-way dispatch |
| `case` / `default` | arms inside an `if`-as-match |
| `while` | condition-checked loop |
| `for` | range, array/slice, string, or pack iteration |
| `return` | exit a function, optionally with values |
| `break` / `continue` | loop control |
| `defer` | run on scope exit, reverse order |
| `label` / `goto` | exists for the cases loops and early return don't shape well, used sparingly |

## Compile time

| Keyword | Use |
|---|---|
| `#comptime` | this function only exists at compile time |
| `#if` | branch decided at compile time |
| `#comp_error` | abort compilation with a message |
| `sizeof` / `typeof` | compile-time type introspection |
| `#fields` | iterate a struct's fields at compile time |
| `kind` | what category a value's type falls into, as a string |

## Operators that act like keywords

`::` `:=` `.` `[]` `#as`, see Chapter 4.
