/*----------------------------------------------------------------------------
Copyright (c) 2018-2020, Microsoft Research, Daan Leijen
This is free software; you can redistribute it and/or modify it under the
terms of the MIT license. A copy of the license can be found in the file
"LICENSE" at the root of this distribution.
-----------------------------------------------------------------------------*/

/* -----------------------------------------------------------
  Definition of page queues for each block size
----------------------------------------------------------- */

#ifndef ALLOC_HOOK_IN_PAGE_C
#error "this file should be included from 'page.c'"
#endif

/* -----------------------------------------------------------
  Minimal alignment in machine words (i.e. `sizeof(void*)`)
----------------------------------------------------------- */

#if (ALLOC_HOOK_MAX_ALIGN_SIZE > 4*ALLOC_HOOK_INTPTR_SIZE)
  #error "define alignment for more than 4x word size for this platform"
#elif (ALLOC_HOOK_MAX_ALIGN_SIZE > 2*ALLOC_HOOK_INTPTR_SIZE)
  #define ALLOC_HOOK_ALIGN4W   // 4 machine words minimal alignment
#elif (ALLOC_HOOK_MAX_ALIGN_SIZE > ALLOC_HOOK_INTPTR_SIZE)
  #define ALLOC_HOOK_ALIGN2W   // 2 machine words minimal alignment
#else
  // ok, default alignment is 1 word
#endif


/* -----------------------------------------------------------
  Queue query
----------------------------------------------------------- */


static inline bool alloc_hook_page_queue_is_huge(const alloc_hook_page_queue_t* pq) {
  return (pq->block_size == (ALLOC_HOOK_MEDIUM_OBJ_SIZE_MAX+sizeof(uintptr_t)));
}

static inline bool alloc_hook_page_queue_is_full(const alloc_hook_page_queue_t* pq) {
  return (pq->block_size == (ALLOC_HOOK_MEDIUM_OBJ_SIZE_MAX+(2*sizeof(uintptr_t))));
}

static inline bool alloc_hook_page_queue_is_special(const alloc_hook_page_queue_t* pq) {
  return (pq->block_size > ALLOC_HOOK_MEDIUM_OBJ_SIZE_MAX);
}

/* -----------------------------------------------------------
  Bins
----------------------------------------------------------- */

// Return the bin for a given field size.
// Returns ALLOC_HOOK_BIN_HUGE if the size is too large.
// We use `wsize` for the size in "machine word sizes",
// i.e. byte size == `wsize*sizeof(void*)`.
static inline uint8_t alloc_hook_bin(size_t size) {
  size_t wsize = _alloc_hook_wsize_from_size(size);
  uint8_t bin;
  if (wsize <= 1) {
    bin = 1;
  }
  #if defined(ALLOC_HOOK_ALIGN4W)
  else if (wsize <= 4) {
    bin = (uint8_t)((wsize+1)&~1); // round to double word sizes
  }
  #elif defined(ALLOC_HOOK_ALIGN2W)
  else if (wsize <= 8) {
    bin = (uint8_t)((wsize+1)&~1); // round to double word sizes
  }
  #else
  else if (wsize <= 8) {
    bin = (uint8_t)wsize;
  }
  #endif
  else if (wsize > ALLOC_HOOK_MEDIUM_OBJ_WSIZE_MAX) {
    bin = ALLOC_HOOK_BIN_HUGE;
  }
  else {
    #if defined(ALLOC_HOOK_ALIGN4W)
    if (wsize <= 16) { wsize = (wsize+3)&~3; } // round to 4x word sizes
    #endif
    wsize--;
    // find the highest bit
    uint8_t b = (uint8_t)alloc_hook_bsr(wsize);  // note: wsize != 0
    // and use the top 3 bits to determine the bin (~12.5% worst internal fragmentation).
    // - adjust with 3 because we use do not round the first 8 sizes
    //   which each get an exact bin
    bin = ((b << 2) + (uint8_t)((wsize >> (b - 2)) & 0x03)) - 3;
    alloc_hook_assert_internal(bin < ALLOC_HOOK_BIN_HUGE);
  }
  alloc_hook_assert_internal(bin > 0 && bin <= ALLOC_HOOK_BIN_HUGE);
  return bin;
}



