/* ----------------------------------------------------------------------------
Copyright (c) 2018-2023, Microsoft Research, Daan Leijen
This is free software; you can redistribute it and/or modify it under the
terms of the MIT license. A copy of the license can be found in the file
"LICENSE" at the root of this distribution.
-----------------------------------------------------------------------------*/

#ifndef ALLOC_HOOK_TRACK_H
#define ALLOC_HOOK_TRACK_H

/* ------------------------------------------------------------------------------------------------------
Track memory ranges with macros for tools like Valgrind address sanitizer, or other memory checkers.
These can be defined for tracking allocation:

  #define alloc_hook_track_malloc_size(p,reqsize,size,zero)
  #define alloc_hook_track_free_size(p,_size)

The macros are set up such that the size passed to `alloc_hook_track_free_size`
always matches the size of `alloc_hook_track_malloc_size`. (currently, `size == alloc_hook_usable_size(p)`).
The `reqsize` is what the user requested, and `size >= reqsize`.
The `size` is either byte precise (and `size==reqsize`) if `ALLOC_HOOK_PADDING` is enabled,
or otherwise it is the usable block size which may be larger than the original request.
Use `_alloc_hook_block_size_of(void* p)` to get the full block size that was allocated (including padding etc).
The `zero` parameter is `true` if the allocated block is zero initialized.

Optional:

  #define alloc_hook_track_align(p,alignedp,offset,size)
  #define alloc_hook_track_resize(p,oldsize,newsize)
  #define alloc_hook_track_init()

The `alloc_hook_track_align` is called right after a `alloc_hook_track_malloc` for aligned pointers in a block.
The corresponding `alloc_hook_track_free` still uses the block start pointer and original size (corresponding to the `alloc_hook_track_malloc`).
The `alloc_hook_track_resize` is currently unused but could be called on reallocations within a block.
`alloc_hook_track_init` is called at program start.

The following macros are for tools like asan and valgrind to track whether memory is 
defined, undefined, or not accessible at all:

  #define alloc_hook_track_mem_defined(p,size)
  #define alloc_hook_track_mem_undefined(p,size)
  #define alloc_hook_track_mem_noaccess(p,size)

-------------------------------------------------------------------------------------------------------*/

#if ALLOC_HOOK_TRACK_VALGRIND
// valgrind tool

#define ALLOC_HOOK_TRACK_ENABLED      1
#define ALLOC_HOOK_TRACK_HEAP_DESTROY 1           // track free of individual blocks on heap_destroy
#define ALLOC_HOOK_TRACK_TOOL         "valgrind"

#include <valgrind/valgrind.h>
#include <valgrind/memcheck.h>

#define alloc_hook_track_malloc_size(p,reqsize,size,zero) VALGRIND_MALLOCLIKE_BLOCK(p,size,ALLOC_HOOK_PADDING_SIZE /*red zone*/,zero)
#define alloc_hook_track_free_size(p,_size)               VALGRIND_FREELIKE_BLOCK(p,ALLOC_HOOK_PADDING_SIZE /*red zone*/)
#define alloc_hook_track_resize(p,oldsize,newsize)        VALGRIND_RESIZEINPLACE_BLOCK(p,oldsize,newsize,ALLOC_HOOK_PADDING_SIZE /*red zone*/)
#define alloc_hook_track_mem_defined(p,size)              VALGRIND_MAKE_MEM_DEFINED(p,size)
#define alloc_hook_track_mem_undefined(p,size)            VALGRIND_MAKE_MEM_UNDEFINED(p,size)
#define alloc_hook_track_mem_noaccess(p,size)             VALGRIND_MAKE_MEM_NOACCESS(p,size)

#elif ALLOC_HOOK_TRACK_ASAN
// address sanitizer

#define ALLOC_HOOK_TRACK_ENABLED      1
#define ALLOC_HOOK_TRACK_HEAP_DESTROY 0
#define ALLOC_HOOK_TRACK_TOOL         "asan"

#include <sanitizer/asan_interface.h>

#define alloc_hook_track_malloc_size(p,reqsize,size,zero) ASAN_UNPOISON_MEMORY_REGION(p,size)
#define alloc_hook_track_free_size(p,size)                ASAN_POISON_MEMORY_REGION(p,size)
#define alloc_hook_track_mem_defined(p,size)              ASAN_UNPOISON_MEMORY_REGION(p,size)
#define alloc_hook_track_mem_undefined(p,size)            ASAN_UNPOISON_MEMORY_REGION(p,size)
#define alloc_hook_track_mem_noaccess(p,size)             ASAN_POISON_MEMORY_REGION(p,size)

#elif ALLOC_HOOK_TRACK_ETW
// windows event tracing

#define ALLOC_HOOK_TRACK_ENABLED      1
#define ALLOC_HOOK_TRACK_HEAP_DESTROY 1
#define ALLOC_HOOK_TRACK_TOOL         "ETW"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "../src/prim/windows/etw.h"

#define alloc_hook_track_init()                           EventRegistermicrosoft_windows_alloc_hook();
#define alloc_hook_track_malloc_size(p,reqsize,size,zero) EventWriteETW_ALLOC_HOOK_ALLOC((UINT64)(p), size)
#define alloc_hook_track_free_size(p,size)                EventWriteETW_ALLOC_HOOK_FREE((UINT64)(p), size)

#else
// no tracking

#define ALLOC_HOOK_TRACK_ENABLED      0
#define ALLOC_HOOK_TRACK_HEAP_DESTROY 0 
#define ALLOC_HOOK_TRACK_TOOL         "none"

#define alloc_hook_track_malloc_size(p,reqsize,size,zero)
#define alloc_hook_track_free_size(p,_size)

#endif

// -------------------
// Utility definitions

#ifndef alloc_hook_track_resize
#define alloc_hook_track_resize(p,oldsize,newsize)      alloc_hook_track_free_size(p,oldsize); alloc_hook_track_malloc(p,newsize,false)
#endif

#ifndef alloc_hook_track_align
#define alloc_hook_track_align(p,alignedp,offset,size)  alloc_hook_track_mem_noaccess(p,offset)
#endif

#ifndef alloc_hook_track_init
#define alloc_hook_track_init()
#endif

#ifndef alloc_hook_track_mem_defined
#define alloc_hook_track_mem_defined(p,size)
#endif

#ifndef alloc_hook_track_mem_undefined
#define alloc_hook_track_mem_undefined(p,size)
#endif

#ifndef alloc_hook_track_mem_noaccess
#define alloc_hook_track_mem_noaccess(p,size)
#endif


#if ALLOC_HOOK_PADDING
#define alloc_hook_track_malloc(p,reqsize,zero) \
  if ((p)!=NULL) { \
    alloc_hook_assert_internal(alloc_hook_usable_size(p)==(reqsize)); \
    alloc_hook_track_malloc_size(p,reqsize,reqsize,zero); \
  }
#else
#define alloc_hook_track_malloc(p,reqsize,zero) \
  if ((p)!=NULL) { \
    alloc_hook_assert_internal(alloc_hook_usable_size(p)>=(reqsize)); \
    alloc_hook_track_malloc_size(p,reqsize,alloc_hook_usable_size(p),zero); \
  }
#endif

#endif
