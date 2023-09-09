/* ----------------------------------------------------------------------------
Copyright (c) 2018-2023, Microsoft Research, Daan Leijen
This is free software; you can redistribute it and/or modify it under the
terms of the MIT license. A copy of the license can be found in the file
"LICENSE" at the root of this distribution.
-----------------------------------------------------------------------------*/

#ifndef ALLOC_HOOK_INTERNAL_H
#define ALLOC_HOOK_INTERNAL_H

// --------------------------------------------------------------------------
// This file contains the interal API's of alloc_hook and various utility
// functions and macros.
// --------------------------------------------------------------------------

#include "alloc_hook_types.h"
#include "alloc_hook_track.h"

#define ALLOC_HOOK_CACHE_LINE          64
#if defined(_MSC_VER)
#pragma warning(disable:4127)   // suppress constant conditional warning (due to ALLOC_HOOK_SECURE paths)
#pragma warning(disable:26812)  // unscoped enum warning
#define alloc_hook_decl_noinline        __declspec(noinline)
#define alloc_hook_decl_thread          __declspec(thread)
#define alloc_hook_decl_cache_align     __declspec(align(ALLOC_HOOK_CACHE_LINE))
#elif (defined(__GNUC__) && (__GNUC__ >= 3)) || defined(__clang__) // includes clang and icc
#define alloc_hook_decl_noinline        __attribute__((noinline))
#define alloc_hook_decl_thread          __thread
#define alloc_hook_decl_cache_align     __attribute__((aligned(ALLOC_HOOK_CACHE_LINE)))
#else
#define alloc_hook_decl_noinline
#define alloc_hook_decl_thread          __thread        // hope for the best :-)
#define alloc_hook_decl_cache_align
#endif

#if defined(__EMSCRIPTEN__) && !defined(__wasi__)
#define __wasi__
#endif

#if defined(__cplusplus)
#define alloc_hook_decl_externc       extern "C"
#else
#define alloc_hook_decl_externc
#endif

// pthreads
#if !defined(_WIN32) && !defined(__wasi__)
#define  ALLOC_HOOK_USE_PTHREADS
#include <pthread.h>
#endif

// "options.c"
void       _alloc_hook_fputs(alloc_hook_output_fun* out, void* arg, const char* prefix, const char* message);
void       _alloc_hook_fprintf(alloc_hook_output_fun* out, void* arg, const char* fmt, ...);
void       _alloc_hook_warning_message(const char* fmt, ...);
void       _alloc_hook_verbose_message(const char* fmt, ...);
void       _alloc_hook_trace_message(const char* fmt, ...);
void       _alloc_hook_options_init(void);
void       _alloc_hook_error_message(int err, const char* fmt, ...);

// random.c
void       _alloc_hook_random_init(alloc_hook_random_ctx_t* ctx);
void       _alloc_hook_random_init_weak(alloc_hook_random_ctx_t* ctx);
void       _alloc_hook_random_reinit_if_weak(alloc_hook_random_ctx_t * ctx);
void       _alloc_hook_random_split(alloc_hook_random_ctx_t* ctx, alloc_hook_random_ctx_t* new_ctx);
uintptr_t  _alloc_hook_random_next(alloc_hook_random_ctx_t* ctx);
uintptr_t  _alloc_hook_heap_random_next(alloc_hook_heap_t* heap);
uintptr_t  _alloc_hook_os_random_weak(uintptr_t extra_seed);
static inline uintptr_t _alloc_hook_random_shuffle(uintptr_t x);

// init.c
extern alloc_hook_decl_cache_align alloc_hook_stats_t       _alloc_hook_stats_main;
extern alloc_hook_decl_cache_align const alloc_hook_page_t  _alloc_hook_page_empty;
void       _alloc_hook_write_stdout(const void * msg);
void       _alloc_hook_write_stderr(const void * msg);
bool       _alloc_hook_is_main_thread(void);
size_t     _alloc_hook_current_thread_count(void);
bool       _alloc_hook_preloading(void);           // true while the C runtime is not initialized yet
alloc_hook_threadid_t _alloc_hook_thread_id(void) alloc_hook_attr_noexcept;
alloc_hook_heap_t*    _alloc_hook_heap_main_get(void);     // statically allocated main backing heap
void       _alloc_hook_thread_done(alloc_hook_heap_t* heap);
void       _alloc_hook_thread_data_collect(void);

// os.c
void       _alloc_hook_os_init(void);                                            // called from process init
void*      _alloc_hook_os_alloc(size_t size, alloc_hook_memid_t* memid, alloc_hook_stats_t* stats);  
void       _alloc_hook_os_free(void* p, size_t size, alloc_hook_memid_t memid, alloc_hook_stats_t* stats);
void       _alloc_hook_os_free_ex(void* p, size_t size, bool still_committed, alloc_hook_memid_t memid, alloc_hook_stats_t* stats);

size_t     _alloc_hook_os_page_size(void);
size_t     _alloc_hook_os_good_alloc_size(size_t size);
bool       _alloc_hook_os_has_overcommit(void);
bool       _alloc_hook_os_has_virtual_reserve(void);

bool       _alloc_hook_os_purge(void* p, size_t size, alloc_hook_stats_t* stats);
bool       _alloc_hook_os_reset(void* addr, size_t size, alloc_hook_stats_t* tld_stats);
bool       _alloc_hook_os_commit(void* p, size_t size, bool* is_zero, alloc_hook_stats_t* stats);
bool       _alloc_hook_os_decommit(void* addr, size_t size, alloc_hook_stats_t* stats);
bool       _alloc_hook_os_protect(void* addr, size_t size);
bool       _alloc_hook_os_unprotect(void* addr, size_t size);
bool       _alloc_hook_os_purge(void* p, size_t size, alloc_hook_stats_t* stats);
bool       _alloc_hook_os_purge_ex(void* p, size_t size, bool allow_reset, alloc_hook_stats_t* stats);

void*      _alloc_hook_os_alloc_aligned(size_t size, size_t alignment, bool commit, bool allow_large, alloc_hook_memid_t* memid, alloc_hook_stats_t* stats);
void*      _alloc_hook_os_alloc_aligned_at_offset(size_t size, size_t alignment, size_t align_offset, bool commit, bool allow_large, alloc_hook_memid_t* memid, alloc_hook_stats_t* tld_stats);

void*      _alloc_hook_os_get_aligned_hint(size_t try_alignment, size_t size);
bool       _alloc_hook_os_use_large_page(size_t size, size_t alignment);
size_t     _alloc_hook_os_large_page_size(void);

