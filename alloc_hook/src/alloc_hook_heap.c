/*----------------------------------------------------------------------------
Copyright (c) 2018-2021, Microsoft Research, Daan Leijen
This is free software; you can redistribute it and/or modify it under the
terms of the MIT license. A copy of the license can be found in the file
"LICENSE" at the root of this distribution.
-----------------------------------------------------------------------------*/

#include "alloc_hook.h"
#include "alloc_hook_internal.h"
#include "alloc_hook_atomic.h"
#include "alloc_hook_prim.h"  // alloc_hook_prim_get_default_heap

#include <string.h>  // memset, memcpy

#if defined(_MSC_VER) && (_MSC_VER < 1920)
#pragma warning(disable:4204)  // non-constant aggregate initializer
#endif

/* -----------------------------------------------------------
  Helpers
----------------------------------------------------------- */

// return `true` if ok, `false` to break
typedef bool (heap_page_visitor_fun)(alloc_hook_heap_t* heap, alloc_hook_page_queue_t* pq, alloc_hook_page_t* page, void* arg1, void* arg2);

// Visit all pages in a heap; returns `false` if break was called.
static bool alloc_hook_heap_visit_pages(alloc_hook_heap_t* heap, heap_page_visitor_fun* fn, void* arg1, void* arg2)
{
  if (heap==NULL || heap->page_count==0) return 0;

  // visit all pages
  #if ALLOC_HOOK_DEBUG>1
  size_t total = heap->page_count;
  size_t count = 0;
  #endif  

  for (size_t i = 0; i <= ALLOC_HOOK_BIN_FULL; i++) {
    alloc_hook_page_queue_t* pq = &heap->pages[i];
    alloc_hook_page_t* page = pq->first;
    while(page != NULL) {
      alloc_hook_page_t* next = page->next; // save next in case the page gets removed from the queue
      alloc_hook_assert_internal(alloc_hook_page_heap(page) == heap);
      #if ALLOC_HOOK_DEBUG>1
      count++;
      #endif
      if (!fn(heap, pq, page, arg1, arg2)) return false;
      page = next; // and continue
    }
  }
  alloc_hook_assert_internal(count == total);
  return true;
}


#if ALLOC_HOOK_DEBUG>=2
static bool alloc_hook_heap_page_is_valid(alloc_hook_heap_t* heap, alloc_hook_page_queue_t* pq, alloc_hook_page_t* page, void* arg1, void* arg2) {
  ALLOC_HOOK_UNUSED(arg1);
  ALLOC_HOOK_UNUSED(arg2);
  ALLOC_HOOK_UNUSED(pq);
  alloc_hook_assert_internal(alloc_hook_page_heap(page) == heap);
  alloc_hook_segment_t* segment = _alloc_hook_page_segment(page);
  alloc_hook_assert_internal(segment->thread_id == heap->thread_id);
  alloc_hook_assert_expensive(_alloc_hook_page_is_valid(page));
  return true;
}
#endif
#if ALLOC_HOOK_DEBUG>=3
static bool alloc_hook_heap_is_valid(alloc_hook_heap_t* heap) {
  alloc_hook_assert_internal(heap!=NULL);
  alloc_hook_heap_visit_pages(heap, &alloc_hook_heap_page_is_valid, NULL, NULL);
  return true;
}
#endif




/* -----------------------------------------------------------
  "Collect" pages by migrating `local_free` and `thread_free`
  lists and freeing empty pages. This is done when a thread
  stops (and in that case abandons pages if there are still
  blocks alive)
----------------------------------------------------------- */

typedef enum alloc_hook_collect_e {
  ALLOC_HOOK_NORMAL,
  ALLOC_HOOK_FORCE,
  ALLOC_HOOK_ABANDON
} alloc_hook_collect_t;


static bool alloc_hook_heap_page_collect(alloc_hook_heap_t* heap, alloc_hook_page_queue_t* pq, alloc_hook_page_t* page, void* arg_collect, void* arg2 ) {
  ALLOC_HOOK_UNUSED(arg2);
  ALLOC_HOOK_UNUSED(heap);
  alloc_hook_assert_internal(alloc_hook_heap_page_is_valid(heap, pq, page, NULL, NULL));
  alloc_hook_collect_t collect = *((alloc_hook_collect_t*)arg_collect);
  _alloc_hook_page_free_collect(page, collect >= ALLOC_HOOK_FORCE);
  if (alloc_hook_page_all_free(page)) {
    // no more used blocks, free the page.
    // note: this will free retired pages as well.
    _alloc_hook_page_free(page, pq, collect >= ALLOC_HOOK_FORCE);
  }
  else if (collect == ALLOC_HOOK_ABANDON) {
    // still used blocks but the thread is done; abandon the page
    _alloc_hook_page_abandon(page, pq);
  }
  return true; // don't break
}

