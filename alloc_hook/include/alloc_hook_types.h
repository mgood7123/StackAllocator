/* ----------------------------------------------------------------------------
Copyright (c) 2018-2023, Microsoft Research, Daan Leijen
This is free software; you can redistribute it and/or modify it under the
terms of the MIT license. A copy of the license can be found in the file
"LICENSE" at the root of this distribution.
-----------------------------------------------------------------------------*/

#ifndef ALLOC_HOOK_TYPES_H
#define ALLOC_HOOK_TYPES_H

// --------------------------------------------------------------------------
// This file contains the main type definitions for alloc_hook:
// alloc_hook_heap_t      : all data for a thread-local heap, contains
//                  lists of all managed heap pages.
// alloc_hook_segment_t   : a larger chunk of memory (32GiB) from where pages
//                  are allocated.
// alloc_hook_page_t      : a alloc_hook page (usually 64KiB or 512KiB) from
//                  where objects are allocated.
// --------------------------------------------------------------------------


#include <stddef.h>   // ptrdiff_t
#include <stdint.h>   // uintptr_t, uint16_t, etc
#include "alloc_hook_atomic.h"  // _Atomic

#ifdef _MSC_VER
#pragma warning(disable:4214) // bitfield is not int
#endif

// Minimal alignment necessary. On most platforms 16 bytes are needed
// due to SSE registers for example. This must be at least `sizeof(void*)`
#ifndef ALLOC_HOOK_MAX_ALIGN_SIZE
#define ALLOC_HOOK_MAX_ALIGN_SIZE  16   // sizeof(max_align_t)
#endif

// ------------------------------------------------------
// Variants
// ------------------------------------------------------

// Define NDEBUG in the release version to disable assertions.
// #define NDEBUG

// Define ALLOC_HOOK_TRACK_<tool> to enable tracking support
// #define ALLOC_HOOK_TRACK_VALGRIND 1
// #define ALLOC_HOOK_TRACK_ASAN     1
// #define ALLOC_HOOK_TRACK_ETW      1

// Define ALLOC_HOOK_STAT as 1 to maintain statistics; set it to 2 to have detailed statistics (but costs some performance).
// #define ALLOC_HOOK_STAT 1

// Define ALLOC_HOOK_SECURE to enable security mitigations
// #define ALLOC_HOOK_SECURE 1  // guard page around metadata
// #define ALLOC_HOOK_SECURE 2  // guard page around each alloc_hook page
// #define ALLOC_HOOK_SECURE 3  // encode free lists (detect corrupted free list (buffer overflow), and invalid pointer free)
// #define ALLOC_HOOK_SECURE 4  // checks for double free. (may be more expensive)

#if !defined(ALLOC_HOOK_SECURE)
#define ALLOC_HOOK_SECURE 0
#endif

// Define ALLOC_HOOK_DEBUG for debug mode
// #define ALLOC_HOOK_DEBUG 1  // basic assertion checks and statistics, check double free, corrupted free list, and invalid pointer free.
// #define ALLOC_HOOK_DEBUG 2  // + internal assertion checks
// #define ALLOC_HOOK_DEBUG 3  // + extensive internal invariant checking (cmake -DALLOC_HOOK_DEBUG_FULL=ON)
#if !defined(ALLOC_HOOK_DEBUG)
#if !defined(NDEBUG) || defined(_DEBUG)
#define ALLOC_HOOK_DEBUG 2
#else
#define ALLOC_HOOK_DEBUG 0
#endif
#endif

// Reserve extra padding at the end of each block to be more resilient against heap block overflows.
// The padding can detect buffer overflow on free.
#if !defined(ALLOC_HOOK_PADDING) && (ALLOC_HOOK_SECURE>=3 || ALLOC_HOOK_DEBUG>=1 || (ALLOC_HOOK_TRACK_VALGRIND || ALLOC_HOOK_TRACK_ASAN || ALLOC_HOOK_TRACK_ETW))
#define ALLOC_HOOK_PADDING  1
#endif

// Check padding bytes; allows byte-precise buffer overflow detection
#if !defined(ALLOC_HOOK_PADDING_CHECK) && ALLOC_HOOK_PADDING && (ALLOC_HOOK_SECURE>=3 || ALLOC_HOOK_DEBUG>=1)
#define ALLOC_HOOK_PADDING_CHECK 1
#endif


// Encoded free lists allow detection of corrupted free lists
// and can detect buffer overflows, modify after free, and double `free`s.
#if (ALLOC_HOOK_SECURE>=3 || ALLOC_HOOK_DEBUG>=1)
#define ALLOC_HOOK_ENCODE_FREELIST  1
#endif


