/*----------------------------------------------------------------------------
Copyright (c) 2018-2020, Microsoft Research, Daan Leijen
This is free software; you can redistribute it and/or modify it under the
terms of the MIT license. A copy of the license can be found in the file
"LICENSE" at the root of this distribution.
-----------------------------------------------------------------------------*/

/* -----------------------------------------------------------
  The core of the allocator. Every segment contains
  pages of a certain block size. The main function
  exported is `alloc_hook_malloc_generic`.
----------------------------------------------------------- */

#include "alloc_hook.h"
#include "alloc_hook_internal.h"
#include "alloc_hook_atomic.h"

/* -----------------------------------------------------------
  Definition of page queues for each block size
----------------------------------------------------------- */

#define ALLOC_HOOK_IN_PAGE_C
#include "alloc_hook_page_queue.c"
#undef ALLOC_HOOK_IN_PAGE_C


/* -----------------------------------------------------------
  Page helpers
----------------------------------------------------------- */

// Index a block in a page
static inline alloc_hook_block_t* alloc_hook_page_block_at(const alloc_hook_page_t* page, void* page_start, size_t block_size, size_t i) {
  ALLOC_HOOK_UNUSED(page);
  alloc_hook_assert_internal(page != NULL);
  alloc_hook_assert_internal(i <= page->reserved);
  return (alloc_hook_block_t*)((uint8_t*)page_start + (i * block_size));
}

static void alloc_hook_page_init(alloc_hook_heap_t* heap, alloc_hook_page_t* page, size_t size, alloc_hook_tld_t* tld);
static void alloc_hook_page_extend_free(alloc_hook_heap_t* heap, alloc_hook_page_t* page, alloc_hook_tld_t* tld);

#if (ALLOC_HOOK_DEBUG>=3)
static size_t alloc_hook_page_list_count(alloc_hook_page_t* page, alloc_hook_block_t* head) {
  size_t count = 0;
  while (head != NULL) {
    alloc_hook_assert_internal(page == _alloc_hook_ptr_page(head));
    count++;
    head = alloc_hook_block_next(page, head);
  }
  return count;
}

/*
// Start of the page available memory
static inline uint8_t* alloc_hook_page_area(const alloc_hook_page_t* page) {
  return _alloc_hook_page_start(_alloc_hook_page_segment(page), page, NULL);
}
*/

static bool alloc_hook_page_list_is_valid(alloc_hook_page_t* page, alloc_hook_block_t* p) {
  size_t psize;
  uint8_t* page_area = _alloc_hook_page_start(_alloc_hook_page_segment(page), page, &psize);
  alloc_hook_block_t* start = (alloc_hook_block_t*)page_area;
  alloc_hook_block_t* end   = (alloc_hook_block_t*)(page_area + psize);
  while(p != NULL) {
    if (p < start || p >= end) return false;
    p = alloc_hook_block_next(page, p);
  }
#if ALLOC_HOOK_DEBUG>3 // generally too expensive to check this
  if (page->free_is_zero) {
    const size_t ubsize = alloc_hook_page_usable_block_size(page);
    for (alloc_hook_block_t* block = page->free; block != NULL; block = alloc_hook_block_next(page, block)) {
      alloc_hook_assert_expensive(alloc_hook_mem_is_zero(block + 1, ubsize - sizeof(alloc_hook_block_t)));
    }
  }
#endif
  return true;
}

static bool alloc_hook_page_is_valid_init(alloc_hook_page_t* page) {
  alloc_hook_assert_internal(page->xblock_size > 0);
  alloc_hook_assert_internal(page->used <= page->capacity);
  alloc_hook_assert_internal(page->capacity <= page->reserved);

  alloc_hook_segment_t* segment = _alloc_hook_page_segment(page);
  uint8_t* start = _alloc_hook_page_start(segment,page,NULL);
  alloc_hook_assert_internal(start == _alloc_hook_segment_page_start(segment,page,NULL));
  //const size_t bsize = alloc_hook_page_block_size(page);
  //alloc_hook_assert_internal(start + page->capacity*page->block_size == page->top);

  alloc_hook_assert_internal(alloc_hook_page_list_is_valid(page,page->free));
  alloc_hook_assert_internal(alloc_hook_page_list_is_valid(page,page->local_free));

  #if ALLOC_HOOK_DEBUG>3 // generally too expensive to check this
  if (page->free_is_zero) {
    const size_t ubsize = alloc_hook_page_usable_block_size(page);
    for(alloc_hook_block_t* block = page->free; block != NULL; block = alloc_hook_block_next(page,block)) {
      alloc_hook_assert_expensive(alloc_hook_mem_is_zero(block + 1, ubsize - sizeof(alloc_hook_block_t)));
    }
  }
  #endif

  #if !ALLOC_HOOK_TRACK_ENABLED && !ALLOC_HOOK_TSAN
  alloc_hook_block_t* tfree = alloc_hook_page_thread_free(page);
  alloc_hook_assert_internal(alloc_hook_page_list_is_valid(page, tfree));
  //size_t tfree_count = alloc_hook_page_list_count(page, tfree);
  //alloc_hook_assert_internal(tfree_count <= page->thread_freed + 1);
  #endif

  size_t free_count = alloc_hook_page_list_count(page, page->free) + alloc_hook_page_list_count(page, page->local_free);
  alloc_hook_assert_internal(page->used + free_count == page->capacity);

  return true;
}

extern bool _alloc_hook_process_is_initialized;             // has alloc_hook_process_init been called?

