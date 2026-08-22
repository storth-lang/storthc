# 14 - Compile-time basics

storth has no macros. Anything that needs to happen before the program runs is real storth code, executed by the compiler itself.

## #if

Branch decided at compile time, only the taken branch's code exists in the final program:

```rs
#if kind(v) {
    case "i32" then io_write_int(fd, v);
    case "string" then io_write_str(fd, v);
    default { #comp_error(caller, "print: no formatter for this argument's type"); }
}
```

## kind

Returns a string naming the general category of a value's type: `"i32"`, `"string"`, `"struct"`, and so on. Meaningful only at compile time, since it's asking about a type, not a runtime value.

## sizeof / typeof

Compile-time introspection: the size of a type, or the type of an expression.

## #comp_error

Aborts compilation with a message you write:

```rs
#comp_error(caller, "print: no formatter for this argument's type");
```

Used to turn "this doesn't make sense" into a clear error at the actual call site, instead of a confusing one from inside a function's internals.

## #fields

Iterates a struct's fields at compile time:

```rs
for field, idx: #fields(v) {
    print_value(fd, field);
}
```

No `#` needed on the `for` itself, walking a type's fields has no runtime equivalent, so the compiler already treats it as compile-time regardless.
