# 4 - Operators

## Arithmetic

`+ - * / %`, plus compound assignment `+= -= *= /=`, plus post-increment/decrement `++ --` (no pre-increment/decrement).

## Boolean

The usual C-style comparison and logical operators apply: `== != < > <= >=`, and, or, not.

## Declaration operators

| Operator | Meaning |
|---|---|
| `::` | constant, or (via `using`) a type alias |
| `:=` | declare and initialize, type inferred |
| `.` | field access on a struct, or namespace access into an imported module |
| `[]` | array creation / indexing |

## Casting

`#as` is the only way to convert between types. See Chapter 1 for why, and Chapter 3 for what counts as "different types" for this purpose.
