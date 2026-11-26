struct Linux { }
pub trait Directory($T) {
    self = *$T;
    fn read(self: *Self, path : string) -> ([]string, bool);
}

pub dir := Directory(Linux) {
    fn read(self: *Self, path : string) -> ([]string, bool) {
        directory: [..]string;
        dire := opendir(path);
        if dire == null {
            log_error("Could not read directory {}", path);
            return directory, false;
        }

        defer closedir(dire);
        while true {
            entry := readdir(dire);
            if entry == null then break;
            array_add(&directory, to_string(entry.d_name.ptr));
        }

        return directory, true;
    }
};

