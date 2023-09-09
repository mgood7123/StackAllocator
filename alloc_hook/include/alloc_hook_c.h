/* ----------------------------------------------------------------------------
Copyright (c) 2018-2023, Microsoft Research, Daan Leijen
This is free software; you can redistribute it and/or modify it under the
terms of the MIT license. A copy of the license can be found in the file
"LICENSE" at the root of this distribution.
-----------------------------------------------------------------------------*/

#ifndef ALLOC_HOOK_H
#define ALLOC_HOOK_H

#define ALLOC_HOOK_MALLOC_VERSION 212   // major + 2 digits minor

// ------------------------------------------------------
// Compiler specific attributes
// ------------------------------------------------------

#ifdef __cplusplus
  #if (__cplusplus >= 201103L) || (_MSC_VER > 1900)  // C++11
    #define alloc_hook_attr_noexcept   noexcept
  #else
    #define alloc_hook_attr_noexcept   throw()
  #endif
#else
  #define alloc_hook_attr_noexcept
#endif

#if defined(__cplusplus) && (__cplusplus >= 201703)
  #define alloc_hook_decl_nodiscard    [[nodiscard]]
#elif (defined(__GNUC__) && (__GNUC__ >= 4)) || defined(__clang__)  // includes clang, icc, and clang-cl
  #define alloc_hook_decl_nodiscard    __attribute__((warn_unused_result))
#elif defined(_HAS_NODISCARD)
  #define alloc_hook_decl_nodiscard    _NODISCARD
#elif (_MSC_VER >= 1700)
  #define alloc_hook_decl_nodiscard    _Check_return_
#else
  #define alloc_hook_decl_nodiscard
#endif

#if defined(_MSC_VER) || defined(__MINGW32__)
  #if !defined(ALLOC_HOOK_SHARED_LIB)
    #define alloc_hook_decl_export
  #elif defined(ALLOC_HOOK_SHARED_LIB_EXPORT)
    #define alloc_hook_decl_export              __declspec(dllexport)
  #else
    #define alloc_hook_decl_export              __declspec(dllimport)
  #endif
  #if defined(__MINGW32__)
    #define alloc_hook_decl_restrict
    #define alloc_hook_attr_malloc              __attribute__((malloc))
  #else
    #if (_MSC_VER >= 1900) && !defined(__EDG__)
      #define alloc_hook_decl_restrict          __declspec(allocator) __declspec(restrict)
    #else
      #define alloc_hook_decl_restrict          __declspec(restrict)
    #endif
    #define alloc_hook_attr_malloc
  #endif
  #define alloc_hook_cdecl                      __cdecl
  #define alloc_hook_attr_alloc_size(s)
  #define alloc_hook_attr_alloc_size2(s1,s2)
  #define alloc_hook_attr_alloc_align(p)
#elif defined(__GNUC__)                 // includes clang and icc
  #if defined(ALLOC_HOOK_SHARED_LIB) && defined(ALLOC_HOOK_SHARED_LIB_EXPORT)
    #define alloc_hook_decl_export              __attribute__((visibility("default")))
  #else
    #define alloc_hook_decl_export
  #endif
  #define alloc_hook_cdecl                      // leads to warnings... __attribute__((cdecl))
  #define alloc_hook_decl_restrict
  #define alloc_hook_attr_malloc                __attribute__((malloc))
  #if (defined(__clang_major__) && (__clang_major__ < 4)) || (__GNUC__ < 5)
    #define alloc_hook_attr_alloc_size(s)
    #define alloc_hook_attr_alloc_size2(s1,s2)
    #define alloc_hook_attr_alloc_align(p)
  #elif defined(__INTEL_COMPILER)
    #define alloc_hook_attr_alloc_size(s)       __attribute__((alloc_size(s)))
    #define alloc_hook_attr_alloc_size2(s1,s2)  __attribute__((alloc_size(s1,s2)))
    #define alloc_hook_attr_alloc_align(p)
  #else
    #define alloc_hook_attr_alloc_size(s)       __attribute__((alloc_size(s)))
    #define alloc_hook_attr_alloc_size2(s1,s2)  __attribute__((alloc_size(s1,s2)))
    #define alloc_hook_attr_alloc_align(p)      __attribute__((alloc_align(p)))
  #endif
#else
  #define alloc_hook_cdecl
  #define alloc_hook_decl_export
  #define alloc_hook_decl_restrict
  #define alloc_hook_attr_malloc
  #define alloc_hook_attr_alloc_size(s)
  #define alloc_hook_attr_alloc_size2(s1,s2)
  #define alloc_hook_attr_alloc_align(p)
#endif

// ------------------------------------------------------
// Includes
// ------------------------------------------------------

#include <stddef.h>     // size_t
#include <stdbool.h>    // bool
#include <stdint.h>     // INTPTR_MAX