static bool alloc_hook_heap_page_never_delayed_free(alloc_hook_heap_t* heap, alloc_hook_page_queue_t* pq, alloc_hook_page_t* page, void* arg1, void* arg2) {
  ALLOC_HOOK_UNUSED(arg1);
  ALLOC_HOOK_UNUSED(arg2);
  ALLOC_HOOK_UNUSED(heap);
  ALLOC_HOOK_UNUSED(pq);
  _alloc_hook_page_use_delayed_free(page, ALLOC_HOOK_NEVER_DELAYED_FREE, false);
  return true; // don't break
}

static void alloc_hook_heap_collect_ex(alloc_hook_heap_t* heap, alloc_hook_collect_t collect)
{
  if (heap==NULL || !alloc_hook_heap_is_initialized(heap)) return;

  const bool force = collect >= ALLOC_HOOK_FORCE;  
  _alloc_hook_deferred_free(heap, force);

  // note: never reclaim on collect but leave it to threads that need storage to reclaim 
  const bool force_main = 
    #ifdef NDEBUG
      collect == ALLOC_HOOK_FORCE
    #else
      collect >= ALLOC_HOOK_FORCE
    #endif
      && _alloc_hook_is_main_thread() && alloc_hook_heap_is_backing(heap) && !heap->no_reclaim;

  if (force_main) {
    // the main thread is abandoned (end-of-program), try to reclaim all abandoned segments.
    // if all memory is freed by now, all segments should be freed.
    _alloc_hook_abandoned_reclaim_all(heap, &heap->tld->segments);
  }

  // if abandoning, mark all pages to no longer add to delayed_free
  if (collect == ALLOC_HOOK_ABANDON) {
    alloc_hook_heap_visit_pages(heap, &alloc_hook_heap_page_never_delayed_free, NULL, NULL);
  }

  // free all current thread delayed blocks.
  // (if abandoning, after this there are no more thread-delayed references into the pages.)
  _alloc_hook_heap_delayed_free_all(heap);

  // collect retired pages
  _alloc_hook_heap_collect_retired(heap, force);

  // collect all pages owned by this thread
  alloc_hook_heap_visit_pages(heap, &alloc_hook_heap_page_collect, &collect, NULL);
  alloc_hook_assert_internal( collect != ALLOC_HOOK_ABANDON || alloc_hook_atomic_load_ptr_acquire(alloc_hook_block_t,&heap->thread_delayed_free) == NULL );

  // collect abandoned segments (in particular, purge expired parts of segments in the abandoned segment list)
  // note: forced purge can be quite expensive if many threads are created/destroyed so we do not force on abandonment
  _alloc_hook_abandoned_collect(heap, collect == ALLOC_HOOK_FORCE /* force? */, &heap->tld->segments);

  // collect segment local caches
  if (force) {
    _alloc_hook_segment_thread_collect(&heap->tld->segments);
  }

  // collect regions on program-exit (or shared library unload)
  if (force && _alloc_hook_is_main_thread() && alloc_hook_heap_is_backing(heap)) {
    _alloc_hook_thread_data_collect();  // collect thread data cache
    _alloc_hook_arena_collect(true /* force purge */, &heap->tld->stats);
  }
}

void _alloc_hook_heap_collect_abandon(alloc_hook_heap_t* heap) {
  alloc_hook_heap_collect_ex(heap, ALLOC_HOOK_ABANDON);
}

void alloc_hook_heap_collect(alloc_hook_heap_t* heap, bool force) alloc_hook_attr_noexcept {
  alloc_hook_heap_collect_ex(heap, (force ? ALLOC_HOOK_FORCE : ALLOC_HOOK_NORMAL));
}

void alloc_hook_collect(bool force) alloc_hook_attr_noexcept {
  alloc_hook_heap_collect(alloc_hook_prim_get_default_heap(), force);
}


