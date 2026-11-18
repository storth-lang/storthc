pub union Option {
    None;
    Some($T);
}

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


/*The Result type in Rust provides various methods for error handling, including is_ok, is_err, map, and_then, and or_else. These methods allow you to check the result's status, transform values, and handle errors more effectively*/

pub union Result {
    Ok($O);
    Error($E);
}

pub impl Result {
    fn is_ok(self: Self) -> bool {
        if self {
            case Ok(v) { return true; }
            default { return false; }
        }
    }

    fn is_err(self: Self) -> bool {
        if self {
            case Error(v) { return true; }
            default { return false; }
        }
    }

    fn unwrap(self: Self) -> $O {
        if self {
            case Ok(v) { return v; }
            default { return cast($O)null; } 
        }
    }

    fn unwrap_or(self: Self, val: $O) -> $O {
        if self {
            case Ok(v) { return v; }
            default    { return val; }
        }
    }

}

#import "Libc";