void*      _alloc_hook_os_alloc_huge_os_pages(size_t pages, int numa_node, alloc_hook_msecs_t max_secs, size_t* pages_reserved, size_t* psize, alloc_hook_memid_t* memid);

// arena.c
alloc_hook_arena_id_t _alloc_hook_arena_id_none(void);
void       _alloc_hook_arena_free(void* p, size_t size, size_t still_committed_size, alloc_hook_memid_t memid, alloc_hook_stats_t* stats);
void*      _alloc_hook_arena_alloc(size_t size, bool commit, bool allow_large, alloc_hook_arena_id_t req_arena_id, alloc_hook_memid_t* memid, alloc_hook_os_tld_t* tld);
void*      _alloc_hook_arena_alloc_aligned(size_t size, size_t alignment, size_t align_offset, bool commit, bool allow_large, alloc_hook_arena_id_t req_arena_id, alloc_hook_memid_t* memid, alloc_hook_os_tld_t* tld);
bool       _alloc_hook_arena_memid_is_suitable(alloc_hook_memid_t memid, alloc_hook_arena_id_t request_arena_id);
bool       _alloc_hook_arena_contains(const void* p);
void       _alloc_hook_arena_collect(bool force_purge, alloc_hook_stats_t* stats);
void       _alloc_hook_arena_unsafe_destroy_all(alloc_hook_stats_t* stats);

// "segment-map.c"
void       _alloc_hook_segment_map_allocated_at(const alloc_hook_segment_t* segment);
void       _alloc_hook_segment_map_freed_at(const alloc_hook_segment_t* segment);

// "segment.c"
alloc_hook_page_t* _alloc_hook_segment_page_alloc(alloc_hook_heap_t* heap, size_t block_size, size_t page_alignment, alloc_hook_segments_tld_t* tld, alloc_hook_os_tld_t* os_tld);
void       _alloc_hook_segment_page_free(alloc_hook_page_t* page, bool force, alloc_hook_segments_tld_t* tld);
void       _alloc_hook_segment_page_abandon(alloc_hook_page_t* page, alloc_hook_segments_tld_t* tld);
bool       _alloc_hook_segment_try_reclaim_abandoned( alloc_hook_heap_t* heap, bool try_all, alloc_hook_segments_tld_t* tld);
void       _alloc_hook_segment_thread_collect(alloc_hook_segments_tld_t* tld);

#if ALLOC_HOOK_HUGE_PAGE_ABANDON
void       _alloc_hook_segment_huge_page_free(alloc_hook_segment_t* segment, alloc_hook_page_t* page, alloc_hook_block_t* block);
#else
void       _alloc_hook_segment_huge_page_reset(alloc_hook_segment_t* segment, alloc_hook_page_t* page, alloc_hook_block_t* block);
#endif

uint8_t*   _alloc_hook_segment_page_start(const alloc_hook_segment_t* segment, const alloc_hook_page_t* page, size_t* page_size); // page start for any page
void       _alloc_hook_abandoned_reclaim_all(alloc_hook_heap_t* heap, alloc_hook_segments_tld_t* tld);
void       _alloc_hook_abandoned_await_readers(void);
void       _alloc_hook_abandoned_collect(alloc_hook_heap_t* heap, bool force, alloc_hook_segments_tld_t* tld);

// "page.c"
void*      _alloc_hook_malloc_generic(alloc_hook_heap_t* heap, size_t size, bool zero, size_t huge_alignment)  alloc_hook_attr_noexcept alloc_hook_attr_malloc;

void       _alloc_hook_page_retire(alloc_hook_page_t* page) alloc_hook_attr_noexcept;                  // free the page if there are no other pages with many free blocks
void       _alloc_hook_page_unfull(alloc_hook_page_t* page);
void       _alloc_hook_page_free(alloc_hook_page_t* page, alloc_hook_page_queue_t* pq, bool force);   // free the page
void       _alloc_hook_page_abandon(alloc_hook_page_t* page, alloc_hook_page_queue_t* pq);            // abandon the page, to be picked up by another thread...
void       _alloc_hook_heap_delayed_free_all(alloc_hook_heap_t* heap);
bool       _alloc_hook_heap_delayed_free_partial(alloc_hook_heap_t* heap);
void       _alloc_hook_heap_collect_retired(alloc_hook_heap_t* heap, bool force);

void       _alloc_hook_page_use_delayed_free(alloc_hook_page_t* page, alloc_hook_delayed_t delay, bool override_never);
bool       _alloc_hook_page_try_use_delayed_free(alloc_hook_page_t* page, alloc_hook_delayed_t delay, bool override_never);
size_t     _alloc_hook_page_queue_append(alloc_hook_heap_t* heap, alloc_hook_page_queue_t* pq, alloc_hook_page_queue_t* append);
void       _alloc_hook_deferred_free(alloc_hook_heap_t* heap, bool force);

void       _alloc_hook_page_free_collect(alloc_hook_page_t* page,bool force);
void       _alloc_hook_page_reclaim(alloc_hook_heap_t* heap, alloc_hook_page_t* page);   // callback from segments

size_t     _alloc_hook_bin_size(uint8_t bin);           // for stats
uint8_t    _alloc_hook_bin(size_t size);                // for stats

// "heap.c"
void       _alloc_hook_heap_destroy_pages(alloc_hook_heap_t* heap);
void       _alloc_hook_heap_collect_abandon(alloc_hook_heap_t* heap);
void       _alloc_hook_heap_set_default_direct(alloc_hook_heap_t* heap);
bool       _alloc_hook_heap_memid_is_suitable(alloc_hook_heap_t* heap, alloc_hook_memid_t memid);
void       _alloc_hook_heap_unsafe_destroy_all(void);

// "stats.c"
void       _alloc_hook_stats_done(alloc_hook_stats_t* stats);
alloc_hook_msecs_t  _alloc_hook_clock_now(void);
alloc_hook_msecs_t  _alloc_hook_clock_end(alloc_hook_msecs_t start);
alloc_hook_msecs_t  _alloc_hook_clock_start(void);

