#load "print.st";

pub fn exit(code: i32) {
    #asm {
        mov rax, 231
        mov edi, code
        syscall
    };
}
