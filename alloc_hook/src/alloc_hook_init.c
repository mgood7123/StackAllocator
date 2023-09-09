/* ----------------------------------------------------------------------------
Copyright (c) 2018-2022, Microsoft Research, Daan Leijen
This is free software; you can redistribute it and/or modify it under the
terms of the MIT license. A copy of the license can be found in the file
"LICENSE" at the root of this distribution.
-----------------------------------------------------------------------------*/
#include "alloc_hook.h"
#include "alloc_hook_internal.h"
#include "alloc_hook_prim.h"

#include <string.h>  // memcpy, memset
#include <stdlib.h>  // atexit

// Empty page used to initialize the small free pages array
const alloc_hook_page_t _alloc_hook_page_empty = {
  0, false, false, false,
  0,       // capacity
  0,       // reserved capacity
  { 0 },   // flags
  false,   // is_zero
  0,       // retire_expire
  NULL,    // free
  0,       // used
  0,       // xblock_size
  NULL,    // local_free
  #if (ALLOC_HOOK_PADDING || ALLOC_HOOK_ENCODE_FREELIST)
  { 0, 0 },
  #endif
  ALLOC_HOOK_ATOMIC_VAR_INIT(0), // xthread_free
  ALLOC_HOOK_ATOMIC_VAR_INIT(0), // xheap
  NULL, NULL
  #if ALLOC_HOOK_INTPTR_SIZE==8
  , { 0 }  // padding
  #endif
};

#define ALLOC_HOOK_PAGE_EMPTY() ((alloc_hook_page_t*)&_alloc_hook_page_empty)

#if (ALLOC_HOOK_SMALL_WSIZE_MAX==128)
#if (ALLOC_HOOK_PADDING>0) && (ALLOC_HOOK_INTPTR_SIZE >= 8)
#define ALLOC_HOOK_SMALL_PAGES_EMPTY  { ALLOC_HOOK_INIT128(ALLOC_HOOK_PAGE_EMPTY), ALLOC_HOOK_PAGE_EMPTY(), ALLOC_HOOK_PAGE_EMPTY() }
#elif (ALLOC_HOOK_PADDING>0)
#define ALLOC_HOOK_SMALL_PAGES_EMPTY  { ALLOC_HOOK_INIT128(ALLOC_HOOK_PAGE_EMPTY), ALLOC_HOOK_PAGE_EMPTY(), ALLOC_HOOK_PAGE_EMPTY(), ALLOC_HOOK_PAGE_EMPTY() }
#else
#define ALLOC_HOOK_SMALL_PAGES_EMPTY  { ALLOC_HOOK_INIT128(ALLOC_HOOK_PAGE_EMPTY), ALLOC_HOOK_PAGE_EMPTY() }
#endif
#else
#error "define right initialization sizes corresponding to ALLOC_HOOK_SMALL_WSIZE_MAX"
#endif

// Empty page queues for every bin
#define QNULL(sz)  { NULL, NULL, (sz)*sizeof(uintptr_t) }
#define ALLOC_HOOK_PAGE_QUEUES_EMPTY \
  { QNULL(1), \
    QNULL(     1), QNULL(     2), QNULL(     3), QNULL(     4), QNULL(     5), QNULL(     6), QNULL(     7), QNULL(     8), /* 8 */ \
    QNULL(    10), QNULL(    12), QNULL(    14), QNULL(    16), QNULL(    20), QNULL(    24), QNULL(    28), QNULL(    32), /* 16 */ \
    QNULL(    40), QNULL(    48), QNULL(    56), QNULL(    64), QNULL(    80), QNULL(    96), QNULL(   112), QNULL(   128), /* 24 */ \
    QNULL(   160), QNULL(   192), QNULL(   224), QNULL(   256), QNULL(   320), QNULL(   384), QNULL(   448), QNULL(   512), /* 32 */ \
    QNULL(   640), QNULL(   768), QNULL(   896), QNULL(  1024), QNULL(  1280), QNULL(  1536), QNULL(  1792), QNULL(  2048), /* 40 */ \
    QNULL(  2560), QNULL(  3072), QNULL(  3584), QNULL(  4096), QNULL(  5120), QNULL(  6144), QNULL(  7168), QNULL(  8192), /* 48 */ \
    QNULL( 10240), QNULL( 12288), QNULL( 14336), QNULL( 16384), QNULL( 20480), QNULL( 24576), QNULL( 28672), QNULL( 32768), /* 56 */ \
    QNULL( 40960), QNULL( 49152), QNULL( 57344), QNULL( 65536), QNULL( 81920), QNULL( 98304), QNULL(114688), QNULL(131072), /* 64 */ \
    QNULL(163840), QNULL(196608), QNULL(229376), QNULL(262144), QNULL(327680), QNULL(393216), QNULL(458752), QNULL(524288), /* 72 */ \
    QNULL(ALLOC_HOOK_MEDIUM_OBJ_WSIZE_MAX + 1  /* 655360, Huge queue */), \
    QNULL(ALLOC_HOOK_MEDIUM_OBJ_WSIZE_MAX + 2) /* Full queue */ }