bool _alloc_hook_page_is_valid(alloc_hook_page_t* page) {
  alloc_hook_assert_internal(alloc_hook_page_is_valid_init(page));
  #if ALLOC_HOOK_SECURE
  alloc_hook_assert_internal(page->keys[0] != 0);
  #endif
  if (alloc_hook_page_heap(page)!=NULL) {
    alloc_hook_segment_t* segment = _alloc_hook_page_segment(page);

    alloc_hook_assert_internal(!_alloc_hook_process_is_initialized || segment->thread_id==0 || segment->thread_id == alloc_hook_page_heap(page)->thread_id);
    #if ALLOC_HOOK_HUGE_PAGE_ABANDON
    if (segment->kind != ALLOC_HOOK_SEGMENT_HUGE) 
    #endif
    {    
      alloc_hook_page_queue_t* pq = alloc_hook_page_queue_of(page);
      alloc_hook_assert_internal(alloc_hook_page_queue_contains(pq, page));
      alloc_hook_assert_internal(pq->block_size==alloc_hook_page_block_size(page) || alloc_hook_page_block_size(page) > ALLOC_HOOK_MEDIUM_OBJ_SIZE_MAX || alloc_hook_page_is_in_full(page));
      alloc_hook_assert_internal(alloc_hook_heap_contains_queue(alloc_hook_page_heap(page),pq));
    }
  }
  return true;
}
#endif

void _alloc_hook_page_use_delayed_free(alloc_hook_page_t* page, alloc_hook_delayed_t delay, bool override_never) {
  while (!_alloc_hook_page_try_use_delayed_free(page, delay, override_never)) {
    alloc_hook_atomic_yield();
  }
}

bool _alloc_hook_page_try_use_delayed_free(alloc_hook_page_t* page, alloc_hook_delayed_t delay, bool override_never) {
  alloc_hook_thread_free_t tfreex;
  alloc_hook_delayed_t     old_delay;
  alloc_hook_thread_free_t tfree;
  size_t yield_count = 0;
  do {
    tfree = alloc_hook_atomic_load_acquire(&page->xthread_free); // note: must acquire as we can break/repeat this loop and not do a CAS;
    tfreex = alloc_hook_tf_set_delayed(tfree, delay);
    old_delay = alloc_hook_tf_delayed(tfree);
    if alloc_hook_unlikely(old_delay == ALLOC_HOOK_DELAYED_FREEING) {
      if (yield_count >= 4) return false;  // give up after 4 tries
      yield_count++;
      alloc_hook_atomic_yield(); // delay until outstanding ALLOC_HOOK_DELAYED_FREEING are done.
      // tfree = alloc_hook_tf_set_delayed(tfree, ALLOC_HOOK_NO_DELAYED_FREE); // will cause CAS to busy fail
    }
    else if (delay == old_delay) {
      break; // avoid atomic operation if already equal
    }
    else if (!override_never && old_delay == ALLOC_HOOK_NEVER_DELAYED_FREE) {
      break; // leave never-delayed flag set
    }
  } while ((old_delay == ALLOC_HOOK_DELAYED_FREEING) ||
           !alloc_hook_atomic_cas_weak_release(&page->xthread_free, &tfree, tfreex));

  return true; // success
}

/* -----------------------------------------------------------
  Page collect the `local_free` and `thread_free` lists
----------------------------------------------------------- */

// Collect the local `thread_free` list using an atomic exchange.
// Note: The exchange must be done atomically as this is used right after
// moving to the full list in `alloc_hook_page_collect_ex` and we need to
// ensure that there was no race where the page became unfull just before the move.
static void _alloc_hook_page_thread_free_collect(alloc_hook_page_t* page)
{
  alloc_hook_block_t* head;
  alloc_hook_thread_free_t tfreex;
  alloc_hook_thread_free_t tfree = alloc_hook_atomic_load_relaxed(&page->xthread_free);
  do {
    head = alloc_hook_tf_block(tfree);
    tfreex = alloc_hook_tf_set_block(tfree,NULL);
  } while (!alloc_hook_atomic_cas_weak_acq_rel(&page->xthread_free, &tfree, tfreex));

  // return if the list is empty
  if (head == NULL) return;

  // find the tail -- also to get a proper count (without data races)
  uint32_t max_count = page->capacity; // cannot collect more than capacity
  uint32_t count = 1;
  alloc_hook_block_t* tail = head;
  alloc_hook_block_t* next;
  while ((next = alloc_hook_block_next(page,tail)) != NULL && count <= max_count) {
    count++;
    tail = next;
  }
  // if `count > max_count` there was a memory corruption (possibly infinite list due to double multi-threaded free)
  if (count > max_count) {
    _alloc_hook_error_message(EFAULT, "corrupted thread-free list\n");
    return; // the thread-free items cannot be freed
  }

  // and append the current local free list
  alloc_hook_block_set_next(page,tail, page->local_free);
  page->local_free = head;

  // update counts now
  page->used -= count;
}

void _alloc_hook_page_free_collect(alloc_hook_page_t* page, bool force) {
  alloc_hook_assert_internal(page!=NULL);

  // collect the thread free list
  if (force || alloc_hook_page_thread_free(page) != NULL) {  // quick test to avoid an atomic operation
    _alloc_hook_page_thread_free_collect(page);
  }

  // and the local free list
  if (page->local_free != NULL) {
    if alloc_hook_likely(page->free == NULL) {
      // usual case
      page->free = page->local_free;
      page->local_free = NULL;
      page->free_is_zero = false;
    }
    else if (force) {
      // append -- only on shutdown (force) as this is a linear operation
      alloc_hook_block_t* tail = page->local_free;
      alloc_hook_block_t* next;
      while ((next = alloc_hook_block_next(page, tail)) != NULL) {
        tail = next;
      }
      alloc_hook_block_set_next(page, tail, page->free);
      page->free = page->local_free;
      page->local_free = NULL;
      page->free_is_zero = false;
    }
  }

  alloc_hook_assert_internal(!force || page->local_free == NULL);
}



/* -----------------------------------------------------------
  Page fresh and retire
----------------------------------------------------------- */

