STDOUT :: 1;
STDERR :: 2;

fn io_write_byte(fd: i32, b: i8) {
    #asm {
        xor rbx, rbx
        mov bl, b
        dec rsp
        mov [rsp], bl
        mov rax, 1
        mov rdi, fd
        mov rsi, rsp
        mov rdx, 1
        syscall
        inc rsp
    }
}

fn io_write_int(fd: i32, n: i64) {
    if n < 0 {
        io_write_byte(fd, '-' #as i8);
        io_write_int(fd, -n);
        return;
    }
    if n >= 10 then io_write_int(fd, n / 10);
    io_write_byte(fd, ((n % 10) + ('0' #as i64)) #as i8);
}

fn io_write_uint(fd: i32, n: u64) {
    if n >= 10 then io_write_uint(fd, n / 10);
    io_write_byte(fd, ((n % 10) + (48 #as u64)) #as i8);
}

fn io_write_str(fd: i32, s: string) {
    for i: 0..s.len {
        io_write_byte(fd, s.ptr[i] #as i8);
    }
}

fn io_write_float(fd: i32, f: f32) {
    whole := f #as i64;
    io_write_int(fd, whole);
    io_write_byte(fd, '.' #as i8);
    frac := f - (whole #as f32);
    if frac < 0.0 then frac *= -1;
    frac = frac * 1000.0;
    frac_int := frac #as i64;
    if frac_int < 100 then io_write_byte(fd, '0' #as i8);
    if frac_int < 10 then io_write_byte(fd, '0' #as i8);
    io_write_int(fd, frac_int);
}

fn io_write_float64(fd: i32, f: f64) {
    whole := f #as i64;
    io_write_int(fd, whole);
    io_write_byte(fd, '.' #as i8);
    frac := f - (whole #as f64);
    if frac < 0.0 then frac *= -1;
    frac = frac * 1000.0;
    frac_int := frac #as i64;
    if frac_int < 100 then io_write_byte(fd, '0' #as i8);
    if frac_int < 10 then io_write_byte(fd, '0' #as i8);
    io_write_int(fd, frac_int);
}

fn io_write_bool(fd: i32, b: bool) {
    if b then io_write_str(fd, "true");
    else then io_write_str(fd, "false");
}

fn io_write_hex_digit(fd: i32, d: i64) {
    if d < 10 {
        io_write_byte(fd, (d + ('0' #as i64)) #as i8);
    } else {
        io_write_byte(fd, (d - 10 + ('a' #as i64)) #as i8);
    }
}

fn io_write_hex(fd: i32, n: u64) {
    if n >= 16 then io_write_hex(fd, n / 16);
    io_write_hex_digit(fd, (n % 16) #as i64);
}

fn io_write_ptr(fd: i32, addr: u64) {
    if addr == 0 {
        io_write_str(fd, "null");
        return;
    }
    io_write_str(fd, "0x");
    io_write_hex(fd, addr);
}

fn print_value(fd: i32, v: $V) {
    #if kind(v) {
        case "i8"  then io_write_int(fd, v #as i64);
        case "i16" then io_write_int(fd, v #as i64);
        case "i32" then io_write_int(fd, v #as i64);
        case "char" then io_write_byte(fd, v #as i8);
        case "i64" then io_write_int(fd, v);
        case "u8"  then io_write_uint(fd, v #as u64);
        case "u16" then io_write_uint(fd, v #as u64);
        case "u32" then io_write_uint(fd, v #as u64);
        case "u64" then io_write_uint(fd, v);
        case "f32" then io_write_float(fd, v);
        case "f64" then io_write_float64(fd, v);
        case "bool" then io_write_bool(fd, v);
        case "string" then io_write_str(fd, v);
        case "ptr" then io_write_ptr(fd, v #as u64);
        case "enum" then io_write_int(fd, v #as i64);
        case "struct" {
            io_write_byte(fd, '{' #as i8);
            #for field, idx: #fields(v) {
                #if idx > 0 {
                    io_write_byte(fd, ',' #as i8);
                    io_write_byte(fd, ' ' #as i8);
                }
                print_value(fd, field);
            }
            io_write_byte(fd, '}' #as i8);
        }
        case "array" {
            io_write_byte(fd, '[' #as i8);
            #for field, idx: #fields(v) {
                if idx > 0 {
                    io_write_byte(fd, ',' #as i8);
                    io_write_byte(fd, ' ' #as i8);
                }
                print_value(fd, field);
            }
            io_write_byte(fd, ']' #as i8);
        }
        case "dyn_array" {
            io_write_byte(fd, '[' #as i8);
            for i: 0..v.count {
                if i > 0 {
                    io_write_byte(fd, ',' #as i8);
                    io_write_byte(fd, ' ' #as i8);
                }
                print_value(fd, v.items[i]);
            }
            io_write_byte(fd, ']' #as i8);
        }
        case "slice" {
            io_write_byte(fd, '[' #as i8);
            for i: 0..v.len {
                if i > 0 {
                    io_write_byte(fd, ',' #as i8);
                    io_write_byte(fd, ' ' #as i8);
                }
                print_value(fd, v.ptr[i]);
            }
            io_write_byte(fd, ']' #as i8);
        }
        case "tag_union" {
            io_write_byte(fd, '{' #as i8);
            #for field, idx: #fields(v) {
                print_value(fd, field);
            }
            io_write_byte(fd, '}' #as i8);
        }
        default { #comp_error(caller, "print: no formatter for this argument's type"); }
    }
}

fn write_until_percent(fd: i32, fmt: string, pos: i64) -> i64 {
    i := pos;
    while i < fmt.len #as i64 {
        ch := fmt.ptr[i] #as char;
        i = i + 1;
        if ch == '%' then return i;
        io_write_byte(fd, ch #as i8);
    }
    return i;
}

pub fn print(fmt: string, args: $T...) {
    pos : i64 = 0;
    for a: args {
        pos = write_until_percent(STDOUT, fmt, pos);
        print_value(STDOUT, a);
    }
    write_until_percent(STDOUT, fmt, pos);
}

pub fn eprint(fmt: string, args: $T...) {
    pos : i64 = 0;
    for a: args {
        pos = write_until_percent(STDERR, fmt, pos);
        print_value(STDERR, a);
    }
    write_until_percent(STDERR, fmt, pos);
}