#ifdef __cplusplus
extern "C" {
#endif

// ------------------------------------------------------
// Standard malloc interface
// ------------------------------------------------------

alloc_hook_decl_nodiscard alloc_hook_decl_export alloc_hook_decl_restrict void* alloc_hook_malloc(size_t size)  alloc_hook_attr_noexcept alloc_hook_attr_malloc alloc_hook_attr_alloc_size(1);
alloc_hook_decl_nodiscard alloc_hook_decl_export alloc_hook_decl_restrict void* alloc_hook_calloc(size_t count, size_t size)  alloc_hook_attr_noexcept alloc_hook_attr_malloc alloc_hook_attr_alloc_size2(1,2);
alloc_hook_decl_nodiscard alloc_hook_decl_export void* alloc_hook_realloc(void* p, size_t newsize)      alloc_hook_attr_noexcept alloc_hook_attr_alloc_size(2);
alloc_hook_decl_export void* alloc_hook_expand(void* p, size_t newsize)                         alloc_hook_attr_noexcept alloc_hook_attr_alloc_size(2);

alloc_hook_decl_export void alloc_hook_free(void* p) alloc_hook_attr_noexcept;
alloc_hook_decl_nodiscard alloc_hook_decl_export alloc_hook_decl_restrict char* alloc_hook_strdup(const char* s) alloc_hook_attr_noexcept alloc_hook_attr_malloc;
alloc_hook_decl_nodiscard alloc_hook_decl_export alloc_hook_decl_restrict char* alloc_hook_strndup(const char* s, size_t n) alloc_hook_attr_noexcept alloc_hook_attr_malloc;
alloc_hook_decl_nodiscard alloc_hook_decl_export alloc_hook_decl_restrict char* alloc_hook_realpath(const char* fname, char* resolved_name) alloc_hook_attr_noexcept alloc_hook_attr_malloc;

// ------------------------------------------------------
// Extended functionality
// ------------------------------------------------------
#define ALLOC_HOOK_SMALL_WSIZE_MAX  (128)
#define ALLOC_HOOK_SMALL_SIZE_MAX   (ALLOC_HOOK_SMALL_WSIZE_MAX*sizeof(void*))

alloc_hook_decl_nodiscard alloc_hook_decl_export alloc_hook_decl_restrict void* alloc_hook_malloc_small(size_t size) alloc_hook_attr_noexcept alloc_hook_attr_malloc alloc_hook_attr_alloc_size(1);
alloc_hook_decl_nodiscard alloc_hook_decl_export alloc_hook_decl_restrict void* alloc_hook_zalloc_small(size_t size) alloc_hook_attr_noexcept alloc_hook_attr_malloc alloc_hook_attr_alloc_size(1);
alloc_hook_decl_nodiscard alloc_hook_decl_export alloc_hook_decl_restrict void* alloc_hook_zalloc(size_t size)       alloc_hook_attr_noexcept alloc_hook_attr_malloc alloc_hook_attr_alloc_size(1);

alloc_hook_decl_nodiscard alloc_hook_decl_export alloc_hook_decl_restrict void* alloc_hook_mallocn(size_t count, size_t size) alloc_hook_attr_noexcept alloc_hook_attr_malloc alloc_hook_attr_alloc_size2(1,2);
alloc_hook_decl_nodiscard alloc_hook_decl_export void* alloc_hook_reallocn(void* p, size_t count, size_t size)        alloc_hook_attr_noexcept alloc_hook_attr_alloc_size2(2,3);
alloc_hook_decl_nodiscard alloc_hook_decl_export void* alloc_hook_reallocf(void* p, size_t newsize)                   alloc_hook_attr_noexcept alloc_hook_attr_alloc_size(2);

alloc_hook_decl_nodiscard alloc_hook_decl_export size_t alloc_hook_usable_size(const void* p) alloc_hook_attr_noexcept;
alloc_hook_decl_nodiscard alloc_hook_decl_export size_t alloc_hook_good_size(size_t size)     alloc_hook_attr_noexcept;


// ------------------------------------------------------
// Internals
// ------------------------------------------------------

typedef void (alloc_hook_cdecl alloc_hook_deferred_free_fun)(bool force, unsigned long long heartbeat, void* arg);
alloc_hook_decl_export void alloc_hook_register_deferred_free(alloc_hook_deferred_free_fun* deferred_free, void* arg) alloc_hook_attr_noexcept;

typedef void (alloc_hook_cdecl alloc_hook_output_fun)(const char* msg, void* arg);
alloc_hook_decl_export void alloc_hook_register_output(alloc_hook_output_fun* out, void* arg) alloc_hook_attr_noexcept;

typedef void (alloc_hook_cdecl alloc_hook_error_fun)(int err, void* arg);
alloc_hook_decl_export void alloc_hook_register_error(alloc_hook_error_fun* fun, void* arg);

alloc_hook_decl_export void alloc_hook_collect(bool force)    alloc_hook_attr_noexcept;
alloc_hook_decl_export int  alloc_hook_version(void)          alloc_hook_attr_noexcept;
alloc_hook_decl_export void alloc_hook_stats_reset(void)      alloc_hook_attr_noexcept;
alloc_hook_decl_export void alloc_hook_stats_merge(void)      alloc_hook_attr_noexcept;
alloc_hook_decl_export void alloc_hook_stats_print(void* out) alloc_hook_attr_noexcept;  // backward compatibility: `out` is ignored and should be NULL
alloc_hook_decl_export void alloc_hook_stats_print_out(alloc_hook_output_fun* out, void* arg) alloc_hook_attr_noexcept;

alloc_hook_decl_export void alloc_hook_process_init(void)     alloc_hook_attr_noexcept;
alloc_hook_decl_export void alloc_hook_thread_init(void)      alloc_hook_attr_noexcept;
alloc_hook_decl_export void alloc_hook_thread_done(void)      alloc_hook_attr_noexcept;
alloc_hook_decl_export void alloc_hook_thread_stats_print_out(alloc_hook_output_fun* out, void* arg) alloc_hook_attr_noexcept;

alloc_hook_decl_export void alloc_hook_process_info(size_t* elapsed_msecs, size_t* user_msecs, size_t* system_msecs,
                                    size_t* current_rss, size_t* peak_rss,
                                    size_t* current_commit, size_t* peak_commit, size_t* page_faults) alloc_hook_attr_noexcept;

// -------------------------------------------------------------------------------------
// Aligned allocation
// Note that `alignment` always follows `size` for consistency with unaligned
// allocation, but unfortunately this differs from `posix_memalign` and `aligned_alloc`.
// -------------------------------------------------------------------------------------

alloc_hook_decl_nodiscard alloc_hook_decl_export alloc_hook_decl_restrict void* alloc_hook_malloc_aligned(size_t size, size_t alignment) alloc_hook_attr_noexcept alloc_hook_attr_malloc alloc_hook_attr_alloc_size(1) alloc_hook_attr_alloc_align(2);
alloc_hook_decl_nodiscard alloc_hook_decl_export alloc_hook_decl_restrict void* alloc_hook_malloc_aligned_at(size_t size, size_t alignment, size_t offset) alloc_hook_attr_noexcept alloc_hook_attr_malloc alloc_hook_attr_alloc_size(1);
alloc_hook_decl_nodiscard alloc_hook_decl_export alloc_hook_decl_restrict void* alloc_hook_zalloc_aligned(size_t size, size_t alignment) alloc_hook_attr_noexcept alloc_hook_attr_malloc alloc_hook_attr_alloc_size(1) alloc_hook_attr_alloc_align(2);
alloc_hook_decl_nodiscard alloc_hook_decl_export alloc_hook_decl_restrict void* alloc_hook_zalloc_aligned_at(size_t size, size_t alignment, size_t offset) alloc_hook_attr_noexcept alloc_hook_attr_malloc alloc_hook_attr_alloc_size(1);
alloc_hook_decl_nodiscard alloc_hook_decl_export alloc_hook_decl_restrict void* alloc_hook_calloc_aligned(size_t count, size_t size, size_t alignment) alloc_hook_attr_noexcept alloc_hook_attr_malloc alloc_hook_attr_alloc_size2(1,2) alloc_hook_attr_alloc_align(3);
alloc_hook_decl_nodiscard alloc_hook_decl_export alloc_hook_decl_restrict void* alloc_hook_calloc_aligned_at(size_t count, size_t size, size_t alignment, size_t offset) alloc_hook_attr_noexcept alloc_hook_attr_malloc alloc_hook_attr_alloc_size2(1,2);
alloc_hook_decl_nodiscard alloc_hook_decl_export void* alloc_hook_realloc_aligned(void* p, size_t newsize, size_t alignment) alloc_hook_attr_noexcept alloc_hook_attr_alloc_size(2) alloc_hook_attr_alloc_align(3);
alloc_hook_decl_nodiscard alloc_hook_decl_export void* alloc_hook_realloc_aligned_at(void* p, size_t newsize, size_t alignment, size_t offset) alloc_hook_attr_noexcept alloc_hook_attr_alloc_size(2);


// -------------------------------------------------------------------------------------
// Heaps: first-class, but can only allocate from the same thread that created it.
// -------------------------------------------------------------------------------------

struct alloc_hook_heap_s;
typedef struct alloc_hook_heap_s alloc_hook_heap_t;

alloc_hook_decl_nodiscard alloc_hook_decl_export alloc_hook_heap_t* alloc_hook_heap_new(void);
alloc_hook_decl_export void       alloc_hook_heap_delete(alloc_hook_heap_t* heap);
alloc_hook_decl_export void       alloc_hook_heap_destroy(alloc_hook_heap_t* heap);
alloc_hook_decl_export alloc_hook_heap_t* alloc_hook_heap_set_default(alloc_hook_heap_t* heap);
alloc_hook_decl_export alloc_hook_heap_t* alloc_hook_heap_get_default(void);
alloc_hook_decl_export alloc_hook_heap_t* alloc_hook_heap_get_backing(void);
alloc_hook_decl_export void       alloc_hook_heap_collect(alloc_hook_heap_t* heap, bool force) alloc_hook_attr_noexcept;

alloc_hook_decl_nodiscard alloc_hook_decl_export alloc_hook_decl_restrict void* alloc_hook_heap_malloc(alloc_hook_heap_t* heap, size_t size) alloc_hook_attr_noexcept alloc_hook_attr_malloc alloc_hook_attr_alloc_size(2);
alloc_hook_decl_nodiscard alloc_hook_decl_export alloc_hook_decl_restrict void* alloc_hook_heap_zalloc(alloc_hook_heap_t* heap, size_t size) alloc_hook_attr_noexcept alloc_hook_attr_malloc alloc_hook_attr_alloc_size(2);
alloc_hook_decl_nodiscard alloc_hook_decl_export alloc_hook_decl_restrict void* alloc_hook_heap_calloc(alloc_hook_heap_t* heap, size_t count, size_t size) alloc_hook_attr_noexcept alloc_hook_attr_malloc alloc_hook_attr_alloc_size2(2, 3);
alloc_hook_decl_nodiscard alloc_hook_decl_export alloc_hook_decl_restrict void* alloc_hook_heap_mallocn(alloc_hook_heap_t* heap, size_t count, size_t size) alloc_hook_attr_noexcept alloc_hook_attr_malloc alloc_hook_attr_alloc_size2(2, 3);
alloc_hook_decl_nodiscard alloc_hook_decl_export alloc_hook_decl_restrict void* alloc_hook_heap_malloc_small(alloc_hook_heap_t* heap, size_t size) alloc_hook_attr_noexcept alloc_hook_attr_malloc alloc_hook_attr_alloc_size(2);

alloc_hook_decl_nodiscard alloc_hook_decl_export void* alloc_hook_heap_realloc(alloc_hook_heap_t* heap, void* p, size_t newsize)              alloc_hook_attr_noexcept alloc_hook_attr_alloc_size(3);
alloc_hook_decl_nodiscard alloc_hook_decl_export void* alloc_hook_heap_reallocn(alloc_hook_heap_t* heap, void* p, size_t count, size_t size)  alloc_hook_attr_noexcept alloc_hook_attr_alloc_size2(3,4);
alloc_hook_decl_nodiscard alloc_hook_decl_export void* alloc_hook_heap_reallocf(alloc_hook_heap_t* heap, void* p, size_t newsize)             alloc_hook_attr_noexcept alloc_hook_attr_alloc_size(3);

alloc_hook_decl_nodiscard alloc_hook_decl_export alloc_hook_decl_restrict char* alloc_hook_heap_strdup(alloc_hook_heap_t* heap, const char* s)            alloc_hook_attr_noexcept alloc_hook_attr_malloc;
alloc_hook_decl_nodiscard alloc_hook_decl_export alloc_hook_decl_restrict char* alloc_hook_heap_strndup(alloc_hook_heap_t* heap, const char* s, size_t n) alloc_hook_attr_noexcept alloc_hook_attr_malloc;
alloc_hook_decl_nodiscard alloc_hook_decl_export alloc_hook_decl_restrict char* alloc_hook_heap_realpath(alloc_hook_heap_t* heap, const char* fname, char* resolved_name) alloc_hook_attr_noexcept alloc_hook_attr_malloc;

alloc_hook_decl_nodiscard alloc_hook_decl_export alloc_hook_decl_restrict void* alloc_hook_heap_malloc_aligned(alloc_hook_heap_t* heap, size_t size, size_t alignment) alloc_hook_attr_noexcept alloc_hook_attr_malloc alloc_hook_attr_alloc_size(2) alloc_hook_attr_alloc_align(3);
alloc_hook_decl_nodiscard alloc_hook_decl_export alloc_hook_decl_restrict void* alloc_hook_heap_malloc_aligned_at(alloc_hook_heap_t* heap, size_t size, size_t alignment, size_t offset) alloc_hook_attr_noexcept alloc_hook_attr_malloc alloc_hook_attr_alloc_size(2);
alloc_hook_decl_nodiscard alloc_hook_decl_export alloc_hook_decl_restrict void* alloc_hook_heap_zalloc_aligned(alloc_hook_heap_t* heap, size_t size, size_t alignment) alloc_hook_attr_noexcept alloc_hook_attr_malloc alloc_hook_attr_alloc_size(2) alloc_hook_attr_alloc_align(3);
alloc_hook_decl_nodiscard alloc_hook_decl_export alloc_hook_decl_restrict void* alloc_hook_heap_zalloc_aligned_at(alloc_hook_heap_t* heap, size_t size, size_t alignment, size_t offset) alloc_hook_attr_noexcept alloc_hook_attr_malloc alloc_hook_attr_alloc_size(2);
alloc_hook_decl_nodiscard alloc_hook_decl_export alloc_hook_decl_restrict void* alloc_hook_heap_calloc_aligned(alloc_hook_heap_t* heap, size_t count, size_t size, size_t alignment) alloc_hook_attr_noexcept alloc_hook_attr_malloc alloc_hook_attr_alloc_size2(2, 3) alloc_hook_attr_alloc_align(4);
alloc_hook_decl_nodiscard alloc_hook_decl_export alloc_hook_decl_restrict void* alloc_hook_heap_calloc_aligned_at(alloc_hook_heap_t* heap, size_t count, size_t size, size_t alignment, size_t offset) alloc_hook_attr_noexcept alloc_hook_attr_malloc alloc_hook_attr_alloc_size2(2, 3);
alloc_hook_decl_nodiscard alloc_hook_decl_export void* alloc_hook_heap_realloc_aligned(alloc_hook_heap_t* heap, void* p, size_t newsize, size_t alignment) alloc_hook_attr_noexcept alloc_hook_attr_alloc_size(3) alloc_hook_attr_alloc_align(4);
alloc_hook_decl_nodiscard alloc_hook_decl_export void* alloc_hook_heap_realloc_aligned_at(alloc_hook_heap_t* heap, void* p, size_t newsize, size_t alignment, size_t offset) alloc_hook_attr_noexcept alloc_hook_attr_alloc_size(3);


// --------------------------------------------------------------------------------
// Zero initialized re-allocation.
// Only valid on memory that was originally allocated with zero initialization too.
// e.g. `alloc_hook_calloc`, `alloc_hook_zalloc`, `alloc_hook_zalloc_aligned` etc.
// see <https://github.com/microsoft/alloc_hook/issues/63#issuecomment-508272992>
// --------------------------------------------------------------------------------

alloc_hook_decl_nodiscard alloc_hook_decl_export void* alloc_hook_rezalloc(void* p, size_t newsize)                alloc_hook_attr_noexcept alloc_hook_attr_alloc_size(2);
alloc_hook_decl_nodiscard alloc_hook_decl_export void* alloc_hook_recalloc(void* p, size_t newcount, size_t size)  alloc_hook_attr_noexcept alloc_hook_attr_alloc_size2(2,3);

alloc_hook_decl_nodiscard alloc_hook_decl_export void* alloc_hook_rezalloc_aligned(void* p, size_t newsize, size_t alignment) alloc_hook_attr_noexcept alloc_hook_attr_alloc_size(2) alloc_hook_attr_alloc_align(3);
alloc_hook_decl_nodiscard alloc_hook_decl_export void* alloc_hook_rezalloc_aligned_at(void* p, size_t newsize, size_t alignment, size_t offset) alloc_hook_attr_noexcept alloc_hook_attr_alloc_size(2);
alloc_hook_decl_nodiscard alloc_hook_decl_export void* alloc_hook_recalloc_aligned(void* p, size_t newcount, size_t size, size_t alignment) alloc_hook_attr_noexcept alloc_hook_attr_alloc_size2(2,3) alloc_hook_attr_alloc_align(4);
alloc_hook_decl_nodiscard alloc_hook_decl_export void* alloc_hook_recalloc_aligned_at(void* p, size_t newcount, size_t size, size_t alignment, size_t offset) alloc_hook_attr_noexcept alloc_hook_attr_alloc_size2(2,3);

alloc_hook_decl_nodiscard alloc_hook_decl_export void* alloc_hook_heap_rezalloc(alloc_hook_heap_t* heap, void* p, size_t newsize)                alloc_hook_attr_noexcept alloc_hook_attr_alloc_size(3);
alloc_hook_decl_nodiscard alloc_hook_decl_export void* alloc_hook_heap_recalloc(alloc_hook_heap_t* heap, void* p, size_t newcount, size_t size)  alloc_hook_attr_noexcept alloc_hook_attr_alloc_size2(3,4);

alloc_hook_decl_nodiscard alloc_hook_decl_export void* alloc_hook_heap_rezalloc_aligned(alloc_hook_heap_t* heap, void* p, size_t newsize, size_t alignment) alloc_hook_attr_noexcept alloc_hook_attr_alloc_size(3) alloc_hook_attr_alloc_align(4);
alloc_hook_decl_nodiscard alloc_hook_decl_export void* alloc_hook_heap_rezalloc_aligned_at(alloc_hook_heap_t* heap, void* p, size_t newsize, size_t alignment, size_t offset) alloc_hook_attr_noexcept alloc_hook_attr_alloc_size(3);
alloc_hook_decl_nodiscard alloc_hook_decl_export void* alloc_hook_heap_recalloc_aligned(alloc_hook_heap_t* heap, void* p, size_t newcount, size_t size, size_t alignment) alloc_hook_attr_noexcept alloc_hook_attr_alloc_size2(3,4) alloc_hook_attr_alloc_align(5);
alloc_hook_decl_nodiscard alloc_hook_decl_export void* alloc_hook_heap_recalloc_aligned_at(alloc_hook_heap_t* heap, void* p, size_t newcount, size_t size, size_t alignment, size_t offset) alloc_hook_attr_noexcept alloc_hook_attr_alloc_size2(3,4);


// ------------------------------------------------------
// Analysis
// ------------------------------------------------------

alloc_hook_decl_export bool alloc_hook_heap_contains_block(alloc_hook_heap_t* heap, const void* p);
alloc_hook_decl_export bool alloc_hook_heap_check_owned(alloc_hook_heap_t* heap, const void* p);
alloc_hook_decl_export bool alloc_hook_check_owned(const void* p);

// An area of heap space contains blocks of a single size.
typedef struct alloc_hook_heap_area_s {
  void*  blocks;      // start of the area containing heap blocks
  size_t reserved;    // bytes reserved for this area (virtual)
  size_t committed;   // current available bytes for this area
  size_t used;        // number of allocated blocks
  size_t block_size;  // size in bytes of each block
  size_t full_block_size; // size in bytes of a full block including padding and metadata.
} alloc_hook_heap_area_t;

typedef bool (alloc_hook_cdecl alloc_hook_block_visit_fun)(const alloc_hook_heap_t* heap, const alloc_hook_heap_area_t* area, void* block, size_t block_size, void* arg);

alloc_hook_decl_export bool alloc_hook_heap_visit_blocks(const alloc_hook_heap_t* heap, bool visit_all_blocks, alloc_hook_block_visit_fun* visitor, void* arg);

// Experimental
alloc_hook_decl_nodiscard alloc_hook_decl_export bool alloc_hook_is_in_heap_region(const void* p) alloc_hook_attr_noexcept;
alloc_hook_decl_nodiscard alloc_hook_decl_export bool alloc_hook_is_redirected(void) alloc_hook_attr_noexcept;

alloc_hook_decl_export int alloc_hook_reserve_huge_os_pages_interleave(size_t pages, size_t numa_nodes, size_t timeout_msecs) alloc_hook_attr_noexcept;
alloc_hook_decl_export int alloc_hook_reserve_huge_os_pages_at(size_t pages, int numa_node, size_t timeout_msecs) alloc_hook_attr_noexcept;

alloc_hook_decl_export int  alloc_hook_reserve_os_memory(size_t size, bool commit, bool allow_large) alloc_hook_attr_noexcept;
alloc_hook_decl_export bool alloc_hook_manage_os_memory(void* start, size_t size, bool is_committed, bool is_large, bool is_zero, int numa_node) alloc_hook_attr_noexcept;

alloc_hook_decl_export void alloc_hook_debug_show_arenas(void) alloc_hook_attr_noexcept;

// Experimental: heaps associated with specific memory arena's
typedef int alloc_hook_arena_id_t;
alloc_hook_decl_export void* alloc_hook_arena_area(alloc_hook_arena_id_t arena_id, size_t* size);
alloc_hook_decl_export int   alloc_hook_reserve_huge_os_pages_at_ex(size_t pages, int numa_node, size_t timeout_msecs, bool exclusive, alloc_hook_arena_id_t* arena_id) alloc_hook_attr_noexcept;
alloc_hook_decl_export int   alloc_hook_reserve_os_memory_ex(size_t size, bool commit, bool allow_large, bool exclusive, alloc_hook_arena_id_t* arena_id) alloc_hook_attr_noexcept;
alloc_hook_decl_export bool  alloc_hook_manage_os_memory_ex(void* start, size_t size, bool is_committed, bool is_large, bool is_zero, int numa_node, bool exclusive, alloc_hook_arena_id_t* arena_id) alloc_hook_attr_noexcept;

#if ALLOC_HOOK_MALLOC_VERSION >= 182
// Create a heap that only allocates in the specified arena
alloc_hook_decl_nodiscard alloc_hook_decl_export alloc_hook_heap_t* alloc_hook_heap_new_in_arena(alloc_hook_arena_id_t arena_id);
#endif

// deprecated
alloc_hook_decl_export int  alloc_hook_reserve_huge_os_pages(size_t pages, double max_secs, size_t* pages_reserved) alloc_hook_attr_noexcept;


// ------------------------------------------------------
// Convenience
// ------------------------------------------------------

#define alloc_hook_malloc_tp(tp)                ((tp*)alloc_hook_malloc(sizeof(tp)))
#define alloc_hook_zalloc_tp(tp)                ((tp*)alloc_hook_zalloc(sizeof(tp)))
#define alloc_hook_calloc_tp(tp,n)              ((tp*)alloc_hook_calloc(n,sizeof(tp)))
#define alloc_hook_mallocn_tp(tp,n)             ((tp*)alloc_hook_mallocn(n,sizeof(tp)))
#define alloc_hook_reallocn_tp(p,tp,n)          ((tp*)alloc_hook_reallocn(p,n,sizeof(tp)))
#define alloc_hook_recalloc_tp(p,tp,n)          ((tp*)alloc_hook_recalloc(p,n,sizeof(tp)))

#define alloc_hook_heap_malloc_tp(hp,tp)        ((tp*)alloc_hook_heap_malloc(hp,sizeof(tp)))
#define alloc_hook_heap_zalloc_tp(hp,tp)        ((tp*)alloc_hook_heap_zalloc(hp,sizeof(tp)))
#define alloc_hook_heap_calloc_tp(hp,tp,n)      ((tp*)alloc_hook_heap_calloc(hp,n,sizeof(tp)))
#define alloc_hook_heap_mallocn_tp(hp,tp,n)     ((tp*)alloc_hook_heap_mallocn(hp,n,sizeof(tp)))
#define alloc_hook_heap_reallocn_tp(hp,p,tp,n)  ((tp*)alloc_hook_heap_reallocn(hp,p,n,sizeof(tp)))
#define alloc_hook_heap_recalloc_tp(hp,p,tp,n)  ((tp*)alloc_hook_heap_recalloc(hp,p,n,sizeof(tp)))


// ------------------------------------------------------
// Options
// ------------------------------------------------------

typedef enum alloc_hook_option_e {
  // stable options
  alloc_hook_option_show_errors,              // print error messages
  alloc_hook_option_show_stats,               // print statistics on termination
  alloc_hook_option_verbose,                  // print verbose messages
  // the following options are experimental (see src/options.h)
  alloc_hook_option_eager_commit,             // eager commit segments? (after `eager_commit_delay` segments) (=1)
  alloc_hook_option_arena_eager_commit,       // eager commit arenas? Use 2 to enable just on overcommit systems (=2)
  alloc_hook_option_purge_decommits,          // should a memory purge decommit (or only reset) (=1)
  alloc_hook_option_allow_large_os_pages,     // allow large (2MiB) OS pages, implies eager commit
  alloc_hook_option_reserve_huge_os_pages,    // reserve N huge OS pages (1GiB/page) at startup
  alloc_hook_option_reserve_huge_os_pages_at, // reserve huge OS pages at a specific NUMA node
  alloc_hook_option_reserve_os_memory,        // reserve specified amount of OS memory in an arena at startup
  alloc_hook_option_deprecated_segment_cache,
  alloc_hook_option_deprecated_page_reset,
  alloc_hook_option_abandoned_page_purge,     // immediately purge delayed purges on thread termination
  alloc_hook_option_deprecated_segment_reset, 
  alloc_hook_option_eager_commit_delay,       
  alloc_hook_option_purge_delay,              // memory purging is delayed by N milli seconds; use 0 for immediate purging or -1 for no purging at all.
  alloc_hook_option_use_numa_nodes,           // 0 = use all available numa nodes, otherwise use at most N nodes.
  alloc_hook_option_limit_os_alloc,           // 1 = do not use OS memory for allocation (but only programmatically reserved arenas)
  alloc_hook_option_os_tag,                   // tag used for OS logging (macOS only for now)
  alloc_hook_option_max_errors,               // issue at most N error messages
  alloc_hook_option_max_warnings,             // issue at most N warning messages
  alloc_hook_option_max_segment_reclaim,      
  alloc_hook_option_destroy_on_exit,          // if set, release all memory on exit; sometimes used for dynamic unloading but can be unsafe.
  alloc_hook_option_arena_reserve,            // initial memory size in KiB for arena reservation (1GiB on 64-bit)
  alloc_hook_option_arena_purge_mult,         
  alloc_hook_option_purge_extend_delay,
  _alloc_hook_option_last,
  // legacy option names
  alloc_hook_option_large_os_pages = alloc_hook_option_allow_large_os_pages,
  alloc_hook_option_eager_region_commit = alloc_hook_option_arena_eager_commit,
  alloc_hook_option_reset_decommits = alloc_hook_option_purge_decommits,
  alloc_hook_option_reset_delay = alloc_hook_option_purge_delay,
  alloc_hook_option_abandoned_page_reset = alloc_hook_option_abandoned_page_purge
} alloc_hook_option_t;


alloc_hook_decl_nodiscard alloc_hook_decl_export bool alloc_hook_option_is_enabled(alloc_hook_option_t option);
alloc_hook_decl_export void alloc_hook_option_enable(alloc_hook_option_t option);
alloc_hook_decl_export void alloc_hook_option_disable(alloc_hook_option_t option);
alloc_hook_decl_export void alloc_hook_option_set_enabled(alloc_hook_option_t option, bool enable);
alloc_hook_decl_export void alloc_hook_option_set_enabled_default(alloc_hook_option_t option, bool enable);

alloc_hook_decl_nodiscard alloc_hook_decl_export long   alloc_hook_option_get(alloc_hook_option_t option);
alloc_hook_decl_nodiscard alloc_hook_decl_export long   alloc_hook_option_get_clamp(alloc_hook_option_t option, long min, long max);
alloc_hook_decl_nodiscard alloc_hook_decl_export size_t alloc_hook_option_get_size(alloc_hook_option_t option);
alloc_hook_decl_export void alloc_hook_option_set(alloc_hook_option_t option, long value);
alloc_hook_decl_export void alloc_hook_option_set_default(alloc_hook_option_t option, long value);

// ------------------------------------------------------
// Logging
// ------------------------------------------------------

alloc_hook_decl_export void alloc_hook_warning_message(const char* fmt, ...);
alloc_hook_decl_export void alloc_hook_verbose_message(const char* fmt, ...);
alloc_hook_decl_export void alloc_hook_trace_message(const char* fmt, ...);
alloc_hook_decl_export void alloc_hook_error_message(int err, const char* fmt, ...);


// -------------------------------------------------------------------------------------------------------
// "mi" prefixed implementations of various posix, Unix, Windows, and C++ allocation functions.
// (This can be convenient when providing overrides of these functions as done in `alloc_hook-override.h`.)
// note: we use `alloc_hook_cfree` as "checked free" and it checks if the pointer is in our heap before free-ing.
// -------------------------------------------------------------------------------------------------------

alloc_hook_decl_export void  alloc_hook_cfree(void* p) alloc_hook_attr_noexcept;
alloc_hook_decl_export void* alloc_hook__expand(void* p, size_t newsize) alloc_hook_attr_noexcept;
alloc_hook_decl_nodiscard alloc_hook_decl_export size_t alloc_hook_malloc_size(const void* p)        alloc_hook_attr_noexcept;
alloc_hook_decl_nodiscard alloc_hook_decl_export size_t alloc_hook_malloc_good_size(size_t size)     alloc_hook_attr_noexcept;
alloc_hook_decl_nodiscard alloc_hook_decl_export size_t alloc_hook_malloc_usable_size(const void *p) alloc_hook_attr_noexcept;

alloc_hook_decl_export int alloc_hook_posix_memalign(void** p, size_t alignment, size_t size)   alloc_hook_attr_noexcept;
alloc_hook_decl_nodiscard alloc_hook_decl_export alloc_hook_decl_restrict void* alloc_hook_memalign(size_t alignment, size_t size) alloc_hook_attr_noexcept alloc_hook_attr_malloc alloc_hook_attr_alloc_size(2) alloc_hook_attr_alloc_align(1);
alloc_hook_decl_nodiscard alloc_hook_decl_export alloc_hook_decl_restrict void* alloc_hook_valloc(size_t size)  alloc_hook_attr_noexcept alloc_hook_attr_malloc alloc_hook_attr_alloc_size(1);
alloc_hook_decl_nodiscard alloc_hook_decl_export alloc_hook_decl_restrict void* alloc_hook_pvalloc(size_t size) alloc_hook_attr_noexcept alloc_hook_attr_malloc alloc_hook_attr_alloc_size(1);
alloc_hook_decl_nodiscard alloc_hook_decl_export alloc_hook_decl_restrict void* alloc_hook_aligned_alloc(size_t alignment, size_t size) alloc_hook_attr_noexcept alloc_hook_attr_malloc alloc_hook_attr_alloc_size(2) alloc_hook_attr_alloc_align(1);

alloc_hook_decl_nodiscard alloc_hook_decl_export void* alloc_hook_reallocarray(void* p, size_t count, size_t size) alloc_hook_attr_noexcept alloc_hook_attr_alloc_size2(2,3);
alloc_hook_decl_nodiscard alloc_hook_decl_export int   alloc_hook_reallocarr(void* p, size_t count, size_t size) alloc_hook_attr_noexcept;
alloc_hook_decl_nodiscard alloc_hook_decl_export void* alloc_hook_aligned_recalloc(void* p, size_t newcount, size_t size, size_t alignment) alloc_hook_attr_noexcept;
alloc_hook_decl_nodiscard alloc_hook_decl_export void* alloc_hook_aligned_offset_recalloc(void* p, size_t newcount, size_t size, size_t alignment, size_t offset) alloc_hook_attr_noexcept;

alloc_hook_decl_nodiscard alloc_hook_decl_export alloc_hook_decl_restrict unsigned short* alloc_hook_wcsdup(const unsigned short* s) alloc_hook_attr_noexcept alloc_hook_attr_malloc;
alloc_hook_decl_nodiscard alloc_hook_decl_export alloc_hook_decl_restrict unsigned char*  alloc_hook_mbsdup(const unsigned char* s)  alloc_hook_attr_noexcept alloc_hook_attr_malloc;
alloc_hook_decl_export int alloc_hook_dupenv_s(char** buf, size_t* size, const char* name)                      alloc_hook_attr_noexcept;
alloc_hook_decl_export int alloc_hook_wdupenv_s(unsigned short** buf, size_t* size, const unsigned short* name) alloc_hook_attr_noexcept;

alloc_hook_decl_export void alloc_hook_free_size(void* p, size_t size)                           alloc_hook_attr_noexcept;
alloc_hook_decl_export void alloc_hook_free_size_aligned(void* p, size_t size, size_t alignment) alloc_hook_attr_noexcept;
alloc_hook_decl_export void alloc_hook_free_aligned(void* p, size_t alignment)                   alloc_hook_attr_noexcept;

// The `alloc_hook_new` wrappers implement C++ semantics on out-of-memory instead of directly returning `NULL`.
// (and call `std::get_new_handler` and potentially raise a `std::bad_alloc` exception).
alloc_hook_decl_nodiscard alloc_hook_decl_export alloc_hook_decl_restrict void* alloc_hook_new(size_t size)                   alloc_hook_attr_malloc alloc_hook_attr_alloc_size(1);
alloc_hook_decl_nodiscard alloc_hook_decl_export alloc_hook_decl_restrict void* alloc_hook_new_aligned(size_t size, size_t alignment) alloc_hook_attr_malloc alloc_hook_attr_alloc_size(1) alloc_hook_attr_alloc_align(2);
alloc_hook_decl_nodiscard alloc_hook_decl_export alloc_hook_decl_restrict void* alloc_hook_new_nothrow(size_t size)           alloc_hook_attr_noexcept alloc_hook_attr_malloc alloc_hook_attr_alloc_size(1);
alloc_hook_decl_nodiscard alloc_hook_decl_export alloc_hook_decl_restrict void* alloc_hook_new_aligned_nothrow(size_t size, size_t alignment) alloc_hook_attr_noexcept alloc_hook_attr_malloc alloc_hook_attr_alloc_size(1) alloc_hook_attr_alloc_align(2);
alloc_hook_decl_nodiscard alloc_hook_decl_export alloc_hook_decl_restrict void* alloc_hook_new_n(size_t count, size_t size)   alloc_hook_attr_malloc alloc_hook_attr_alloc_size2(1, 2);
alloc_hook_decl_nodiscard alloc_hook_decl_export void* alloc_hook_new_realloc(void* p, size_t newsize)                alloc_hook_attr_alloc_size(2);
alloc_hook_decl_nodiscard alloc_hook_decl_export void* alloc_hook_new_reallocn(void* p, size_t newcount, size_t size) alloc_hook_attr_alloc_size2(2, 3);

alloc_hook_decl_nodiscard alloc_hook_decl_export alloc_hook_decl_restrict void* alloc_hook_heap_alloc_new(alloc_hook_heap_t* heap, size_t size)                alloc_hook_attr_malloc alloc_hook_attr_alloc_size(2);
alloc_hook_decl_nodiscard alloc_hook_decl_export alloc_hook_decl_restrict void* alloc_hook_heap_alloc_new_n(alloc_hook_heap_t* heap, size_t count, size_t size) alloc_hook_attr_malloc alloc_hook_attr_alloc_size2(2, 3);

#ifdef __cplusplus
}
#endif