// called from segments when reclaiming abandoned pages
void _alloc_hook_page_reclaim(alloc_hook_heap_t* heap, alloc_hook_page_t* page) {
  alloc_hook_assert_expensive(alloc_hook_page_is_valid_init(page));

  alloc_hook_assert_internal(alloc_hook_page_heap(page) == heap);
  alloc_hook_assert_internal(alloc_hook_page_thread_free_flag(page) != ALLOC_HOOK_NEVER_DELAYED_FREE);
  #if ALLOC_HOOK_HUGE_PAGE_ABANDON
  alloc_hook_assert_internal(_alloc_hook_page_segment(page)->kind != ALLOC_HOOK_SEGMENT_HUGE);
  #endif
  
  // TODO: push on full queue immediately if it is full?
  alloc_hook_page_queue_t* pq = alloc_hook_page_queue(heap, alloc_hook_page_block_size(page));
  alloc_hook_page_queue_push(heap, pq, page);
  alloc_hook_assert_expensive(_alloc_hook_page_is_valid(page));
}

// allocate a fresh page from a segment
static alloc_hook_page_t* alloc_hook_page_fresh_alloc(alloc_hook_heap_t* heap, alloc_hook_page_queue_t* pq, size_t block_size, size_t page_alignment) {
  #if !ALLOC_HOOK_HUGE_PAGE_ABANDON
  alloc_hook_assert_internal(pq != NULL);
  alloc_hook_assert_internal(alloc_hook_heap_contains_queue(heap, pq));
  alloc_hook_assert_internal(page_alignment > 0 || block_size > ALLOC_HOOK_MEDIUM_OBJ_SIZE_MAX || block_size == pq->block_size);
  #endif
  alloc_hook_page_t* page = _alloc_hook_segment_page_alloc(heap, block_size, page_alignment, &heap->tld->segments, &heap->tld->os);
  if (page == NULL) {
    // this may be out-of-memory, or an abandoned page was reclaimed (and in our queue)
    return NULL;
  }
  alloc_hook_assert_internal(page_alignment >0 || block_size > ALLOC_HOOK_MEDIUM_OBJ_SIZE_MAX || _alloc_hook_page_segment(page)->kind != ALLOC_HOOK_SEGMENT_HUGE);
  alloc_hook_assert_internal(pq!=NULL || page->xblock_size != 0);
  alloc_hook_assert_internal(pq!=NULL || alloc_hook_page_block_size(page) >= block_size);
  // a fresh page was found, initialize it
  const size_t full_block_size = ((pq == NULL || alloc_hook_page_queue_is_huge(pq)) ? alloc_hook_page_block_size(page) : block_size); // see also: alloc_hook_segment_huge_page_alloc
  alloc_hook_assert_internal(full_block_size >= block_size);
  alloc_hook_page_init(heap, page, full_block_size, heap->tld);
  alloc_hook_heap_stat_increase(heap, pages, 1);
  if (pq != NULL) { alloc_hook_page_queue_push(heap, pq, page); }
  alloc_hook_assert_expensive(_alloc_hook_page_is_valid(page));
  return page;
}

// Get a fresh page to use
static alloc_hook_page_t* alloc_hook_page_fresh(alloc_hook_heap_t* heap, alloc_hook_page_queue_t* pq) {
  alloc_hook_assert_internal(alloc_hook_heap_contains_queue(heap, pq));
  alloc_hook_page_t* page = alloc_hook_page_fresh_alloc(heap, pq, pq->block_size, 0);
  if (page==NULL) return NULL;
  alloc_hook_assert_internal(pq->block_size==alloc_hook_page_block_size(page));
  alloc_hook_assert_internal(pq==alloc_hook_page_queue(heap, alloc_hook_page_block_size(page)));
  return page;
}

/* -----------------------------------------------------------
   Do any delayed frees
   (put there by other threads if they deallocated in a full page)
----------------------------------------------------------- */
void _alloc_hook_heap_delayed_free_all(alloc_hook_heap_t* heap) {
  while (!_alloc_hook_heap_delayed_free_partial(heap)) {
    alloc_hook_atomic_yield();
  }
}

// returns true if all delayed frees were processed
bool _alloc_hook_heap_delayed_free_partial(alloc_hook_heap_t* heap) {
  // take over the list (note: no atomic exchange since it is often NULL)
  alloc_hook_block_t* block = alloc_hook_atomic_load_ptr_relaxed(alloc_hook_block_t, &heap->thread_delayed_free);
  while (block != NULL && !alloc_hook_atomic_cas_ptr_weak_acq_rel(alloc_hook_block_t, &heap->thread_delayed_free, &block, NULL)) { /* nothing */ };
  bool all_freed = true;

  // and free them all
  while(block != NULL) {
    alloc_hook_block_t* next = alloc_hook_block_nextx(heap,block, heap->keys);
    // use internal free instead of regular one to keep stats etc correct
    if (!_alloc_hook_free_delayed_block(block)) {
      // we might already start delayed freeing while another thread has not yet
      // reset the delayed_freeing flag; in that case delay it further by reinserting the current block
      // into the delayed free list
      all_freed = false;
      alloc_hook_block_t* dfree = alloc_hook_atomic_load_ptr_relaxed(alloc_hook_block_t, &heap->thread_delayed_free);
      do {
        alloc_hook_block_set_nextx(heap, block, dfree, heap->keys);
      } while (!alloc_hook_atomic_cas_ptr_weak_release(alloc_hook_block_t,&heap->thread_delayed_free, &dfree, block));
    }
    block = next;
  }
  return all_freed;
}

/* -----------------------------------------------------------
  Unfull, abandon, free and retire
----------------------------------------------------------- */

// Move a page from the full list back to a regular list
void _alloc_hook_page_unfull(alloc_hook_page_t* page) {
  alloc_hook_assert_internal(page != NULL);
  alloc_hook_assert_expensive(_alloc_hook_page_is_valid(page));
  alloc_hook_assert_internal(alloc_hook_page_is_in_full(page));
  if (!alloc_hook_page_is_in_full(page)) return;

  alloc_hook_heap_t* heap = alloc_hook_page_heap(page);
  alloc_hook_page_queue_t* pqfull = &heap->pages[ALLOC_HOOK_BIN_FULL];
  alloc_hook_page_set_in_full(page, false); // to get the right queue
  alloc_hook_page_queue_t* pq = alloc_hook_heap_page_queue_of(heap, page);
  alloc_hook_page_set_in_full(page, true);
  alloc_hook_page_queue_enqueue_from(pq, pqfull, page);
}

