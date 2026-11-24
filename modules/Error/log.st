enum_flag logKind {
    Todo,
    Error,
    Assert,
}

struct Out {
    fd: i32;
    len: i64;
    pos: i64;
    fmt: string;
    buf: [1024]char;
}

fn write_all(fd: i32, p: *char, n: u64) {
    off : u64 = 0;
    while off < n {
        r := cast(u64)write(fd, p + off, n - off);
        if r > 0 {
            off += r;
        } else if r != -4 {
            return;
        }
    }
}

fn out_flush(o: *Out) {
    if o.len > 0 {
        write_all(o.fd, &o.buf[0], cast(u64)o.len);
        o.len = 0;
    }
}

fn out_char(o: *Out, c: char) {
    if o.len >= 1024 then out_flush(o);
    o.buf[o.len] = c;
    o.len += 1;
}

fn out_bytes(o: *Out, p: *char, n: u64) {
    if n <= 0 then return;
    if n >= 1024 {
        out_flush(o);
        write_all(o.fd, p, n);
        return;
    }
    if o.len + cast(i64)n > 1024 then out_flush(o);
    memcpy(&o.buf[o.len], p, n);
    o.len += cast(i64) n;
}

fn out_str(o: *Out, s: string) {
    out_bytes(o, s.ptr, cast(u64)s.len);
}

fn out_u64(o: *Out, v: u64) {
    tmp : [20]char;
    i : i64 = 20;
    x := v;
    if x == 0 {
        i -= 1;
        tmp[i] = '0';
    }
    while x > 0 {
        i -= 1;
        tmp[i] = cast(char) (48 + x % 10);
        x = x / 10;
    }
    out_bytes(o, &tmp[i], 20 - cast(u64)i);
}

fn out_i64(o: *Out, v: i64) {
    if v < 0 {
        out_char(o, '-');
        out_u64(o, cast(u64) (0 - v));
    } else {
        out_u64(o, cast(u64) v);
    }
}

fn out_hex(o: *Out, v: u64) {
    tmp : [16]char;
    i : i64 = 16;
    x := v;
    if x == 0 {
        i -= 1;
        tmp[i] = '0';
    }
    while x > 0 {
        d := x % 16;
        i -= 1;
        if d < 10 {
            tmp[i] = cast(char) (48 + d);
        } else {
            tmp[i] = cast(char) (87 + d);
        }
        x = x / 16;
    }
    out_str(o, "0x");
    out_bytes(o, &tmp[i], 16 - cast(u64)i);
}

fn out_f64(o: *Out, v: f64) {
    if v != v {
        out_str(o, "NaN");
        return;
    }
    x := v;
    if x < 0.0 {
        out_char(o, '-');
        x = 0.0 - x;
    }
    if x > 1.0e300 {
        out_str(o, "inf");
        return;
    }
    exp : i32 = 0;
    big := x >= 1.0e15;
    if big {
        while x >= 10.0 {
            x = x / 10.0;
            exp += 1;
        }
    }
    ip := cast(u64) x;
    fp := x - cast(f64) ip;
    r := cast(u64) (fp * 1000000.0 + 0.5);
    if r >= 1000000 {
        ip += 1;
        r -= 1000000;
    }
    out_u64(o, ip);
    if r != 0 {
        out_char(o, '.');
        digits : [6]char;
        k : i64 = 5;
        while k >= 0 {
            digits[k] = cast(char) (48 + r % 10);
            r = r / 10;
            k -= 1;
        }
        last : i64 = 5;
        while last > 0 && digits[last] == '0' {
            last -= 1;
        }
        out_bytes(o, &digits[0], cast(u64)(last + 1));
    }
    if big {
        out_char(o, 'e');
        out_i64(o, cast(i64) exp);
    }
}

fn out_cstr(o: *Out, p: *char) {
    if p == null {
        out_str(o, "(null)");
        return;
    }
    i : i64 = 0;
    while p[i] != '\0' {
        i += 1;
    }
    out_bytes(o, p, cast(u64)i);
}

fn ld_u8(d: *void) -> u8 {
    p := cast(*u8) d;
    return *p;
}

fn ld_i8(d: *void) -> i8 {
    p := cast(*i8) d;
    return *p;
}

fn ld_u16(d: *void) -> u16 {
    p := cast(*u16) d;
    return *p;
}

fn ld_i16(d: *void) -> i16 {
    p := cast(*i16) d;
    return *p;
}

fn ld_u32(d: *void) -> u32 {
    p := cast(*u32) d;
    return *p;
}

fn ld_i32(d: *void) -> i32 {
    p := cast(*i32) d;
    return *p;
}

fn ld_u64(d: *void) -> u64 {
    p := cast(*u64) d;
    return *p;
}

fn ld_i64(d: *void) -> i64 {
    p := cast(*i64) d;
    return *p;
}

fn ld_f32(d: *void) -> f32 {
    p := cast(*f32) d;
    return *p;
}

fn ld_f64(d: *void) -> f64 {
    p := cast(*f64) d;
    return *p;
}