// "alloc.c"
void*       _alloc_hook_page_malloc(alloc_hook_heap_t* heap, alloc_hook_page_t* page, size_t size, bool zero) alloc_hook_attr_noexcept;  // called from `_alloc_hook_malloc_generic`
void*       _alloc_hook_heap_malloc_zero(alloc_hook_heap_t* heap, size_t size, bool zero) alloc_hook_attr_noexcept;
void*       _alloc_hook_heap_malloc_zero_ex(alloc_hook_heap_t* heap, size_t size, bool zero, size_t huge_alignment) alloc_hook_attr_noexcept;     // called from `_alloc_hook_heap_malloc_aligned`
void*       _alloc_hook_heap_realloc_zero(alloc_hook_heap_t* heap, void* p, size_t newsize, bool zero) alloc_hook_attr_noexcept;
alloc_hook_block_t* _alloc_hook_page_ptr_unalign(const alloc_hook_segment_t* segment, const alloc_hook_page_t* page, const void* p);
bool        _alloc_hook_free_delayed_block(alloc_hook_block_t* block);
void        _alloc_hook_free_generic(const alloc_hook_segment_t* segment, alloc_hook_page_t* page, bool is_local, void* p) alloc_hook_attr_noexcept;  // for runtime integration
void        _alloc_hook_padding_shrink(const alloc_hook_page_t* page, const alloc_hook_block_t* block, const size_t min_size);

// option.c, c primitives
char        _alloc_hook_toupper(char c);
int         _alloc_hook_strnicmp(const char* s, const char* t, size_t n);
void        _alloc_hook_strlcpy(char* dest, const char* src, size_t dest_size);
void        _alloc_hook_strlcat(char* dest, const char* src, size_t dest_size);
size_t      _alloc_hook_strlen(const char* s);
size_t      _alloc_hook_strnlen(const char* s, size_t max_len);


#if ALLOC_HOOK_DEBUG>1
bool        _alloc_hook_page_is_valid(alloc_hook_page_t* page);
#endif


// ------------------------------------------------------
// Branches
// ------------------------------------------------------

#if defined(__GNUC__) || defined(__clang__)
#define alloc_hook_unlikely(x)     (__builtin_expect(!!(x),false))
#define alloc_hook_likely(x)       (__builtin_expect(!!(x),true))
#elif (defined(__cplusplus) && (__cplusplus >= 202002L)) || (defined(_MSVC_LANG) && _MSVC_LANG >= 202002L)
#define alloc_hook_unlikely(x)     (x) [[unlikely]]
#define alloc_hook_likely(x)       (x) [[likely]]
#else
#define alloc_hook_unlikely(x)     (x)
#define alloc_hook_likely(x)       (x)
#endif

#ifndef __has_builtin
#define __has_builtin(x)  0
#endif


/* -----------------------------------------------------------
  Error codes passed to `_alloc_hook_fatal_error`
  All are recoverable but EFAULT is a serious error and aborts by default in secure mode.
  For portability define undefined error codes using common Unix codes:
  <https://www-numi.fnal.gov/offline_software/srt_public_context/WebDocs/Errors/unix_system_errors.html>
----------------------------------------------------------- */
#include <errno.h>
#ifndef EAGAIN         // double free
#define EAGAIN (11)
#endif
#ifndef ENOMEM         // out of memory
#define ENOMEM (12)
#endif
#ifndef EFAULT         // corrupted free-list or meta-data
#define EFAULT (14)
#endif
#ifndef EINVAL         // trying to free an invalid pointer
#define EINVAL (22)
#endif
#ifndef EOVERFLOW      // count*size overflow
#define EOVERFLOW (75)
#endif


/* -----------------------------------------------------------
  Inlined definitions
----------------------------------------------------------- */
#define ALLOC_HOOK_UNUSED(x)     (void)(x)
#if (ALLOC_HOOK_DEBUG>0)
#define ALLOC_HOOK_UNUSED_RELEASE(x)
#else
#define ALLOC_HOOK_UNUSED_RELEASE(x)  ALLOC_HOOK_UNUSED(x)
#endif

#define ALLOC_HOOK_INIT4(x)   x(),x(),x(),x()
#define ALLOC_HOOK_INIT8(x)   ALLOC_HOOK_INIT4(x),ALLOC_HOOK_INIT4(x)
#define ALLOC_HOOK_INIT16(x)  ALLOC_HOOK_INIT8(x),ALLOC_HOOK_INIT8(x)
#define ALLOC_HOOK_INIT32(x)  ALLOC_HOOK_INIT16(x),ALLOC_HOOK_INIT16(x)
#define ALLOC_HOOK_INIT64(x)  ALLOC_HOOK_INIT32(x),ALLOC_HOOK_INIT32(x)
#define ALLOC_HOOK_INIT128(x) ALLOC_HOOK_INIT64(x),ALLOC_HOOK_INIT64(x)
#define ALLOC_HOOK_INIT256(x) ALLOC_HOOK_INIT128(x),ALLOC_HOOK_INIT128(x)


#include <string.h>
// initialize a local variable to zero; use memset as compilers optimize constant sized memset's
#define _alloc_hook_memzero_var(x)  memset(&x,0,sizeof(x))

// Is `x` a power of two? (0 is considered a power of two)
static inline bool _alloc_hook_is_power_of_two(uintptr_t x) {
  return ((x & (x - 1)) == 0);
}

// Is a pointer aligned?
static inline bool _alloc_hook_is_aligned(void* p, size_t alignment) {
  alloc_hook_assert_internal(alignment != 0);
  return (((uintptr_t)p % alignment) == 0);
}

// Align upwards
static inline uintptr_t _alloc_hook_align_up(uintptr_t sz, size_t alignment) {
  alloc_hook_assert_internal(alignment != 0);
  uintptr_t mask = alignment - 1;
  if ((alignment & mask) == 0) {  // power of two?
    return ((sz + mask) & ~mask);
  }
  else {
    return (((sz + mask)/alignment)*alignment);
  }
}

// Align downwards
static inline uintptr_t _alloc_hook_align_down(uintptr_t sz, size_t alignment) {
  alloc_hook_assert_internal(alignment != 0);
  uintptr_t mask = alignment - 1;
  if ((alignment & mask) == 0) { // power of two?
    return (sz & ~mask);
  }
  else {
    return ((sz / alignment) * alignment);
  }
}

// Divide upwards: `s <= _alloc_hook_divide_up(s,d)*d < s+d`.
static inline uintptr_t _alloc_hook_divide_up(uintptr_t size, size_t divider) {
  alloc_hook_assert_internal(divider != 0);
  return (divider == 0 ? size : ((size + divider - 1) / divider));
}