static void alloc_hook_page_to_full(alloc_hook_page_t* page, alloc_hook_page_queue_t* pq) {
  alloc_hook_assert_internal(pq == alloc_hook_page_queue_of(page));
  alloc_hook_assert_internal(!alloc_hook_page_immediate_available(page));
  alloc_hook_assert_internal(!alloc_hook_page_is_in_full(page));

  if (alloc_hook_page_is_in_full(page)) return;
  alloc_hook_page_queue_enqueue_from(&alloc_hook_page_heap(page)->pages[ALLOC_HOOK_BIN_FULL], pq, page);
  _alloc_hook_page_free_collect(page,false);  // try to collect right away in case another thread freed just before ALLOC_HOOK_USE_DELAYED_FREE was set
}


// Abandon a page with used blocks at the end of a thread.
// Note: only call if it is ensured that no references exist from
// the `page->heap->thread_delayed_free` into this page.
// Currently only called through `alloc_hook_heap_collect_ex` which ensures this.
void _alloc_hook_page_abandon(alloc_hook_page_t* page, alloc_hook_page_queue_t* pq) {
  alloc_hook_assert_internal(page != NULL);
  alloc_hook_assert_expensive(_alloc_hook_page_is_valid(page));
  alloc_hook_assert_internal(pq == alloc_hook_page_queue_of(page));
  alloc_hook_assert_internal(alloc_hook_page_heap(page) != NULL);

  alloc_hook_heap_t* pheap = alloc_hook_page_heap(page);

  // remove from our page list
  alloc_hook_segments_tld_t* segments_tld = &pheap->tld->segments;
  alloc_hook_page_queue_remove(pq, page);

  // page is no longer associated with our heap
  alloc_hook_assert_internal(alloc_hook_page_thread_free_flag(page)==ALLOC_HOOK_NEVER_DELAYED_FREE);
  alloc_hook_page_set_heap(page, NULL);

#if (ALLOC_HOOK_DEBUG>1) && !ALLOC_HOOK_TRACK_ENABLED
  // check there are no references left..
  for (alloc_hook_block_t* block = (alloc_hook_block_t*)pheap->thread_delayed_free; block != NULL; block = alloc_hook_block_nextx(pheap, block, pheap->keys)) {
    alloc_hook_assert_internal(_alloc_hook_ptr_page(block) != page);
  }
#endif

  // and abandon it
  alloc_hook_assert_internal(alloc_hook_page_heap(page) == NULL);
  _alloc_hook_segment_page_abandon(page,segments_tld);
}


// Free a page with no more free blocks
void _alloc_hook_page_free(alloc_hook_page_t* page, alloc_hook_page_queue_t* pq, bool force) {
  alloc_hook_assert_internal(page != NULL);
  alloc_hook_assert_expensive(_alloc_hook_page_is_valid(page));
  alloc_hook_assert_internal(pq == alloc_hook_page_queue_of(page));
  alloc_hook_assert_internal(alloc_hook_page_all_free(page));
  alloc_hook_assert_internal(alloc_hook_page_thread_free_flag(page)!=ALLOC_HOOK_DELAYED_FREEING);

  // no more aligned blocks in here
  alloc_hook_page_set_has_aligned(page, false);

  alloc_hook_heap_t* heap = alloc_hook_page_heap(page);

  // remove from the page list
  // (no need to do _alloc_hook_heap_delayed_free first as all blocks are already free)
  alloc_hook_segments_tld_t* segments_tld = &heap->tld->segments;
  alloc_hook_page_queue_remove(pq, page);

  // and free it
  alloc_hook_page_set_heap(page,NULL);
  _alloc_hook_segment_page_free(page, force, segments_tld);
}

// Retire parameters
#define ALLOC_HOOK_MAX_RETIRE_SIZE    (ALLOC_HOOK_MEDIUM_OBJ_SIZE_MAX)
#define ALLOC_HOOK_RETIRE_CYCLES      (16)

// Retire a page with no more used blocks
// Important to not retire too quickly though as new
// allocations might coming.
// Note: called from `alloc_hook_free` and benchmarks often
// trigger this due to freeing everything and then
// allocating again so careful when changing this.
void _alloc_hook_page_retire(alloc_hook_page_t* page) alloc_hook_attr_noexcept {
  alloc_hook_assert_internal(page != NULL);
  alloc_hook_assert_expensive(_alloc_hook_page_is_valid(page));
  alloc_hook_assert_internal(alloc_hook_page_all_free(page));
  
  alloc_hook_page_set_has_aligned(page, false);

  // don't retire too often..
  // (or we end up retiring and re-allocating most of the time)
  // NOTE: refine this more: we should not retire if this
  // is the only page left with free blocks. It is not clear
  // how to check this efficiently though...
  // for now, we don't retire if it is the only page left of this size class.
  alloc_hook_page_queue_t* pq = alloc_hook_page_queue_of(page);
  if alloc_hook_likely(page->xblock_size <= ALLOC_HOOK_MAX_RETIRE_SIZE && !alloc_hook_page_queue_is_special(pq)) {  // not too large && not full or huge queue?
    if (pq->last==page && pq->first==page) { // the only page in the queue?
      alloc_hook_stat_counter_increase(_alloc_hook_stats_main.page_no_retire,1);
      page->retire_expire = 1 + (page->xblock_size <= ALLOC_HOOK_SMALL_OBJ_SIZE_MAX ? ALLOC_HOOK_RETIRE_CYCLES : ALLOC_HOOK_RETIRE_CYCLES/4);      
      alloc_hook_heap_t* heap = alloc_hook_page_heap(page);
      alloc_hook_assert_internal(pq >= heap->pages);
      const size_t index = pq - heap->pages;
      alloc_hook_assert_internal(index < ALLOC_HOOK_BIN_FULL && index < ALLOC_HOOK_BIN_HUGE);
      if (index < heap->page_retired_min) heap->page_retired_min = index;
      if (index > heap->page_retired_max) heap->page_retired_max = index;
      alloc_hook_assert_internal(alloc_hook_page_all_free(page));
      return; // dont't free after all
    }
  }
  _alloc_hook_page_free(page, pq, false);
}

