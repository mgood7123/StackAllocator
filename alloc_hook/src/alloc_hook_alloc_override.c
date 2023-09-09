/* ----------------------------------------------------------------------------
Copyright (c) 2018-2021, Microsoft Research, Daan Leijen
This is free software; you can redistribute it and/or modify it under the
terms of the MIT license. A copy of the license can be found in the file
"LICENSE" at the root of this distribution.
-----------------------------------------------------------------------------*/

#if !defined(ALLOC_HOOK_IN_ALLOC_C)
#error "this file should be included from 'alloc.c' (so aliases can work)"
#endif

#if defined(ALLOC_HOOK_MALLOC_OVERRIDE) && defined(_WIN32) && !(defined(ALLOC_HOOK_SHARED_LIB) && defined(_DLL))
#error "It is only possible to override "malloc" on Windows when building as a DLL (and linking the C runtime as a DLL)"
#endif

#if defined(ALLOC_HOOK_MALLOC_OVERRIDE) && !(defined(_WIN32))

#if defined(__APPLE__)
#include <AvailabilityMacros.h>
alloc_hook_decl_externc void   vfree(void* p);
alloc_hook_decl_externc size_t malloc_size(const void* p);
alloc_hook_decl_externc size_t malloc_good_size(size_t size);
#endif

// helper definition for C override of C++ new
typedef struct alloc_hook_nothrow_s { int _tag; } alloc_hook_nothrow_t;

// ------------------------------------------------------
// Override system malloc
// ------------------------------------------------------

#if (defined(__GNUC__) || defined(__clang__)) && !defined(__APPLE__) && !ALLOC_HOOK_TRACK_ENABLED
  // gcc, clang: use aliasing to alias the exported function to one of our `alloc_hook_` functions
  #if (defined(__GNUC__) && __GNUC__ >= 9)
    #pragma GCC diagnostic ignored "-Wattributes"  // or we get warnings that nodiscard is ignored on a forward
    #define ALLOC_HOOK_FORWARD(fun)      __attribute__((alias(#fun), used, visibility("default"), copy(fun)));
  #else
    #define ALLOC_HOOK_FORWARD(fun)      __attribute__((alias(#fun), used, visibility("default")));
  #endif
  #define ALLOC_HOOK_FORWARD1(fun,x)      ALLOC_HOOK_FORWARD(fun)
  #define ALLOC_HOOK_FORWARD2(fun,x,y)    ALLOC_HOOK_FORWARD(fun)
  #define ALLOC_HOOK_FORWARD3(fun,x,y,z)  ALLOC_HOOK_FORWARD(fun)
  #define ALLOC_HOOK_FORWARD0(fun,x)      ALLOC_HOOK_FORWARD(fun)
  #define ALLOC_HOOK_FORWARD02(fun,x,y)   ALLOC_HOOK_FORWARD(fun)
#else
  // otherwise use forwarding by calling our `alloc_hook_` function
  #define ALLOC_HOOK_FORWARD1(fun,x)      { return fun(x); }
  #define ALLOC_HOOK_FORWARD2(fun,x,y)    { return fun(x,y); }
  #define ALLOC_HOOK_FORWARD3(fun,x,y,z)  { return fun(x,y,z); }
  #define ALLOC_HOOK_FORWARD0(fun,x)      { fun(x); }
  #define ALLOC_HOOK_FORWARD02(fun,x,y)   { fun(x,y); }
#endif


