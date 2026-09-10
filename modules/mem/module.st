// TODO: Scratch Allocator: By @Ian not responsible for segfault

pub struct Arena {
    base:      *u8;
    reserved:  i64;
    committed: i64;
    used:      i64;
}

DEFAULT_ARENA_SIZE :: #comptime 67108864 ;

fn arena_init(reserve : i64) -> (*Arena, memoryError) {
    raw := sys_mmap(null, reserve #as i64, PROT_NONE,
	            (MAP_PRIVATE | MAP_ANON) #as i32, -1 #as i32, 0);
    if (raw #as i64) == -1 {
	   return null, memoryError.ReserveFailed;
    }
    base := raw #as *u8;
    if sys_mprotect(base, PAGE, (PROT_READ | PROT_WRITE) #as i32) != 0 {
	   sys_munmap(base, reserve #as i64);
	   return null, memoryError.ProtectFailed;
    }
    a := base #as *Arena;
    a.base      = base;
    a.reserved  = reserve #as i64;
    a.committed = PAGE;
    a.used      = sizeof(Arena);
    return a, memoryError.None;
}

pub fn arenaAllocate(reserve : i64 = DEFAULT_ARENA_SIZE) -> (*Arena, bool) {
    arena, err := arena_init(reserve);
    if err != memoryError.None {
        errorReport(err);
        return arena, false;
    }
    return arena, true;
}

pub fn arenaPush(a: *Arena, size: i64, align: i64) -> (*u8, memoryError) {
    cur     := (a.base #as i64) + a.used;
    aligned := ((cur + align - 1) / align) * align;
    offset  := aligned - (a.base #as i64);
    end     := offset + size;
    if end > a.reserved {
	return null, memoryError.OutOfBounds;
    }
    if end > a.committed {
	want := ((end + PAGE - 1) / PAGE) * PAGE;
	if sys_mprotect(a.base + a.committed, want - a.committed, (PROT_READ | PROT_WRITE) #as i32) != 0 {
	    return null, memoryError.CommitFailed;
	}
	a.committed = want;
    }
    a.used = end;
    return a.base + offset, memoryError.None;
}

pub fn bump(a: *Arena, size: i64) -> (*u8, memoryError) {
    p, err := arenaPush(a, size, 16);
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

HDR :: #comptime 16;

pub fn alloc(size: i64) -> (*void, memoryError) {
    total := ((size + HDR + PAGE - 1) / PAGE) * PAGE;
    raw := sys_mmap(null, total, (PROT_READ | PROT_WRITE) #as i32,
	            (MAP_PRIVATE | MAP_ANON) #as i32, -1 #as i32, 0);
    if (raw #as i64) == -1 then return null, memoryError.ReserveFailed;

    base := raw #as *u8;
    szp := base #as *i64;
    szp[0] = total;
    return (base + HDR) #as *void, memoryError.None;
}

pub fn free(p: *void) -> void {
    base := ((p #as i64) - HDR) #as *i64;
    sys_munmap(base #as *void, base[0]);
}

pub fn new(f: $T) -> (*$T, Error) {
    p, err := heap_alloc(sizeof($T));
    if err != Error.None then return null, err;

    typed := p #as *$T;
    typed[0] = f;
    return typed, Error.None;
}

pub fn memcpy(dst: *u8, src: *u8, n: i64) {
    for i: 0..n {
	   dst[i] = src[i];
    }
}

pub fn realloc(ptr: *void, new_size: i64) -> (*void, memoryError) {
    new_raw, err := alloc(new_size);
    if err != memoryError.None {
	return null, err;
    }
    if ptr != null {
	old_base := ((ptr #as i64) - HDR) #as *i64;
	old_size := old_base[0] - HDR;
	copy_size := old_size;
	if new_size < copy_size {
	    copy_size = new_size;
	}
	memcpy(new_raw #as *u8, ptr #as *u8, copy_size);
    }
    return new_raw, memoryError.None;
}

#import "error";
#import "os";
