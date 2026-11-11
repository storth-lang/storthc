pub union Option {
    None;
    Some($T);
}

// pub fn error_out(msg := "", loc : Souce_Code_Location) -> noreturn {
//     if msg.len > 0 {
//         printf("%s:%d:$d: %s\n", cstr(loc.filename), loc.line, loc.col, cstr(msg));
//     }
//     abort();
// }

pub impl Option {
    pub fn unwrap_or(self: Self, val: $T) -> $T {
        if self {
            case Some(v) { return v; }
            case None { return val; }
        }
    }

    pub fn unwrap(self: Self, msg := "") -> $T {
        if self {
            case Some(v) { return v; }
            // case None { error_out(msg, loc); }
            case None { abort(); }
        }
    }
}

pub union Result {
    Error($E);
    Ok($O);
}

#import "Libc";