// free retired pages: we don't need to look at the entire queues
// since we only retire pages that are at the head position in a queue.
void _alloc_hook_heap_collect_retired(alloc_hook_heap_t* heap, bool force) {
  size_t min = ALLOC_HOOK_BIN_FULL;
  size_t max = 0;
  for(size_t bin = heap->page_retired_min; bin <= heap->page_retired_max; bin++) {
    alloc_hook_page_queue_t* pq   = &heap->pages[bin];
    alloc_hook_page_t*       page = pq->first;
    if (page != NULL && page->retire_expire != 0) {
      if (alloc_hook_page_all_free(page)) {
        page->retire_expire--;
        if (force || page->retire_expire == 0) {
          _alloc_hook_page_free(pq->first, pq, force);
        }
        else {
          // keep retired, update min/max
          if (bin < min) min = bin;
          if (bin > max) max = bin;
        }
      }
      else {
        page->retire_expire = 0;
      }
    }
  }
  heap->page_retired_min = min;
  heap->page_retired_max = max;
}


/* -----------------------------------------------------------
  Initialize the initial free list in a page.
  In secure mode we initialize a randomized list by
  alternating between slices.
----------------------------------------------------------- */

#define ALLOC_HOOK_MAX_SLICE_SHIFT  (6)   // at most 64 slices
#define ALLOC_HOOK_MAX_SLICES       (1UL << ALLOC_HOOK_MAX_SLICE_SHIFT)
#define ALLOC_HOOK_MIN_SLICES       (2)

static void alloc_hook_page_free_list_extend_secure(alloc_hook_heap_t* const heap, alloc_hook_page_t* const page, const size_t bsize, const size_t extend, alloc_hook_stats_t* const stats) {
  ALLOC_HOOK_UNUSED(stats);
  #if (ALLOC_HOOK_SECURE<=2)
  alloc_hook_assert_internal(page->free == NULL);
  alloc_hook_assert_internal(page->local_free == NULL);
  #endif
  alloc_hook_assert_internal(page->capacity + extend <= page->reserved);
  alloc_hook_assert_internal(bsize == alloc_hook_page_block_size(page));
  void* const page_area = _alloc_hook_page_start(_alloc_hook_page_segment(page), page, NULL);

  // initialize a randomized free list
  // set up `slice_count` slices to alternate between
  size_t shift = ALLOC_HOOK_MAX_SLICE_SHIFT;
  while ((extend >> shift) == 0) {
    shift--;
  }
  const size_t slice_count = (size_t)1U << shift;
  const size_t slice_extend = extend / slice_count;
  alloc_hook_assert_internal(slice_extend >= 1);
  alloc_hook_block_t* blocks[ALLOC_HOOK_MAX_SLICES];   // current start of the slice
  size_t      counts[ALLOC_HOOK_MAX_SLICES];   // available objects in the slice
  for (size_t i = 0; i < slice_count; i++) {
    blocks[i] = alloc_hook_page_block_at(page, page_area, bsize, page->capacity + i*slice_extend);
    counts[i] = slice_extend;
  }
  counts[slice_count-1] += (extend % slice_count);  // final slice holds the modulus too (todo: distribute evenly?)

  // and initialize the free list by randomly threading through them
  // set up first element
  const uintptr_t r = _alloc_hook_heap_random_next(heap);
  size_t current = r % slice_count;
  counts[current]--;
  alloc_hook_block_t* const free_start = blocks[current];
  // and iterate through the rest; use `random_shuffle` for performance
  uintptr_t rnd = _alloc_hook_random_shuffle(r|1); // ensure not 0
  for (size_t i = 1; i < extend; i++) {
    // call random_shuffle only every INTPTR_SIZE rounds
    const size_t round = i%ALLOC_HOOK_INTPTR_SIZE;
    if (round == 0) rnd = _alloc_hook_random_shuffle(rnd);
    // select a random next slice index
    size_t next = ((rnd >> 8*round) & (slice_count-1));
    while (counts[next]==0) {                            // ensure it still has space
      next++;
      if (next==slice_count) next = 0;
    }
    // and link the current block to it
    counts[next]--;
    alloc_hook_block_t* const block = blocks[current];
    blocks[current] = (alloc_hook_block_t*)((uint8_t*)block + bsize);  // bump to the following block
    alloc_hook_block_set_next(page, block, blocks[next]);   // and set next; note: we may have `current == next`
    current = next;
  }
  // prepend to the free list (usually NULL)
  alloc_hook_block_set_next(page, blocks[current], page->free);  // end of the list
  page->free = free_start;
}

static alloc_hook_decl_noinline void alloc_hook_page_free_list_extend( alloc_hook_page_t* const page, const size_t bsize, const size_t extend, alloc_hook_stats_t* const stats)
{
  ALLOC_HOOK_UNUSED(stats);
  #if (ALLOC_HOOK_SECURE <= 2)
  alloc_hook_assert_internal(page->free == NULL);
  alloc_hook_assert_internal(page->local_free == NULL);
  #endif
  alloc_hook_assert_internal(page->capacity + extend <= page->reserved);
  alloc_hook_assert_internal(bsize == alloc_hook_page_block_size(page));
  void* const page_area = _alloc_hook_page_start(_alloc_hook_page_segment(page), page, NULL );

  alloc_hook_block_t* const start = alloc_hook_page_block_at(page, page_area, bsize, page->capacity);

  // initialize a sequential free list
  alloc_hook_block_t* const last = alloc_hook_page_block_at(page, page_area, bsize, page->capacity + extend - 1);
  alloc_hook_block_t* block = start;
  while(block <= last) {
    alloc_hook_block_t* next = (alloc_hook_block_t*)((uint8_t*)block + bsize);
    alloc_hook_block_set_next(page,block,next);
    block = next;
  }
  // prepend to free list (usually `NULL`)
  alloc_hook_block_set_next(page, last, page->free);
  page->free = start;
}