/* -----------------------------------------------------------
  Heap new
----------------------------------------------------------- */

alloc_hook_heap_t* alloc_hook_heap_get_default(void) {
  alloc_hook_thread_init();
  return alloc_hook_prim_get_default_heap();
}

static bool alloc_hook_heap_is_default(const alloc_hook_heap_t* heap) {
  return (heap == alloc_hook_prim_get_default_heap());
}


alloc_hook_heap_t* alloc_hook_heap_get_backing(void) {
  alloc_hook_heap_t* heap = alloc_hook_heap_get_default();
  alloc_hook_assert_internal(heap!=NULL);
  alloc_hook_heap_t* bheap = heap->tld->heap_backing;
  alloc_hook_assert_internal(bheap!=NULL);
  alloc_hook_assert_internal(bheap->thread_id == _alloc_hook_thread_id());
  return bheap;
}

alloc_hook_decl_nodiscard alloc_hook_heap_t* alloc_hook_heap_new_in_arena(alloc_hook_arena_id_t arena_id) {
  alloc_hook_heap_t* bheap = alloc_hook_heap_get_backing();
  alloc_hook_heap_t* heap = alloc_hook_heap_malloc_tp(bheap, alloc_hook_heap_t);  // todo: OS allocate in secure mode?
  if (heap == NULL) return NULL;
  _alloc_hook_memcpy_aligned(heap, &_alloc_hook_heap_empty, sizeof(alloc_hook_heap_t));
  heap->tld = bheap->tld;
  heap->thread_id = _alloc_hook_thread_id();
  heap->arena_id = arena_id;
  _alloc_hook_random_split(&bheap->random, &heap->random);
  heap->cookie = _alloc_hook_heap_random_next(heap) | 1;
  heap->keys[0] = _alloc_hook_heap_random_next(heap);
  heap->keys[1] = _alloc_hook_heap_random_next(heap);
  heap->no_reclaim = true;  // don't reclaim abandoned pages or otherwise destroy is unsafe
  // push on the thread local heaps list
  heap->next = heap->tld->heaps;
  heap->tld->heaps = heap;
  return heap;
}

alloc_hook_decl_nodiscard alloc_hook_heap_t* alloc_hook_heap_new(void) {
  return alloc_hook_heap_new_in_arena(_alloc_hook_arena_id_none());
}

bool _alloc_hook_heap_memid_is_suitable(alloc_hook_heap_t* heap, alloc_hook_memid_t memid) {
  return _alloc_hook_arena_memid_is_suitable(memid, heap->arena_id);
}

uintptr_t _alloc_hook_heap_random_next(alloc_hook_heap_t* heap) {
  return _alloc_hook_random_next(&heap->random);
}

// zero out the page queues
static void alloc_hook_heap_reset_pages(alloc_hook_heap_t* heap) {
  alloc_hook_assert_internal(heap != NULL);
  alloc_hook_assert_internal(alloc_hook_heap_is_initialized(heap));
  // TODO: copy full empty heap instead?
  memset(&heap->pages_free_direct, 0, sizeof(heap->pages_free_direct));
  _alloc_hook_memcpy_aligned(&heap->pages, &_alloc_hook_heap_empty.pages, sizeof(heap->pages));
  heap->thread_delayed_free = NULL;
  heap->page_count = 0;
}

// called from `alloc_hook_heap_destroy` and `alloc_hook_heap_delete` to free the internal heap resources.
static void alloc_hook_heap_free(alloc_hook_heap_t* heap) {
  alloc_hook_assert(heap != NULL);
  alloc_hook_assert_internal(alloc_hook_heap_is_initialized(heap));
  if (heap==NULL || !alloc_hook_heap_is_initialized(heap)) return;
  if (alloc_hook_heap_is_backing(heap)) return; // dont free the backing heap

  // reset default
  if (alloc_hook_heap_is_default(heap)) {
    _alloc_hook_heap_set_default_direct(heap->tld->heap_backing);
  }

  // remove ourselves from the thread local heaps list
  // linear search but we expect the number of heaps to be relatively small
  alloc_hook_heap_t* prev = NULL;
  alloc_hook_heap_t* curr = heap->tld->heaps;
  while (curr != heap && curr != NULL) {
    prev = curr;
    curr = curr->next;
  }
  alloc_hook_assert_internal(curr == heap);
  if (curr == heap) {
    if (prev != NULL) { prev->next = heap->next; }
                 else { heap->tld->heaps = heap->next; }
  }
  alloc_hook_assert_internal(heap->tld->heaps != NULL);

  // and free the used memory
  alloc_hook_free(heap);
}