#define ALLOC_HOOK_STAT_COUNT_NULL()  {0,0,0,0}

// Empty statistics
#if ALLOC_HOOK_STAT>1
#define ALLOC_HOOK_STAT_COUNT_END_NULL()  , { ALLOC_HOOK_STAT_COUNT_NULL(), ALLOC_HOOK_INIT32(ALLOC_HOOK_STAT_COUNT_NULL) }
#else
#define ALLOC_HOOK_STAT_COUNT_END_NULL()
#endif

#define ALLOC_HOOK_STATS_NULL  \
  ALLOC_HOOK_STAT_COUNT_NULL(), ALLOC_HOOK_STAT_COUNT_NULL(), \
  ALLOC_HOOK_STAT_COUNT_NULL(), ALLOC_HOOK_STAT_COUNT_NULL(), \
  ALLOC_HOOK_STAT_COUNT_NULL(), ALLOC_HOOK_STAT_COUNT_NULL(), \
  ALLOC_HOOK_STAT_COUNT_NULL(), ALLOC_HOOK_STAT_COUNT_NULL(), \
  ALLOC_HOOK_STAT_COUNT_NULL(), ALLOC_HOOK_STAT_COUNT_NULL(), \
  ALLOC_HOOK_STAT_COUNT_NULL(), ALLOC_HOOK_STAT_COUNT_NULL(), \
  ALLOC_HOOK_STAT_COUNT_NULL(), ALLOC_HOOK_STAT_COUNT_NULL(), \
  ALLOC_HOOK_STAT_COUNT_NULL(), \
  { 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 0 }, \
  { 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 0 } \
  ALLOC_HOOK_STAT_COUNT_END_NULL()


// Empty slice span queues for every bin
#define SQNULL(sz)  { NULL, NULL, sz }
#define ALLOC_HOOK_SEGMENT_SPAN_QUEUES_EMPTY \
  { SQNULL(1), \
    SQNULL(     1), SQNULL(     2), SQNULL(     3), SQNULL(     4), SQNULL(     5), SQNULL(     6), SQNULL(     7), SQNULL(    10), /*  8 */ \
    SQNULL(    12), SQNULL(    14), SQNULL(    16), SQNULL(    20), SQNULL(    24), SQNULL(    28), SQNULL(    32), SQNULL(    40), /* 16 */ \
    SQNULL(    48), SQNULL(    56), SQNULL(    64), SQNULL(    80), SQNULL(    96), SQNULL(   112), SQNULL(   128), SQNULL(   160), /* 24 */ \
    SQNULL(   192), SQNULL(   224), SQNULL(   256), SQNULL(   320), SQNULL(   384), SQNULL(   448), SQNULL(   512), SQNULL(   640), /* 32 */ \
    SQNULL(   768), SQNULL(   896), SQNULL(  1024) /* 35 */ }


// --------------------------------------------------------
// Statically allocate an empty heap as the initial
// thread local value for the default heap,
// and statically allocate the backing heap for the main
// thread so it can function without doing any allocation
// itself (as accessing a thread local for the first time
// may lead to allocation itself on some platforms)
// --------------------------------------------------------

alloc_hook_decl_cache_align const alloc_hook_heap_t _alloc_hook_heap_empty = {
  NULL,
  ALLOC_HOOK_SMALL_PAGES_EMPTY,
  ALLOC_HOOK_PAGE_QUEUES_EMPTY,
  ALLOC_HOOK_ATOMIC_VAR_INIT(NULL),
  0,                // tid
  0,                // cookie
  0,                // arena id
  { 0, 0 },         // keys
  { {0}, {0}, 0, true }, // random
  0,                // page count
  ALLOC_HOOK_BIN_FULL, 0,   // page retired min/max
  NULL,             // next
  false
};

#define tld_empty_stats  ((alloc_hook_stats_t*)((uint8_t*)&tld_empty + offsetof(alloc_hook_tld_t,stats)))
#define tld_empty_os     ((alloc_hook_os_tld_t*)((uint8_t*)&tld_empty + offsetof(alloc_hook_tld_t,os)))