// Is memory zero initialized?
static inline bool alloc_hook_mem_is_zero(const void* p, size_t size) {
  for (size_t i = 0; i < size; i++) {
    if (((uint8_t*)p)[i] != 0) return false;
  }
  return true;
}


// Align a byte size to a size in _machine words_,
// i.e. byte size == `wsize*sizeof(void*)`.
static inline size_t _alloc_hook_wsize_from_size(size_t size) {
  alloc_hook_assert_internal(size <= SIZE_MAX - sizeof(uintptr_t));
  return (size + sizeof(uintptr_t) - 1) / sizeof(uintptr_t);
}

// Overflow detecting multiply
#if __has_builtin(__builtin_umul_overflow) || (defined(__GNUC__) && (__GNUC__ >= 5))
#include <limits.h>      // UINT_MAX, ULONG_MAX
#if defined(_CLOCK_T)    // for Illumos
#undef _CLOCK_T
#endif
static inline bool alloc_hook_mul_overflow(size_t count, size_t size, size_t* total) {
  #if (SIZE_MAX == ULONG_MAX)
    return __builtin_umull_overflow(count, size, (unsigned long *)total);
  #elif (SIZE_MAX == UINT_MAX)
    return __builtin_umul_overflow(count, size, (unsigned int *)total);
  #else
    return __builtin_umulll_overflow(count, size, (unsigned long long *)total);
  #endif
}
#else /* __builtin_umul_overflow is unavailable */
static inline bool alloc_hook_mul_overflow(size_t count, size_t size, size_t* total) {
  #define ALLOC_HOOK_MUL_NO_OVERFLOW ((size_t)1 << (4*sizeof(size_t)))  // sqrt(SIZE_MAX)
  *total = count * size;
  // note: gcc/clang optimize this to directly check the overflow flag
  return ((size >= ALLOC_HOOK_MUL_NO_OVERFLOW || count >= ALLOC_HOOK_MUL_NO_OVERFLOW) && size > 0 && (SIZE_MAX / size) < count);
}
#endif

// Safe multiply `count*size` into `total`; return `true` on overflow.
static inline bool alloc_hook_count_size_overflow(size_t count, size_t size, size_t* total) {
  if (count==1) {  // quick check for the case where count is one (common for C++ allocators)
    *total = size;
    return false;
  }
  else if alloc_hook_unlikely(alloc_hook_mul_overflow(count, size, total)) {
    #if ALLOC_HOOK_DEBUG > 0
    _alloc_hook_error_message(EOVERFLOW, "allocation request is too large (%zu * %zu bytes)\n", count, size);
    #endif
    *total = SIZE_MAX;
    return true;
  }
  else return false;
}


/*----------------------------------------------------------------------------------------
  Heap functions
------------------------------------------------------------------------------------------- */

extern const alloc_hook_heap_t _alloc_hook_heap_empty;  // read-only empty heap, initial value of the thread local default heap

static inline bool alloc_hook_heap_is_backing(const alloc_hook_heap_t* heap) {
  return (heap->tld->heap_backing == heap);
}

static inline bool alloc_hook_heap_is_initialized(alloc_hook_heap_t* heap) {
  alloc_hook_assert_internal(heap != NULL);
  return (heap != &_alloc_hook_heap_empty);
}

static inline uintptr_t _alloc_hook_ptr_cookie(const void* p) {
  extern alloc_hook_heap_t _alloc_hook_heap_main;
  alloc_hook_assert_internal(_alloc_hook_heap_main.cookie != 0);
  return ((uintptr_t)p ^ _alloc_hook_heap_main.cookie);
}

/* -----------------------------------------------------------
  Pages
----------------------------------------------------------- */

static inline alloc_hook_page_t* _alloc_hook_heap_get_free_small_page(alloc_hook_heap_t* heap, size_t size) {
  alloc_hook_assert_internal(size <= (ALLOC_HOOK_SMALL_SIZE_MAX + ALLOC_HOOK_PADDING_SIZE));
  const size_t idx = _alloc_hook_wsize_from_size(size);
  alloc_hook_assert_internal(idx < ALLOC_HOOK_PAGES_DIRECT);
  return heap->pages_free_direct[idx];
}

// Segment that contains the pointer
// Large aligned blocks may be aligned at N*ALLOC_HOOK_SEGMENT_SIZE (inside a huge segment > ALLOC_HOOK_SEGMENT_SIZE),
// and we need align "down" to the segment info which is `ALLOC_HOOK_SEGMENT_SIZE` bytes before it;
// therefore we align one byte before `p`.
static inline alloc_hook_segment_t* _alloc_hook_ptr_segment(const void* p) {
  alloc_hook_assert_internal(p != NULL);
  return (alloc_hook_segment_t*)(((uintptr_t)p - 1) & ~ALLOC_HOOK_SEGMENT_MASK);
}

static inline alloc_hook_page_t* alloc_hook_slice_to_page(alloc_hook_slice_t* s) {
  alloc_hook_assert_internal(s->slice_offset== 0 && s->slice_count > 0);
  return (alloc_hook_page_t*)(s);
}

static inline alloc_hook_slice_t* alloc_hook_page_to_slice(alloc_hook_page_t* p) {
  alloc_hook_assert_internal(p->slice_offset== 0 && p->slice_count > 0);
  return (alloc_hook_slice_t*)(p);
}

// Segment belonging to a page
static inline alloc_hook_segment_t* _alloc_hook_page_segment(const alloc_hook_page_t* page) {
  alloc_hook_segment_t* segment = _alloc_hook_ptr_segment(page); 
  alloc_hook_assert_internal(segment == NULL || ((alloc_hook_slice_t*)page >= segment->slices && (alloc_hook_slice_t*)page < segment->slices + segment->slice_entries));
  return segment;
}

static inline alloc_hook_slice_t* alloc_hook_slice_first(const alloc_hook_slice_t* slice) {
  alloc_hook_slice_t* start = (alloc_hook_slice_t*)((uint8_t*)slice - slice->slice_offset);
  alloc_hook_assert_internal(start >= _alloc_hook_ptr_segment(slice)->slices);
  alloc_hook_assert_internal(start->slice_offset == 0);
  alloc_hook_assert_internal(start + start->slice_count > slice);
  return start;
}

