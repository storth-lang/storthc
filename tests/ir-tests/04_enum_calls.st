tag_union Foo {
    name : string;
    age: u32;
}

fn main() -> i32 {
    f : Foo;
    f.tag = Foo.Name;
    return 0;
}
