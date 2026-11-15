#import "Libc";
#import "Array";

struct Vec2 {
    x: i64;
    y: i64;
}

struct Player {
    pos: Vec2;
    name: string;
    alive: bool;
}

struct Enemy {
    pos: Vec2;
    name: string;
}

pub fn moveObject(v : *$T { pos: Vec2, ... }, pos : Vec2) {
    v.pos.x += pos.x;
    v.pos.y += pos.y;
}

pub fn main() -> i32 {
    enemies: [..]Enemy;
    player := Player {
        {23, 43},
        "Aboba",
        true
    };
    
    for i: 0..=3 {
        it := cast(i64)i;
        array_add(&enemies, Enemy {
            {it * 2, it * 3},
            "Enemy"
        });
    }

    moveObject(&player, Vec2{200,300});
    for it, &x: enemies {
        moveObject(x, Vec2{200 * it, 300 * it});
    }

    printf("Hello Player %s\n", cstr(player.name));
    printf("    => Player pos %d %d\n", player.pos.x, player.pos.y);

    for enemy: enemies {
        printf("Enemy %s\n", cstr(enemy.name));
        printf("    => Enemy pos %d %d\n", enemy.pos.x, enemy.pos.y);
    }

    return 0;
}