/* -----------------------------------------------------------
  Queue of pages with free blocks
----------------------------------------------------------- */

uint8_t _alloc_hook_bin(size_t size) {
  return alloc_hook_bin(size);
}

size_t _alloc_hook_bin_size(uint8_t bin) {
  return _alloc_hook_heap_empty.pages[bin].block_size;
}

// Good size for allocation
size_t alloc_hook_good_size(size_t size) alloc_hook_attr_noexcept {
  if (size <= ALLOC_HOOK_MEDIUM_OBJ_SIZE_MAX) {
    return _alloc_hook_bin_size(alloc_hook_bin(size));
  }
  else {
    return _alloc_hook_align_up(size,_alloc_hook_os_page_size());
  }
}

#if (ALLOC_HOOK_DEBUG>1)
static bool alloc_hook_page_queue_contains(alloc_hook_page_queue_t* queue, const alloc_hook_page_t* page) {
  alloc_hook_assert_internal(page != NULL);
  alloc_hook_page_t* list = queue->first;
  while (list != NULL) {
    alloc_hook_assert_internal(list->next == NULL || list->next->prev == list);
    alloc_hook_assert_internal(list->prev == NULL || list->prev->next == list);
    if (list == page) break;
    list = list->next;
  }
  return (list == page);
}

#endif

#if (ALLOC_HOOK_DEBUG>1)
static bool alloc_hook_heap_contains_queue(const alloc_hook_heap_t* heap, const alloc_hook_page_queue_t* pq) {
  return (pq >= &heap->pages[0] && pq <= &heap->pages[ALLOC_HOOK_BIN_FULL]);
}
#endif

static alloc_hook_page_queue_t* alloc_hook_page_queue_of(const alloc_hook_page_t* page) {
  uint8_t bin = (alloc_hook_page_is_in_full(page) ? ALLOC_HOOK_BIN_FULL : alloc_hook_bin(page->xblock_size));
  alloc_hook_heap_t* heap = alloc_hook_page_heap(page);
  alloc_hook_assert_internal(heap != NULL && bin <= ALLOC_HOOK_BIN_FULL);
  alloc_hook_page_queue_t* pq = &heap->pages[bin];
  alloc_hook_assert_internal(bin >= ALLOC_HOOK_BIN_HUGE || page->xblock_size == pq->block_size);
  alloc_hook_assert_expensive(alloc_hook_page_queue_contains(pq, page));
  return pq;
}

static alloc_hook_page_queue_t* alloc_hook_heap_page_queue_of(alloc_hook_heap_t* heap, const alloc_hook_page_t* page) {
  uint8_t bin = (alloc_hook_page_is_in_full(page) ? ALLOC_HOOK_BIN_FULL : alloc_hook_bin(page->xblock_size));
  alloc_hook_assert_internal(bin <= ALLOC_HOOK_BIN_FULL);
  alloc_hook_page_queue_t* pq = &heap->pages[bin];
  alloc_hook_assert_internal(alloc_hook_page_is_in_full(page) || page->xblock_size == pq->block_size);
  return pq;
}