alloc_hook_decl_cache_align static const alloc_hook_tld_t tld_empty = {
  0,
  false,
  NULL, NULL,
  { ALLOC_HOOK_SEGMENT_SPAN_QUEUES_EMPTY, 0, 0, 0, 0, tld_empty_stats, tld_empty_os }, // segments
  { 0, tld_empty_stats }, // os
  { ALLOC_HOOK_STATS_NULL }       // stats
};

alloc_hook_threadid_t _alloc_hook_thread_id(void) alloc_hook_attr_noexcept {
  return _alloc_hook_prim_thread_id();
}

// the thread-local default heap for allocation
alloc_hook_decl_thread alloc_hook_heap_t* _alloc_hook_heap_default = (alloc_hook_heap_t*)&_alloc_hook_heap_empty;

extern alloc_hook_heap_t _alloc_hook_heap_main;

static alloc_hook_tld_t tld_main = {
  0, false,
  &_alloc_hook_heap_main, & _alloc_hook_heap_main,
  { ALLOC_HOOK_SEGMENT_SPAN_QUEUES_EMPTY, 0, 0, 0, 0, &tld_main.stats, &tld_main.os }, // segments
  { 0, &tld_main.stats },  // os
  { ALLOC_HOOK_STATS_NULL }       // stats
};

alloc_hook_heap_t _alloc_hook_heap_main = {
  &tld_main,
  ALLOC_HOOK_SMALL_PAGES_EMPTY,
  ALLOC_HOOK_PAGE_QUEUES_EMPTY,
  ALLOC_HOOK_ATOMIC_VAR_INIT(NULL),
  0,                // thread id
  0,                // initial cookie
  0,                // arena id
  { 0, 0 },         // the key of the main heap can be fixed (unlike page keys that need to be secure!)
  { {0x846ca68b}, {0}, 0, true },  // random
  0,                // page count
  ALLOC_HOOK_BIN_FULL, 0,   // page retired min/max
  NULL,             // next heap
  false             // can reclaim
};

bool _alloc_hook_process_is_initialized = false;  // set to `true` in `alloc_hook_process_init`.

alloc_hook_stats_t _alloc_hook_stats_main = { ALLOC_HOOK_STATS_NULL };


static void alloc_hook_heap_main_init(void) {
  if (_alloc_hook_heap_main.cookie == 0) {
    _alloc_hook_heap_main.thread_id = _alloc_hook_thread_id();
    _alloc_hook_heap_main.cookie = 1;
    #if defined(_WIN32) && !defined(ALLOC_HOOK_SHARED_LIB)
      _alloc_hook_random_init_weak(&_alloc_hook_heap_main.random);    // prevent allocation failure during bcrypt dll initialization with static linking
    #else
      _alloc_hook_random_init(&_alloc_hook_heap_main.random);
    #endif
    _alloc_hook_heap_main.cookie  = _alloc_hook_heap_random_next(&_alloc_hook_heap_main);
    _alloc_hook_heap_main.keys[0] = _alloc_hook_heap_random_next(&_alloc_hook_heap_main);
    _alloc_hook_heap_main.keys[1] = _alloc_hook_heap_random_next(&_alloc_hook_heap_main);
  }
}

alloc_hook_heap_t* _alloc_hook_heap_main_get(void) {
  alloc_hook_heap_main_init();
  return &_alloc_hook_heap_main;
}


/* -----------------------------------------------------------
  Initialization and freeing of the thread local heaps
----------------------------------------------------------- */

// note: in x64 in release build `sizeof(alloc_hook_thread_data_t)` is under 4KiB (= OS page size).
typedef struct alloc_hook_thread_data_s {
  alloc_hook_heap_t  heap;  // must come first due to cast in `_alloc_hook_heap_done`
  alloc_hook_tld_t   tld;
  alloc_hook_memid_t memid;
} alloc_hook_thread_data_t;


// Thread meta-data is allocated directly from the OS. For
// some programs that do not use thread pools and allocate and
// destroy many OS threads, this may causes too much overhead
// per thread so we maintain a small cache of recently freed metadata.

#define TD_CACHE_SIZE (16)
static _Atomic(alloc_hook_thread_data_t*) td_cache[TD_CACHE_SIZE];