// We used to abandon huge pages but to eagerly deallocate if freed from another thread,
// but that makes it not possible to visit them during a heap walk or include them in a
// `alloc_hook_heap_destroy`. We therefore instead reset/decommit the huge blocks if freed from
// another thread so most memory is available until it gets properly freed by the owning thread.
// #define ALLOC_HOOK_HUGE_PAGE_ABANDON 1


// ------------------------------------------------------
// Platform specific values
// ------------------------------------------------------

// ------------------------------------------------------
// Size of a pointer.
// We assume that `sizeof(void*)==sizeof(intptr_t)`
// and it holds for all platforms we know of.
//
// However, the C standard only requires that:
//  p == (void*)((intptr_t)p))
// but we also need:
//  i == (intptr_t)((void*)i)
// or otherwise one might define an intptr_t type that is larger than a pointer...
// ------------------------------------------------------

#if INTPTR_MAX > INT64_MAX
# define ALLOC_HOOK_INTPTR_SHIFT (4)  // assume 128-bit  (as on arm CHERI for example)
#elif INTPTR_MAX == INT64_MAX
# define ALLOC_HOOK_INTPTR_SHIFT (3)
#elif INTPTR_MAX == INT32_MAX
# define ALLOC_HOOK_INTPTR_SHIFT (2)
#else
#error platform pointers must be 32, 64, or 128 bits
#endif

#if SIZE_MAX == UINT64_MAX
# define ALLOC_HOOK_SIZE_SHIFT (3)
typedef int64_t  alloc_hook_ssize_t;
#elif SIZE_MAX == UINT32_MAX
# define ALLOC_HOOK_SIZE_SHIFT (2)
typedef int32_t  alloc_hook_ssize_t;
#else
#error platform objects must be 32 or 64 bits
#endif

#if (SIZE_MAX/2) > LONG_MAX
# define ALLOC_HOOK_ZU(x)  x##ULL
# define ALLOC_HOOK_ZI(x)  x##LL
#else
# define ALLOC_HOOK_ZU(x)  x##UL
# define ALLOC_HOOK_ZI(x)  x##L
#endif

#define ALLOC_HOOK_INTPTR_SIZE  (1<<ALLOC_HOOK_INTPTR_SHIFT)
#define ALLOC_HOOK_INTPTR_BITS  (ALLOC_HOOK_INTPTR_SIZE*8)

#define ALLOC_HOOK_SIZE_SIZE  (1<<ALLOC_HOOK_SIZE_SHIFT)
#define ALLOC_HOOK_SIZE_BITS  (ALLOC_HOOK_SIZE_SIZE*8)

#define ALLOC_HOOK_KiB     (ALLOC_HOOK_ZU(1024))
#define ALLOC_HOOK_MiB     (ALLOC_HOOK_KiB*ALLOC_HOOK_KiB)
#define ALLOC_HOOK_GiB     (ALLOC_HOOK_MiB*ALLOC_HOOK_KiB)


// ------------------------------------------------------
// Main internal data-structures
// ------------------------------------------------------

// Main tuning parameters for segment and page sizes
// Sizes for 64-bit (usually divide by two for 32-bit)
#define ALLOC_HOOK_SEGMENT_SLICE_SHIFT            (13 + ALLOC_HOOK_INTPTR_SHIFT)         // 64KiB  (32KiB on 32-bit)

#if ALLOC_HOOK_INTPTR_SIZE > 4
#define ALLOC_HOOK_SEGMENT_SHIFT                  ( 9 + ALLOC_HOOK_SEGMENT_SLICE_SHIFT)  // 32MiB
#else
#define ALLOC_HOOK_SEGMENT_SHIFT                  ( 7 + ALLOC_HOOK_SEGMENT_SLICE_SHIFT)  // 4MiB on 32-bit
#endif

#define ALLOC_HOOK_SMALL_PAGE_SHIFT               (ALLOC_HOOK_SEGMENT_SLICE_SHIFT)       // 64KiB
#define ALLOC_HOOK_MEDIUM_PAGE_SHIFT              ( 3 + ALLOC_HOOK_SMALL_PAGE_SHIFT)     // 512KiB


