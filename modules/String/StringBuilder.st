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
            self.data = mem.realloc(self.data, self.capacity 
                                    * sizeof(StringBuilder)).unwrap();
        }
    }

    fn add_to_builder(self: *Self) {}

    fn add_to_builder(self: *Self, str: $T(string, []char)) {
        self.reserve();
        mem.copy(self.data + self.count, str.ptr, str.len);
        self.count += str.len;
    }

    fn add_to_builder(self: *Self, str: *char) {
        self.reserve();
        mem.copy(self.data + self.count, str, strlen(str));
        self.count += strlen(str);
    }

    fn add_to_builder(self: *Self, str: $U(*char, []char, string),
                      args: $T(*char, []char, string)...) {
        self.add_to_builder(str);
        self.add_to_builder(...args);
    }
    
}

#import "Allocator";
