#import "Libc";

struct Person {
    name: string;
    age : i32;
}

fn rename_person(p: *Person, name: string) {
    p.name = name;
}

pub fn main() -> i32 {
    p := Person {
        name = "Pogmega",
        age = 28
    };

    printf("Before mode => %s\n", cstr(p.name));

    rename_person(&p, "Urmom");
    printf("After mode => %s\n", cstr(p.name));

    pp := Person { "Hallo", 3445, };
    printf("Designated => %s %d\n", cstr(pp.name), pp.age);

    return 0;
}