// Get the page containing the pointer (performance critical as it is called in alloc_hook_free)
static inline alloc_hook_page_t* _alloc_hook_segment_page_of(const alloc_hook_segment_t* segment, const void* p) {
  alloc_hook_assert_internal(p > (void*)segment);
  ptrdiff_t diff = (uint8_t*)p - (uint8_t*)segment;
  alloc_hook_assert_internal(diff > 0 && diff <= (ptrdiff_t)ALLOC_HOOK_SEGMENT_SIZE);
  size_t idx = (size_t)diff >> ALLOC_HOOK_SEGMENT_SLICE_SHIFT;
  alloc_hook_assert_internal(idx <= segment->slice_entries);
  alloc_hook_slice_t* slice0 = (alloc_hook_slice_t*)&segment->slices[idx];
  alloc_hook_slice_t* slice = alloc_hook_slice_first(slice0);  // adjust to the block that holds the page data
  alloc_hook_assert_internal(slice->slice_offset == 0);
  alloc_hook_assert_internal(slice >= segment->slices && slice < segment->slices + segment->slice_entries);
  return alloc_hook_slice_to_page(slice);
}

// Quick page start for initialized pages
static inline uint8_t* _alloc_hook_page_start(const alloc_hook_segment_t* segment, const alloc_hook_page_t* page, size_t* page_size) {
  return _alloc_hook_segment_page_start(segment, page, page_size);
}

// Get the page containing the pointer
static inline alloc_hook_page_t* _alloc_hook_ptr_page(void* p) {
  return _alloc_hook_segment_page_of(_alloc_hook_ptr_segment(p), p);
}

// Get the block size of a page (special case for huge objects)
static inline size_t alloc_hook_page_block_size(const alloc_hook_page_t* page) {
  const size_t bsize = page->xblock_size;
  alloc_hook_assert_internal(bsize > 0);
  if alloc_hook_likely(bsize < ALLOC_HOOK_HUGE_BLOCK_SIZE) {
    return bsize;
  }
  else {
    size_t psize;
    _alloc_hook_segment_page_start(_alloc_hook_page_segment(page), page, &psize);
    return psize;
  }
}

static inline bool alloc_hook_page_is_huge(const alloc_hook_page_t* page) {
  return (_alloc_hook_page_segment(page)->kind == ALLOC_HOOK_SEGMENT_HUGE);
}

// Get the usable block size of a page without fixed padding.
// This may still include internal padding due to alignment and rounding up size classes.
static inline size_t alloc_hook_page_usable_block_size(const alloc_hook_page_t* page) {
  return alloc_hook_page_block_size(page) - ALLOC_HOOK_PADDING_SIZE;
}

// size of a segment
static inline size_t alloc_hook_segment_size(alloc_hook_segment_t* segment) {
  return segment->segment_slices * ALLOC_HOOK_SEGMENT_SLICE_SIZE;
}

static inline uint8_t* alloc_hook_segment_end(alloc_hook_segment_t* segment) {
  return (uint8_t*)segment + alloc_hook_segment_size(segment);
}

// Thread free access
static inline alloc_hook_block_t* alloc_hook_page_thread_free(const alloc_hook_page_t* page) {
  return (alloc_hook_block_t*)(alloc_hook_atomic_load_relaxed(&((alloc_hook_page_t*)page)->xthread_free) & ~3);
}

static inline alloc_hook_delayed_t alloc_hook_page_thread_free_flag(const alloc_hook_page_t* page) {
  return (alloc_hook_delayed_t)(alloc_hook_atomic_load_relaxed(&((alloc_hook_page_t*)page)->xthread_free) & 3);
}

// Heap access
static inline alloc_hook_heap_t* alloc_hook_page_heap(const alloc_hook_page_t* page) {
  return (alloc_hook_heap_t*)(alloc_hook_atomic_load_relaxed(&((alloc_hook_page_t*)page)->xheap));
}

static inline void alloc_hook_page_set_heap(alloc_hook_page_t* page, alloc_hook_heap_t* heap) {
  alloc_hook_assert_internal(alloc_hook_page_thread_free_flag(page) != ALLOC_HOOK_DELAYED_FREEING);
  alloc_hook_atomic_store_release(&page->xheap,(uintptr_t)heap);
}

// Thread free flag helpers
static inline alloc_hook_block_t* alloc_hook_tf_block(alloc_hook_thread_free_t tf) {
  return (alloc_hook_block_t*)(tf & ~0x03);
}
static inline alloc_hook_delayed_t alloc_hook_tf_delayed(alloc_hook_thread_free_t tf) {
  return (alloc_hook_delayed_t)(tf & 0x03);
}
static inline alloc_hook_thread_free_t alloc_hook_tf_make(alloc_hook_block_t* block, alloc_hook_delayed_t delayed) {
  return (alloc_hook_thread_free_t)((uintptr_t)block | (uintptr_t)delayed);
}
static inline alloc_hook_thread_free_t alloc_hook_tf_set_delayed(alloc_hook_thread_free_t tf, alloc_hook_delayed_t delayed) {
  return alloc_hook_tf_make(alloc_hook_tf_block(tf),delayed);
}
static inline alloc_hook_thread_free_t alloc_hook_tf_set_block(alloc_hook_thread_free_t tf, alloc_hook_block_t* block) {
  return alloc_hook_tf_make(block, alloc_hook_tf_delayed(tf));
}

// are all blocks in a page freed?
// note: needs up-to-date used count, (as the `xthread_free` list may not be empty). see `_alloc_hook_page_collect_free`.
static inline bool alloc_hook_page_all_free(const alloc_hook_page_t* page) {
  alloc_hook_assert_internal(page != NULL);
  return (page->used == 0);
}

// are there any available blocks?
static inline bool alloc_hook_page_has_any_available(const alloc_hook_page_t* page) {
  alloc_hook_assert_internal(page != NULL && page->reserved > 0);
  return (page->used < page->reserved || (alloc_hook_page_thread_free(page) != NULL));
}

// are there immediately available blocks, i.e. blocks available on the free list.
static inline bool alloc_hook_page_immediate_available(const alloc_hook_page_t* page) {
  alloc_hook_assert_internal(page != NULL);
  return (page->free != NULL);
}

// is more than 7/8th of a page in use?
static inline bool alloc_hook_page_mostly_used(const alloc_hook_page_t* page) {
  if (page==NULL) return true;
  uint16_t frac = page->reserved / 8U;
  return (page->reserved - page->used <= frac);
}

static inline alloc_hook_page_queue_t* alloc_hook_page_queue(const alloc_hook_heap_t* heap, size_t size) {
  return &((alloc_hook_heap_t*)heap)->pages[_alloc_hook_bin(size)];
}



