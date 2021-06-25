Mimalloc for Unikraft
=====================

This is the port of the Mimalloc [0] general-purpose memory allocator for
Unikraft as an external library.

How to use this allocator in your unikernel application:

- select "Mimalloc" in `ukboot > Default memory allocator`
- pass at least 256MiB of memory to the unikernel

[0] https://microsoft.github.io/mimalloc/
