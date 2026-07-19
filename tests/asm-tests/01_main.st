fn main() -> i64 {
    printf("Hello world\n");
    return 0;
}

extern fn printf(fmt : *u8) -> i64;