static alloc_hook_thread_data_t* alloc_hook_thread_data_zalloc(void) {
  // try to find thread metadata in the cache
  bool is_zero = false;
  alloc_hook_thread_data_t* td = NULL;
  for (int i = 0; i < TD_CACHE_SIZE; i++) {
    td = alloc_hook_atomic_load_ptr_relaxed(alloc_hook_thread_data_t, &td_cache[i]);
    if (td != NULL) {
      // found cached allocation, try use it
      td = alloc_hook_atomic_exchange_ptr_acq_rel(alloc_hook_thread_data_t, &td_cache[i], NULL);
      if (td != NULL) {
        break;
      }
    }
  }

  // if that fails, allocate as meta data
  if (td == NULL) {
    alloc_hook_memid_t memid;
    td = (alloc_hook_thread_data_t*)_alloc_hook_os_alloc(sizeof(alloc_hook_thread_data_t), &memid, &_alloc_hook_stats_main);
    if (td == NULL) {
      // if this fails, try once more. (issue #257)
      td = (alloc_hook_thread_data_t*)_alloc_hook_os_alloc(sizeof(alloc_hook_thread_data_t), &memid, &_alloc_hook_stats_main);
      if (td == NULL) {
        // really out of memory
        _alloc_hook_error_message(ENOMEM, "unable to allocate thread local heap metadata (%zu bytes)\n", sizeof(alloc_hook_thread_data_t));
      }
    }
    if (td != NULL) {
      td->memid = memid;
      is_zero = memid.initially_zero;
    }
  }
  
  if (td != NULL && !is_zero) {
    _alloc_hook_memzero_aligned(td, sizeof(*td));
  }
  return td;
}

static void alloc_hook_thread_data_free( alloc_hook_thread_data_t* tdfree ) {
  // try to add the thread metadata to the cache
  for (int i = 0; i < TD_CACHE_SIZE; i++) {
    alloc_hook_thread_data_t* td = alloc_hook_atomic_load_ptr_relaxed(alloc_hook_thread_data_t, &td_cache[i]);
    if (td == NULL) {
      alloc_hook_thread_data_t* expected = NULL;
      if (alloc_hook_atomic_cas_ptr_weak_acq_rel(alloc_hook_thread_data_t, &td_cache[i], &expected, tdfree)) {
        return;
      }
    }
  }
  // if that fails, just free it directly
  _alloc_hook_os_free(tdfree, sizeof(alloc_hook_thread_data_t), tdfree->memid, &_alloc_hook_stats_main);
}

void _alloc_hook_thread_data_collect(void) {
  // free all thread metadata from the cache
  for (int i = 0; i < TD_CACHE_SIZE; i++) {
    alloc_hook_thread_data_t* td = alloc_hook_atomic_load_ptr_relaxed(alloc_hook_thread_data_t, &td_cache[i]);
    if (td != NULL) {
      td = alloc_hook_atomic_exchange_ptr_acq_rel(alloc_hook_thread_data_t, &td_cache[i], NULL);
      if (td != NULL) {
        _alloc_hook_os_free(td, sizeof(alloc_hook_thread_data_t), td->memid, &_alloc_hook_stats_main);
      }
    }
  }
}

// Initialize the thread local default heap, called from `alloc_hook_thread_init`
static bool _alloc_hook_heap_init(void) {
  if (alloc_hook_heap_is_initialized(alloc_hook_prim_get_default_heap())) return true;
  if (_alloc_hook_is_main_thread()) {
    // alloc_hook_assert_internal(_alloc_hook_heap_main.thread_id != 0);  // can happen on freeBSD where alloc is called before any initialization
    // the main heap is statically allocated
    alloc_hook_heap_main_init();
    _alloc_hook_heap_set_default_direct(&_alloc_hook_heap_main);
    //alloc_hook_assert_internal(_alloc_hook_heap_default->tld->heap_backing == alloc_hook_prim_get_default_heap());
  }
  else {
    // use `_alloc_hook_os_alloc` to allocate directly from the OS
    alloc_hook_thread_data_t* td = alloc_hook_thread_data_zalloc();
    if (td == NULL) return false;

    alloc_hook_tld_t*  tld = &td->tld;
    alloc_hook_heap_t* heap = &td->heap;
    _alloc_hook_memcpy_aligned(tld, &tld_empty, sizeof(*tld));
    _alloc_hook_memcpy_aligned(heap, &_alloc_hook_heap_empty, sizeof(*heap));
    heap->thread_id = _alloc_hook_thread_id();
    _alloc_hook_random_init(&heap->random);
    heap->cookie  = _alloc_hook_heap_random_next(heap) | 1;
    heap->keys[0] = _alloc_hook_heap_random_next(heap);
    heap->keys[1] = _alloc_hook_heap_random_next(heap);
    heap->tld = tld;
    tld->heap_backing = heap;
    tld->heaps = heap;
    tld->segments.stats = &tld->stats;
    tld->segments.os = &tld->os;
    tld->os.stats = &tld->stats;
    _alloc_hook_heap_set_default_direct(heap);
  }
  return false;
}

