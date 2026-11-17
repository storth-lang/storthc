pub struct StringBuilder {
    data: *char;
    count: u32;
    capacity: u32;
}

pub impl StringBuilder {
    fn reserve(self: *Self, size : u32 = 256) {
        if self.count >= self.capacity {
            if self.capacity {
                case 0 { self.capacity = size; }
                default { self.capacity *= 2; }
            }
            self.data = mem.reallocate(self.data, self.capacity).unwrap();
        }
    }

    fn add_to_builder(self: *Self) {}

    fn add_to_builder(self: *Self, str: $T(string, []char)) {
        self.reserve();
        n := cast(u32) str.len;
        mem.copy(self.data + self.count, str.ptr, n);
        self.count += n;
    }

    fn add_to_builder(self: *Self, str: *char) {
        self.reserve();
        n := cast(u32) strlen(str);
        mem.copy(self.data + self.count, str, n);
        self.count += n;
    }

    fn add_to_builder(self: *Self, str: $U(*char, []char, string),
                      args: $T(*char, []char, string)...) {
        self.add_to_builder(str);
        self.add_to_builder(...args);
    }
    
}

#import "Allocator";
