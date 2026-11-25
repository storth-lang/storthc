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

    pub fn unwrap(self: Self, msg := "", loc := #caller_location) -> $T {
        if self {
            case Some(v) { return v; }
            case None { log_error(msg, loc); }
        }
    }

    pub fn map(self: Self, func: fn(x: $T) -> $T, loc := #caller_location) -> $T {
        if self {
            case Some(v) { return fun(v); }
            case None { log_error("Trying to map on none value", loc); }
        }
    }

    pub fn map_or(self: Self, func: fn(x: $T) -> $T, val: $T) -> $T {
        if self {
            case Some(v) { return fun(v); }
            case None { return val; }
        }
    }
}

pub union Result {
    Ok($O);
    Error($E);
}

pub impl Result {
    pub fn is_ok(self: Self) -> bool {
        if self {
            case Ok(v) { return true; }
            case Error(_) { return false; }
        }
    }

    pub fn is_err(self: Self) -> bool {
        if self {
            case Error(v) { return true; }
            case Ok(_) { return false; }
        }
    }

    pub fn unwrap(self: Self, msg := "", loc := #caller_location) -> $O {
        if self {
            case Ok(v) { return v; }
            case Error(e) {
                log_error("{} {}", loc, msg, e);
            }
        }
    }

    pub fn ok(self: Self, loc := #caller_location) -> $O {
        if self {
            case Ok(v) { return v; }
            case Error(e) {
                log_error("Value is not okay {}", loc, e);
            }
        }
    }

    pub fn unwrap_or(self: Self, val: $O) -> $O {
        if self {
            case Ok(v) { return v; }
            case Error(_) { return val; }
        }
    }

    pub fn map(self: Self, func: fn(x: $O) -> $O, loc := #caller_location) -> $O {
        if self {
            case Ok(v) { return func(v); }
            case Error(e) {
                log_error("Could not map because of error {}", loc, e);
            }
        }
    }

    pub fn map_or(self: Self, func: fn(x: $O) -> $O, val: $O) -> $O {
        if self {
            case Ok(v) { return fun(v); }
            case Error(_) { return val; }
        }
    }
}

#import "Libc";
#import "Logger";
#import "Os";
