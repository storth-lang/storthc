#import "Libc";

enum Color {
    Red,
    Green,
    Yellow,
}

enum_flag Permission : u8 {
    Read,
    Write,
}

fn print_color(color: Color) {
    printf("Color is =>");

    if color {
        case .Red then printf("Red\n");
        case .Green then printf("Green\n");
        default { printf("Default boo\n"); }
    }
}

pub fn main() -> i32 {
    print_color(.Red);
    bit_set : Permission = .Read | .Write;
    printf("Size of Permission %zu and bit mask is %u\n",
           sizeof(bit_set), bit_set);
    return 0;
}