// ---------------------------------------------------------------------------------------------
// Implement the C++ std::allocator interface for use in STL containers.
// (note: see `alloc_hook-new-delete.h` for overriding the new/delete operators globally)
// ---------------------------------------------------------------------------------------------
#ifdef __cplusplus

#include <cstddef>     // std::size_t
#include <cstdint>     // PTRDIFF_MAX
#if (__cplusplus >= 201103L) || (_MSC_VER > 1900)  // C++11
#include <type_traits> // std::true_type
#include <utility>     // std::forward
#endif

template<class T> struct _alloc_hook_stl_allocator_common {
  typedef T                 value_type;
  typedef std::size_t       size_type;
  typedef std::ptrdiff_t    difference_type;
  typedef value_type&       reference;
  typedef value_type const& const_reference;
  typedef value_type*       pointer;
  typedef value_type const* const_pointer;

  #if ((__cplusplus >= 201103L) || (_MSC_VER > 1900))  // C++11
  using propagate_on_container_copy_assignment = std::true_type;
  using propagate_on_container_move_assignment = std::true_type;
  using propagate_on_container_swap            = std::true_type;
  template <class U, class ...Args> void construct(U* p, Args&& ...args) { ::new(p) U(std::forward<Args>(args)...); }
  template <class U> void destroy(U* p) alloc_hook_attr_noexcept { p->~U(); }
  #else
  void construct(pointer p, value_type const& val) { ::new(p) value_type(val); }
  void destroy(pointer p) { p->~value_type(); }
  #endif

  size_type     max_size() const alloc_hook_attr_noexcept { return (PTRDIFF_MAX/sizeof(value_type)); }
  pointer       address(reference x) const        { return &x; }
  const_pointer address(const_reference x) const  { return &x; }
};