// Free the thread local default heap (called from `alloc_hook_thread_done`)
static bool _alloc_hook_heap_done(alloc_hook_heap_t* heap) {
  if (!alloc_hook_heap_is_initialized(heap)) return true;

  // reset default heap
  _alloc_hook_heap_set_default_direct(_alloc_hook_is_main_thread() ? &_alloc_hook_heap_main : (alloc_hook_heap_t*)&_alloc_hook_heap_empty);

  // switch to backing heap
  heap = heap->tld->heap_backing;
  if (!alloc_hook_heap_is_initialized(heap)) return false;

  // delete all non-backing heaps in this thread
  alloc_hook_heap_t* curr = heap->tld->heaps;
  while (curr != NULL) {
    alloc_hook_heap_t* next = curr->next; // save `next` as `curr` will be freed
    if (curr != heap) {
      alloc_hook_assert_internal(!alloc_hook_heap_is_backing(curr));
      alloc_hook_heap_delete(curr);
    }
    curr = next;
  }
  alloc_hook_assert_internal(heap->tld->heaps == heap && heap->next == NULL);
  alloc_hook_assert_internal(alloc_hook_heap_is_backing(heap));

  // collect if not the main thread
  if (heap != &_alloc_hook_heap_main) {
    _alloc_hook_heap_collect_abandon(heap);
  }

  // merge stats
  _alloc_hook_stats_done(&heap->tld->stats);

  // free if not the main thread
  if (heap != &_alloc_hook_heap_main) {
    // the following assertion does not always hold for huge segments as those are always treated
    // as abondened: one may allocate it in one thread, but deallocate in another in which case
    // the count can be too large or negative. todo: perhaps not count huge segments? see issue #363
    // alloc_hook_assert_internal(heap->tld->segments.count == 0 || heap->thread_id != _alloc_hook_thread_id());
    alloc_hook_thread_data_free((alloc_hook_thread_data_t*)heap);
  }
  else {
    #if 0
    // never free the main thread even in debug mode; if a dll is linked statically with alloc_hook,
    // there may still be delete/free calls after the alloc_hook_fls_done is called. Issue #207
    _alloc_hook_heap_destroy_pages(heap);
    alloc_hook_assert_internal(heap->tld->heap_backing == &_alloc_hook_heap_main);
    #endif
  }
  return false;
}



// --------------------------------------------------------
// Try to run `alloc_hook_thread_done()` automatically so any memory
// owned by the thread but not yet released can be abandoned
// and re-owned by another thread.
//
// 1. windows dynamic library:
//     call from DllMain on DLL_THREAD_DETACH
// 2. windows static library:
//     use `FlsAlloc` to call a destructor when the thread is done
// 3. unix, pthreads:
//     use a pthread key to call a destructor when a pthread is done
//
// In the last two cases we also need to call `alloc_hook_process_init`
// to set up the thread local keys.
// --------------------------------------------------------

// Set up handlers so `alloc_hook_thread_done` is called automatically
static void alloc_hook_process_setup_auto_thread_done(void) {
  static bool tls_initialized = false; // fine if it races
  if (tls_initialized) return;
  tls_initialized = true;
  _alloc_hook_prim_thread_init_auto_done();
  _alloc_hook_heap_set_default_direct(&_alloc_hook_heap_main);
}


bool _alloc_hook_is_main_thread(void) {
  return (_alloc_hook_heap_main.thread_id==0 || _alloc_hook_heap_main.thread_id == _alloc_hook_thread_id());
}

static _Atomic(size_t) thread_count = ALLOC_HOOK_ATOMIC_VAR_INIT(1);

size_t  _alloc_hook_current_thread_count(void) {
  return alloc_hook_atomic_load_relaxed(&thread_count);
}

// This is called from the `alloc_hook_malloc_generic`
void alloc_hook_thread_init(void) alloc_hook_attr_noexcept
{
  // ensure our process has started already
  alloc_hook_process_init();

  // initialize the thread local default heap
  // (this will call `_alloc_hook_heap_set_default_direct` and thus set the
  //  fiber/pthread key to a non-zero value, ensuring `_alloc_hook_thread_done` is called)
  if (_alloc_hook_heap_init()) return;  // returns true if already initialized

  _alloc_hook_stat_increase(&_alloc_hook_stats_main.threads, 1);
  alloc_hook_atomic_increment_relaxed(&thread_count);
  _alloc_hook_verbose_message("thread init: 0x%zx\n", _alloc_hook_thread_id());
}

void alloc_hook_thread_done(void) alloc_hook_attr_noexcept {
  _alloc_hook_thread_done(NULL);
}

