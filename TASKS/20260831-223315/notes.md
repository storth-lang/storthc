# comptime_function_should_not_be_accessible_outside_of_comptime

- ID: 20260831-223315

## PRIORITY: 100

## STATUS: OPEN

## TAGS:

## NOTES:

pub fn main(args: []string) -> i32 {
	a :: #comptime 20;
	a :: #comptime 20;
}
