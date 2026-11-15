#import "Libc";

trait Ui($T) {
    fn create_window(name: string, width: i32,
                     height: i32) -> (bool, Self);
}

// NOTE: this is a test structure
struct X11 {
    dpy: *void;
    win: *void;
}

enum OS {
    Linux,
    Win32,
}

struct WinGdi {
    handle: *void;
}

fn XOpenDisplay(item: *void) -> *void {
    return cast(*void)76;
}

fn XCreateWindow(item: *void, name: string, width: i32, height: i32) -> *void {
    return cast(*void)76;
}

fn CreateWindowEx(name: string, width: i32, height: i32) -> *void {
    return cast(*void)76;
}

pub fn main() -> i32 {
    ui := Ui(X11) {
        fn create_window(name: string, width: i32,
                         height: i32) -> (bool, Self) {
            dpy := XOpenDisplay(null);
            if dpy == null then return false, {};

            win := XCreateWindow(dpy, name, width, height);
            if win == null then return false, {};
            return true, .{ dpy, win };
        }
    };
    ok, win := ui.create_window("Foo", 800, 600);
    if !ok then return 1;
    printf("%d\n", *(cast(*i32)&win));
    
    return 0;
}