void _alloc_hook_thread_done(alloc_hook_heap_t* heap) 
{
  // calling with NULL implies using the default heap
  if (heap == NULL) { 
    heap = alloc_hook_prim_get_default_heap(); 
    if (heap == NULL) return;
  }

  // prevent re-entrancy through heap_done/heap_set_default_direct (issue #699)
  if (!alloc_hook_heap_is_initialized(heap)) {
    return; 
  }

  // adjust stats
  alloc_hook_atomic_decrement_relaxed(&thread_count);
  _alloc_hook_stat_decrease(&_alloc_hook_stats_main.threads, 1);
  
  // check thread-id as on Windows shutdown with FLS the main (exit) thread may call this on thread-local heaps...
  if (heap->thread_id != _alloc_hook_thread_id()) return;

  // abandon the thread local heap
  if (_alloc_hook_heap_done(heap)) return;  // returns true if already ran
  _alloc_hook_verbose_message("thread done: 0x%zx\n", _alloc_hook_thread_id());
}

void _alloc_hook_heap_set_default_direct(alloc_hook_heap_t* heap)  {
  alloc_hook_assert_internal(heap != NULL);
  #if defined(ALLOC_HOOK_TLS_SLOT)
  alloc_hook_prim_tls_slot_set(ALLOC_HOOK_TLS_SLOT,heap);
  #elif defined(ALLOC_HOOK_TLS_PTHREAD_SLOT_OFS)
  *alloc_hook_tls_pthread_heap_slot() = heap;
  #elif defined(ALLOC_HOOK_TLS_PTHREAD)
  // we use _alloc_hook_heap_default_key
  #else
  _alloc_hook_heap_default = heap;
  #endif

  // ensure the default heap is passed to `_alloc_hook_thread_done`
  // setting to a non-NULL value also ensures `alloc_hook_thread_done` is called.
  _alloc_hook_prim_thread_associate_default_heap(heap);    
}


// --------------------------------------------------------
// Run functions on process init/done, and thread init/done
// --------------------------------------------------------
static void alloc_hook_cdecl alloc_hook_process_done(void);

static bool os_preloading = true;    // true until this module is initialized
static bool alloc_hook_redirected = false;   // true if malloc redirects to alloc_hook_malloc

// Returns true if this module has not been initialized; Don't use C runtime routines until it returns false.
bool alloc_hook_decl_noinline _alloc_hook_preloading(void) {
  return os_preloading;
}

alloc_hook_decl_nodiscard bool alloc_hook_is_redirected(void) alloc_hook_attr_noexcept {
  return alloc_hook_redirected;
}

// Communicate with the redirection module on Windows
#if defined(_WIN32) && defined(ALLOC_HOOK_SHARED_LIB) && !defined(ALLOC_HOOK_WIN_NOREDIRECT)
#ifdef __cplusplus
extern "C" {
#endif
alloc_hook_decl_export void _alloc_hook_redirect_entry(DWORD reason) {
  // called on redirection; careful as this may be called before DllMain
  if (reason == DLL_PROCESS_ATTACH) {
    alloc_hook_redirected = true;
  }
  else if (reason == DLL_PROCESS_DETACH) {
    alloc_hook_redirected = false;
  }
  else if (reason == DLL_THREAD_DETACH) {
    alloc_hook_thread_done();
  }
}
__declspec(dllimport) bool alloc_hook_cdecl alloc_hook_allocator_init(const char** message);
__declspec(dllimport) void alloc_hook_cdecl alloc_hook_allocator_done(void);
#ifdef __cplusplus
}
#endif
#else
static bool alloc_hook_allocator_init(const char** message) {
  if (message != NULL) *message = NULL;
  return true;
}
static void alloc_hook_allocator_done(void) {
  // nothing to do
}
#endif

static int alloc_hook_stdin;
static int alloc_hook_stdout;
static int alloc_hook_stderr;

void _alloc_hook_write_stdout(const void * msg) {
  alloc_hook_prim_write(alloc_hook_stdout, msg, _alloc_hook_strlen((char *)msg));
}

void _alloc_hook_write_stderr(const void * msg) {
  alloc_hook_prim_write(alloc_hook_stderr, msg, _alloc_hook_strlen((char *)msg));
}

