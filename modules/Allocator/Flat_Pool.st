pub struct Flat_Pool {
    base      : *u8;
    used      : u64;
    capacity  : u64;
    allocator : Allocator;
}

pub impl Flat_Pool {
    fn init(allocator : Allocator = mem, size : u64 = 1024 * 256,
            loc := #caller_location) -> Self {
        p := allocator.allocate(cast(u32)size);
        if p {
            case Some(v) { return { cast(*u8)v, 0, size, allocator }; }
            case None { printf("%s:%d:%d Could not allocate memory", loc.filename,
                                loc.line loc.column); 
            }
        }
        return {};
    }

    fn deinit(self: *Self) {
        self.allocator.deallocate(self.base);
        self.used = 0;
        self.capacity = 0;
    }

    fn allocate(self: *Self, size: u64, align : u64 = 16,
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
        return cast(*type)self.allocate(sizeof(type), align_of(type), loc);
    }
}