template<class T> struct alloc_hook_stl_allocator : public _alloc_hook_stl_allocator_common<T> {
  using typename _alloc_hook_stl_allocator_common<T>::size_type;
  using typename _alloc_hook_stl_allocator_common<T>::value_type;
  using typename _alloc_hook_stl_allocator_common<T>::pointer;
  template <class U> struct rebind { typedef alloc_hook_stl_allocator<U> other; };

  alloc_hook_stl_allocator()                                             alloc_hook_attr_noexcept = default;
  alloc_hook_stl_allocator(const alloc_hook_stl_allocator&)                      alloc_hook_attr_noexcept = default;
  template<class U> alloc_hook_stl_allocator(const alloc_hook_stl_allocator<U>&) alloc_hook_attr_noexcept { }
  alloc_hook_stl_allocator  select_on_container_copy_construction() const { return *this; }
  void              deallocate(T* p, size_type) { alloc_hook_free(p); }

  #if (__cplusplus >= 201703L)  // C++17
  alloc_hook_decl_nodiscard T* allocate(size_type count) { return static_cast<T*>(alloc_hook_new_n(count, sizeof(T))); }
  alloc_hook_decl_nodiscard T* allocate(size_type count, const void*) { return allocate(count); }
  #else
  alloc_hook_decl_nodiscard pointer allocate(size_type count, const void* = 0) { return static_cast<pointer>(alloc_hook_new_n(count, sizeof(value_type))); }
  #endif

  #if ((__cplusplus >= 201103L) || (_MSC_VER > 1900))  // C++11
  using is_always_equal = std::true_type;
  #endif
};

