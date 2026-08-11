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
    io_write_int(fd, frac #as i64);
}

fn print_value(fd: i32, v: $V) {
    #if kind(v) {
        #case "i8"  then io_write_int(fd, v #as i64);
        #case "i16" then io_write_int(fd, v #as i64);
        #case "i32" then io_write_int(fd, v #as i64);
        #case "char" then io_write_char(fd, v);
        #case "i64" then io_write_int(fd, v);
        #case "u8"  then io_write_uint(fd, v #as u64);
        #case "u16" then io_write_uint(fd, v #as u64);
        #case "u32" then io_write_uint(fd, v #as u64);
        #case "u64" then io_write_uint(fd, v);
        #case "f32" then io_write_float(fd, v);
        #case "string" then io_write_str(fd, v);
        #case "struct" {
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
        #case "array" {
            io_write_byte(fd, '[' #as i8);
            #for field, idx: #fields(v) {
                #if idx > 0 {
                    io_write_byte(fd, ',' #as i8);
                    io_write_byte(fd, ' ' #as i8);
                }
                print_value(fd, field);
            }
            io_write_byte(fd, ']' #as i8);
        }
        #default then #comp_error("print: no formatter for this argument's type");
    }
}

pub fn print(fmt: string, args: $T...) {
    #for ch, spec: fmt {
        #if ch == '%' then print_value(STDOUT, args[spec]);
        #else then io_write_byte(STDOUT, ch #as i8);
    }
}