//-----------------------------------------------------------
// Page flags
//-----------------------------------------------------------
static inline bool alloc_hook_page_is_in_full(const alloc_hook_page_t* page) {
  return page->flags.x.in_full;
}

static inline void alloc_hook_page_set_in_full(alloc_hook_page_t* page, bool in_full) {
  page->flags.x.in_full = in_full;
}

static inline bool alloc_hook_page_has_aligned(const alloc_hook_page_t* page) {
  return page->flags.x.has_aligned;
}

static inline void alloc_hook_page_set_has_aligned(alloc_hook_page_t* page, bool has_aligned) {
  page->flags.x.has_aligned = has_aligned;
}


/* -------------------------------------------------------------------
Encoding/Decoding the free list next pointers

This is to protect against buffer overflow exploits where the
free list is mutated. Many hardened allocators xor the next pointer `p`
with a secret key `k1`, as `p^k1`. This prevents overwriting with known
values but might be still too weak: if the attacker can guess
the pointer `p` this  can reveal `k1` (since `p^k1^p == k1`).
Moreover, if multiple blocks can be read as well, the attacker can
xor both as `(p1^k1) ^ (p2^k1) == p1^p2` which may reveal a lot
about the pointers (and subsequently `k1`).

Instead alloc_hook uses an extra key `k2` and encodes as `((p^k2)<<<k1)+k1`.
Since these operations are not associative, the above approaches do not
work so well any more even if the `p` can be guesstimated. For example,
for the read case we can subtract two entries to discard the `+k1` term,
but that leads to `((p1^k2)<<<k1) - ((p2^k2)<<<k1)` at best.
We include the left-rotation since xor and addition are otherwise linear
in the lowest bit. Finally, both keys are unique per page which reduces
the re-use of keys by a large factor.

We also pass a separate `null` value to be used as `NULL` or otherwise
`(k2<<<k1)+k1` would appear (too) often as a sentinel value.
------------------------------------------------------------------- */

static inline bool alloc_hook_is_in_same_segment(const void* p, const void* q) {
  return (_alloc_hook_ptr_segment(p) == _alloc_hook_ptr_segment(q));
}

static inline bool alloc_hook_is_in_same_page(const void* p, const void* q) {
  alloc_hook_segment_t* segment = _alloc_hook_ptr_segment(p);
  if (_alloc_hook_ptr_segment(q) != segment) return false;
  // assume q may be invalid // return (_alloc_hook_segment_page_of(segment, p) == _alloc_hook_segment_page_of(segment, q));
  alloc_hook_page_t* page = _alloc_hook_segment_page_of(segment, p);
  size_t psize;
  uint8_t* start = _alloc_hook_segment_page_start(segment, page, &psize);
  return (start <= (uint8_t*)q && (uint8_t*)q < start + psize);
}

static inline uintptr_t alloc_hook_rotl(uintptr_t x, uintptr_t shift) {
  shift %= ALLOC_HOOK_INTPTR_BITS;
  return (shift==0 ? x : ((x << shift) | (x >> (ALLOC_HOOK_INTPTR_BITS - shift))));
}
static inline uintptr_t alloc_hook_rotr(uintptr_t x, uintptr_t shift) {
  shift %= ALLOC_HOOK_INTPTR_BITS;
  return (shift==0 ? x : ((x >> shift) | (x << (ALLOC_HOOK_INTPTR_BITS - shift))));
}

static inline void* alloc_hook_ptr_decode(const void* null, const alloc_hook_encoded_t x, const uintptr_t* keys) {
  void* p = (void*)(alloc_hook_rotr(x - keys[0], keys[0]) ^ keys[1]);
  return (p==null ? NULL : p);
}

static inline alloc_hook_encoded_t alloc_hook_ptr_encode(const void* null, const void* p, const uintptr_t* keys) {
  uintptr_t x = (uintptr_t)(p==NULL ? null : p);
  return alloc_hook_rotl(x ^ keys[1], keys[0]) + keys[0];
}

static inline alloc_hook_block_t* alloc_hook_block_nextx( const void* null, const alloc_hook_block_t* block, const uintptr_t* keys ) {
  alloc_hook_track_mem_defined(block,sizeof(alloc_hook_block_t));
  alloc_hook_block_t* next;
  #ifdef ALLOC_HOOK_ENCODE_FREELIST
  next = (alloc_hook_block_t*)alloc_hook_ptr_decode(null, block->next, keys);
  #else
  ALLOC_HOOK_UNUSED(keys); ALLOC_HOOK_UNUSED(null);
  next = (alloc_hook_block_t*)block->next;
  #endif
  alloc_hook_track_mem_noaccess(block,sizeof(alloc_hook_block_t));
  return next;
}

static inline void alloc_hook_block_set_nextx(const void* null, alloc_hook_block_t* block, const alloc_hook_block_t* next, const uintptr_t* keys) {
  alloc_hook_track_mem_undefined(block,sizeof(alloc_hook_block_t));
  #ifdef ALLOC_HOOK_ENCODE_FREELIST
  block->next = alloc_hook_ptr_encode(null, next, keys);
  #else
  ALLOC_HOOK_UNUSED(keys); ALLOC_HOOK_UNUSED(null);
  block->next = (alloc_hook_encoded_t)next;
  #endif
  alloc_hook_track_mem_noaccess(block,sizeof(alloc_hook_block_t));
}

static inline alloc_hook_block_t* alloc_hook_block_next(const alloc_hook_page_t* page, const alloc_hook_block_t* block) {
  #ifdef ALLOC_HOOK_ENCODE_FREELIST
  alloc_hook_block_t* next = alloc_hook_block_nextx(page,block,page->keys);
  // check for free list corruption: is `next` at least in the same page?
  // TODO: check if `next` is `page->block_size` aligned?
  if alloc_hook_unlikely(next!=NULL && !alloc_hook_is_in_same_page(block, next)) {
    _alloc_hook_error_message(EFAULT, "corrupted free list entry of size %zub at %p: value 0x%zx\n", alloc_hook_page_block_size(page), block, (uintptr_t)next);
    next = NULL;
  }
  return next;
  #else
  ALLOC_HOOK_UNUSED(page);
  return alloc_hook_block_nextx(page,block,NULL);
  #endif
}

static inline void alloc_hook_block_set_next(const alloc_hook_page_t* page, alloc_hook_block_t* block, const alloc_hook_block_t* next) {
  #ifdef ALLOC_HOOK_ENCODE_FREELIST
  alloc_hook_block_set_nextx(page,block,next, page->keys);
  #else
  ALLOC_HOOK_UNUSED(page);
  alloc_hook_block_set_nextx(page,block,next,NULL);
  #endif
}


