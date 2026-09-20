# Enum flags should have bitsize

- STATUS: OPEN
- PRIORITY: 80
- TAGS: semantic, type

enum_flag Foo : u8 {
}

if I do sizeof(Foo) it still is freaking 8 bytes instead of 1 byte.