/* -----------------------------------------------------------
  Heap destroy
----------------------------------------------------------- */

static bool _alloc_hook_heap_page_destroy(alloc_hook_heap_t* heap, alloc_hook_page_queue_t* pq, alloc_hook_page_t* page, void* arg1, void* arg2) {
  ALLOC_HOOK_UNUSED(arg1);
  ALLOC_HOOK_UNUSED(arg2);
  ALLOC_HOOK_UNUSED(heap);
  ALLOC_HOOK_UNUSED(pq);

  // ensure no more thread_delayed_free will be added
  _alloc_hook_page_use_delayed_free(page, ALLOC_HOOK_NEVER_DELAYED_FREE, false);

  // stats
  const size_t bsize = alloc_hook_page_block_size(page);
  if (bsize > ALLOC_HOOK_MEDIUM_OBJ_SIZE_MAX) {
    if (bsize <= ALLOC_HOOK_LARGE_OBJ_SIZE_MAX) {
      alloc_hook_heap_stat_decrease(heap, large, bsize);
    }
    else {
      alloc_hook_heap_stat_decrease(heap, huge, bsize);
    }
  }
#if (ALLOC_HOOK_STAT)
  _alloc_hook_page_free_collect(page, false);  // update used count
  const size_t inuse = page->used;
  if (bsize <= ALLOC_HOOK_LARGE_OBJ_SIZE_MAX) {
    alloc_hook_heap_stat_decrease(heap, normal, bsize * inuse);
#if (ALLOC_HOOK_STAT>1)
    alloc_hook_heap_stat_decrease(heap, normal_bins[_alloc_hook_bin(bsize)], inuse);
#endif
  }
  alloc_hook_heap_stat_decrease(heap, malloc, bsize * inuse);  // todo: off for aligned blocks...
#endif

  /// pretend it is all free now
  alloc_hook_assert_internal(alloc_hook_page_thread_free(page) == NULL);
  page->used = 0;

  // and free the page
  // alloc_hook_page_free(page,false);
  page->next = NULL;
  page->prev = NULL;
  _alloc_hook_segment_page_free(page,false /* no force? */, &heap->tld->segments);

  return true; // keep going
}

void _alloc_hook_heap_destroy_pages(alloc_hook_heap_t* heap) {
  alloc_hook_heap_visit_pages(heap, &_alloc_hook_heap_page_destroy, NULL, NULL);
  alloc_hook_heap_reset_pages(heap);
}

#if ALLOC_HOOK_TRACK_HEAP_DESTROY
static bool alloc_hook_cdecl alloc_hook_heap_track_block_free(const alloc_hook_heap_t* heap, const alloc_hook_heap_area_t* area, void* block, size_t block_size, void* arg) {
  ALLOC_HOOK_UNUSED(heap); ALLOC_HOOK_UNUSED(area);  ALLOC_HOOK_UNUSED(arg); ALLOC_HOOK_UNUSED(block_size);
  alloc_hook_track_free_size(block,alloc_hook_usable_size(block));
  return true;
}
#endif

void alloc_hook_heap_destroy(alloc_hook_heap_t* heap) {
  alloc_hook_assert(heap != NULL);
  alloc_hook_assert(alloc_hook_heap_is_initialized(heap));
  alloc_hook_assert(heap->no_reclaim);
  alloc_hook_assert_expensive(alloc_hook_heap_is_valid(heap));
  if (heap==NULL || !alloc_hook_heap_is_initialized(heap)) return;
  if (!heap->no_reclaim) {
    // don't free in case it may contain reclaimed pages
    alloc_hook_heap_delete(heap);
  }
  else {
    // track all blocks as freed
    #if ALLOC_HOOK_TRACK_HEAP_DESTROY
    alloc_hook_heap_visit_blocks(heap, true, alloc_hook_heap_track_block_free, NULL);
    #endif
    // free all pages
    _alloc_hook_heap_destroy_pages(heap);
    alloc_hook_heap_free(heap);
  }
}