// Derived constants
#define ALLOC_HOOK_SEGMENT_SIZE                   (ALLOC_HOOK_ZU(1)<<ALLOC_HOOK_SEGMENT_SHIFT)
#define ALLOC_HOOK_SEGMENT_ALIGN                  ALLOC_HOOK_SEGMENT_SIZE
#define ALLOC_HOOK_SEGMENT_MASK                   ((uintptr_t)(ALLOC_HOOK_SEGMENT_ALIGN - 1))
#define ALLOC_HOOK_SEGMENT_SLICE_SIZE             (ALLOC_HOOK_ZU(1)<< ALLOC_HOOK_SEGMENT_SLICE_SHIFT)
#define ALLOC_HOOK_SLICES_PER_SEGMENT             (ALLOC_HOOK_SEGMENT_SIZE / ALLOC_HOOK_SEGMENT_SLICE_SIZE) // 1024

#define ALLOC_HOOK_SMALL_PAGE_SIZE                (ALLOC_HOOK_ZU(1)<<ALLOC_HOOK_SMALL_PAGE_SHIFT)
#define ALLOC_HOOK_MEDIUM_PAGE_SIZE               (ALLOC_HOOK_ZU(1)<<ALLOC_HOOK_MEDIUM_PAGE_SHIFT)

#define ALLOC_HOOK_SMALL_OBJ_SIZE_MAX             (ALLOC_HOOK_SMALL_PAGE_SIZE/4)   // 8KiB on 64-bit
#define ALLOC_HOOK_MEDIUM_OBJ_SIZE_MAX            (ALLOC_HOOK_MEDIUM_PAGE_SIZE/4)  // 128KiB on 64-bit
#define ALLOC_HOOK_MEDIUM_OBJ_WSIZE_MAX           (ALLOC_HOOK_MEDIUM_OBJ_SIZE_MAX/ALLOC_HOOK_INTPTR_SIZE)   
#define ALLOC_HOOK_LARGE_OBJ_SIZE_MAX             (ALLOC_HOOK_SEGMENT_SIZE/2)      // 32MiB on 64-bit
#define ALLOC_HOOK_LARGE_OBJ_WSIZE_MAX            (ALLOC_HOOK_LARGE_OBJ_SIZE_MAX/ALLOC_HOOK_INTPTR_SIZE)

// Maximum number of size classes. (spaced exponentially in 12.5% increments)
#define ALLOC_HOOK_BIN_HUGE  (73U)

#if (ALLOC_HOOK_MEDIUM_OBJ_WSIZE_MAX >= 655360)
#error "alloc_hook internal: define more bins"
#endif

// Maximum slice offset (15)
#define ALLOC_HOOK_MAX_SLICE_OFFSET               ((ALLOC_HOOK_ALIGNMENT_MAX / ALLOC_HOOK_SEGMENT_SLICE_SIZE) - 1)

// Used as a special value to encode block sizes in 32 bits.
#define ALLOC_HOOK_HUGE_BLOCK_SIZE                ((uint32_t)(2*ALLOC_HOOK_GiB))

// blocks up to this size are always allocated aligned
#define ALLOC_HOOK_MAX_ALIGN_GUARANTEE            (8*ALLOC_HOOK_MAX_ALIGN_SIZE)  

// Alignments over ALLOC_HOOK_ALIGNMENT_MAX are allocated in dedicated huge page segments 
#define ALLOC_HOOK_ALIGNMENT_MAX                  (ALLOC_HOOK_SEGMENT_SIZE >> 1)  


// ------------------------------------------------------
// Mimalloc pages contain allocated blocks
// ------------------------------------------------------

// The free lists use encoded next fields
// (Only actually encodes when ALLOC_HOOK_ENCODED_FREELIST is defined.)
typedef uintptr_t  alloc_hook_encoded_t;

// thread id's
typedef size_t     alloc_hook_threadid_t;

// free lists contain blocks
typedef struct alloc_hook_block_s {
  alloc_hook_encoded_t next;
} alloc_hook_block_t;


// The delayed flags are used for efficient multi-threaded free-ing
typedef enum alloc_hook_delayed_e {
  ALLOC_HOOK_USE_DELAYED_FREE   = 0, // push on the owning heap thread delayed list
  ALLOC_HOOK_DELAYED_FREEING    = 1, // temporary: another thread is accessing the owning heap
  ALLOC_HOOK_NO_DELAYED_FREE    = 2, // optimize: push on page local thread free queue if another block is already in the heap thread delayed free list
  ALLOC_HOOK_NEVER_DELAYED_FREE = 3  // sticky, only resets on page reclaim
} alloc_hook_delayed_t;