/* -----------------------------------------------------------
  Page initialize and extend the capacity
----------------------------------------------------------- */

#define ALLOC_HOOK_MAX_EXTEND_SIZE    (4*1024)      // heuristic, one OS page seems to work well.
#if (ALLOC_HOOK_SECURE>0)
#define ALLOC_HOOK_MIN_EXTEND         (8*ALLOC_HOOK_SECURE) // extend at least by this many
#else
#define ALLOC_HOOK_MIN_EXTEND         (4)
#endif

// Extend the capacity (up to reserved) by initializing a free list
// We do at most `ALLOC_HOOK_MAX_EXTEND` to avoid touching too much memory
// Note: we also experimented with "bump" allocation on the first
// allocations but this did not speed up any benchmark (due to an
// extra test in malloc? or cache effects?)
static void alloc_hook_page_extend_free(alloc_hook_heap_t* heap, alloc_hook_page_t* page, alloc_hook_tld_t* tld) {
  ALLOC_HOOK_UNUSED(tld); 
  alloc_hook_assert_expensive(alloc_hook_page_is_valid_init(page));
  #if (ALLOC_HOOK_SECURE<=2)
  alloc_hook_assert(page->free == NULL);
  alloc_hook_assert(page->local_free == NULL);
  if (page->free != NULL) return;
  #endif
  if (page->capacity >= page->reserved) return;

  size_t page_size;
  _alloc_hook_page_start(_alloc_hook_page_segment(page), page, &page_size);
  alloc_hook_stat_counter_increase(tld->stats.pages_extended, 1);

  // calculate the extend count
  const size_t bsize = (page->xblock_size < ALLOC_HOOK_HUGE_BLOCK_SIZE ? page->xblock_size : page_size);
  size_t extend = page->reserved - page->capacity;
  alloc_hook_assert_internal(extend > 0);

  size_t max_extend = (bsize >= ALLOC_HOOK_MAX_EXTEND_SIZE ? ALLOC_HOOK_MIN_EXTEND : ALLOC_HOOK_MAX_EXTEND_SIZE/(uint32_t)bsize);
  if (max_extend < ALLOC_HOOK_MIN_EXTEND) { max_extend = ALLOC_HOOK_MIN_EXTEND; }
  alloc_hook_assert_internal(max_extend > 0);

  if (extend > max_extend) {
    // ensure we don't touch memory beyond the page to reduce page commit.
    // the `lean` benchmark tests this. Going from 1 to 8 increases rss by 50%.
    extend = max_extend;
  }

  alloc_hook_assert_internal(extend > 0 && extend + page->capacity <= page->reserved);
  alloc_hook_assert_internal(extend < (1UL<<16));

  // and append the extend the free list
  if (extend < ALLOC_HOOK_MIN_SLICES || ALLOC_HOOK_SECURE==0) { //!alloc_hook_option_is_enabled(alloc_hook_option_secure)) {
    alloc_hook_page_free_list_extend(page, bsize, extend, &tld->stats );
  }
  else {
    alloc_hook_page_free_list_extend_secure(heap, page, bsize, extend, &tld->stats);
  }
  // enable the new free list
  page->capacity += (uint16_t)extend;
  alloc_hook_stat_increase(tld->stats.page_committed, extend * bsize);
  alloc_hook_assert_expensive(alloc_hook_page_is_valid_init(page));
}

// Initialize a fresh page
static void alloc_hook_page_init(alloc_hook_heap_t* heap, alloc_hook_page_t* page, size_t block_size, alloc_hook_tld_t* tld) {
  alloc_hook_assert(page != NULL);
  alloc_hook_segment_t* segment = _alloc_hook_page_segment(page);
  alloc_hook_assert(segment != NULL);
  alloc_hook_assert_internal(block_size > 0);
  // set fields
  alloc_hook_page_set_heap(page, heap);
  page->xblock_size = (block_size < ALLOC_HOOK_HUGE_BLOCK_SIZE ? (uint32_t)block_size : ALLOC_HOOK_HUGE_BLOCK_SIZE); // initialize before _alloc_hook_segment_page_start
  size_t page_size;
  const void* page_start = _alloc_hook_segment_page_start(segment, page, &page_size);
  ALLOC_HOOK_UNUSED(page_start);
  alloc_hook_track_mem_noaccess(page_start,page_size);
  alloc_hook_assert_internal(alloc_hook_page_block_size(page) <= page_size);
  alloc_hook_assert_internal(page_size <= page->slice_count*ALLOC_HOOK_SEGMENT_SLICE_SIZE);
  alloc_hook_assert_internal(page_size / block_size < (1L<<16));
  page->reserved = (uint16_t)(page_size / block_size);
  alloc_hook_assert_internal(page->reserved > 0);
  #if (ALLOC_HOOK_PADDING || ALLOC_HOOK_ENCODE_FREELIST)
  page->keys[0] = _alloc_hook_heap_random_next(heap);
  page->keys[1] = _alloc_hook_heap_random_next(heap);
  #endif
  page->free_is_zero = page->is_zero_init;
  #if ALLOC_HOOK_DEBUG>2
  if (page->is_zero_init) {
    alloc_hook_track_mem_defined(page_start, page_size);
    alloc_hook_assert_expensive(alloc_hook_mem_is_zero(page_start, page_size));
  }
  #endif
  
  alloc_hook_assert_internal(page->is_committed);
  alloc_hook_assert_internal(page->capacity == 0);
  alloc_hook_assert_internal(page->free == NULL);
  alloc_hook_assert_internal(page->used == 0);
  alloc_hook_assert_internal(page->xthread_free == 0);
  alloc_hook_assert_internal(page->next == NULL);
  alloc_hook_assert_internal(page->prev == NULL);
  alloc_hook_assert_internal(page->retire_expire == 0);
  alloc_hook_assert_internal(!alloc_hook_page_has_aligned(page));
  #if (ALLOC_HOOK_PADDING || ALLOC_HOOK_ENCODE_FREELIST)
  alloc_hook_assert_internal(page->keys[0] != 0);
  alloc_hook_assert_internal(page->keys[1] != 0);
  #endif
  alloc_hook_assert_expensive(alloc_hook_page_is_valid_init(page));

  // initialize an initial free list
  alloc_hook_page_extend_free(heap,page,tld);
  alloc_hook_assert(alloc_hook_page_immediate_available(page));
}


