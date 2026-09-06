# Const fold for ::


- PRIORITY: 100
- STATUS: OPEN
- TAGS: semantic, parser

```rs
pub fn main() -> i32 {
	f :: 20;
	x : [f]i32;
}

```

This is an invalid declaration but it should be valid even though the code should work as the semantic analyzer does not const fold constant into their own new mechanism. However if I mark it in a global var it works.