// The `in_full` and `has_aligned` page flags are put in a union to efficiently
// test if both are false (`full_aligned == 0`) in the `alloc_hook_free` routine.
#if !ALLOC_HOOK_TSAN
typedef union alloc_hook_page_flags_s {
  uint8_t full_aligned;
  struct {
    uint8_t in_full : 1;
    uint8_t has_aligned : 1;
  } x;
} alloc_hook_page_flags_t;
#else
// under thread sanitizer, use a byte for each flag to suppress warning, issue #130
typedef union alloc_hook_page_flags_s {
  uint16_t full_aligned;
  struct {
    uint8_t in_full;
    uint8_t has_aligned;
  } x;
} alloc_hook_page_flags_t;
#endif

// Thread free list.
// We use the bottom 2 bits of the pointer for alloc_hook_delayed_t flags
typedef uintptr_t alloc_hook_thread_free_t;

// A page contains blocks of one specific size (`block_size`).
// Each page has three list of free blocks:
// `free` for blocks that can be allocated,
// `local_free` for freed blocks that are not yet available to `alloc_hook_malloc`
// `thread_free` for freed blocks by other threads
// The `local_free` and `thread_free` lists are migrated to the `free` list
// when it is exhausted. The separate `local_free` list is necessary to
// implement a monotonic heartbeat. The `thread_free` list is needed for
// avoiding atomic operations in the common case.
//
//
// `used - |thread_free|` == actual blocks that are in use (alive)
// `used - |thread_free| + |free| + |local_free| == capacity`
//
// We don't count `freed` (as |free|) but use `used` to reduce
// the number of memory accesses in the `alloc_hook_page_all_free` function(s).
//
// Notes:
// - Access is optimized for `alloc_hook_free` and `alloc_hook_page_alloc` (in `alloc.c`)
// - Using `uint16_t` does not seem to slow things down
// - The size is 8 words on 64-bit which helps the page index calculations
//   (and 10 words on 32-bit, and encoded free lists add 2 words. Sizes 10
//    and 12 are still good for address calculation)
// - To limit the structure size, the `xblock_size` is 32-bits only; for
//   blocks > ALLOC_HOOK_HUGE_BLOCK_SIZE the size is determined from the segment page size
// - `thread_free` uses the bottom bits as a delayed-free flags to optimize
//   concurrent frees where only the first concurrent free adds to the owning
//   heap `thread_delayed_free` list (see `alloc.c:alloc_hook_free_block_mt`).
//   The invariant is that no-delayed-free is only set if there is
//   at least one block that will be added, or as already been added, to
//   the owning heap `thread_delayed_free` list. This guarantees that pages
//   will be freed correctly even if only other threads free blocks.
typedef struct alloc_hook_page_s {
  // "owned" by the segment
  uint32_t              slice_count;       // slices in this page (0 if not a page)
  uint32_t              slice_offset;      // distance from the actual page data slice (0 if a page)  
  uint8_t               is_committed : 1;  // `true` if the page virtual memory is committed
  uint8_t               is_zero_init : 1;  // `true` if the page was initially zero initialized

  // layout like this to optimize access in `alloc_hook_malloc` and `alloc_hook_free`
  uint16_t              capacity;          // number of blocks committed, must be the first field, see `segment.c:page_clear`
  uint16_t              reserved;          // number of blocks reserved in memory
  alloc_hook_page_flags_t       flags;             // `in_full` and `has_aligned` flags (8 bits)
  uint8_t               free_is_zero : 1;  // `true` if the blocks in the free list are zero initialized
  uint8_t               retire_expire : 7; // expiration count for retired blocks

  alloc_hook_block_t*           free;              // list of available free blocks (`malloc` allocates from this list)
  uint32_t              used;              // number of blocks in use (including blocks in `local_free` and `thread_free`)
  uint32_t              xblock_size;       // size available in each block (always `>0`)
  alloc_hook_block_t*           local_free;        // list of deferred free blocks by this thread (migrates to `free`)

  #if (ALLOC_HOOK_ENCODE_FREELIST || ALLOC_HOOK_PADDING)
  uintptr_t             keys[2];           // two random keys to encode the free lists (see `_alloc_hook_block_next`) or padding canary
  #endif

  _Atomic(alloc_hook_thread_free_t) xthread_free;  // list of deferred free blocks freed by other threads
  _Atomic(uintptr_t)        xheap;

  struct alloc_hook_page_s*     next;              // next page owned by this thread with the same `block_size`
  struct alloc_hook_page_s*     prev;              // previous page owned by this thread with the same `block_size`

  // 64-bit 9 words, 32-bit 12 words, (+2 for secure)
  #if ALLOC_HOOK_INTPTR_SIZE==8
  uintptr_t padding[1];
  #endif
} alloc_hook_page_t;