// forcefully destroy all heaps in the current thread
void _alloc_hook_heap_unsafe_destroy_all(void) {
  alloc_hook_heap_t* bheap = alloc_hook_heap_get_backing();
  alloc_hook_heap_t* curr = bheap->tld->heaps;
  while (curr != NULL) {
    alloc_hook_heap_t* next = curr->next;
    if (curr->no_reclaim) {
      alloc_hook_heap_destroy(curr);
    }
    else {
      _alloc_hook_heap_destroy_pages(curr);
    }
    curr = next;
  }
}

/* -----------------------------------------------------------
  Safe Heap delete
----------------------------------------------------------- */

// Transfer the pages from one heap to the other
static void alloc_hook_heap_absorb(alloc_hook_heap_t* heap, alloc_hook_heap_t* from) {
  alloc_hook_assert_internal(heap!=NULL);
  if (from==NULL || from->page_count == 0) return;

  // reduce the size of the delayed frees
  _alloc_hook_heap_delayed_free_partial(from);

  // transfer all pages by appending the queues; this will set a new heap field
  // so threads may do delayed frees in either heap for a while.
  // note: appending waits for each page to not be in the `ALLOC_HOOK_DELAYED_FREEING` state
  // so after this only the new heap will get delayed frees
  for (size_t i = 0; i <= ALLOC_HOOK_BIN_FULL; i++) {
    alloc_hook_page_queue_t* pq = &heap->pages[i];
    alloc_hook_page_queue_t* append = &from->pages[i];
    size_t pcount = _alloc_hook_page_queue_append(heap, pq, append);
    heap->page_count += pcount;
    from->page_count -= pcount;
  }
  alloc_hook_assert_internal(from->page_count == 0);

  // and do outstanding delayed frees in the `from` heap
  // note: be careful here as the `heap` field in all those pages no longer point to `from`,
  // turns out to be ok as `_alloc_hook_heap_delayed_free` only visits the list and calls a
  // the regular `_alloc_hook_free_delayed_block` which is safe.
  _alloc_hook_heap_delayed_free_all(from);
  #if !defined(_MSC_VER) || (_MSC_VER > 1900) // somehow the following line gives an error in VS2015, issue #353
  alloc_hook_assert_internal(alloc_hook_atomic_load_ptr_relaxed(alloc_hook_block_t,&from->thread_delayed_free) == NULL);
  #endif

  // and reset the `from` heap
  alloc_hook_heap_reset_pages(from);
}

// Safe delete a heap without freeing any still allocated blocks in that heap.
void alloc_hook_heap_delete(alloc_hook_heap_t* heap)
{
  alloc_hook_assert(heap != NULL);
  alloc_hook_assert(alloc_hook_heap_is_initialized(heap));
  alloc_hook_assert_expensive(alloc_hook_heap_is_valid(heap));
  if (heap==NULL || !alloc_hook_heap_is_initialized(heap)) return;

  if (!alloc_hook_heap_is_backing(heap)) {
    // tranfer still used pages to the backing heap
    alloc_hook_heap_absorb(heap->tld->heap_backing, heap);
  }
  else {
    // the backing heap abandons its pages
    _alloc_hook_heap_collect_abandon(heap);
  }
  alloc_hook_assert_internal(heap->page_count==0);
  alloc_hook_heap_free(heap);
}

alloc_hook_heap_t* alloc_hook_heap_set_default(alloc_hook_heap_t* heap) {
  alloc_hook_assert(heap != NULL);
  alloc_hook_assert(alloc_hook_heap_is_initialized(heap));
  if (heap==NULL || !alloc_hook_heap_is_initialized(heap)) return NULL;
  alloc_hook_assert_expensive(alloc_hook_heap_is_valid(heap));
  alloc_hook_heap_t* old = alloc_hook_prim_get_default_heap();
  _alloc_hook_heap_set_default_direct(heap);
  return old;
}




/* -----------------------------------------------------------
  Analysis
----------------------------------------------------------- */

// static since it is not thread safe to access heaps from other threads.
static alloc_hook_heap_t* alloc_hook_heap_of_block(const void* p) {
  if (p == NULL) return NULL;
  alloc_hook_segment_t* segment = _alloc_hook_ptr_segment(p);
  bool valid = (_alloc_hook_ptr_cookie(segment) == segment->cookie);
  alloc_hook_assert_internal(valid);
  if alloc_hook_unlikely(!valid) return NULL;
  return alloc_hook_page_heap(_alloc_hook_segment_page_of(segment,p));
}