#if defined(__APPLE__) && defined(ALLOC_HOOK_SHARED_LIB_EXPORT) && defined(ALLOC_HOOK_OSX_INTERPOSE)
  // define ALLOC_HOOK_OSX_IS_INTERPOSED as we should not provide forwarding definitions for
  // functions that are interposed (or the interposing does not work)
  #define ALLOC_HOOK_OSX_IS_INTERPOSED

  alloc_hook_decl_externc size_t alloc_hook_malloc_size_checked(void *p) {
    if (!alloc_hook_is_in_heap_region(p)) return 0;
    return alloc_hook_usable_size(p);
  }

  // use interposing so `DYLD_INSERT_LIBRARIES` works without `DYLD_FORCE_FLAT_NAMESPACE=1`
  // See: <https://books.google.com/books?id=K8vUkpOXhN4C&pg=PA73>
  struct alloc_hook_interpose_s {
    const void* replacement;
    const void* target;
  };
  #define ALLOC_HOOK_INTERPOSE_FUN(oldfun,newfun) { (const void*)&newfun, (const void*)&oldfun }
  #define ALLOC_HOOK_INTERPOSE_MI(fun)            ALLOC_HOOK_INTERPOSE_FUN(fun,alloc_hook_##fun)

  __attribute__((used)) static struct alloc_hook_interpose_s _alloc_hook_interposes[]  __attribute__((section("__DATA, __interpose"))) =
  {
    ALLOC_HOOK_INTERPOSE_MI(malloc),
    ALLOC_HOOK_INTERPOSE_MI(calloc),
    ALLOC_HOOK_INTERPOSE_MI(realloc),
    ALLOC_HOOK_INTERPOSE_MI(strdup),
    ALLOC_HOOK_INTERPOSE_MI(strndup),
    ALLOC_HOOK_INTERPOSE_MI(realpath),
    ALLOC_HOOK_INTERPOSE_MI(posix_memalign),
    ALLOC_HOOK_INTERPOSE_MI(reallocf),
    ALLOC_HOOK_INTERPOSE_MI(valloc),
    ALLOC_HOOK_INTERPOSE_FUN(malloc_size,alloc_hook_malloc_size_checked),
    ALLOC_HOOK_INTERPOSE_MI(malloc_good_size),
    #if defined(MAC_OS_X_VERSION_10_15) && MAC_OS_X_VERSION_MAX_ALLOWED >= MAC_OS_X_VERSION_10_15
    ALLOC_HOOK_INTERPOSE_MI(aligned_alloc),
    #endif
    #ifdef ALLOC_HOOK_OSX_ZONE
    // we interpose malloc_default_zone in alloc-override-osx.c so we can use alloc_hook_free safely
    ALLOC_HOOK_INTERPOSE_MI(free),
    ALLOC_HOOK_INTERPOSE_FUN(vfree,alloc_hook_free),
    #else
    // sometimes code allocates from default zone but deallocates using plain free :-( (like NxHashResizeToCapacity <https://github.com/nneonneo/osx-10.9-opensource/blob/master/objc4-551.1/runtime/hashtable2.mm>)
    ALLOC_HOOK_INTERPOSE_FUN(free,alloc_hook_cfree), // use safe free that checks if pointers are from us
    ALLOC_HOOK_INTERPOSE_FUN(vfree,alloc_hook_cfree),
    #endif
  };

  #ifdef __cplusplus
  extern "C" {
  #endif
  void  _ZdlPv(void* p);   // delete
  void  _ZdaPv(void* p);   // delete[]
  void  _ZdlPvm(void* p, size_t n);  // delete
  void  _ZdaPvm(void* p, size_t n);  // delete[]
  void* _Znwm(size_t n);  // new
  void* _Znam(size_t n);  // new[]
  void* _ZnwmRKSt9nothrow_t(size_t n, alloc_hook_nothrow_t tag); // new nothrow
  void* _ZnamRKSt9nothrow_t(size_t n, alloc_hook_nothrow_t tag); // new[] nothrow
  #ifdef __cplusplus
  }
  #endif
  __attribute__((used)) static struct alloc_hook_interpose_s _alloc_hook_cxx_interposes[]  __attribute__((section("__DATA, __interpose"))) =
  {
    ALLOC_HOOK_INTERPOSE_FUN(_ZdlPv,alloc_hook_free),
    ALLOC_HOOK_INTERPOSE_FUN(_ZdaPv,alloc_hook_free),
    ALLOC_HOOK_INTERPOSE_FUN(_ZdlPvm,alloc_hook_free_size),
    ALLOC_HOOK_INTERPOSE_FUN(_ZdaPvm,alloc_hook_free_size),
    ALLOC_HOOK_INTERPOSE_FUN(_Znwm,alloc_hook_new),
    ALLOC_HOOK_INTERPOSE_FUN(_Znam,alloc_hook_new),
    ALLOC_HOOK_INTERPOSE_FUN(_ZnwmRKSt9nothrow_t,alloc_hook_new_nothrow),
    ALLOC_HOOK_INTERPOSE_FUN(_ZnamRKSt9nothrow_t,alloc_hook_new_nothrow),
  };

#elif defined(_MSC_VER)
  // cannot override malloc unless using a dll.
  // we just override new/delete which does work in a static library.
#else
  // On all other systems forward to our API
  alloc_hook_decl_export void* malloc(size_t size)              ALLOC_HOOK_FORWARD1(alloc_hook_malloc, size)
  alloc_hook_decl_export void* calloc(size_t size, size_t n)    ALLOC_HOOK_FORWARD2(alloc_hook_calloc, size, n)
  alloc_hook_decl_export void* realloc(void* p, size_t newsize) ALLOC_HOOK_FORWARD2(alloc_hook_realloc, p, newsize)
  alloc_hook_decl_export void  free(void* p)                    ALLOC_HOOK_FORWARD0(alloc_hook_free, p)
