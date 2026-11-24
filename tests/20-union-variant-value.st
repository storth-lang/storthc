#import "Libc";
#import "Array";

union Inst {
    Ret;
    Push(i64);
}

fn take(i: Inst) -> i32 {
    if i {
        case Ret { return 1; }
        case Push(v) { return 2; }
    }
}

pub fn main() -> i32 {
    insts : [..]Inst;
    array_add(&insts, Inst.Ret);
    array_add(&insts, Inst.Push(4));
    printf("%d %d %ld\n", take(Inst.Ret), take(insts.items[1]), insts.count);
    return 0;
}