/* -----------------------------------------------------------
  Find pages with free blocks
-------------------------------------------------------------*/

// Find a page with free blocks of `page->block_size`.
static alloc_hook_page_t* alloc_hook_page_queue_find_free_ex(alloc_hook_heap_t* heap, alloc_hook_page_queue_t* pq, bool first_try)
{
  // search through the pages in "next fit" order
  #if ALLOC_HOOK_STAT
  size_t count = 0;
  #endif
  alloc_hook_page_t* page = pq->first;
  while (page != NULL)
  {
    alloc_hook_page_t* next = page->next; // remember next
    #if ALLOC_HOOK_STAT    
    count++;
    #endif

    // 0. collect freed blocks by us and other threads
    _alloc_hook_page_free_collect(page, false);

    // 1. if the page contains free blocks, we are done
    if (alloc_hook_page_immediate_available(page)) {
      break;  // pick this one
    }

    // 2. Try to extend
    if (page->capacity < page->reserved) {
      alloc_hook_page_extend_free(heap, page, heap->tld);
      alloc_hook_assert_internal(alloc_hook_page_immediate_available(page));
      break;
    }

    // 3. If the page is completely full, move it to the `alloc_hook_pages_full`
    // queue so we don't visit long-lived pages too often.
    alloc_hook_assert_internal(!alloc_hook_page_is_in_full(page) && !alloc_hook_page_immediate_available(page));
    alloc_hook_page_to_full(page, pq);

    page = next;
  } // for each page

  alloc_hook_heap_stat_counter_increase(heap, searches, count);

  if (page == NULL) {
    _alloc_hook_heap_collect_retired(heap, false); // perhaps make a page available?
    page = alloc_hook_page_fresh(heap, pq);
    if (page == NULL && first_try) {
      // out-of-memory _or_ an abandoned page with free blocks was reclaimed, try once again
      page = alloc_hook_page_queue_find_free_ex(heap, pq, false);
    }
  }
  else {
    alloc_hook_assert(pq->first == page);
    page->retire_expire = 0;
  }
  alloc_hook_assert_internal(page == NULL || alloc_hook_page_immediate_available(page));
  return page;
}



// Find a page with free blocks of `size`.
static inline alloc_hook_page_t* alloc_hook_find_free_page(alloc_hook_heap_t* heap, size_t size) {
  alloc_hook_page_queue_t* pq = alloc_hook_page_queue(heap,size);
  alloc_hook_page_t* page = pq->first;
  if (page != NULL) {
   #if (ALLOC_HOOK_SECURE>=3) // in secure mode, we extend half the time to increase randomness
    if (page->capacity < page->reserved && ((_alloc_hook_heap_random_next(heap) & 1) == 1)) {
      alloc_hook_page_extend_free(heap, page, heap->tld);
      alloc_hook_assert_internal(alloc_hook_page_immediate_available(page));
    }
    else
   #endif
    {
      _alloc_hook_page_free_collect(page,false);
    }

    if (alloc_hook_page_immediate_available(page)) {
      page->retire_expire = 0;
      return page; // fast path
    }
  }
  return alloc_hook_page_queue_find_free_ex(heap, pq, true);
}


/* -----------------------------------------------------------
  Users can register a deferred free function called
  when the `free` list is empty. Since the `local_free`
  is separate this is deterministically called after
  a certain number of allocations.
----------------------------------------------------------- */

static alloc_hook_deferred_free_fun* volatile deferred_free = NULL;
static _Atomic(void*) deferred_arg; // = NULL

void _alloc_hook_deferred_free(alloc_hook_heap_t* heap, bool force) {
  heap->tld->heartbeat++;
  if (deferred_free != NULL && !heap->tld->recurse) {
    heap->tld->recurse = true;
    deferred_free(force, heap->tld->heartbeat, alloc_hook_atomic_load_ptr_relaxed(void,&deferred_arg));
    heap->tld->recurse = false;
  }
}

void alloc_hook_register_deferred_free(alloc_hook_deferred_free_fun* fn, void* arg) alloc_hook_attr_noexcept {
  deferred_free = fn;
  alloc_hook_atomic_store_ptr_release(void,&deferred_arg, arg);
}


/* -----------------------------------------------------------
  General allocation
----------------------------------------------------------- */