// The current small page array is for efficiency and for each
// small size (up to 256) it points directly to the page for that
// size without having to compute the bin. This means when the
// current free page queue is updated for a small bin, we need to update a
// range of entries in `_alloc_hook_page_small_free`.
static inline void alloc_hook_heap_queue_first_update(alloc_hook_heap_t* heap, const alloc_hook_page_queue_t* pq) {
  alloc_hook_assert_internal(alloc_hook_heap_contains_queue(heap,pq));
  size_t size = pq->block_size;
  if (size > ALLOC_HOOK_SMALL_SIZE_MAX) return;

  alloc_hook_page_t* page = pq->first;
  if (pq->first == NULL) page = (alloc_hook_page_t*)&_alloc_hook_page_empty;

  // find index in the right direct page array
  size_t start;
  size_t idx = _alloc_hook_wsize_from_size(size);
  alloc_hook_page_t** pages_free = heap->pages_free_direct;

  if (pages_free[idx] == page) return;  // already set

  // find start slot
  if (idx<=1) {
    start = 0;
  }
  else {
    // find previous size; due to minimal alignment upto 3 previous bins may need to be skipped
    uint8_t bin = alloc_hook_bin(size);
    const alloc_hook_page_queue_t* prev = pq - 1;
    while( bin == alloc_hook_bin(prev->block_size) && prev > &heap->pages[0]) {
      prev--;
    }
    start = 1 + _alloc_hook_wsize_from_size(prev->block_size);
    if (start > idx) start = idx;
  }

  // set size range to the right page
  alloc_hook_assert(start <= idx);
  for (size_t sz = start; sz <= idx; sz++) {
    pages_free[sz] = page;
  }
}

/*
static bool alloc_hook_page_queue_is_empty(alloc_hook_page_queue_t* queue) {
  return (queue->first == NULL);
}
*/

static void alloc_hook_page_queue_remove(alloc_hook_page_queue_t* queue, alloc_hook_page_t* page) {
  alloc_hook_assert_internal(page != NULL);
  alloc_hook_assert_expensive(alloc_hook_page_queue_contains(queue, page));
  alloc_hook_assert_internal(page->xblock_size == queue->block_size || (page->xblock_size > ALLOC_HOOK_MEDIUM_OBJ_SIZE_MAX && alloc_hook_page_queue_is_huge(queue))  || (alloc_hook_page_is_in_full(page) && alloc_hook_page_queue_is_full(queue)));
  alloc_hook_heap_t* heap = alloc_hook_page_heap(page);

  if (page->prev != NULL) page->prev->next = page->next;
  if (page->next != NULL) page->next->prev = page->prev;
  if (page == queue->last)  queue->last = page->prev;
  if (page == queue->first) {
    queue->first = page->next;
    // update first
    alloc_hook_assert_internal(alloc_hook_heap_contains_queue(heap, queue));
    alloc_hook_heap_queue_first_update(heap,queue);
  }
  heap->page_count--;
  page->next = NULL;
  page->prev = NULL;
  // alloc_hook_atomic_store_ptr_release(alloc_hook_atomic_cast(void*, &page->heap), NULL);
  alloc_hook_page_set_in_full(page,false);
}


static void alloc_hook_page_queue_push(alloc_hook_heap_t* heap, alloc_hook_page_queue_t* queue, alloc_hook_page_t* page) {
  alloc_hook_assert_internal(alloc_hook_page_heap(page) == heap);
  alloc_hook_assert_internal(!alloc_hook_page_queue_contains(queue, page));
  #if ALLOC_HOOK_HUGE_PAGE_ABANDON
  alloc_hook_assert_internal(_alloc_hook_page_segment(page)->kind != ALLOC_HOOK_SEGMENT_HUGE);
  #endif
  alloc_hook_assert_internal(page->xblock_size == queue->block_size ||
                      (page->xblock_size > ALLOC_HOOK_MEDIUM_OBJ_SIZE_MAX) ||
                        (alloc_hook_page_is_in_full(page) && alloc_hook_page_queue_is_full(queue)));

  alloc_hook_page_set_in_full(page, alloc_hook_page_queue_is_full(queue));
  // alloc_hook_atomic_store_ptr_release(alloc_hook_atomic_cast(void*, &page->heap), heap);
  page->next = queue->first;
  page->prev = NULL;
  if (queue->first != NULL) {
    alloc_hook_assert_internal(queue->first->prev == NULL);
    queue->first->prev = page;
    queue->first = page;
  }
  else {
    queue->first = queue->last = page;
  }

  // update direct
  alloc_hook_heap_queue_first_update(heap, queue);
  heap->page_count++;
}


