# 16 - Modules

## #import

```rs
#import "io";
#import "error";
```

Pulls in a module by name. Resolution order, relative to the importing file's own directory:

1. `<dir>/modules/<name>/module.st`
2. `$STORTHC_MODULE_PATH/<name>/module.st`, if that variable is set
3. `/usr/local/storthc/modules/<name>/module.st`

Every decl from the imported module gets renamed internally (`modulename__decl`) so it can't collide with anything, but you refer to it either bare (if the name is unambiguous across everything imported) or qualified: `io.print(...)`.

Only `pub` declarations are reachable from outside the module's own file.

## #load

Textual inclusion of a file's tokens, no renaming, no namespacing. Used for splitting one logical file across several physical ones, not for pulling in a separate module.

```rs
#load "helpers.st";
```

## Where imports can go

Declaration order doesn't matter. `#import` and `#load` can appear anywhere in the file, including after the code that uses what they bring in.