// Called once by the process loader
static void alloc_hook_process_load(void) {

  // we are linked into the process or loaded by the dynamic linker (hopefully)

  // dup stdin, stdout, and stderr so we can call them even if thay have been closed

  alloc_hook_stdin = alloc_hook_prim_dup(0);
  alloc_hook_stdout = alloc_hook_prim_dup(1);
  alloc_hook_stderr = alloc_hook_prim_dup(2);


  alloc_hook_heap_main_init();
  #if defined(__APPLE__) || defined(ALLOC_HOOK_TLS_RECURSE_GUARD)
  volatile alloc_hook_heap_t* dummy = _alloc_hook_heap_default; // access TLS to allocate it before setting tls_initialized to true;
  if (dummy == NULL) return;                    // use dummy or otherwise the access may get optimized away (issue #697)
  #endif
  os_preloading = false;
  alloc_hook_assert_internal(_alloc_hook_is_main_thread());
  #if !(defined(_WIN32) && defined(ALLOC_HOOK_SHARED_LIB))  // use Dll process detach (see below) instead of atexit (issue #521)
  atexit(&alloc_hook_process_done);
  #endif
  _alloc_hook_options_init();
  alloc_hook_process_setup_auto_thread_done();
  alloc_hook_process_init();
  if (alloc_hook_redirected) _alloc_hook_verbose_message("malloc is redirected.\n");

  // show message from the redirector (if present)
  const char* msg = NULL;
  alloc_hook_allocator_init(&msg);
  if (msg != NULL && (alloc_hook_option_is_enabled(alloc_hook_option_verbose) || alloc_hook_option_is_enabled(alloc_hook_option_show_errors))) {
    _alloc_hook_fputs(NULL,NULL,NULL,msg);
  }

  // reseed random
  _alloc_hook_random_reinit_if_weak(&_alloc_hook_heap_main.random);
}

#if defined(_WIN32) && (defined(_M_IX86) || defined(_M_X64))
#include <intrin.h>
alloc_hook_decl_cache_align bool _alloc_hook_cpu_has_fsrm = false;

static void alloc_hook_detect_cpu_features(void) {
  // FSRM for fast rep movsb support (AMD Zen3+ (~2020) or Intel Ice Lake+ (~2017))
  int32_t cpu_info[4];
  __cpuid(cpu_info, 7);
  _alloc_hook_cpu_has_fsrm = ((cpu_info[3] & (1 << 4)) != 0); // bit 4 of EDX : see <https://en.wikipedia.org/wiki/CPUID#EAX=7,_ECX=0:_Extended_Features>
}
#else
static void alloc_hook_detect_cpu_features(void) {
  // nothing
}
#endif

// Initialize the process; called by thread_init or the process loader
void alloc_hook_process_init(void) alloc_hook_attr_noexcept {
  // ensure we are called once
  static alloc_hook_atomic_once_t process_init;
	#if _MSC_VER < 1920
	alloc_hook_heap_main_init(); // vs2017 can dynamically re-initialize _alloc_hook_heap_main
	#endif
  if (!alloc_hook_atomic_once(&process_init)) return;
  _alloc_hook_process_is_initialized = true;
  _alloc_hook_verbose_message("process init: 0x%zx\n", _alloc_hook_thread_id());
  alloc_hook_process_setup_auto_thread_done();

  alloc_hook_detect_cpu_features();
  _alloc_hook_os_init();
  alloc_hook_heap_main_init();
  #if ALLOC_HOOK_DEBUG
  _alloc_hook_verbose_message("debug level : %d\n", ALLOC_HOOK_DEBUG);
  #endif
  _alloc_hook_verbose_message("secure level: %d\n", ALLOC_HOOK_SECURE);
  _alloc_hook_verbose_message("mem tracking: %s\n", ALLOC_HOOK_TRACK_TOOL);
  #if ALLOC_HOOK_TSAN
  _alloc_hook_verbose_message("thread santizer enabled\n");
  #endif
  alloc_hook_thread_init();

  #if defined(_WIN32)
  // On windows, when building as a static lib the FLS cleanup happens to early for the main thread.
  // To avoid this, set the FLS value for the main thread to NULL so the fls cleanup
  // will not call _alloc_hook_thread_done on the (still executing) main thread. See issue #508.
  _alloc_hook_prim_thread_associate_default_heap(NULL);
  #endif

  alloc_hook_stats_reset();  // only call stat reset *after* thread init (or the heap tld == NULL)
  alloc_hook_track_init();

  if (alloc_hook_option_is_enabled(alloc_hook_option_reserve_huge_os_pages)) {
    size_t pages = alloc_hook_option_get_clamp(alloc_hook_option_reserve_huge_os_pages, 0, 128*1024);
    long reserve_at = alloc_hook_option_get(alloc_hook_option_reserve_huge_os_pages_at);
    if (reserve_at != -1) {
      alloc_hook_reserve_huge_os_pages_at(pages, reserve_at, pages*500);
    } else {
      alloc_hook_reserve_huge_os_pages_interleave(pages, 0, pages*500);
    }
  }
  if (alloc_hook_option_is_enabled(alloc_hook_option_reserve_os_memory)) {
    long ksize = alloc_hook_option_get(alloc_hook_option_reserve_os_memory);
    if (ksize > 0) {
      alloc_hook_reserve_os_memory((size_t)ksize*ALLOC_HOOK_KiB, true /* commit? */, true /* allow large pages? */);
    }
  }
}