// Large and huge page allocation.
// Huge pages are allocated directly without being in a queue.
// Because huge pages contain just one block, and the segment contains
// just that page, we always treat them as abandoned and any thread
// that frees the block can free the whole page and segment directly.
// Huge pages are also use if the requested alignment is very large (> ALLOC_HOOK_ALIGNMENT_MAX).
static alloc_hook_page_t* alloc_hook_large_huge_page_alloc(alloc_hook_heap_t* heap, size_t size, size_t page_alignment) {
  size_t block_size = _alloc_hook_os_good_alloc_size(size);
  alloc_hook_assert_internal(alloc_hook_bin(block_size) == ALLOC_HOOK_BIN_HUGE || page_alignment > 0);
  bool is_huge = (block_size > ALLOC_HOOK_LARGE_OBJ_SIZE_MAX || page_alignment > 0);
  #if ALLOC_HOOK_HUGE_PAGE_ABANDON
  alloc_hook_page_queue_t* pq = (is_huge ? NULL : alloc_hook_page_queue(heap, block_size));
  #else
  alloc_hook_page_queue_t* pq = alloc_hook_page_queue(heap, is_huge ? ALLOC_HOOK_HUGE_BLOCK_SIZE : block_size); // not block_size as that can be low if the page_alignment > 0
  alloc_hook_assert_internal(!is_huge || alloc_hook_page_queue_is_huge(pq));
  #endif
  alloc_hook_page_t* page = alloc_hook_page_fresh_alloc(heap, pq, block_size, page_alignment);
  if (page != NULL) {
    alloc_hook_assert_internal(alloc_hook_page_immediate_available(page));
    
    if (is_huge) {
      alloc_hook_assert_internal(_alloc_hook_page_segment(page)->kind == ALLOC_HOOK_SEGMENT_HUGE);
      alloc_hook_assert_internal(_alloc_hook_page_segment(page)->used==1);
      #if ALLOC_HOOK_HUGE_PAGE_ABANDON
      alloc_hook_assert_internal(_alloc_hook_page_segment(page)->thread_id==0); // abandoned, not in the huge queue
      alloc_hook_page_set_heap(page, NULL);
      #endif      
    }
    else {
      alloc_hook_assert_internal(_alloc_hook_page_segment(page)->kind != ALLOC_HOOK_SEGMENT_HUGE);
    }
    
    const size_t bsize = alloc_hook_page_usable_block_size(page);  // note: not `alloc_hook_page_block_size` to account for padding
    if (bsize <= ALLOC_HOOK_LARGE_OBJ_SIZE_MAX) {
      alloc_hook_heap_stat_increase(heap, large, bsize);
      alloc_hook_heap_stat_counter_increase(heap, large_count, 1);
    }
    else {
      alloc_hook_heap_stat_increase(heap, huge, bsize);
      alloc_hook_heap_stat_counter_increase(heap, huge_count, 1);
    }
  }
  return page;
}


// Allocate a page
// Note: in debug mode the size includes ALLOC_HOOK_PADDING_SIZE and might have overflowed.
static alloc_hook_page_t* alloc_hook_find_page(alloc_hook_heap_t* heap, size_t size, size_t huge_alignment) alloc_hook_attr_noexcept {
  // huge allocation?
  const size_t req_size = size - ALLOC_HOOK_PADDING_SIZE;  // correct for padding_size in case of an overflow on `size`  
  if alloc_hook_unlikely(req_size > (ALLOC_HOOK_MEDIUM_OBJ_SIZE_MAX - ALLOC_HOOK_PADDING_SIZE) || huge_alignment > 0) {
    if alloc_hook_unlikely(req_size > PTRDIFF_MAX) {  // we don't allocate more than PTRDIFF_MAX (see <https://sourceware.org/ml/libc-announce/2019/msg00001.html>)
      _alloc_hook_error_message(EOVERFLOW, "allocation request is too large (%zu bytes)\n", req_size);
      return NULL;
    }
    else {
      return alloc_hook_large_huge_page_alloc(heap,size,huge_alignment);
    }
  }
  else {
    // otherwise find a page with free blocks in our size segregated queues
    #if ALLOC_HOOK_PADDING
    alloc_hook_assert_internal(size >= ALLOC_HOOK_PADDING_SIZE); 
    #endif
    return alloc_hook_find_free_page(heap, size);
  }
}

// Generic allocation routine if the fast path (`alloc.c:alloc_hook_page_malloc`) does not succeed.
// Note: in debug mode the size includes ALLOC_HOOK_PADDING_SIZE and might have overflowed.
// The `huge_alignment` is normally 0 but is set to a multiple of ALLOC_HOOK_SEGMENT_SIZE for
// very large requested alignments in which case we use a huge segment.
void* _alloc_hook_malloc_generic(alloc_hook_heap_t* heap, size_t size, bool zero, size_t huge_alignment) alloc_hook_attr_noexcept
{
  alloc_hook_assert_internal(heap != NULL);

  // initialize if necessary
  if alloc_hook_unlikely(!alloc_hook_heap_is_initialized(heap)) {
    heap = alloc_hook_heap_get_default(); // calls alloc_hook_thread_init 
    if alloc_hook_unlikely(!alloc_hook_heap_is_initialized(heap)) { return NULL; }
  }
  alloc_hook_assert_internal(alloc_hook_heap_is_initialized(heap));

  // call potential deferred free routines
  _alloc_hook_deferred_free(heap, false);

  // free delayed frees from other threads (but skip contended ones)
  _alloc_hook_heap_delayed_free_partial(heap);

  // find (or allocate) a page of the right size
  alloc_hook_page_t* page = alloc_hook_find_page(heap, size, huge_alignment);
  if alloc_hook_unlikely(page == NULL) { // first time out of memory, try to collect and retry the allocation once more
    alloc_hook_heap_collect(heap, true /* force */);
    page = alloc_hook_find_page(heap, size, huge_alignment);
  }

  if alloc_hook_unlikely(page == NULL) { // out of memory
    const size_t req_size = size - ALLOC_HOOK_PADDING_SIZE;  // correct for padding_size in case of an overflow on `size`
    _alloc_hook_error_message(ENOMEM, "unable to allocate memory (%zu bytes)\n", req_size);
    return NULL;
  }

  alloc_hook_assert_internal(alloc_hook_page_immediate_available(page));
  alloc_hook_assert_internal(alloc_hook_page_block_size(page) >= size);

  // and try again, this time succeeding! (i.e. this should never recurse through _alloc_hook_page_malloc)
  if alloc_hook_unlikely(zero && page->xblock_size == 0) {
    // note: we cannot call _alloc_hook_page_malloc with zeroing for huge blocks; we zero it afterwards in that case.
    void* p = _alloc_hook_page_malloc(heap, page, size, false);
    alloc_hook_assert_internal(p != NULL);
    _alloc_hook_memzero_aligned(p, alloc_hook_page_usable_block_size(page));
    return p;
  }
  else {
    return _alloc_hook_page_malloc(heap, page, size, zero);
  }
}