#endif

#if (defined(__GNUC__) || defined(__clang__)) && !defined(__APPLE__)
#pragma GCC visibility push(default)
#endif

// ------------------------------------------------------
// Override new/delete
// This is not really necessary as they usually call
// malloc/free anyway, but it improves performance.
// ------------------------------------------------------
#ifdef __cplusplus
  // ------------------------------------------------------
  // With a C++ compiler we override the new/delete operators.
  // see <https://en.cppreference.com/w/cpp/memory/new/operator_new>
  // ------------------------------------------------------
  #include <new>

  #ifndef ALLOC_HOOK_OSX_IS_INTERPOSED
    void operator delete(void* p) noexcept              ALLOC_HOOK_FORWARD0(alloc_hook_free,p)
    void operator delete[](void* p) noexcept            ALLOC_HOOK_FORWARD0(alloc_hook_free,p)

    void* operator new(std::size_t n) noexcept(false)   ALLOC_HOOK_FORWARD1(alloc_hook_new,n)
    void* operator new[](std::size_t n) noexcept(false) ALLOC_HOOK_FORWARD1(alloc_hook_new,n)

    void* operator new  (std::size_t n, const std::nothrow_t& tag) noexcept { ALLOC_HOOK_UNUSED(tag); return alloc_hook_new_nothrow(n); }
    void* operator new[](std::size_t n, const std::nothrow_t& tag) noexcept { ALLOC_HOOK_UNUSED(tag); return alloc_hook_new_nothrow(n); }

    #if (__cplusplus >= 201402L || _MSC_VER >= 1916)
    void operator delete  (void* p, std::size_t n) noexcept ALLOC_HOOK_FORWARD02(alloc_hook_free_size,p,n)
    void operator delete[](void* p, std::size_t n) noexcept ALLOC_HOOK_FORWARD02(alloc_hook_free_size,p,n)
    #endif
  #endif

  #if (__cplusplus > 201402L && defined(__cpp_aligned_new)) && (!defined(__GNUC__) || (__GNUC__ > 5))
  void operator delete  (void* p, std::align_val_t al) noexcept { alloc_hook_free_aligned(p, static_cast<size_t>(al)); }
  void operator delete[](void* p, std::align_val_t al) noexcept { alloc_hook_free_aligned(p, static_cast<size_t>(al)); }
  void operator delete  (void* p, std::size_t n, std::align_val_t al) noexcept { alloc_hook_free_size_aligned(p, n, static_cast<size_t>(al)); };
  void operator delete[](void* p, std::size_t n, std::align_val_t al) noexcept { alloc_hook_free_size_aligned(p, n, static_cast<size_t>(al)); };
  void operator delete  (void* p, std::align_val_t al, const std::nothrow_t&) noexcept { alloc_hook_free_aligned(p, static_cast<size_t>(al)); }
  void operator delete[](void* p, std::align_val_t al, const std::nothrow_t&) noexcept { alloc_hook_free_aligned(p, static_cast<size_t>(al)); }

  void* operator new( std::size_t n, std::align_val_t al)   noexcept(false) { return alloc_hook_new_aligned(n, static_cast<size_t>(al)); }
  void* operator new[]( std::size_t n, std::align_val_t al) noexcept(false) { return alloc_hook_new_aligned(n, static_cast<size_t>(al)); }
  void* operator new  (std::size_t n, std::align_val_t al, const std::nothrow_t&) noexcept { return alloc_hook_new_aligned_nothrow(n, static_cast<size_t>(al)); }
  void* operator new[](std::size_t n, std::align_val_t al, const std::nothrow_t&) noexcept { return alloc_hook_new_aligned_nothrow(n, static_cast<size_t>(al)); }
  #endif