// ------------------------------------------------------
// Mimalloc segments contain alloc_hook pages
// ------------------------------------------------------

typedef enum alloc_hook_page_kind_e {
  ALLOC_HOOK_PAGE_SMALL,    // small blocks go into 64KiB pages inside a segment
  ALLOC_HOOK_PAGE_MEDIUM,   // medium blocks go into medium pages inside a segment
  ALLOC_HOOK_PAGE_LARGE,    // larger blocks go into a page of just one block
  ALLOC_HOOK_PAGE_HUGE,     // huge blocks (> 16 MiB) are put into a single page in a single segment.
} alloc_hook_page_kind_t;

typedef enum alloc_hook_segment_kind_e {
  ALLOC_HOOK_SEGMENT_NORMAL, // ALLOC_HOOK_SEGMENT_SIZE size with pages inside.
  ALLOC_HOOK_SEGMENT_HUGE,   // > ALLOC_HOOK_LARGE_SIZE_MAX segment with just one huge page inside.
} alloc_hook_segment_kind_t;

// ------------------------------------------------------
// A segment holds a commit mask where a bit is set if
// the corresponding ALLOC_HOOK_COMMIT_SIZE area is committed.
// The ALLOC_HOOK_COMMIT_SIZE must be a multiple of the slice
// size. If it is equal we have the most fine grained 
// decommit (but setting it higher can be more efficient).
// The ALLOC_HOOK_MINIMAL_COMMIT_SIZE is the minimal amount that will
// be committed in one go which can be set higher than
// ALLOC_HOOK_COMMIT_SIZE for efficiency (while the decommit mask
// is still tracked in fine-grained ALLOC_HOOK_COMMIT_SIZE chunks)
// ------------------------------------------------------

#define ALLOC_HOOK_MINIMAL_COMMIT_SIZE      (1*ALLOC_HOOK_SEGMENT_SLICE_SIZE)            
#define ALLOC_HOOK_COMMIT_SIZE              (ALLOC_HOOK_SEGMENT_SLICE_SIZE)              // 64KiB
#define ALLOC_HOOK_COMMIT_MASK_BITS         (ALLOC_HOOK_SEGMENT_SIZE / ALLOC_HOOK_COMMIT_SIZE)  
#define ALLOC_HOOK_COMMIT_MASK_FIELD_BITS    ALLOC_HOOK_SIZE_BITS
#define ALLOC_HOOK_COMMIT_MASK_FIELD_COUNT  (ALLOC_HOOK_COMMIT_MASK_BITS / ALLOC_HOOK_COMMIT_MASK_FIELD_BITS)

#if (ALLOC_HOOK_COMMIT_MASK_BITS != (ALLOC_HOOK_COMMIT_MASK_FIELD_COUNT * ALLOC_HOOK_COMMIT_MASK_FIELD_BITS))
#error "the segment size must be exactly divisible by the (commit size * size_t bits)"
#endif

typedef struct alloc_hook_commit_mask_s {
  size_t mask[ALLOC_HOOK_COMMIT_MASK_FIELD_COUNT];
} alloc_hook_commit_mask_t;

typedef alloc_hook_page_t  alloc_hook_slice_t;
typedef int64_t    alloc_hook_msecs_t;


// Memory can reside in arena's, direct OS allocated, or statically allocated. The memid keeps track of this.
typedef enum alloc_hook_memkind_e {
  ALLOC_HOOK_MEM_NONE,      // not allocated
  ALLOC_HOOK_MEM_EXTERNAL,  // not owned by alloc_hook but provided externally (via `alloc_hook_manage_os_memory` for example)
  ALLOC_HOOK_MEM_STATIC,    // allocated in a static area and should not be freed (for arena meta data for example)
  ALLOC_HOOK_MEM_OS,        // allocated from the OS
  ALLOC_HOOK_MEM_OS_HUGE,   // allocated as huge os pages
  ALLOC_HOOK_MEM_OS_REMAP,  // allocated in a remapable area (i.e. using `mremap`)
  ALLOC_HOOK_MEM_ARENA      // allocated from an arena (the usual case)
} alloc_hook_memkind_t;

static inline bool alloc_hook_memkind_is_os(alloc_hook_memkind_t memkind) {
  return (memkind >= ALLOC_HOOK_MEM_OS && memkind <= ALLOC_HOOK_MEM_OS_REMAP);
}

