#load "linux.st";

pub enum Error {
	None;
	Reserve_Failed;
	Protect_Failed;
	Commit_Failed;
	Out_Of_Bounds;
}

pub fn get_error_string(err: Error) -> string {
	if err {
		case Error.None { return "no error"; }
		case Error.Reserve_Failed { return "failed to reserve address space (mmap)"; }
		case Error.Protect_Failed { return "failed to make the initial arena page read/write (mprotect)"; }
		case Error.Commit_Failed { return "failed to commit additional arena pages (mprotect)"; }
		case Error.Out_Of_Bounds { return "allocation exceeds the arena's reserved size"; }
		default { return "unknown mem error"; }
	}
}

pub struct Arena {
	base:      *u8;
	reserved:  i64;
	committed: i64;
	used:      i64;
}

DEFAULT_ARENA_SIZE :: 67108864;

pub fn arena_init(reserve := DEFAULT_ARENA_SIZE) -> (*Arena, Error) {
	raw := sys_mmap(null, reserve #as i64, PROT_NONE,
	                (MAP_PRIVATE | MAP_ANON) #as i32, -1 #as i32, 0);
	if (raw #as i64) == -1 {
		return null, Error.Reserve_Failed;
	}
	base := raw #as *u8;
	if sys_mprotect(base, PAGE, (PROT_READ | PROT_WRITE) #as i32) != 0 {
		sys_munmap(base, reserve #as i64);
		return null, Error.Protect_Failed;
	}
	a := base #as *Arena;
	a.base      = base;
	a.reserved  = reserve #as i64;
	a.committed = PAGE;
	a.used      = sizeof(Arena);
	return a, Error.None;
}

pub fn arena_alloc(a: *Arena, size: i64, align: i64) -> (*u8, Error) {
	cur     := (a.base #as i64) + a.used;
	aligned := ((cur + align - 1) / align) * align;
	offset  := aligned - (a.base #as i64);
	end     := offset + size;
	if end > a.reserved {
		return null, Error.Out_Of_Bounds;
	}
	if end > a.committed {
		want := ((end + PAGE - 1) / PAGE) * PAGE;
		if sys_mprotect(a.base + a.committed, want - a.committed, (PROT_READ | PROT_WRITE) #as i32) != 0 {
			return null, Error.Commit_Failed;
		}
		a.committed = want;
	}
	a.used = end;
	return a.base + offset, Error.None;
}

pub fn bump(a: *Arena, size: i64) -> (*u8, Error) {
	p, err := arena_alloc(a, size, 16);
	return p, err;
}

pub fn arena_reset(a: *Arena) -> void {
	a.used = sizeof(Arena);
}

pub fn arena_decommit(a: *Arena) -> void {
	if a.committed > PAGE {
		sys_madvise(a.base + PAGE, a.committed - PAGE, MADV_DONTNEED #as i32);
	}
	a.committed = PAGE;
	a.used      = sizeof(Arena);
}

pub fn arena_destroy(a: *Arena) -> void {
	sys_munmap(a.base, a.reserved);
}

pub struct Temp {
	arena: *Arena;
	mark: i64;
}

pub fn temp_begin(a: *Arena) -> Temp {
	t: Temp;
	t.arena = a;
	t.mark  = a.used;
	return t;
}

pub fn temp_end(t: Temp) -> void {
	t.arena.used = t.mark;
}

HDR :: 16;

pub fn heap_alloc(size: i64) -> (*void, Error) {
	total := ((size + HDR + PAGE - 1) / PAGE) * PAGE;
	raw := sys_mmap(null, total, (PROT_READ | PROT_WRITE) #as i32,
	                (MAP_PRIVATE | MAP_ANON) #as i32, -1 #as i32, 0);
	if (raw #as i64) == -1 {
		return null, Error.Reserve_Failed;
	}
	base := raw #as *u8;
	szp := base #as *i64;
	szp[0] = total;
	return (base + HDR) #as *void, Error.None;
}

pub fn heap_free(p: *void) -> void {
	base := ((p #as i64) - HDR) #as *i64;
	sys_munmap(base #as *void, base[0]);
}

pub fn heap_new(f: $T) -> (*$T, Error) {
	p, err := heap_alloc(sizeof($T));
	if err != Error.None {
		return null, err;
	}
	typed := p #as *$T;
	typed[0] = f;
	return typed, Error.None;
}

pub fn mem_copy(dst: *u8, src: *u8, n: i64) {
	for i: 0..n {
		dst[i] = src[i];
	}
}

pub fn reallocate(ptr: *void, new_size: i64) -> (*void, Error) {
	new_raw, err := heap_alloc(new_size);
	if err != Error.None {
		return null, err;
	}
	if ptr != null {
		old_base := ((ptr #as i64) - HDR) #as *i64;
		old_size := old_base[0] - HDR;
		copy_size := old_size;
		if new_size < copy_size {
			copy_size = new_size;
		}
		mem_copy(new_raw #as *u8, ptr #as *u8, copy_size);
		heap_free(ptr);
	}
	return new_raw, Error.None;
}
