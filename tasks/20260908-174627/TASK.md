# Binding generator

- STATUS: OPEN
- PRIORITY: 100
- TAGS: bindgen

typedef struct Foo Foo can not be parsed and generated with bind generator

```
typedef struct Foo Foo;
```

My bind gen can not know forward decalration of such opaque structures
so it should do

```
using Foo *void;
```