typedef struct alloc_hook_memid_os_info {
  void*         base;               // actual base address of the block (used for offset aligned allocations)
  size_t        alignment;          // alignment at allocation
} alloc_hook_memid_os_info_t;

typedef struct alloc_hook_memid_arena_info {
  size_t        block_index;        // index in the arena
  alloc_hook_arena_id_t id;                 // arena id (>= 1)
  bool          is_exclusive;       // the arena can only be used for specific arena allocations
} alloc_hook_memid_arena_info_t;

typedef struct alloc_hook_memid_s {
  union {
    alloc_hook_memid_os_info_t    os;       // only used for ALLOC_HOOK_MEM_OS
    alloc_hook_memid_arena_info_t arena;    // only used for ALLOC_HOOK_MEM_ARENA
  } mem;
  bool          is_pinned;          // `true` if we cannot decommit/reset/protect in this memory (e.g. when allocated using large OS pages)
  bool          initially_committed;// `true` if the memory was originally allocated as committed
  bool          initially_zero;     // `true` if the memory was originally zero initialized
  alloc_hook_memkind_t  memkind;
} alloc_hook_memid_t;


// Segments are large allocated memory blocks (8mb on 64 bit) from
// the OS. Inside segments we allocated fixed size _pages_ that
// contain blocks.
typedef struct alloc_hook_segment_s {
  // constant fields
  alloc_hook_memid_t        memid;              // memory id for arena allocation
  bool              allow_decommit;
  bool              allow_purge;
  size_t            segment_size;

  // segment fields
  alloc_hook_msecs_t        purge_expire;
  alloc_hook_commit_mask_t  purge_mask;
  alloc_hook_commit_mask_t  commit_mask;

  _Atomic(struct alloc_hook_segment_s*) abandoned_next;

  // from here is zero initialized
  struct alloc_hook_segment_s* next;            // the list of freed segments in the cache (must be first field, see `segment.c:alloc_hook_segment_init`)
  
  size_t            abandoned;          // abandoned pages (i.e. the original owning thread stopped) (`abandoned <= used`)
  size_t            abandoned_visits;   // count how often this segment is visited in the abandoned list (to force reclaim it it is too long)
  size_t            used;               // count of pages in use
  uintptr_t         cookie;             // verify addresses in debug mode: `alloc_hook_ptr_cookie(segment) == segment->cookie`  

  size_t            segment_slices;      // for huge segments this may be different from `ALLOC_HOOK_SLICES_PER_SEGMENT`
  size_t            segment_info_slices; // initial slices we are using segment info and possible guard pages.

  // layout like this to optimize access in `alloc_hook_free`
  alloc_hook_segment_kind_t kind;
  size_t            slice_entries;       // entries in the `slices` array, at most `ALLOC_HOOK_SLICES_PER_SEGMENT`
  _Atomic(alloc_hook_threadid_t) thread_id;      // unique id of the thread owning this segment

  alloc_hook_slice_t        slices[ALLOC_HOOK_SLICES_PER_SEGMENT+1];  // one more for huge blocks with large alignment
} alloc_hook_segment_t;


// ------------------------------------------------------
// Heaps
// Provide first-class heaps to allocate from.
// A heap just owns a set of pages for allocation and
// can only be allocate/reallocate from the thread that created it.
// Freeing blocks can be done from any thread though.
// Per thread, the segments are shared among its heaps.
// Per thread, there is always a default heap that is
// used for allocation; it is initialized to statically
// point to an empty heap to avoid initialization checks
// in the fast path.
// ------------------------------------------------------

// Thread local data
typedef struct alloc_hook_tld_s alloc_hook_tld_t;

// Pages of a certain block size are held in a queue.
typedef struct alloc_hook_page_queue_s {
  alloc_hook_page_t* first;
  alloc_hook_page_t* last;
  size_t     block_size;
} alloc_hook_page_queue_t;

#define ALLOC_HOOK_BIN_FULL  (ALLOC_HOOK_BIN_HUGE+1)

// Random context
typedef struct alloc_hook_random_cxt_s {
  uint32_t input[16];
  uint32_t output[16];
  int      output_available;
  bool     weak;
} alloc_hook_random_ctx_t;


