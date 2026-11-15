#import "Libc";

fn sum() -> f32 {
    return 0.0;
}

fn sum(first: f32, items: $T(f32)...) -> f32 {
    return first + sum(...items);
}

pub fn main() -> i32 {
    xs := [1.0, 3.0, 5.0];
    sum := sum(1.0, ...xs);

    printf("%.2f\n", sum);

    return 0;
}
