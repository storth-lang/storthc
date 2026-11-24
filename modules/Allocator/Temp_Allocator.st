#import "Libc";
#import "Error";

pub struct TempChunk {
    next: *TempChunk;
    capacity: u64;
    pos: u64;
    base: *u8;
}

pub struct Temp_Allocator {
    head: *TempChunk;
    cur: *TempChunk;
    alloc: Allocator;
}

fn alloc_chunk(alloc: Allocator, size: u64, loc: Source_Code_Location) -> *TempChunk {
    chunk: *TempChunk = null;
    h := alloc.allocate(cast(u32)sizeof(TempChunk));
    if h {
        case Some(v) { chunk = cast(*TempChunk)v; }
        case None { assert(false, "Could not allocate temp chunk header", loc); }
    }
    b := alloc.allocate(cast(u32)size);
    if b {
        case Some(v) { chunk.base = cast(*u8)v; }
        case None { assert(false, "Could not allocate temp chunk memory", loc); }
    }
    chunk.next = null;
    chunk.capacity = size;
    chunk.pos = 0;
    return chunk;
}


impl Temp_Allocator {
    fn init(size: u64 = 256 * 1024 * 1024,
            alloc: Allocator = libc, loc := #caller_location) -> Self {
        first := alloc_chunk(alloc, size, loc);
        return { first, first, alloc };
    }

    fn deinit(self: *Self) {
        c := self.head;
        while c != null {
            next := c.next;
            self.alloc.deallocate(cast(*void)c.base);
            self.alloc.deallocate(cast(*void)c);
            c = next;
        }
        self.head = null;
        self.cur = null;
    }

    fn push(self: *Self, size: u64) -> *void {
        n := (size + 15) & ~15;
        c := self.cur;
        while c != null {
            if c.pos + n <= c.capacity {
                p := cast(u64)c.base + c.pos;
                c.pos = c.pos + n;
                self.cur = c;
                return cast(*void)p;
            }
            if c.next == null {
                cap := c.capacity;
                if n > cap { cap = n; }
                nc: *TempChunk = null;
                h := self.alloc.allocate(cast(u32)sizeof(TempChunk));
                if h {
                    case Some(v) { nc = cast(*TempChunk)v; }
                    case None { return null; }
                }
                b := self.alloc.allocate(cast(u32)cap);
                if b {
                    case Some(v) { nc.base = cast(*u8)v; }
                    case None { return null; }
                }
                nc.next = null;
                nc.capacity = cap;
                nc.pos = 0;
                c.next = nc;
            }
            c = c.next;
        }
        return null;
    }

    fn as_allocator(self: *Self) -> Allocator {
        a := Allocator(Temp_Allocator) {
            self = self;
            fn allocate(self: *Self, size : u32) -> Option(*void) {
                p := self.push(cast(u64)size);
                if p == null then return .None;
                return .Some(p);
            }
            fn reallocate(self: *Self, ptr: *void, size : u32) -> Option(*void) {
                p := self.push(cast(u64)size * sizeof(ptr));
                if p == null then return .None;
                return .Some(cast(type_of(ptr))p);
            }
            fn copy(self: *Self, ptr: *void, ptr2 : *void, size : u32) -> void {
                Libc.memcpy(ptr, ptr2, cast(u64)size);
            }
            fn deallocate(self: *Self, ptr: *void) -> void { }
        };
        return a;
    }

    fn reset(self: *Self, item: *void = null) -> *void {
        c := self.head;
        while c != null {
            c.pos = 0;
            c = c.next;
        }
        self.cur = self.head;
        return item;
    }
}
