pub struct Flat_Pool {
    base      : *u8;
    used      : u64;
    capacity  : u64;
    allocator : Allocator;
}

pub impl Flat_Pool {
    fn init(allocator : Allocator = allocator, size : u64 = 1024 * 256,
            loc := #caller_location) -> Self {
        p := allocator.allocate(cast(u32)size);
        if p {
            case Some(v) { return { cast(*u8)v, 0, size, allocator }; }
            case None { assert(false, "Could not allocate Memory", loc); }
        }
        return {};
    }

    fn deinit(self: *Self) {
        self.allocator.deallocate(self.base);
        self.used = 0;
        self.capacity = 0;
    }

    fn push(self: *Self, size: u64, align : u64 = 16,
                loc := #caller_location) -> *void {
        assert(self.capacity > 0, "Trying to allocate on non initialized pool", loc);
        cur := cast(u64)self.base + self.used;
        aligned := (cur + (align - 1)) & ~(align - 1);
        new_used := (aligned - cast(u64)self.base) + size;
        if new_used > self.capacity or new_used < self.used { return null; }
        self.used = new_used;
        return cast(*void)aligned;
    }

    fn new(self: *Self, type: Type_Info, loc := #caller_location) -> *type {
        return cast(*type)self.push(sizeof(type), align_of(type), loc);
    }

    fn as_allocator(self: *Self) -> Allocator {
        a := Allocator(Flat_Pool) {
            self = self;
            fn allocate(self: *Self, size : u32) -> Option(*void) {
                p := self.push(cast(u64)size);
                if p == null then return .None;
                return .Some(p);
            }
            fn reallocate(self: *Self, ptr: *void, size : u32) -> Option(*void) {
                p := self.push(cast(u64)size * sizeof(ptr));
                if p == null then return .None;
                return .Some(p);
            }
            fn copy(self: *Self, ptr: *void, ptr2 : *void, size : u32) -> void {
                Libc.memcpy(ptr, ptr2, cast(u64)size);
            }
            fn deallocate(self: *Self, ptr: *void) -> void { }
        };
        return a;
    }
}

