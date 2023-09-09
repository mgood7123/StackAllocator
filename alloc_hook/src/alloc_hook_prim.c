/* ----------------------------------------------------------------------------
Copyright (c) 2018-2023, Microsoft Research, Daan Leijen
This is free software; you can redistribute it and/or modify it under the
terms of the MIT license. A copy of the license can be found in the file
"LICENSE" at the root of this distribution.
-----------------------------------------------------------------------------*/

// Select the implementation of the primitives
// depending on the OS.

#if defined(_WIN32)
    // VirtualAlloc (Windows)
    #include "alloc_hook_prim_win.c"
#elif defined(__wasi__)
    // wasi - Web Assembly System Interface
    // memory-grow or sbrk (Wasm)
    #define ALLOC_HOOK_USE_SBRK
    #include "alloc_hook_prim_wasi.c"
#else
    // mmap() (Linux, macOSX, BSD, Illumnos, Haiku, DragonFly, Android, etc.)
    #include "alloc_hook_prim_unix.c"
#endif