// In debug mode there is a padding structure at the end of the blocks to check for buffer overflows
#if (ALLOC_HOOK_PADDING)
typedef struct alloc_hook_padding_s {
  uint32_t canary; // encoded block value to check validity of the padding (in case of overflow)
  uint32_t delta;  // padding bytes before the block. (alloc_hook_usable_size(p) - delta == exact allocated bytes)
} alloc_hook_padding_t;
#define ALLOC_HOOK_PADDING_SIZE   (sizeof(alloc_hook_padding_t))
#define ALLOC_HOOK_PADDING_WSIZE  ((ALLOC_HOOK_PADDING_SIZE + ALLOC_HOOK_INTPTR_SIZE - 1) / ALLOC_HOOK_INTPTR_SIZE)
#else
#define ALLOC_HOOK_PADDING_SIZE   0
#define ALLOC_HOOK_PADDING_WSIZE  0
#endif

#define ALLOC_HOOK_PAGES_DIRECT   (ALLOC_HOOK_SMALL_WSIZE_MAX + ALLOC_HOOK_PADDING_WSIZE + 1)


// A heap owns a set of pages.
struct alloc_hook_heap_s {
  alloc_hook_tld_t*             tld;
  alloc_hook_page_t*            pages_free_direct[ALLOC_HOOK_PAGES_DIRECT];  // optimize: array where every entry points a page with possibly free blocks in the corresponding queue for that size.
  alloc_hook_page_queue_t       pages[ALLOC_HOOK_BIN_FULL + 1];              // queue of pages for each size class (or "bin")
  _Atomic(alloc_hook_block_t*)  thread_delayed_free;
  alloc_hook_threadid_t         thread_id;                           // thread this heap belongs too
  alloc_hook_arena_id_t         arena_id;                            // arena id if the heap belongs to a specific arena (or 0)  
  uintptr_t             cookie;                              // random cookie to verify pointers (see `_alloc_hook_ptr_cookie`)
  uintptr_t             keys[2];                             // two random keys used to encode the `thread_delayed_free` list
  alloc_hook_random_ctx_t       random;                              // random number context used for secure allocation
  size_t                page_count;                          // total number of pages in the `pages` queues.
  size_t                page_retired_min;                    // smallest retired index (retired pages are fully free, but still in the page queues)
  size_t                page_retired_max;                    // largest retired index into the `pages` array.
  alloc_hook_heap_t*            next;                                // list of heaps per thread
  bool                  no_reclaim;                          // `true` if this heap should not reclaim abandoned pages
};



// ------------------------------------------------------
// Debug
// ------------------------------------------------------

#if !defined(ALLOC_HOOK_DEBUG_UNINIT)
#define ALLOC_HOOK_DEBUG_UNINIT     (0xD0)
#endif
#if !defined(ALLOC_HOOK_DEBUG_FREED)
#define ALLOC_HOOK_DEBUG_FREED      (0xDF)
#endif
#if !defined(ALLOC_HOOK_DEBUG_PADDING)
#define ALLOC_HOOK_DEBUG_PADDING    (0xDE)
#endif

#if (ALLOC_HOOK_DEBUG)
// use our own assertion to print without memory allocation
void _alloc_hook_assert_fail(const char* assertion, const char* fname, unsigned int line, const char* func );
#define alloc_hook_assert(expr)     ((expr) ? (void)0 : _alloc_hook_assert_fail(#expr,__FILE__,__LINE__,__func__))
#else
#define alloc_hook_assert(x)
#endif

#if (ALLOC_HOOK_DEBUG>1)
#define alloc_hook_assert_internal    alloc_hook_assert
#else
#define alloc_hook_assert_internal(x)
#endif

#if (ALLOC_HOOK_DEBUG>2)
#define alloc_hook_assert_expensive   alloc_hook_assert
#else
#define alloc_hook_assert_expensive(x)
#endif

// ------------------------------------------------------
// Statistics
// ------------------------------------------------------

#ifndef ALLOC_HOOK_STAT
#if (ALLOC_HOOK_DEBUG>0)
#define ALLOC_HOOK_STAT 2
#else
#define ALLOC_HOOK_STAT 0
#endif
#endif

typedef struct alloc_hook_stat_count_s {
  int64_t allocated;
  int64_t freed;
  int64_t peak;
  int64_t current;
} alloc_hook_stat_count_t;

typedef struct alloc_hook_stat_counter_s {
  int64_t total;
  int64_t count;
} alloc_hook_stat_counter_t;