fn ld_char(d: *void) -> char {
    p := cast(*char) d;
    return *p;
}

fn ld_string(d: *void) -> string {
    p := cast(*string) d;
    return *p;
}

fn ld_cstr(d: *void) -> *char {
    p := cast(**char) d;
    return *p;
}

fn out_any(o: *Out, a: any) {
    info := a.info;
    kind := info.kind;
    size := info.size;
    name := info.name;
    d := a.data;
    if kind == 1 {
        b := ld_u8(d);
        if b != 0 { out_str(o, "true"); } else { out_str(o, "false"); }
    } else if kind == 2 {
        out_char(o, ld_char(d));
    } else if kind == 3 {
        out_str(o, ld_string(d));
    } else if kind == 5 || kind == 14 {
        neg := name.len > 0 && name.ptr[0] == 'i';
        if size == 1 {
            if neg { out_i64(o, cast(i64) ld_i8(d)); } else { out_u64(o, cast(u64) ld_u8(d)); }
        } else if size == 2 {
            if neg { out_i64(o, cast(i64) ld_i16(d)); } else { out_u64(o, cast(u64) ld_u16(d)); }
        } else if size == 4 {
            if neg { out_i64(o, cast(i64) ld_i32(d)); } else { out_u64(o, cast(u64) ld_u32(d)); }
        } else {
            if neg { out_i64(o, ld_i64(d)); } else { out_u64(o, ld_u64(d)); }
        }
    } else if kind == 6 {
        if size == 4 { out_f64(o, cast(f64) ld_f32(d)); } else { out_f64(o, ld_f64(d)); }
    } else if kind == 9 {
        if name == "*char" {
            out_cstr(o, ld_cstr(d));
        } else {
            out_hex(o, ld_u64(d));
        }
    } else {
        out_str(o, "<");
        out_str(o, name);
        out_str(o, ">");
    }
}

fn format_text(o: *Out) -> bool {
    f := o.fmt;
    start := o.pos;
    while o.pos < f.len {
        c := f.ptr[o.pos];
        if c == '{' || c == '}' {
            out_bytes(o, f.ptr + start, cast(u64)(o.pos - start));
            has_next := o.pos + 1 < f.len;
            if has_next && f.ptr[o.pos + 1] == c {
                out_char(o, c);
                o.pos += 2;
                start = o.pos;
            } else if c == '{' && has_next && f.ptr[o.pos + 1] == '}' {
                o.pos += 2;
                return true;
            } else {
                out_char(o, c);
                o.pos += 1;
                start = o.pos;
            }
        } else {
            o.pos += 1;
        }
    }
    out_bytes(o, f.ptr + start, cast(u64)(o.pos - start));
    return false;
}

fn format_args(o: *Out) {
    while format_text(o) {
        out_str(o, "{missing}");
    }
}

fn format_args(o: *Out, first: $T, rest: $U...) {
    if format_text(o) {
        a : any = first;
        out_any(o, a);
    }
    format_args(o, ...rest);
}

pub fn print(fmt: string, args: $T...) {
    o : Out;
    o.fd = 1;
    o.fmt = fmt;
    format_args(&o, ...args);
    out_flush(&o);
}

pub fn println(fmt: string, args: $T...) {
    o : Out;
    o.fd = 1;
    o.fmt = fmt;
    format_args(&o, ...args);
    out_char(&o, '\n');
    out_flush(&o);
}

pub fn eprint(fmt: string, args: $T...) {
    o : Out;
    o.fd = 2;
    o.fmt = fmt;
    format_args(&o, ...args);
    out_flush(&o);
}

pub fn eprintln(fmt: string, args: $T...) {
    o : Out;
    o.fd = 2;
    o.fmt = fmt;
    format_args(&o, ...args);
    out_char(&o, '\n');
    out_flush(&o);
}

fn panic(type: logKind, loc: Source_Code_Location, fmt: string, args: $T...) -> noreturn {
    o : Out;
    o.fd = 2;
    o.fmt = fmt;
    out_str(&o, loc.filename);
    out_char(&o, ':');
    out_i64(&o, cast(i64) loc.line);
    out_char(&o, ':');
    out_i64(&o, cast(i64) loc.column);
    out_str(&o, ": ");
    if type {
        case .Todo then out_str(&o, "TODO: ");
        case .Error then out_str(&o, "ERROR: ");
        case .Assert then out_str(&o, "ASSERT: ");
    }
    format_args(&o, ...args);
    out_char(&o, '\n');
    out_flush(&o);
    exit(1);
}

pub fn todo(msg: string, loc := #caller_location, args: $T...) -> void {
    panic(logKind.Todo, loc, msg, ...args);
}

pub fn log_error(msg: string, loc := #caller_location, args: $T...) -> noreturn {
    panic(logKind.Error, loc, msg, ...args);
}

pub fn assert(cond: bool, msg: string = "assertion failed", loc := #caller_location, args: $T...) -> void {
    if cond then return;
    panic(logKind.Assert, loc, msg, ...args);
}
