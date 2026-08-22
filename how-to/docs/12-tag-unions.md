# 12 - Tag unions

`tag_union` declares a type that holds exactly one of several named variants, some of which can carry a payload:

```rs
pub tag_union Shape {
    Circle: f64;       // variant with a payload
    Square: f64;
    Empty;              // variant with no payload
}
```

Matched the same way an enum is, with `case` inside an `if` (Chapter 6, Chapter 11). A variant with no payload can't be matched by value the same way, compare its kind instead of a payload.

This chapter is intentionally short: the payload-carrying case hasn't been exercised against a real, tested example in this guide the way the rest of the language has. Treat the shape above as structurally correct, not as a verified example, until you've compiled it yourself.