template<class T1,class T2> bool operator==(const alloc_hook_stl_allocator<T1>& , const alloc_hook_stl_allocator<T2>& ) alloc_hook_attr_noexcept { return true; }
template<class T1,class T2> bool operator!=(const alloc_hook_stl_allocator<T1>& , const alloc_hook_stl_allocator<T2>& ) alloc_hook_attr_noexcept { return false; }


#if (__cplusplus >= 201103L) || (_MSC_VER >= 1900)  // C++11
#define ALLOC_HOOK_HAS_HEAP_STL_ALLOCATOR 1

#include <memory>      // std::shared_ptr

// Common base class for STL allocators in a specific heap
template<class T, bool _alloc_hook_destroy> struct _alloc_hook_heap_stl_allocator_common : public _alloc_hook_stl_allocator_common<T> {
  using typename _alloc_hook_stl_allocator_common<T>::size_type;
  using typename _alloc_hook_stl_allocator_common<T>::value_type;
  using typename _alloc_hook_stl_allocator_common<T>::pointer;

  _alloc_hook_heap_stl_allocator_common(alloc_hook_heap_t* hp) : heap(hp) { }    /* will not delete nor destroy the passed in heap */

  #if (__cplusplus >= 201703L)  // C++17
  alloc_hook_decl_nodiscard T* allocate(size_type count) { return static_cast<T*>(alloc_hook_heap_alloc_new_n(this->heap.get(), count, sizeof(T))); }
  alloc_hook_decl_nodiscard T* allocate(size_type count, const void*) { return allocate(count); }
  #else
  alloc_hook_decl_nodiscard pointer allocate(size_type count, const void* = 0) { return static_cast<pointer>(alloc_hook_heap_alloc_new_n(this->heap.get(), count, sizeof(value_type))); }
  #endif

  #if ((__cplusplus >= 201103L) || (_MSC_VER > 1900))  // C++11
  using is_always_equal = std::false_type;
  #endif

  void collect(bool force) { alloc_hook_heap_collect(this->heap.get(), force); }
  template<class U> bool is_equal(const _alloc_hook_heap_stl_allocator_common<U, _alloc_hook_destroy>& x) const { return (this->heap == x.heap); }

protected:
  std::shared_ptr<alloc_hook_heap_t> heap;
  template<class U, bool D> friend struct _alloc_hook_heap_stl_allocator_common;
  
  _alloc_hook_heap_stl_allocator_common() {
    alloc_hook_heap_t* hp = alloc_hook_heap_new();
    this->heap.reset(hp, (_alloc_hook_destroy ? &heap_destroy : &heap_delete));  /* calls heap_delete/destroy when the refcount drops to zero */
  }
  _alloc_hook_heap_stl_allocator_common(const _alloc_hook_heap_stl_allocator_common& x) alloc_hook_attr_noexcept : heap(x.heap) { }
  template<class U> _alloc_hook_heap_stl_allocator_common(const _alloc_hook_heap_stl_allocator_common<U, _alloc_hook_destroy>& x) alloc_hook_attr_noexcept : heap(x.heap) { }

private:
  static void heap_delete(alloc_hook_heap_t* hp)  { if (hp != NULL) { alloc_hook_heap_delete(hp); } }
  static void heap_destroy(alloc_hook_heap_t* hp) { if (hp != NULL) { alloc_hook_heap_destroy(hp); } }
};

