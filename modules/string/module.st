// str_from_raw is an intrinsic function to make it so that we accept a pointer

// *char and a size and construct stroth version of string.
pub using StringBuilder [..]char;

pub fn builderToString(sb: StringBuilder) -> string {
    return string {
        ptr = &sb.items[0],
        len = strlen(&sb.items[0])
    };
}

pub fn toString(cstr: $T([]char, *char)) -> string {
    return string {
        ptr = &cstr[0],
        len = strlen(&cstr[0])
    };
}

pub fn stringToBuilder(sb: *StringBuilder, str : string) -> void {
    for c: str {
        arrayPush(sb, c);
    }
}

pub fn chop(str: $T(string, []char, *char), delim : char) -> (string, string) {
    fn chop_by_delim(str: string, delim : char) -> (string, string) {
        i : i64 = 0;
        while i < str.len && str[i] != delim {
            i++;
        }

        skip : i64 = 0;
        if i < str.len then skip = 1;
        new_str := string {
            ptr = str.ptr,
            len = i,
        };

        rest := string {
            ptr = str.ptr + i + skip,
            len = str.len - i - skip,
        };

        return new_str, rest;
    }

    new_str : string;
    rest : string;

    #if kind(str) {
        case "string" {
            new_str, rest = chop_by_delim(str, delim);
        }
        default {
            temp_string := toString(str);
            new_str, rest = chop_by_delim(temp_string, delim);
        }
    }

    return new_str, rest;
}

pub fn lineCount(content : $T(string, []char, *char)) -> i32 {
    count := 0;

    line : string;
    while content.len > 0 {
        line, content = chop(content, '\n');
        count++;
    }

    return count;
}

// pub fn slice(str: *string, end : i64, begin : i64 = 0) -> void inline {
//     str.ptr += end - begin;
//     str.len -= end - begin;
// }

// pub fn slice(str: string, end : i64, begin : i64 = 0) -> string inline {
//     str2 := str;
//     slice(&str2, end, begin);
//     return str2;
// }

#import "libc";
#import "array";