typedef struct alloc_hook_stats_s {
  alloc_hook_stat_count_t segments;
  alloc_hook_stat_count_t pages;
  alloc_hook_stat_count_t reserved;
  alloc_hook_stat_count_t committed;
  alloc_hook_stat_count_t reset;
  alloc_hook_stat_count_t purged;
  alloc_hook_stat_count_t page_committed;
  alloc_hook_stat_count_t segments_abandoned;
  alloc_hook_stat_count_t pages_abandoned;
  alloc_hook_stat_count_t threads;
  alloc_hook_stat_count_t normal;
  alloc_hook_stat_count_t huge;
  alloc_hook_stat_count_t large;
  alloc_hook_stat_count_t malloc;
  alloc_hook_stat_count_t segments_cache;
  alloc_hook_stat_counter_t pages_extended;
  alloc_hook_stat_counter_t mmap_calls;
  alloc_hook_stat_counter_t commit_calls;
  alloc_hook_stat_counter_t reset_calls;
  alloc_hook_stat_counter_t purge_calls;
  alloc_hook_stat_counter_t page_no_retire;
  alloc_hook_stat_counter_t searches;
  alloc_hook_stat_counter_t normal_count;
  alloc_hook_stat_counter_t huge_count;
  alloc_hook_stat_counter_t large_count;
#if ALLOC_HOOK_STAT>1
  alloc_hook_stat_count_t normal_bins[ALLOC_HOOK_BIN_HUGE+1];
#endif
} alloc_hook_stats_t;


void _alloc_hook_stat_increase(alloc_hook_stat_count_t* stat, size_t amount);
void _alloc_hook_stat_decrease(alloc_hook_stat_count_t* stat, size_t amount);
void _alloc_hook_stat_counter_increase(alloc_hook_stat_counter_t* stat, size_t amount);

#if (ALLOC_HOOK_STAT)
#define alloc_hook_stat_increase(stat,amount)         _alloc_hook_stat_increase( &(stat), amount)
#define alloc_hook_stat_decrease(stat,amount)         _alloc_hook_stat_decrease( &(stat), amount)
#define alloc_hook_stat_counter_increase(stat,amount) _alloc_hook_stat_counter_increase( &(stat), amount)
#else
#define alloc_hook_stat_increase(stat,amount)         (void)0
#define alloc_hook_stat_decrease(stat,amount)         (void)0
#define alloc_hook_stat_counter_increase(stat,amount) (void)0
#endif

#define alloc_hook_heap_stat_counter_increase(heap,stat,amount)  alloc_hook_stat_counter_increase( (heap)->tld->stats.stat, amount)
#define alloc_hook_heap_stat_increase(heap,stat,amount)  alloc_hook_stat_increase( (heap)->tld->stats.stat, amount)
#define alloc_hook_heap_stat_decrease(heap,stat,amount)  alloc_hook_stat_decrease( (heap)->tld->stats.stat, amount)

// ------------------------------------------------------
// Thread Local data
// ------------------------------------------------------

// A "span" is is an available range of slices. The span queues keep
// track of slice spans of at most the given `slice_count` (but more than the previous size class).
typedef struct alloc_hook_span_queue_s {
  alloc_hook_slice_t* first;
  alloc_hook_slice_t* last;
  size_t      slice_count;
} alloc_hook_span_queue_t;

#define ALLOC_HOOK_SEGMENT_BIN_MAX (35)     // 35 == alloc_hook_segment_bin(ALLOC_HOOK_SLICES_PER_SEGMENT)

// OS thread local data
typedef struct alloc_hook_os_tld_s {
  size_t                region_idx;   // start point for next allocation
  alloc_hook_stats_t*           stats;        // points to tld stats
} alloc_hook_os_tld_t;


// Segments thread local data
typedef struct alloc_hook_segments_tld_s {
  alloc_hook_span_queue_t     spans[ALLOC_HOOK_SEGMENT_BIN_MAX+1];  // free slice spans inside segments
  size_t              count;        // current number of segments;
  size_t              peak_count;   // peak number of segments
  size_t              current_size; // current size of all segments
  size_t              peak_size;    // peak size of all segments
  alloc_hook_stats_t*         stats;        // points to tld stats
  alloc_hook_os_tld_t*        os;           // points to os stats
} alloc_hook_segments_tld_t;

// Thread local data
struct alloc_hook_tld_s {
  unsigned long long  heartbeat;     // monotonic heartbeat count
  bool                recurse;       // true if deferred was called; used to prevent infinite recursion.
  alloc_hook_heap_t*          heap_backing;  // backing heap of this thread (cannot be deleted)
  alloc_hook_heap_t*          heaps;         // list of heaps in this thread (so we can abandon all when the thread terminates)
  alloc_hook_segments_tld_t   segments;      // segment tld
  alloc_hook_os_tld_t         os;            // os tld
  alloc_hook_stats_t          stats;         // statistics
};

#endif