// Called when the process is done (through `at_exit`)
static void alloc_hook_cdecl alloc_hook_process_done(void) {
  // only shutdown if we were initialized
  if (!_alloc_hook_process_is_initialized) return;
  // ensure we are called once
  static bool process_done = false;
  if (process_done) return;
  process_done = true;

  // release any thread specific resources and ensure _alloc_hook_thread_done is called on all but the main thread
  _alloc_hook_prim_thread_done_auto_done();
  
  #ifndef ALLOC_HOOK_SKIP_COLLECT_ON_EXIT
    #if (ALLOC_HOOK_DEBUG || !defined(ALLOC_HOOK_SHARED_LIB))
    // free all memory if possible on process exit. This is not needed for a stand-alone process
    // but should be done if alloc_hook is statically linked into another shared library which
    // is repeatedly loaded/unloaded, see issue #281.
    alloc_hook_collect(true /* force */ );
    #endif
  #endif

  // Forcefully release all retained memory; this can be dangerous in general if overriding regular malloc/free
  // since after process_done there might still be other code running that calls `free` (like at_exit routines,
  // or C-runtime termination code.
  if (alloc_hook_option_is_enabled(alloc_hook_option_destroy_on_exit)) {
    alloc_hook_collect(true /* force */);
    _alloc_hook_heap_unsafe_destroy_all();     // forcefully release all memory held by all heaps (of this thread only!)
    _alloc_hook_arena_unsafe_destroy_all(& _alloc_hook_heap_main_get()->tld->stats);
  }

  if (alloc_hook_option_is_enabled(alloc_hook_option_show_stats) || alloc_hook_option_is_enabled(alloc_hook_option_verbose)) {
    alloc_hook_stats_print(NULL);
  }
  alloc_hook_allocator_done();
  _alloc_hook_verbose_message("process done: 0x%zx\n", _alloc_hook_heap_main.thread_id);

  // close our duped stdin, stdout, and stderr

  alloc_hook_prim_close(alloc_hook_stdin);
  alloc_hook_prim_close(alloc_hook_stdout);
  alloc_hook_prim_close(alloc_hook_stderr);

  os_preloading = true; // don't call the C runtime anymore
}



#if defined(_WIN32) && defined(ALLOC_HOOK_SHARED_LIB)
  // Windows DLL: easy to hook into process_init and thread_done
  __declspec(dllexport) BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    ALLOC_HOOK_UNUSED(reserved);
    ALLOC_HOOK_UNUSED(inst);
    if (reason==DLL_PROCESS_ATTACH) {
      alloc_hook_process_load();
    }
    else if (reason==DLL_PROCESS_DETACH) {
      alloc_hook_process_done();
    }
    else if (reason==DLL_THREAD_DETACH) {
      if (!alloc_hook_is_redirected()) {
        alloc_hook_thread_done();
      }
    }
    return TRUE;
  }

#elif defined(_MSC_VER)
  // MSVC: use data section magic for static libraries
  // See <https://www.codeguru.com/cpp/misc/misc/applicationcontrol/article.php/c6945/Running-Code-Before-and-After-Main.htm>
  static int _alloc_hook_process_init(void) {
    alloc_hook_process_load();
    return 0;
  }
  typedef int(*_alloc_hook_crt_callback_t)(void);
  #if defined(_M_X64) || defined(_M_ARM64)
    __pragma(comment(linker, "/include:" "_alloc_hook_msvc_initu"))
    #pragma section(".CRT$XIU", long, read)
  #else
    __pragma(comment(linker, "/include:" "__alloc_hook_msvc_initu"))
  #endif
  #pragma data_seg(".CRT$XIU")
  alloc_hook_decl_externc _alloc_hook_crt_callback_t _alloc_hook_msvc_initu[] = { &_alloc_hook_process_init };
  #pragma data_seg()

#elif defined(__cplusplus)
  // C++: use static initialization to detect process start
  static bool _alloc_hook_process_init(void) {
    alloc_hook_process_load();
    return (_alloc_hook_heap_main.thread_id != 0);
  }
  static bool alloc_hook_initialized = _alloc_hook_process_init();

#elif defined(__GNUC__) || defined(__clang__)
  // GCC,Clang: use the constructor attribute
  static void __attribute__((constructor)) _alloc_hook_process_init(void) {
    alloc_hook_process_load();
  }

#else
#pragma message("define a way to call alloc_hook_process_load on your platform")
#endif
