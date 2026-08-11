pub fn string_from_cstring(x: $T(*char | []char)) -> string {
    i := 0;
    while x[i] != '\0' {
        defer i++;
    }

    if i == 0 then
         return null #as string;

    return str_from_raw(x, i);
}