#elif (defined(__GNUC__) || defined(__clang__))
  // ------------------------------------------------------
  // Override by defining the mangled C++ names of the operators (as
  // used by GCC and CLang).
  // See <https://itanium-cxx-abi.github.io/cxx-abi/abi.html#mangling>
  // ------------------------------------------------------

  void _ZdlPv(void* p)            ALLOC_HOOK_FORWARD0(alloc_hook_free,p) // delete
  void _ZdaPv(void* p)            ALLOC_HOOK_FORWARD0(alloc_hook_free,p) // delete[]
  void _ZdlPvm(void* p, size_t n) ALLOC_HOOK_FORWARD02(alloc_hook_free_size,p,n)
  void _ZdaPvm(void* p, size_t n) ALLOC_HOOK_FORWARD02(alloc_hook_free_size,p,n)
  void _ZdlPvSt11align_val_t(void* p, size_t al)            { alloc_hook_free_aligned(p,al); }
  void _ZdaPvSt11align_val_t(void* p, size_t al)            { alloc_hook_free_aligned(p,al); }
  void _ZdlPvmSt11align_val_t(void* p, size_t n, size_t al) { alloc_hook_free_size_aligned(p,n,al); }
  void _ZdaPvmSt11align_val_t(void* p, size_t n, size_t al) { alloc_hook_free_size_aligned(p,n,al); }

  #if (ALLOC_HOOK_INTPTR_SIZE==8)
    void* _Znwm(size_t n)                             ALLOC_HOOK_FORWARD1(alloc_hook_new,n)  // new 64-bit
    void* _Znam(size_t n)                             ALLOC_HOOK_FORWARD1(alloc_hook_new,n)  // new[] 64-bit
    void* _ZnwmRKSt9nothrow_t(size_t n, alloc_hook_nothrow_t tag) { ALLOC_HOOK_UNUSED(tag); return alloc_hook_new_nothrow(n); }
    void* _ZnamRKSt9nothrow_t(size_t n, alloc_hook_nothrow_t tag) { ALLOC_HOOK_UNUSED(tag); return alloc_hook_new_nothrow(n); }
    void* _ZnwmSt11align_val_t(size_t n, size_t al)   ALLOC_HOOK_FORWARD2(alloc_hook_new_aligned, n, al)
    void* _ZnamSt11align_val_t(size_t n, size_t al)   ALLOC_HOOK_FORWARD2(alloc_hook_new_aligned, n, al)
    void* _ZnwmSt11align_val_tRKSt9nothrow_t(size_t n, size_t al, alloc_hook_nothrow_t tag) { ALLOC_HOOK_UNUSED(tag); return alloc_hook_new_aligned_nothrow(n,al); }
    void* _ZnamSt11align_val_tRKSt9nothrow_t(size_t n, size_t al, alloc_hook_nothrow_t tag) { ALLOC_HOOK_UNUSED(tag); return alloc_hook_new_aligned_nothrow(n,al); }
  #elif (ALLOC_HOOK_INTPTR_SIZE==4)
    void* _Znwj(size_t n)                             ALLOC_HOOK_FORWARD1(alloc_hook_new,n)  // new 64-bit
    void* _Znaj(size_t n)                             ALLOC_HOOK_FORWARD1(alloc_hook_new,n)  // new[] 64-bit
    void* _ZnwjRKSt9nothrow_t(size_t n, alloc_hook_nothrow_t tag) { ALLOC_HOOK_UNUSED(tag); return alloc_hook_new_nothrow(n); }
    void* _ZnajRKSt9nothrow_t(size_t n, alloc_hook_nothrow_t tag) { ALLOC_HOOK_UNUSED(tag); return alloc_hook_new_nothrow(n); }
    void* _ZnwjSt11align_val_t(size_t n, size_t al)   ALLOC_HOOK_FORWARD2(alloc_hook_new_aligned, n, al)
    void* _ZnajSt11align_val_t(size_t n, size_t al)   ALLOC_HOOK_FORWARD2(alloc_hook_new_aligned, n, al)
    void* _ZnwjSt11align_val_tRKSt9nothrow_t(size_t n, size_t al, alloc_hook_nothrow_t tag) { ALLOC_HOOK_UNUSED(tag); return alloc_hook_new_aligned_nothrow(n,al); }
    void* _ZnajSt11align_val_tRKSt9nothrow_t(size_t n, size_t al, alloc_hook_nothrow_t tag) { ALLOC_HOOK_UNUSED(tag); return alloc_hook_new_aligned_nothrow(n,al); }
  #else
    #error "define overloads for new/delete for this platform (just for performance, can be skipped)"
  #endif
#endif // __cplusplus

// ------------------------------------------------------
// Further Posix & Unix functions definitions
// ------------------------------------------------------