// -------------------------------------------------------------------
// commit mask
// -------------------------------------------------------------------

static inline void alloc_hook_commit_mask_create_empty(alloc_hook_commit_mask_t* cm) {
  for (size_t i = 0; i < ALLOC_HOOK_COMMIT_MASK_FIELD_COUNT; i++) {
    cm->mask[i] = 0;
  }
}

static inline void alloc_hook_commit_mask_create_full(alloc_hook_commit_mask_t* cm) {
  for (size_t i = 0; i < ALLOC_HOOK_COMMIT_MASK_FIELD_COUNT; i++) {
    cm->mask[i] = ~((size_t)0);
  }
}

static inline bool alloc_hook_commit_mask_is_empty(const alloc_hook_commit_mask_t* cm) {
  for (size_t i = 0; i < ALLOC_HOOK_COMMIT_MASK_FIELD_COUNT; i++) {
    if (cm->mask[i] != 0) return false;
  }
  return true;
}

static inline bool alloc_hook_commit_mask_is_full(const alloc_hook_commit_mask_t* cm) {
  for (size_t i = 0; i < ALLOC_HOOK_COMMIT_MASK_FIELD_COUNT; i++) {
    if (cm->mask[i] != ~((size_t)0)) return false;
  }
  return true;
}

// defined in `segment.c`:
size_t _alloc_hook_commit_mask_committed_size(const alloc_hook_commit_mask_t* cm, size_t total);
size_t _alloc_hook_commit_mask_next_run(const alloc_hook_commit_mask_t* cm, size_t* idx);

#define alloc_hook_commit_mask_foreach(cm,idx,count) \
  idx = 0; \
  while ((count = _alloc_hook_commit_mask_next_run(cm,&idx)) > 0) { 
        
#define alloc_hook_commit_mask_foreach_end() \
    idx += count; \
  }
      


/* -----------------------------------------------------------
  memory id's
----------------------------------------------------------- */

static inline alloc_hook_memid_t _alloc_hook_memid_create(alloc_hook_memkind_t memkind) {
  alloc_hook_memid_t memid;
  _alloc_hook_memzero_var(memid);
  memid.memkind = memkind;
  return memid;
}

static inline alloc_hook_memid_t _alloc_hook_memid_none(void) {
  return _alloc_hook_memid_create(ALLOC_HOOK_MEM_NONE);
}

static inline alloc_hook_memid_t _alloc_hook_memid_create_os(bool committed, bool is_zero, bool is_large) {
  alloc_hook_memid_t memid = _alloc_hook_memid_create(ALLOC_HOOK_MEM_OS);
  memid.initially_committed = committed;
  memid.initially_zero = is_zero;
  memid.is_pinned = is_large;
  return memid;
}


// -------------------------------------------------------------------
// Fast "random" shuffle
// -------------------------------------------------------------------

static inline uintptr_t _alloc_hook_random_shuffle(uintptr_t x) {
  if (x==0) { x = 17; }   // ensure we don't get stuck in generating zeros
#if (ALLOC_HOOK_INTPTR_SIZE==8)
  // by Sebastiano Vigna, see: <http://xoshiro.di.unimi.it/splitmix64.c>
  x ^= x >> 30;
  x *= 0xbf58476d1ce4e5b9UL;
  x ^= x >> 27;
  x *= 0x94d049bb133111ebUL;
  x ^= x >> 31;
#elif (ALLOC_HOOK_INTPTR_SIZE==4)
  // by Chris Wellons, see: <https://nullprogram.com/blog/2018/07/31/>
  x ^= x >> 16;
  x *= 0x7feb352dUL;
  x ^= x >> 15;
  x *= 0x846ca68bUL;
  x ^= x >> 16;
#endif
  return x;
}

// -------------------------------------------------------------------
// Optimize numa node access for the common case (= one node)
// -------------------------------------------------------------------

int    _alloc_hook_os_numa_node_get(alloc_hook_os_tld_t* tld);
size_t _alloc_hook_os_numa_node_count_get(void);

extern _Atomic(size_t) _alloc_hook_numa_node_count;
static inline int _alloc_hook_os_numa_node(alloc_hook_os_tld_t* tld) {
  if alloc_hook_likely(alloc_hook_atomic_load_relaxed(&_alloc_hook_numa_node_count) == 1) { return 0; }
  else return _alloc_hook_os_numa_node_get(tld);
}
static inline size_t _alloc_hook_os_numa_node_count(void) {
  const size_t count = alloc_hook_atomic_load_relaxed(&_alloc_hook_numa_node_count);
  if alloc_hook_likely(count > 0) { return count; }
  else return _alloc_hook_os_numa_node_count_get();
}



// -----------------------------------------------------------------------
// Count bits: trailing or leading zeros (with ALLOC_HOOK_INTPTR_BITS on all zero)
// -----------------------------------------------------------------------

#if defined(__GNUC__)

#include <limits.h>       // LONG_MAX
#define ALLOC_HOOK_HAVE_FAST_BITSCAN
static inline size_t alloc_hook_clz(uintptr_t x) {
  if (x==0) return ALLOC_HOOK_INTPTR_BITS;
#if (INTPTR_MAX == LONG_MAX)
  return __builtin_clzl(x);
#else
  return __builtin_clzll(x);
#endif
}
static inline size_t alloc_hook_ctz(uintptr_t x) {
  if (x==0) return ALLOC_HOOK_INTPTR_BITS;
#if (INTPTR_MAX == LONG_MAX)
  return __builtin_ctzl(x);
#else
  return __builtin_ctzll(x);
#endif
}

#elif defined(_MSC_VER)

#include <limits.h>       // LONG_MAX
#include <intrin.h>       // BitScanReverse64
#define ALLOC_HOOK_HAVE_FAST_BITSCAN
static inline size_t alloc_hook_clz(uintptr_t x) {
  if (x==0) return ALLOC_HOOK_INTPTR_BITS;
  unsigned long idx;
#if (INTPTR_MAX == LONG_MAX)
  _BitScanReverse(&idx, x);
#else
  _BitScanReverse64(&idx, x);
#endif
  return ((ALLOC_HOOK_INTPTR_BITS - 1) - idx);
}
static inline size_t alloc_hook_ctz(uintptr_t x) {
  if (x==0) return ALLOC_HOOK_INTPTR_BITS;
  unsigned long idx;
#if (INTPTR_MAX == LONG_MAX)
  _BitScanForward(&idx, x);
#else
  _BitScanForward64(&idx, x);
#endif
  return idx;
}

