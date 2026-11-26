pub struct Entry {
    key: $K;
    value: $V;
    hash: u32;
}

pub struct Table {
    entries: [..]Entry($K, $V);
    cmp_fn: fn(a: $K, b: $K) -> bool;
    hash_fn: fn(key: $K) -> u32;

    collision: u32;
    allocator: Allocator;
}

fn djb2(key: $K) -> u32 {
    hash: u32 = 5381;
    for it: key {
        hash = hash*33 + cast(u32)it;
    }
    return hash;
}

fn cmp(a: $K, b: $K) -> bool {
    return a == b;
}

pub impl Table {
    fn init(self: *Self, alloc: Allocator = allocator) {
        self.allocator = alloc;
    }

    fn deinit(self: *Self) {
        if self.entries.items != null then self.allocator.deallocate(self.entries.items);
        self.entries.items = null;
        self.entries.count = 0;
        self.entries.capacity = 0;
        self.collision = 0;
    }

    fn ensure_init(self: *Self, loc : Source_Code_Location) {
        has_alloc := cast(*void) self.allocator.self != null;
        has_hash  := cast(*void) self.hash_fn != null;
        has_cmp   := cast(*void) self.cmp_fn != null;

        if has_alloc then return;

        if !has_hash and !has_cmp {
            self.allocator = allocator;
            return;
        }
        assert(false, "Table is partially initialized, call init() manually", loc);
    }

    fn hash_of(self: *Self, key: $K) -> u32 {
        if cast(*void) self.hash_fn == null then return djb2(key);
        return self.hash_fn(key);
    }

    fn keys_equal(self: *Self, a: $K, b: $K) -> bool {
        if cast(*void) self.cmp_fn == null then return cmp(a, b);
        return self.cmp_fn(a, b);
    }

    fn slot_hash(self: *Self, key: $K) -> u32 {
        h := self.hash_of(key);
        if h < 2 then h += 2;
        return h;
    }

    fn grow(self: *Self) {
        old_cap := cast(u64) self.entries.capacity;
        old := self.entries.items;
        new_cap : u64 = 16;
        if old_cap != 0 then new_cap = old_cap * 2;

        bytes := cast(u32) (new_cap * sizeof(Entry));
        fresh := cast(*Entry) self.allocator.allocate(bytes).unwrap();

        mask := new_cap - 1;
        i : u64 = 0;
        while i < old_cap {
            src := old[i];
            if src.hash >= 2 {
                j := cast(u64) src.hash & mask;
                while fresh[j].hash != 0 {
                    j = (j + 1) & mask;
                }
                fresh[j] = src;
            }
            i += 1;
        }

        if old != null then self.allocator.deallocate(old);
        self.entries.items = fresh;
        self.entries.capacity = cast(i64) new_cap;
    }

    fn set(self: *Self, key: $K, value: $V, loc := #caller_location) {
        self.ensure_init(loc = loc);

        cap := cast(u64) self.entries.capacity;
        used := cast(u64) self.entries.count;
        if cap == 0 or (used + 1) * 4 > cap * 3 {
            self.grow();
            cap = cast(u64) self.entries.capacity;
        }

        h := self.slot_hash(key);
        mask := cap - 1;
        i := cast(u64) h & mask;
        while true {
            slot := &self.entries.items[i];
            if slot.hash == 0 {
                slot.key = key;
                slot.value = value;
                slot.hash = h;
                self.entries.count += 1;
                return;
            }
            if slot.hash == h and self.keys_equal(slot.key, key) {
                slot.value = value;
                return;
            }
            self.collision += 1;
            i = (i + 1) & mask;
        }
    }

    fn get(self: *Self, key: $K) -> Option($V) {
        cap := cast(u64) self.entries.capacity;
        if cap == 0 then return .None;

        h := self.slot_hash(key);
        mask := cap - 1;
        i := cast(u64) h & mask;
        while true {
            slot := &self.entries.items[i];
            if slot.hash == 0 then return .None;
            if slot.hash == h and self.keys_equal(slot.key, key) then return .Some(slot.value);
            i = (i + 1) & mask;
        }
        return .None;
    }
}

#import "Allocator";
#import "Error";
#import "Logger";
