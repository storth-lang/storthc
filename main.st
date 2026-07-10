fn foo(name: string) -> (i32, string) {
    if name {
        case "Urmom": { return 2, "string"; }
        default: { return 0, "string"; }
    }
}

fn ptr_test() -> *u32 {
    x := 2;
    return &x #as *u32;
}
 

fn main() {
    x, y := foo("urmom");
    z := ptr_test();
    z = null;
}