#ifdef __cplusplus
extern "C" {
#endif

#ifndef ALLOC_HOOK_OSX_IS_INTERPOSED
  // Forward Posix/Unix calls as well
  void*  reallocf(void* p, size_t newsize) ALLOC_HOOK_FORWARD2(alloc_hook_reallocf,p,newsize)
  size_t malloc_size(const void* p)        ALLOC_HOOK_FORWARD1(alloc_hook_usable_size,p)
  #if !defined(__ANDROID__) && !defined(__FreeBSD__)
  size_t malloc_usable_size(void *p)       ALLOC_HOOK_FORWARD1(alloc_hook_usable_size,p)
  #else
  size_t malloc_usable_size(const void *p) ALLOC_HOOK_FORWARD1(alloc_hook_usable_size,p)
  #endif

  // No forwarding here due to aliasing/name mangling issues
  void*  valloc(size_t size)               { return alloc_hook_valloc(size); }
  void   vfree(void* p)                    { alloc_hook_free(p); }
  size_t malloc_good_size(size_t size)     { return alloc_hook_malloc_good_size(size); }
  int    posix_memalign(void** p, size_t alignment, size_t size) { return alloc_hook_posix_memalign(p, alignment, size); }

  // `aligned_alloc` is only available when __USE_ISOC11 is defined.
  // Note: it seems __USE_ISOC11 is not defined in musl (and perhaps other libc's) so we only check
  // for it if using glibc.
  // Note: Conda has a custom glibc where `aligned_alloc` is declared `static inline` and we cannot
  // override it, but both _ISOC11_SOURCE and __USE_ISOC11 are undefined in Conda GCC7 or GCC9.
  // Fortunately, in the case where `aligned_alloc` is declared as `static inline` it
  // uses internally `memalign`, `posix_memalign`, or `_aligned_malloc` so we  can avoid overriding it ourselves.
  #if !defined(__GLIBC__) || __USE_ISOC11
  void* aligned_alloc(size_t alignment, size_t size) { return alloc_hook_aligned_alloc(alignment, size); }
  #endif
#endif

// no forwarding here due to aliasing/name mangling issues
void  cfree(void* p)                                    { alloc_hook_free(p); }
void* pvalloc(size_t size)                              { return alloc_hook_pvalloc(size); }
void* reallocarray(void* p, size_t count, size_t size)  { return alloc_hook_reallocarray(p, count, size); }
int   reallocarr(void* p, size_t count, size_t size)    { return alloc_hook_reallocarr(p, count, size); }
void* memalign(size_t alignment, size_t size)           { return alloc_hook_memalign(alignment, size); }
void* _aligned_malloc(size_t alignment, size_t size)    { return alloc_hook_aligned_alloc(alignment, size); }

#if defined(__wasi__)
  // forward __libc interface (see PR #667)
  void* __libc_malloc(size_t size)                      ALLOC_HOOK_FORWARD1(alloc_hook_malloc, size)
  void* __libc_calloc(size_t count, size_t size)        ALLOC_HOOK_FORWARD2(alloc_hook_calloc, count, size)
  void* __libc_realloc(void* p, size_t size)            ALLOC_HOOK_FORWARD2(alloc_hook_realloc, p, size)
  void  __libc_free(void* p)                            ALLOC_HOOK_FORWARD0(alloc_hook_free, p)
  void* __libc_memalign(size_t alignment, size_t size)  { return alloc_hook_memalign(alignment, size); }

#elif defined(__GLIBC__) && defined(__linux__)
  // forward __libc interface (needed for glibc-based Linux distributions)
  void* __libc_malloc(size_t size)                      ALLOC_HOOK_FORWARD1(alloc_hook_malloc,size)
  void* __libc_calloc(size_t count, size_t size)        ALLOC_HOOK_FORWARD2(alloc_hook_calloc,count,size)
  void* __libc_realloc(void* p, size_t size)            ALLOC_HOOK_FORWARD2(alloc_hook_realloc,p,size)
  void  __libc_free(void* p)                            ALLOC_HOOK_FORWARD0(alloc_hook_free,p)
  void  __libc_cfree(void* p)                           ALLOC_HOOK_FORWARD0(alloc_hook_free,p)

  void* __libc_valloc(size_t size)                      { return alloc_hook_valloc(size); }
  void* __libc_pvalloc(size_t size)                     { return alloc_hook_pvalloc(size); }
  void* __libc_memalign(size_t alignment, size_t size)  { return alloc_hook_memalign(alignment,size); }
  int   __posix_memalign(void** p, size_t alignment, size_t size) { return alloc_hook_posix_memalign(p,alignment,size); }
#endif

#ifdef __cplusplus
}
#endif

#if (defined(__GNUC__) || defined(__clang__)) && !defined(__APPLE__)
#pragma GCC visibility pop
#endif

#endif // ALLOC_HOOK_MALLOC_OVERRIDE && !_WIN32
