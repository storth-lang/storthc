PROT_NONE     :: 0;
PROT_READ     :: 1;
PROT_WRITE    :: 2;
MAP_PRIVATE   :: 2;
MAP_ANON      :: 32;
MADV_DONTNEED :: 4;
PAGE          :: 4096;

fn sys_mmap(addr: *void, length: i64, prot: i32, flags: i32, fd: i32, offset: i64) -> *void {
	x := #asm {
		mov rax, 9
		mov rdi, addr
		mov rsi, length
		mov edx, prot
		mov r10d, flags
		mov r8d, fd
		mov r9, offset
		syscall
	};
	return x #as *void;
}

fn sys_mprotect(addr: *void, length: i64, prot: i32) -> i64 {
	return #asm {
		mov rax, 10
		mov rdi, addr
		mov rsi, length
		mov edx, prot
		syscall
	};
}

fn sys_munmap(addr: *void, length: i64) -> i64 {
	return #asm {
		mov rax, 11
		mov rdi, addr
		mov rsi, length
		syscall
	};
}

fn sys_madvise(addr: *void, length: i64, advice: i32) -> i64 {
	return #asm {
		mov rax, 28
		mov rdi, addr
		mov rsi, length
		mov edx, advice
		syscall
	};
}
