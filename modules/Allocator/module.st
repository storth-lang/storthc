pub trait Allocator($T) {
   self = *$T;
    fn reallocate(self: *Self, ptr: *void, size : u32) -> Option(*void);
    fn allocate(self: *Self, size : u32) -> Option(*void);
    fn copy(self: *Self, ptr: *void, ptr2 : *void, size : u32) -> void;
    fn deallocate(self: *Self, ptr: *void) -> void;
}

struct Libc{}
inst := Libc{};

pub libc := Allocator(Libc) {
    self = &inst;
    fn allocate(self: *Self, size : u32) -> Option(*void) {
        p := malloc(cast(u64)size);
        if p == null then return .None;
        return .Some(p);
    }
    fn reallocate(self: *Self, ptr: *void, size : u32) -> Option(*void) {
        p := realloc(ptr, cast(u64)size);
        if p == null then return .None;
        return .Some(p);
    }
    fn copy(self: *Self, ptr: *void, ptr2 : *void, size : u32) -> void {
        Libc.memcpy(ptr, ptr2, cast(u64)size);
    }
    fn deallocate(self: *Self, ptr: *void) -> void {
        if ptr != null {
            free(ptr);
            ptr = null;
        }
    }
};

// NOTE: This is the default context allocator
pub allocator : Allocator = libc;

pub fn set_allocator(a: Allocator) {
    allocator = a;
}

#load "Flat_Pool.st";
#load "Temp_Allocator.st";

#import "Libc";
#import "Error";
#import "Logger";