#else
static inline size_t alloc_hook_ctz32(uint32_t x) {
  // de Bruijn multiplication, see <http://supertech.csail.mit.edu/papers/debruijn.pdf>
  static const unsigned char debruijn[32] = {
    0, 1, 28, 2, 29, 14, 24, 3, 30, 22, 20, 15, 25, 17, 4, 8,
    31, 27, 13, 23, 21, 19, 16, 7, 26, 12, 18, 6, 11, 5, 10, 9
  };
  if (x==0) return 32;
  return debruijn[((x & -(int32_t)x) * 0x077CB531UL) >> 27];
}
static inline size_t alloc_hook_clz32(uint32_t x) {
  // de Bruijn multiplication, see <http://supertech.csail.mit.edu/papers/debruijn.pdf>
  static const uint8_t debruijn[32] = {
    31, 22, 30, 21, 18, 10, 29, 2, 20, 17, 15, 13, 9, 6, 28, 1,
    23, 19, 11, 3, 16, 14, 7, 24, 12, 4, 8, 25, 5, 26, 27, 0
  };
  if (x==0) return 32;
  x |= x >> 1;
  x |= x >> 2;
  x |= x >> 4;
  x |= x >> 8;
  x |= x >> 16;
  return debruijn[(uint32_t)(x * 0x07C4ACDDUL) >> 27];
}

static inline size_t alloc_hook_clz(uintptr_t x) {
  if (x==0) return ALLOC_HOOK_INTPTR_BITS;
#if (ALLOC_HOOK_INTPTR_BITS <= 32)
  return alloc_hook_clz32((uint32_t)x);
#else
  size_t count = alloc_hook_clz32((uint32_t)(x >> 32));
  if (count < 32) return count;
  return (32 + alloc_hook_clz32((uint32_t)x));
#endif
}
static inline size_t alloc_hook_ctz(uintptr_t x) {
  if (x==0) return ALLOC_HOOK_INTPTR_BITS;
#if (ALLOC_HOOK_INTPTR_BITS <= 32)
  return alloc_hook_ctz32((uint32_t)x);
#else
  size_t count = alloc_hook_ctz32((uint32_t)x);
  if (count < 32) return count;
  return (32 + alloc_hook_ctz32((uint32_t)(x>>32)));
#endif
}

#endif

// "bit scan reverse": Return index of the highest bit (or ALLOC_HOOK_INTPTR_BITS if `x` is zero)
static inline size_t alloc_hook_bsr(uintptr_t x) {
  return (x==0 ? ALLOC_HOOK_INTPTR_BITS : ALLOC_HOOK_INTPTR_BITS - 1 - alloc_hook_clz(x));
}


// ---------------------------------------------------------------------------------
// Provide our own `_alloc_hook_memcpy` for potential performance optimizations.
//
// For now, only on Windows with msvc/clang-cl we optimize to `rep movsb` if
// we happen to run on x86/x64 cpu's that have "fast short rep movsb" (FSRM) support
// (AMD Zen3+ (~2020) or Intel Ice Lake+ (~2017). See also issue #201 and pr #253.
// ---------------------------------------------------------------------------------

#if !ALLOC_HOOK_TRACK_ENABLED && defined(_WIN32) && (defined(_M_IX86) || defined(_M_X64))
#include <intrin.h>
extern bool _alloc_hook_cpu_has_fsrm;
static inline void _alloc_hook_memcpy(void* dst, const void* src, size_t n) {
  if (_alloc_hook_cpu_has_fsrm) {
    __movsb((unsigned char*)dst, (const unsigned char*)src, n);
  }
  else {
    memcpy(dst, src, n);
  }
}
static inline void _alloc_hook_memzero(void* dst, size_t n) {
  if (_alloc_hook_cpu_has_fsrm) {
    __stosb((unsigned char*)dst, 0, n);
  }
  else {
    memset(dst, 0, n);
  }
}
#else
static inline void _alloc_hook_memcpy(void* dst, const void* src, size_t n) {
  memcpy(dst, src, n);
}
static inline void _alloc_hook_memzero(void* dst, size_t n) {
  memset(dst, 0, n);
}
#endif

// -------------------------------------------------------------------------------
// The `_alloc_hook_memcpy_aligned` can be used if the pointers are machine-word aligned
// This is used for example in `alloc_hook_realloc`.
// -------------------------------------------------------------------------------

#if (defined(__GNUC__) && (__GNUC__ >= 4)) || defined(__clang__)
// On GCC/CLang we provide a hint that the pointers are word aligned.
static inline void _alloc_hook_memcpy_aligned(void* dst, const void* src, size_t n) {
  alloc_hook_assert_internal(((uintptr_t)dst % ALLOC_HOOK_INTPTR_SIZE == 0) && ((uintptr_t)src % ALLOC_HOOK_INTPTR_SIZE == 0));
  void* adst = __builtin_assume_aligned(dst, ALLOC_HOOK_INTPTR_SIZE);
  const void* asrc = __builtin_assume_aligned(src, ALLOC_HOOK_INTPTR_SIZE);
  _alloc_hook_memcpy(adst, asrc, n);
}

static inline void _alloc_hook_memzero_aligned(void* dst, size_t n) {
  alloc_hook_assert_internal((uintptr_t)dst % ALLOC_HOOK_INTPTR_SIZE == 0);
  void* adst = __builtin_assume_aligned(dst, ALLOC_HOOK_INTPTR_SIZE);
  _alloc_hook_memzero(adst, n);
}
#else
// Default fallback on `_alloc_hook_memcpy`
static inline void _alloc_hook_memcpy_aligned(void* dst, const void* src, size_t n) {
  alloc_hook_assert_internal(((uintptr_t)dst % ALLOC_HOOK_INTPTR_SIZE == 0) && ((uintptr_t)src % ALLOC_HOOK_INTPTR_SIZE == 0));
  _alloc_hook_memcpy(dst, src, n);
}

static inline void _alloc_hook_memzero_aligned(void* dst, size_t n) {
  alloc_hook_assert_internal((uintptr_t)dst % ALLOC_HOOK_INTPTR_SIZE == 0);
  _alloc_hook_memzero(dst, n);
}
#endif


#endif
