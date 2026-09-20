pub using StringBuilder [..]char;

pub fn readEntireFile(path: string) -> (StringBuilder, bool) {
    content: StringBuilder;

    f := libc.fopen(path, "rb");
    if *f == null then goto error;
    defer libc.fclose(f);

    SEEK_END :: 2;
    SEEK_SET :: 0;

    if libc.fseek(f, 0, SEEK_END) < 0 then goto error;
    size : u64 = ftell(f) #as u64;
    if libc.fseek(f, 0, SEEK_SET) < 0 then goto error;

    content.items = libc.malloc(size #as u64);
    if content.items == null then goto error;

    content.count = libc.fread(content.items, size, 1, f) #as i64;

label success;
    return content, true;

label error;
    return content, false;

}

#import "libc";