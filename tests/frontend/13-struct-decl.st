struct Cell {
    left: i32;
    center: i32;
    right: i32;
}

width :: 80;

fn rule110(c: Cell) -> i32 {
    pattern := (c.left << 2) | (c.center << 1) | c.right;
    return (110 >> pattern) & 1;
}

fn print_state(state: []i32) {
    for i: 0..width {
        c: char = '#';
        if state[i] == 0 then  c = ' ';
        print("%", c);
    }

    print("\n");
}

fn evolve(state: []i32) {
    next: [width]i32;

    for i: 0..width {
        left : i32 = 0;
        right : i32 = 0;
        if i != 0 then left = state[i - 1];
        if i != width - 1 then  right = state[i + 1];

        cell := Cell {
            left,
            state[i],
            right,
        };

        next[i] = rule110(cell);
    }

    for i: 0..width {
        state[i] = next[i];
    }
}

pub fn main() -> i32 {
    state: [width]i32;

    state[width / 2] = 1;

    for step: 0..40 {
        print_state(state);
        evolve(state);
    }

    return 0;
}

#import "io";