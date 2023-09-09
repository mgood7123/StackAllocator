/* ----------------------------------------------------------------------------
Copyright (c) 2018-2020 Microsoft Research, Daan Leijen
This is free software; you can redistribute it and/or modify it under the
terms of the MIT license. A copy of the license can be found in the file
"LICENSE" at the root of this distribution.
-----------------------------------------------------------------------------*/

#ifndef ALLOC_HOOK_OVERRIDE_H
#define ALLOC_HOOK_OVERRIDE_H

/* ----------------------------------------------------------------------------
This header can be used to statically redirect malloc/free and new/delete
to the alloc_hook variants. This can be useful if one can include this file on
each source file in a project (but be careful when using external code to
not accidentally mix pointers from different allocators).
-----------------------------------------------------------------------------*/

#include <alloc_hook.h>

// Standard C allocation
#define malloc(n)               alloc_hook_malloc(n)
#define calloc(n,c)             alloc_hook_calloc(n,c)
#define realloc(p,n)            alloc_hook_realloc(p,n)
#define free(p)                 alloc_hook_free(p)

#define strdup(s)               alloc_hook_strdup(s)
#define strndup(s,n)              alloc_hook_strndup(s,n)
#define realpath(f,n)           alloc_hook_realpath(f,n)

// Microsoft extensions
#define _expand(p,n)            alloc_hook_expand(p,n)
#define _msize(p)               alloc_hook_usable_size(p)
#define _recalloc(p,n,c)        alloc_hook_recalloc(p,n,c)

#define _strdup(s)              alloc_hook_strdup(s)
#define _strndup(s,n)           alloc_hook_strndup(s,n)
#define _wcsdup(s)              (wchar_t*)alloc_hook_wcsdup((const unsigned short*)(s))
#define _mbsdup(s)              alloc_hook_mbsdup(s)
#define _dupenv_s(b,n,v)        alloc_hook_dupenv_s(b,n,v)
#define _wdupenv_s(b,n,v)       alloc_hook_wdupenv_s((unsigned short*)(b),n,(const unsigned short*)(v))

// Various Posix and Unix variants
#define reallocf(p,n)           alloc_hook_reallocf(p,n)
#define malloc_size(p)          alloc_hook_usable_size(p)
#define malloc_usable_size(p)   alloc_hook_usable_size(p)
#define cfree(p)                alloc_hook_free(p)

#define valloc(n)               alloc_hook_valloc(n)
#define pvalloc(n)              alloc_hook_pvalloc(n)
#define reallocarray(p,s,n)     alloc_hook_reallocarray(p,s,n)
#define reallocarr(p,s,n)       alloc_hook_reallocarr(p,s,n)
#define memalign(a,n)           alloc_hook_memalign(a,n)
#define aligned_alloc(a,n)      alloc_hook_aligned_alloc(a,n)
#define posix_memalign(p,a,n)   alloc_hook_posix_memalign(p,a,n)
#define _posix_memalign(p,a,n)  alloc_hook_posix_memalign(p,a,n)

// Microsoft aligned variants
#define _aligned_malloc(n,a)                  alloc_hook_malloc_aligned(n,a)
#define _aligned_realloc(p,n,a)               alloc_hook_realloc_aligned(p,n,a)
#define _aligned_recalloc(p,s,n,a)            alloc_hook_aligned_recalloc(p,s,n,a)
#define _aligned_msize(p,a,o)                 alloc_hook_usable_size(p)
#define _aligned_free(p)                      alloc_hook_free(p)
#define _aligned_offset_malloc(n,a,o)         alloc_hook_malloc_aligned_at(n,a,o)
#define _aligned_offset_realloc(p,n,a,o)      alloc_hook_realloc_aligned_at(p,n,a,o)
#define _aligned_offset_recalloc(p,s,n,a,o)   alloc_hook_recalloc_aligned_at(p,s,n,a,o)

#endif // ALLOC_HOOK_OVERRIDE_H
