// Bugs
#import "Libc";
#import "Array";

struct Map {
    key: []u32;
    value: []$T;
}

trait Functor($F) {
    fn fmap(f: fn(x: $A) -> $B, m: Self($A)) -> Self($B);
}

functor := Functor(Map) {
    fn fmap(f: fn(f: $A) -> $B, m: Self($A)) -> Self($B) {
        out : [..]$B;
        for i: 0..m.value.len {
           array_add(&out, f(m.value[i]));
        }

        values := to_slice(out);
        return { m.key, values };
    }
};

where functor: Functor($F)
fn twice(f: fn(f: $A) -> $A, m: $F($A)) -> $F($A) {
    return functor.fmap(f, functor.fmap(f, m));
}

fn double(x: i32) -> i32 {
    return x * 2;
}

pub fn main() -> i8 {
    keys : []u32 = {1, 2, 3};
    values : []i32 = {10, 20, 30};
    map := Map { keys, values };

    map = twice(double, map);
    for i: 0..map.value.len {
        printf("Key %zu => Value %d\n", map.key[i], map.value[i]);
    }
    return 0;
}
