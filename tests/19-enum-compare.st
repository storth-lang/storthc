#import "Libc";

enum K { A, B, C }

fn f(k: K) -> bool { return k == .B; }
fn g(k: K) -> bool { return .B != k; }

pub fn main() -> i32 {
    printf("%d %d %d\n", f(K.A), f(K.B), g(K.C));
    return 0;
}
