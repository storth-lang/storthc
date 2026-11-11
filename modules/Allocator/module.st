#import "Libc";
#import "Error";

pub trait Allocator($T) {
    self = *$T;
    fn reallocate(self: *Self, ptr: *$P, size : u32) -> Option(*$P);
    fn allocate(self: *Self, size : u32) -> Option(*void);
    fn copy(self: *Self, ptr: *void, ptr2 : *void, size : u32) -> void;
}

struct Libc{}
inst := Libc{};

// TODO: Check for unallocated memory
pub mem := Allocator(Libc) {
    self = &inst;
    fn allocate(self: *Self, size : u32) -> Option(*void) {
        p := malloc(cast(u64)size);
        if p == null then return .None;
        return .Some(p);
    }
    fn reallocate(self: *Self, ptr: *$P, size : u32) -> Option(*$P) {
        p := cast(type_of(ptr))realloc(cast(*void)ptr, cast(u64)size * sizeof(ptr));
        if p == null then return .None;
        return .Some(p);
    }
    fn copy(self: *Self, ptr: *void, ptr2 : *void, size : u32) -> void {
        memcpy(ptr, ptr2, cast(u64)size);
    }
};