// STL allocator allocation in a specific heap
template<class T> struct alloc_hook_heap_stl_allocator : public _alloc_hook_heap_stl_allocator_common<T, false> {
  using typename _alloc_hook_heap_stl_allocator_common<T, false>::size_type;
  alloc_hook_heap_stl_allocator() : _alloc_hook_heap_stl_allocator_common<T, false>() { } // creates fresh heap that is deleted when the destructor is called
  alloc_hook_heap_stl_allocator(alloc_hook_heap_t* hp) : _alloc_hook_heap_stl_allocator_common<T, false>(hp) { }  // no delete nor destroy on the passed in heap 
  template<class U> alloc_hook_heap_stl_allocator(const alloc_hook_heap_stl_allocator<U>& x) alloc_hook_attr_noexcept : _alloc_hook_heap_stl_allocator_common<T, false>(x) { }

  alloc_hook_heap_stl_allocator select_on_container_copy_construction() const { return *this; }
  void deallocate(T* p, size_type) { alloc_hook_free(p); }
  template<class U> struct rebind { typedef alloc_hook_heap_stl_allocator<U> other; };
};

template<class T1, class T2> bool operator==(const alloc_hook_heap_stl_allocator<T1>& x, const alloc_hook_heap_stl_allocator<T2>& y) alloc_hook_attr_noexcept { return (x.is_equal(y)); }
template<class T1, class T2> bool operator!=(const alloc_hook_heap_stl_allocator<T1>& x, const alloc_hook_heap_stl_allocator<T2>& y) alloc_hook_attr_noexcept { return (!x.is_equal(y)); }