bool alloc_hook_heap_contains_block(alloc_hook_heap_t* heap, const void* p) {
  alloc_hook_assert(heap != NULL);
  if (heap==NULL || !alloc_hook_heap_is_initialized(heap)) return false;
  return (heap == alloc_hook_heap_of_block(p));
}


static bool alloc_hook_heap_page_check_owned(alloc_hook_heap_t* heap, alloc_hook_page_queue_t* pq, alloc_hook_page_t* page, void* p, void* vfound) {
  ALLOC_HOOK_UNUSED(heap);
  ALLOC_HOOK_UNUSED(pq);
  bool* found = (bool*)vfound;
  alloc_hook_segment_t* segment = _alloc_hook_page_segment(page);
  void* start = _alloc_hook_page_start(segment, page, NULL);
  void* end   = (uint8_t*)start + (page->capacity * alloc_hook_page_block_size(page));
  *found = (p >= start && p < end);
  return (!*found); // continue if not found
}

bool alloc_hook_heap_check_owned(alloc_hook_heap_t* heap, const void* p) {
  alloc_hook_assert(heap != NULL);
  if (heap==NULL || !alloc_hook_heap_is_initialized(heap)) return false;
  if (((uintptr_t)p & (ALLOC_HOOK_INTPTR_SIZE - 1)) != 0) return false;  // only aligned pointers
  bool found = false;
  alloc_hook_heap_visit_pages(heap, &alloc_hook_heap_page_check_owned, (void*)p, &found);
  return found;
}

bool alloc_hook_check_owned(const void* p) {
  return alloc_hook_heap_check_owned(alloc_hook_prim_get_default_heap(), p);
}

/* -----------------------------------------------------------
  Visit all heap blocks and areas
  Todo: enable visiting abandoned pages, and
        enable visiting all blocks of all heaps across threads
----------------------------------------------------------- */

// Separate struct to keep `alloc_hook_page_t` out of the public interface
typedef struct alloc_hook_heap_area_ex_s {
  alloc_hook_heap_area_t area;
  alloc_hook_page_t*     page;
} alloc_hook_heap_area_ex_t;

static bool alloc_hook_heap_area_visit_blocks(const alloc_hook_heap_area_ex_t* xarea, alloc_hook_block_visit_fun* visitor, void* arg) {
  alloc_hook_assert(xarea != NULL);
  if (xarea==NULL) return true;
  const alloc_hook_heap_area_t* area = &xarea->area;
  alloc_hook_page_t* page = xarea->page;
  alloc_hook_assert(page != NULL);
  if (page == NULL) return true;

  _alloc_hook_page_free_collect(page,true);
  alloc_hook_assert_internal(page->local_free == NULL);
  if (page->used == 0) return true;

  const size_t bsize = alloc_hook_page_block_size(page);
  const size_t ubsize = alloc_hook_page_usable_block_size(page); // without padding
  size_t   psize;
  uint8_t* pstart = _alloc_hook_page_start(_alloc_hook_page_segment(page), page, &psize);

  if (page->capacity == 1) {
    // optimize page with one block
    alloc_hook_assert_internal(page->used == 1 && page->free == NULL);
    return visitor(alloc_hook_page_heap(page), area, pstart, ubsize, arg);
  }

  // create a bitmap of free blocks.
  #define ALLOC_HOOK_MAX_BLOCKS   (ALLOC_HOOK_SMALL_PAGE_SIZE / sizeof(void*))
  uintptr_t free_map[ALLOC_HOOK_MAX_BLOCKS / sizeof(uintptr_t)];
  memset(free_map, 0, sizeof(free_map));

  #if ALLOC_HOOK_DEBUG>1
  size_t free_count = 0;
  #endif
  for (alloc_hook_block_t* block = page->free; block != NULL; block = alloc_hook_block_next(page,block)) {
    #if ALLOC_HOOK_DEBUG>1
    free_count++;
    #endif
    alloc_hook_assert_internal((uint8_t*)block >= pstart && (uint8_t*)block < (pstart + psize));
    size_t offset = (uint8_t*)block - pstart;
    alloc_hook_assert_internal(offset % bsize == 0);
    size_t blockidx = offset / bsize;  // Todo: avoid division?
    alloc_hook_assert_internal( blockidx < ALLOC_HOOK_MAX_BLOCKS);
    size_t bitidx = (blockidx / sizeof(uintptr_t));
    size_t bit = blockidx - (bitidx * sizeof(uintptr_t));
    free_map[bitidx] |= ((uintptr_t)1 << bit);
  }
  alloc_hook_assert_internal(page->capacity == (free_count + page->used));

  // walk through all blocks skipping the free ones
  #if ALLOC_HOOK_DEBUG>1
  size_t used_count = 0;
  #endif
  for (size_t i = 0; i < page->capacity; i++) {
    size_t bitidx = (i / sizeof(uintptr_t));
    size_t bit = i - (bitidx * sizeof(uintptr_t));
    uintptr_t m = free_map[bitidx];
    if (bit == 0 && m == UINTPTR_MAX) {
      i += (sizeof(uintptr_t) - 1); // skip a run of free blocks
    }
    else if ((m & ((uintptr_t)1 << bit)) == 0) {
      #if ALLOC_HOOK_DEBUG>1
      used_count++;
      #endif
      uint8_t* block = pstart + (i * bsize);
      if (!visitor(alloc_hook_page_heap(page), area, block, ubsize, arg)) return false;
    }
  }
  alloc_hook_assert_internal(page->used == used_count);
  return true;
}

