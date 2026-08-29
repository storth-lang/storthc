// str_from_raw is an intrinsic function to make it so that we accept a pointer

// *char and a size and construct stroth version of string.
pub using StringBuilder [..]char;


// This is a slice function that slices the current string it does not return a new one.
// If you want a slice for a matter of fact you definitely want to obviously modify the current one.
pub fn slice(str: *string, end: i64, begin: i64) {
    if (str.len > end) {
        str.ptr = str.ptr + begin;
        str.len = end - begin;
    }
}

// pub fn builderToString(sb: StringBuilder) -> string;
// This function will convert string builder into arbitrary string.
pub fn builderToString(sb: StringBuilder) -> string {
    return str_from_raw(sb.items, sb.count);
}

// pub fn trim(str: string, delim: char) -> (string, string);
// This function accept a string and a delimiter and then return you a new string.
// as well as the old one after that delimiter however it does not modify the string.
// Since we are not in old stinky C land where we can not have pure functions return two values.
// We can just create a new string from that pointer and till the end no issue.
pub fn trim(str: string, delim: char) -> (string, string) {
    i: i64 = 0;
    while i < str.len && str[i] != delim {
        i++;
    }
    new_str := str_from_raw(str.ptr, i);
    if (i < str.len) {
        old_str := str_from_raw(str.ptr + i + 1, str.len - i - 1);
        return new_str, old_str;
    }
    old_str := str_from_raw(str.ptr + str.len, 0);
    return new_str, old_str;
}


// pub fn freeBuilder(sb: *StringBuilder);
// Since string builder is a dynamic array of characters. We must not forget to not free such array.
// freeBuilder does not just free the dynamic array it also actually resets the count and capacity to 0.
pub fn freeBuilder(sb: *StringBuilder) -> void {
    free(sb.items);
    sb.count = 0;
    sb.capacity = 0;
}

// stringToBuilder will push string characters into string builder.
// Do note that since string builder is a dynamic array of characters do not forget to free it.
// Also for this will not keep pushing into the same buffer but rather into new buffer everytime.
pub fn stringToBuilder(sb: *StringBuilder, s: string) {
    if s.ptr == null then return;
    for c: s {
        arrayPush(sb, c);
    }
}

fn push_int(sb: *StringBuilder, int: i64) {
    if int < 0 {
        arrayPush(sb, '-' #as char);
        arrayPush(sb, -int #as char);
        return;
    }
    if int >= 10 then push_int(sb, int / 10);
    arrayPush(sb, ((int % 10) + ('0' #as i64)) #as char);
}


fn push_uint(sb: *StringBuilder, int: i64) {
    if int < 0 then return;
    if int >= 10 then push_int(sb, int / 10);
    arrayPush(sb, ((int % 10) + ('0' #as i64)) #as char);
}

fn push_float(sb: *StringBuilder, f: $T(f32 | f64)) {
    whole := f #as i64;
    push_int(sb, whole);
    arrayPush(sb, '.');
    frac := f - (whole #as type_of(f));
    if frac < 0.0 then frac *= -1;
    frac = frac * 1000.0;
    frac_int := frac #as i64;
    if frac_int < 100 then arrayPush(sb, '0');
    if frac_int < 10 then arrayPush(sb, '0');
    push_int(sb, frac_int);
}

fn push_string(sb: *StringBuilder, s: string) {
    for c: s {
        arrayPush(sb, c);
    }
}

fn push_hex_digit(sb: *StringBuilder, d: i64) {
    if d < 10 then push_int(sb, (d + ('0' #as i64)));
    else then push_int(sb, (d - 10 + ('a' #as i64)));
}

fn push_hex(sb: *StringBuilder, n: u64) {
    if n >= 16 then push_hex(sb, n / 16);
    push_hex_digit(sb, (n % 16) #as i64);
}

fn push_pointer(sb: *StringBuilder, addr: u64) {
    if addr == 0 {
        push_string(sb, "null");
        return;
    }
    push_string(sb, "0x");
    push_hex(sb, addr);
}

fn push_bool(sb: *StringBuilder, b: bool) {
    if b then push_string(sb, "true");
    else then push_string(sb, "false");
}

pub fn append_value(sb: *StringBuilder, v: $V) {
    #if kind(v) {
        case "i8"  then push_int(sb, v #as i64);
        case "i16" then push_int(sb, v #as i64);
        case "i32" then push_int(sb, v #as i64);
        case "char" then arrayPush(sb, v);
        case "i64" then push_int(sb, v);
        case "u8"  then push_uint(sb, v #as u64);
        case "u16" then push_uint(sb, v #as u64);
        case "u32" then push_uint(sb, v #as i64);
        case "u64" then push_uint(sb, v);
        case "f32" then push_uint(sb, v);
        case "f64" then push_float(sb, v);
        case "bool" then push_bool(sb, v);
        case "string" then push_string(sb, v);
        case "ptr" then push_hex(sb, v #as u64);
        case "enum" then push_int(sb, v #as i64);
        case "struct" {
            arrayPush(sb, '{' );
            #for field, idx: #fields(v) {
                #if idx > 0 {
                    arrayPush(sb, ',' );
                    arrayPush(sb, ' ' );
                }
                append_value(sb, field);
            }
            arrayPush(sb, '}' );
        }
        case "array" {
            arrayPush(sb, '[' );
            #for field, idx: #fields(v) {
                if idx > 0 {
                    arrayPush(sb, ',' );
                    arrayPush(sb, ' ' );
                }
                append_value(sb, field);
            }
            arrayPush(sb, ']' );
        }
        case "dyn_array" {
            arrayPush(sb, '[');
            for i: 0..v.count {
                if i > 0 {
                    arrayPush(sb, ',');
                    arrayPush(sb, ' ');
                }
                append_value(sb, v.items[i]);
            }
            arrayPush(sb, ']' );
        }
        case "slice" {
            arrayPush(sb, '[' );
            for i: 0..v.len {
                if i > 0 {
                    arrayPush(sb, ',');
                    arrayPush(sb, ' ');
                }
                append_value(sb, v.ptr[i]);
            }
            arrayPush(sb, ']');
        }
        case "tag_union" {
            arrayPush(sb, '{' );
            #for field, idx: #fields(v) {
                append_value(sb, field);
            }
            arrayPush(sb, '}');
        }
        default { #comp_error(caller, "print: no formatter for this argument's type"); }
    }
}

fn write_until_percent(sb: *StringBuilder, fmt: string, pos: i64) -> i64 {
    i := pos;
    for i : pos..fmt.len {
        ch := fmt.ptr[i];
        i += 1;
        if ch == '%' then return i;
        arrayPush(sb, ch);
    }
    return i;
}

pub fn format(sb: *StringBuilder, fmt: string, args: $T...) -> void {
    pos : i64 = 0;
    for a: args {
        pos = write_until_percent(sb, fmt, pos);
        append_value(sb, a);
    }
    write_until_percent(sb, fmt, pos);
}

pub fn concatinate(dst: []char, str1: string, str2: string) -> string {
    n : i64 = 0;
    for c : str1 {
        if n >= dst.len then break;
        dst[n] = c;
        n = n + 1;
    }
    for c : str2 {
        if n >= dst.len then break;
        dst[n] = c;
        n = n + 1;
    }
    return str_from_raw(&dst[0], n);
}

#import "mem";
#import "array";