// STL allocator allocation in a specific heap, where `free` does nothing and
// the heap is destroyed in one go on destruction -- use with care!
template<class T> struct alloc_hook_heap_destroy_stl_allocator : public _alloc_hook_heap_stl_allocator_common<T, true> {
  using typename _alloc_hook_heap_stl_allocator_common<T, true>::size_type;
  alloc_hook_heap_destroy_stl_allocator() : _alloc_hook_heap_stl_allocator_common<T, true>() { } // creates fresh heap that is destroyed when the destructor is called
  alloc_hook_heap_destroy_stl_allocator(alloc_hook_heap_t* hp) : _alloc_hook_heap_stl_allocator_common<T, true>(hp) { }  // no delete nor destroy on the passed in heap 
  template<class U> alloc_hook_heap_destroy_stl_allocator(const alloc_hook_heap_destroy_stl_allocator<U>& x) alloc_hook_attr_noexcept : _alloc_hook_heap_stl_allocator_common<T, true>(x) { }

  alloc_hook_heap_destroy_stl_allocator select_on_container_copy_construction() const { return *this; }
  void deallocate(T*, size_type) { /* do nothing as we destroy the heap on destruct. */ }
  template<class U> struct rebind { typedef alloc_hook_heap_destroy_stl_allocator<U> other; };
};

template<class T1, class T2> bool operator==(const alloc_hook_heap_destroy_stl_allocator<T1>& x, const alloc_hook_heap_destroy_stl_allocator<T2>& y) alloc_hook_attr_noexcept { return (x.is_equal(y)); }
template<class T1, class T2> bool operator!=(const alloc_hook_heap_destroy_stl_allocator<T1>& x, const alloc_hook_heap_destroy_stl_allocator<T2>& y) alloc_hook_attr_noexcept { return (!x.is_equal(y)); }

#endif // C++11

#endif // __cplusplus

#endif