static void alloc_hook_page_queue_enqueue_from(alloc_hook_page_queue_t* to, alloc_hook_page_queue_t* from, alloc_hook_page_t* page) {
  alloc_hook_assert_internal(page != NULL);
  alloc_hook_assert_expensive(alloc_hook_page_queue_contains(from, page));
  alloc_hook_assert_expensive(!alloc_hook_page_queue_contains(to, page));

  alloc_hook_assert_internal((page->xblock_size == to->block_size && page->xblock_size == from->block_size) ||
                     (page->xblock_size == to->block_size && alloc_hook_page_queue_is_full(from)) ||
                     (page->xblock_size == from->block_size && alloc_hook_page_queue_is_full(to)) ||
                     (page->xblock_size > ALLOC_HOOK_LARGE_OBJ_SIZE_MAX && alloc_hook_page_queue_is_huge(to)) ||
                     (page->xblock_size > ALLOC_HOOK_LARGE_OBJ_SIZE_MAX && alloc_hook_page_queue_is_full(to)));

  alloc_hook_heap_t* heap = alloc_hook_page_heap(page);
  if (page->prev != NULL) page->prev->next = page->next;
  if (page->next != NULL) page->next->prev = page->prev;
  if (page == from->last)  from->last = page->prev;
  if (page == from->first) {
    from->first = page->next;
    // update first
    alloc_hook_assert_internal(alloc_hook_heap_contains_queue(heap, from));
    alloc_hook_heap_queue_first_update(heap, from);
  }

  page->prev = to->last;
  page->next = NULL;
  if (to->last != NULL) {
    alloc_hook_assert_internal(heap == alloc_hook_page_heap(to->last));
    to->last->next = page;
    to->last = page;
  }
  else {
    to->first = page;
    to->last = page;
    alloc_hook_heap_queue_first_update(heap, to);
  }

  alloc_hook_page_set_in_full(page, alloc_hook_page_queue_is_full(to));
}

// Only called from `alloc_hook_heap_absorb`.
size_t _alloc_hook_page_queue_append(alloc_hook_heap_t* heap, alloc_hook_page_queue_t* pq, alloc_hook_page_queue_t* append) {
  alloc_hook_assert_internal(alloc_hook_heap_contains_queue(heap,pq));
  alloc_hook_assert_internal(pq->block_size == append->block_size);

  if (append->first==NULL) return 0;

  // set append pages to new heap and count
  size_t count = 0;
  for (alloc_hook_page_t* page = append->first; page != NULL; page = page->next) {
    // inline `alloc_hook_page_set_heap` to avoid wrong assertion during absorption;
    // in this case it is ok to be delayed freeing since both "to" and "from" heap are still alive.
    alloc_hook_atomic_store_release(&page->xheap, (uintptr_t)heap);
    // set the flag to delayed free (not overriding NEVER_DELAYED_FREE) which has as a
    // side effect that it spins until any DELAYED_FREEING is finished. This ensures
    // that after appending only the new heap will be used for delayed free operations.
    _alloc_hook_page_use_delayed_free(page, ALLOC_HOOK_USE_DELAYED_FREE, false);
    count++;
  }

  if (pq->last==NULL) {
    // take over afresh
    alloc_hook_assert_internal(pq->first==NULL);
    pq->first = append->first;
    pq->last = append->last;
    alloc_hook_heap_queue_first_update(heap, pq);
  }
  else {
    // append to end
    alloc_hook_assert_internal(pq->last!=NULL);
    alloc_hook_assert_internal(append->first!=NULL);
    pq->last->next = append->first;
    append->first->prev = pq->last;
    pq->last = append->last;
  }
  return count;
}
