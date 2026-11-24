pub struct String_Builder {
    data: *char;
    count: u32;
    capacity: u32;
    allocator : Allocator;
}

pub impl String_Builder {
    fn reserve(self: *Self, size : u32 = 256) {
        self.allocator = allocator;
        if self.count >= self.capacity {
            if self.capacity {
                case 0 { self.capacity = size; }
                default { self.capacity *= 2; }
            }
            self.data = self.allocator.reallocate(self.data, self.capacity).unwrap();
        }
    }

    fn add_to_builder(self: *Self) {}

    fn add_to_builder(self: *Self, str: $T(string, []char)) {
        self.reserve();
        n := cast(u32) str.len;
        self.allocator.copy(self.data + self.count, str.ptr, n);
        self.count += n;
    }

    fn add_to_builder(self: *Self, str: *char) {
        self.reserve();
        n := cast(u32) strlen(str);
        self.allocator.copy(self.data + self.count, str, n);
        self.count += n;
    }

    fn add_to_builder(self: *Self, str: $U(*char, []char, string),
                      args: $T(*char, []char, string)...) {
        self.add_to_builder(str);
        self.add_to_builder(...args);
    }
    
    fn to_string(self: Self) -> string {
        return string {
            ptr = self.data,
            len = self.count
        };
    }

    fn to_cstring(self: Self) -> *char {
        return cstr(self.to_string());
    }
}
