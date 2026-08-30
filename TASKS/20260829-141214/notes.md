# pointer_arithmetic_with_+=_not_supported

- ID: 20260829-141214

## PRIORITY: 30

## STATUS: OPEN

## TAGS: semantic

## NOTES:

if we have an array and we want to increment the pointer by some value n.
```rs
fn 	main() {
	x : [] string = ["Hello", "Foo" , "Bar"];
	x.ptr += 1;
}
```

This will not work as it will error out by saying unknown arithmetic between ```string``` and ```untyped int```.
The temporary solution for such problem is

```rs
fn 	main() {
	x : [] string = ["Hello", "Foo" , "Bar"];
	x.ptr = x.ptr + 1;
}
```

Which is not convinient if you ask me. So I will fix it.