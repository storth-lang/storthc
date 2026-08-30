# Storthc

Blame the skill on the developer not the tool. As tool are just temporary solution to fix the devs skill issue.
The dev should always know what they are doing regardless of the tool.

A programming language aimed for ease of use for people desiring simplicity. C is already the best language and this is not acting like a replacment pipe yourselves down.
It is merely my own hobby language to make an OS. And no rust is BS of a language so I could care less about it.

This is meant for the enjoyment of the humanity and not putting politics into a language.

## Build

As long as you have a c compiler building it is self explanatory. You just run make.

```sh
make -j$(nproc)
```


Papers and Ideas:

The implementation of the standard library is done via inline assembly of linux syscalls.
Right now for majority of the implementation of it I barely care less about platform as getting the language to utmost
usability is much more important.

The linux syscall reference I used comes from (@qy9)[https://codeberg.org/memfd/libsmh]
Do not we do not have any libc in the standard library making the language very portable as all linux OS do have.
Linux syscall.

- stdlib design: https://codeberg.org/memfd/libsmh
- Hash Table: https://arxiv.org/pdf/2501.02305
- SSA IR paper: https://c9x.me/compile/bib/braun13cc.pdf