typedef bool (alloc_hook_heap_area_visit_fun)(const alloc_hook_heap_t* heap, const alloc_hook_heap_area_ex_t* area, void* arg);


static bool alloc_hook_heap_visit_areas_page(alloc_hook_heap_t* heap, alloc_hook_page_queue_t* pq, alloc_hook_page_t* page, void* vfun, void* arg) {
  ALLOC_HOOK_UNUSED(heap);
  ALLOC_HOOK_UNUSED(pq);
  alloc_hook_heap_area_visit_fun* fun = (alloc_hook_heap_area_visit_fun*)vfun;
  alloc_hook_heap_area_ex_t xarea;
  const size_t bsize = alloc_hook_page_block_size(page);
  const size_t ubsize = alloc_hook_page_usable_block_size(page);
  xarea.page = page;
  xarea.area.reserved = page->reserved * bsize;
  xarea.area.committed = page->capacity * bsize;
  xarea.area.blocks = _alloc_hook_page_start(_alloc_hook_page_segment(page), page, NULL);
  xarea.area.used = page->used;   // number of blocks in use (#553)
  xarea.area.block_size = ubsize;
  xarea.area.full_block_size = bsize;
  return fun(heap, &xarea, arg);
}

// Visit all heap pages as areas
static bool alloc_hook_heap_visit_areas(const alloc_hook_heap_t* heap, alloc_hook_heap_area_visit_fun* visitor, void* arg) {
  if (visitor == NULL) return false;
  return alloc_hook_heap_visit_pages((alloc_hook_heap_t*)heap, &alloc_hook_heap_visit_areas_page, (void*)(visitor), arg); // note: function pointer to void* :-{
}

// Just to pass arguments
typedef struct alloc_hook_visit_blocks_args_s {
  bool  visit_blocks;
  alloc_hook_block_visit_fun* visitor;
  void* arg;
} alloc_hook_visit_blocks_args_t;

static bool alloc_hook_heap_area_visitor(const alloc_hook_heap_t* heap, const alloc_hook_heap_area_ex_t* xarea, void* arg) {
  alloc_hook_visit_blocks_args_t* args = (alloc_hook_visit_blocks_args_t*)arg;
  if (!args->visitor(heap, &xarea->area, NULL, xarea->area.block_size, args->arg)) return false;
  if (args->visit_blocks) {
    return alloc_hook_heap_area_visit_blocks(xarea, args->visitor, args->arg);
  }
  else {
    return true;
  }
}

// Visit all blocks in a heap
bool alloc_hook_heap_visit_blocks(const alloc_hook_heap_t* heap, bool visit_blocks, alloc_hook_block_visit_fun* visitor, void* arg) {
  alloc_hook_visit_blocks_args_t args = { visit_blocks, visitor, arg };
  return alloc_hook_heap_visit_areas(heap, &alloc_hook_heap_area_visitor, &args);
}
