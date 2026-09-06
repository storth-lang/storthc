# comptime function should not be accessible outside of comptime

-PRIORITY: 100
-STATUS: OPEN
-TAGS:

pub fn main(args: []string) -> i32 {
	a :: #comptime 20;
	a :: #comptime 20;
}
