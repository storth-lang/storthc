# 22 - Traits and witnesses

A trait is a list of method signatures over a type parameter. It holds no code. The code lives in a witness, a value that says how one concrete type implements the trait.

```rs
trait Net($T) {
    fn read(s: *$T, n: i32) -> i32;
    fn write(s: *$T, data: []char) -> i32;
    fn close(s: *$T) -> void;
}

struct Tcp { fd: i32; }
struct Udp { fd: i32; }

tcp_net := Net(Tcp) {
    fn read(s: *Tcp, n: i32) -> i32 { return n + s.fd; }
    fn write(s: *Tcp, data: []char) -> i32 { return cast(i32) data.len; }
    fn close(s: *Tcp) -> void { s.fd = -1; }
};

udp_net := Net(Udp) {
    fn read(s: *Udp, n: i32) -> i32 { return n * 2; }
    fn write(s: *Udp, data: []char) -> i32 { return cast(i32) data.len; }
    fn close(s: *Udp) -> void { s.fd = -1; }
};
```

Every trait method needs a return annotation, `-> void` when it returns nothing. Inside a witness, the methods are written against the concrete type.

## Calling through a named witness

A witness is an ordinary value, so its methods are called with a dot:

```rs
t : Tcp;
n := tcp_net.read(&t, 5);
```

## Calling by trait and type

Writing `Trait(Type).method(args)` calls the method of the witness for that exact trait and type, without naming the witness:

```rs
t : Tcp;
u : Udp;
a := Net(Tcp).read(&t, 5);
b := Net(Udp).read(&u, 5);
```

This is the same call as `tcp_net.read(&t, 5)`. The compiler looks for a witness whose type is `Net(Tcp)` and rewrites the call to use it. It works anywhere an expression does, including `defer`:

```rs
fn serve(c: *Tcp) {
    defer Net(Tcp).close(c);
}
```

The type arguments are plain types: a name, a pointer `*T`, or a generic instance such as `List(i32)`. The number of arguments has to match the number of parameters of the trait.

A `Trait(Type)` followed by `{` is a witness definition and followed by `.method(` is a call, so the two never clash. A name that is not a trait keeps its normal meaning: if `net` is a function or a variable, `net(x).field` still calls it.

### Which witness is used

The search is by type, and it follows scope the same way names do:

- The innermost scope that holds a witness of that type wins, so a local witness shadows a global one.
- If two witnesses of that type are visible at the same level, the call is ambiguous and is a compile error. Call one of them by name instead.
- A witness from an imported module is only a candidate when it is declared `pub`. A private witness is only used from inside its own module.
- If nothing matches, the error names the type that is missing: `no implementation of 'Net(Udp)' is in scope`.

Functions with a `where` clause still resolve their witness by the name written in the clause, as before. The by-type search is only used for the `Trait(Type).method(...)` form.

## Limits

- The type arguments must be concrete at the call. Inside a generic function, name the witness with a `where` clause instead.
- `Trait(Type)` is only accepted as the receiver of a method call, not as a free-standing value.
