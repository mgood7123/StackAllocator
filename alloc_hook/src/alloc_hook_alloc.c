/* ----------------------------------------------------------------------------
Copyright (c) 2018-2022, Microsoft Research, Daan Leijen
This is free software; you can redistribute it and/or modify it under the
terms of the MIT license. A copy of the license can be found in the file
"LICENSE" at the root of this distribution.
-----------------------------------------------------------------------------*/
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE   // for realpath() on Linux
#endif

#include "alloc_hook.h"
#include "alloc_hook_internal.h"
#include "alloc_hook_atomic.h"
#include "alloc_hook_prim.h"   // _alloc_hook_prim_thread_id()

#include <string.h>      // memset, strlen (for alloc_hook_strdup)
#include <stdlib.h>      // malloc, abort

#define ALLOC_HOOK_IN_ALLOC_C
#include "alloc_hook_alloc_override.c"
#undef ALLOC_HOOK_IN_ALLOC_C

// ------------------------------------------------------
// Allocation
// ------------------------------------------------------

// Fast allocation in a page: just pop from the free list.
// Fall back to generic allocation only if the list is empty.
extern inline void* _alloc_hook_page_malloc(alloc_hook_heap_t* heap, alloc_hook_page_t* page, size_t size, bool zero) alloc_hook_attr_noexcept {
  alloc_hook_assert_internal(page->xblock_size==0||alloc_hook_page_block_size(page) >= size);
  alloc_hook_block_t* const block = page->free;
  if alloc_hook_unlikely(block == NULL) {
    return _alloc_hook_malloc_generic(heap, size, zero, 0);
  }
  alloc_hook_assert_internal(block != NULL && _alloc_hook_ptr_page(block) == page);
  // pop from the free list
  page->used++;
  page->free = alloc_hook_block_next(page, block);
  alloc_hook_assert_internal(page->free == NULL || _alloc_hook_ptr_page(page->free) == page);
  #if ALLOC_HOOK_DEBUG>3
  if (page->free_is_zero) {
    alloc_hook_assert_expensive(alloc_hook_mem_is_zero(block+1,size - sizeof(*block)));
  }
  #endif

  // allow use of the block internally
  // note: when tracking we need to avoid ever touching the ALLOC_HOOK_PADDING since
  // that is tracked by valgrind etc. as non-accessible (through the red-zone, see `alloc_hook/track.h`)
  alloc_hook_track_mem_undefined(block, alloc_hook_page_usable_block_size(page));

  // zero the block? note: we need to zero the full block size (issue #63)
  if alloc_hook_unlikely(zero) {
    alloc_hook_assert_internal(page->xblock_size != 0); // do not call with zero'ing for huge blocks (see _alloc_hook_malloc_generic)
    alloc_hook_assert_internal(page->xblock_size >= ALLOC_HOOK_PADDING_SIZE);
    if (page->free_is_zero) {
      block->next = 0;
      alloc_hook_track_mem_defined(block, page->xblock_size - ALLOC_HOOK_PADDING_SIZE);
    }
    else {
      _alloc_hook_memzero_aligned(block, page->xblock_size - ALLOC_HOOK_PADDING_SIZE);
    }    
  }

#if (ALLOC_HOOK_DEBUG>0) && !ALLOC_HOOK_TRACK_ENABLED && !ALLOC_HOOK_TSAN
  if (!zero && !alloc_hook_page_is_huge(page)) {
    memset(block, ALLOC_HOOK_DEBUG_UNINIT, alloc_hook_page_usable_block_size(page));
  }
#elif (ALLOC_HOOK_SECURE!=0)
  if (!zero) { block->next = 0; } // don't leak internal data
#endif

#if (ALLOC_HOOK_STAT>0)
  const size_t bsize = alloc_hook_page_usable_block_size(page);
  if (bsize <= ALLOC_HOOK_MEDIUM_OBJ_SIZE_MAX) {
    alloc_hook_heap_stat_increase(heap, normal, bsize);
    alloc_hook_heap_stat_counter_increase(heap, normal_count, 1);
#if (ALLOC_HOOK_STAT>1)
    const size_t bin = _alloc_hook_bin(bsize);
    alloc_hook_heap_stat_increase(heap, normal_bins[bin], 1);
#endif
  }
#endif

#if ALLOC_HOOK_PADDING // && !ALLOC_HOOK_TRACK_ENABLED
  alloc_hook_padding_t* const padding = (alloc_hook_padding_t*)((uint8_t*)block + alloc_hook_page_usable_block_size(page));
  ptrdiff_t delta = ((uint8_t*)padding - (uint8_t*)block - (size - ALLOC_HOOK_PADDING_SIZE));
  #if (ALLOC_HOOK_DEBUG>=2)
  alloc_hook_assert_internal(delta >= 0 && alloc_hook_page_usable_block_size(page) >= (size - ALLOC_HOOK_PADDING_SIZE + delta));
  #endif
  alloc_hook_track_mem_defined(padding,sizeof(alloc_hook_padding_t));  // note: re-enable since alloc_hook_page_usable_block_size may set noaccess
  padding->canary = (uint32_t)(alloc_hook_ptr_encode(page,block,page->keys));
  padding->delta  = (uint32_t)(delta);
  #if ALLOC_HOOK_PADDING_CHECK
  if (!alloc_hook_page_is_huge(page)) {
    uint8_t* fill = (uint8_t*)padding - delta;
    const size_t maxpad = (delta > ALLOC_HOOK_MAX_ALIGN_SIZE ? ALLOC_HOOK_MAX_ALIGN_SIZE : delta); // set at most N initial padding bytes
    for (size_t i = 0; i < maxpad; i++) { fill[i] = ALLOC_HOOK_DEBUG_PADDING; }
  }
  #endif
#endif

  return block;
}

static inline alloc_hook_decl_restrict void* alloc_hook_heap_malloc_small_zero(alloc_hook_heap_t* heap, size_t size, bool zero) alloc_hook_attr_noexcept {
  alloc_hook_assert(heap != NULL);
  #if ALLOC_HOOK_DEBUG
  const uintptr_t tid = _alloc_hook_thread_id();
  alloc_hook_assert(heap->thread_id == 0 || heap->thread_id == tid); // heaps are thread local
  #endif
  alloc_hook_assert(size <= ALLOC_HOOK_SMALL_SIZE_MAX);
  #if (ALLOC_HOOK_PADDING)
  if (size == 0) { size = sizeof(void*); }
  #endif
  alloc_hook_page_t* page = _alloc_hook_heap_get_free_small_page(heap, size + ALLOC_HOOK_PADDING_SIZE);
  void* const p = _alloc_hook_page_malloc(heap, page, size + ALLOC_HOOK_PADDING_SIZE, zero);  
  alloc_hook_track_malloc(p,size,zero);
  #if ALLOC_HOOK_STAT>1
  if (p != NULL) {
    if (!alloc_hook_heap_is_initialized(heap)) { heap = alloc_hook_prim_get_default_heap(); }
    alloc_hook_heap_stat_increase(heap, malloc, alloc_hook_usable_size(p));
  }
  #endif
  #if ALLOC_HOOK_DEBUG>3
  if (p != NULL && zero) {
    alloc_hook_assert_expensive(alloc_hook_mem_is_zero(p, size));
  }
  #endif
  return p;
}

// allocate a small block
alloc_hook_decl_nodiscard extern inline alloc_hook_decl_restrict void* alloc_hook_heap_malloc_small(alloc_hook_heap_t* heap, size_t size) alloc_hook_attr_noexcept {
  return alloc_hook_heap_malloc_small_zero(heap, size, false);
}

alloc_hook_decl_nodiscard extern inline alloc_hook_decl_restrict void* alloc_hook_malloc_small(size_t size) alloc_hook_attr_noexcept {
  return alloc_hook_heap_malloc_small(alloc_hook_prim_get_default_heap(), size);
}

// The main allocation function
extern inline void* _alloc_hook_heap_malloc_zero_ex(alloc_hook_heap_t* heap, size_t size, bool zero, size_t huge_alignment) alloc_hook_attr_noexcept {
  if alloc_hook_likely(size <= ALLOC_HOOK_SMALL_SIZE_MAX) {
    alloc_hook_assert_internal(huge_alignment == 0);
    return alloc_hook_heap_malloc_small_zero(heap, size, zero);
  }
  else {
    alloc_hook_assert(heap!=NULL);
    alloc_hook_assert(heap->thread_id == 0 || heap->thread_id == _alloc_hook_thread_id());   // heaps are thread local
    void* const p = _alloc_hook_malloc_generic(heap, size + ALLOC_HOOK_PADDING_SIZE, zero, huge_alignment);  // note: size can overflow but it is detected in malloc_generic
    alloc_hook_track_malloc(p,size,zero);
    #if ALLOC_HOOK_STAT>1
    if (p != NULL) {
      if (!alloc_hook_heap_is_initialized(heap)) { heap = alloc_hook_prim_get_default_heap(); }
      alloc_hook_heap_stat_increase(heap, malloc, alloc_hook_usable_size(p));
    }
    #endif
    #if ALLOC_HOOK_DEBUG>3
    if (p != NULL && zero) {
      alloc_hook_assert_expensive(alloc_hook_mem_is_zero(p, size));
    }
    #endif
    return p;
  }
}

extern inline void* _alloc_hook_heap_malloc_zero(alloc_hook_heap_t* heap, size_t size, bool zero) alloc_hook_attr_noexcept {
  return _alloc_hook_heap_malloc_zero_ex(heap, size, zero, 0);
}

alloc_hook_decl_nodiscard extern inline alloc_hook_decl_restrict void* alloc_hook_heap_malloc(alloc_hook_heap_t* heap, size_t size) alloc_hook_attr_noexcept {
  return _alloc_hook_heap_malloc_zero(heap, size, false);
}

alloc_hook_decl_nodiscard extern inline alloc_hook_decl_restrict void* alloc_hook_malloc(size_t size) alloc_hook_attr_noexcept {
  return alloc_hook_heap_malloc(alloc_hook_prim_get_default_heap(), size);
}

// zero initialized small block
alloc_hook_decl_nodiscard alloc_hook_decl_restrict void* alloc_hook_zalloc_small(size_t size) alloc_hook_attr_noexcept {
  return alloc_hook_heap_malloc_small_zero(alloc_hook_prim_get_default_heap(), size, true);
}

alloc_hook_decl_nodiscard extern inline alloc_hook_decl_restrict void* alloc_hook_heap_zalloc(alloc_hook_heap_t* heap, size_t size) alloc_hook_attr_noexcept {
  return _alloc_hook_heap_malloc_zero(heap, size, true);
}

alloc_hook_decl_nodiscard alloc_hook_decl_restrict void* alloc_hook_zalloc(size_t size) alloc_hook_attr_noexcept {
  return alloc_hook_heap_zalloc(alloc_hook_prim_get_default_heap(),size);
}


// ------------------------------------------------------
// Check for double free in secure and debug mode
// This is somewhat expensive so only enabled for secure mode 4
// ------------------------------------------------------

#if (ALLOC_HOOK_ENCODE_FREELIST && (ALLOC_HOOK_SECURE>=4 || ALLOC_HOOK_DEBUG!=0))
// linear check if the free list contains a specific element
static bool alloc_hook_list_contains(const alloc_hook_page_t* page, const alloc_hook_block_t* list, const alloc_hook_block_t* elem) {
  while (list != NULL) {
    if (elem==list) return true;
    list = alloc_hook_block_next(page, list);
  }
  return false;
}

static alloc_hook_decl_noinline bool alloc_hook_check_is_double_freex(const alloc_hook_page_t* page, const alloc_hook_block_t* block) {
  // The decoded value is in the same page (or NULL).
  // Walk the free lists to verify positively if it is already freed
  if (alloc_hook_list_contains(page, page->free, block) ||
      alloc_hook_list_contains(page, page->local_free, block) ||
      alloc_hook_list_contains(page, alloc_hook_page_thread_free(page), block))
  {
    _alloc_hook_error_message(EAGAIN, "double free detected of block %p with size %zu\n", block, alloc_hook_page_block_size(page));
    return true;
  }
  return false;
}

#define alloc_hook_track_page(page,access)  { size_t psize; void* pstart = _alloc_hook_page_start(_alloc_hook_page_segment(page),page,&psize); alloc_hook_track_mem_##access( pstart, psize); }

static inline bool alloc_hook_check_is_double_free(const alloc_hook_page_t* page, const alloc_hook_block_t* block) {
  bool is_double_free = false;
  alloc_hook_block_t* n = alloc_hook_block_nextx(page, block, page->keys); // pretend it is freed, and get the decoded first field
  if (((uintptr_t)n & (ALLOC_HOOK_INTPTR_SIZE-1))==0 &&  // quick check: aligned pointer?
      (n==NULL || alloc_hook_is_in_same_page(block, n))) // quick check: in same page or NULL?
  {
    // Suspicous: decoded value a in block is in the same page (or NULL) -- maybe a double free?
    // (continue in separate function to improve code generation)
    is_double_free = alloc_hook_check_is_double_freex(page, block);
  }
  return is_double_free;
}
#else
static inline bool alloc_hook_check_is_double_free(const alloc_hook_page_t* page, const alloc_hook_block_t* block) {
  ALLOC_HOOK_UNUSED(page);
  ALLOC_HOOK_UNUSED(block);
  return false;
}
#endif

// ---------------------------------------------------------------------------
// Check for heap block overflow by setting up padding at the end of the block
// ---------------------------------------------------------------------------

#if ALLOC_HOOK_PADDING // && !ALLOC_HOOK_TRACK_ENABLED
static bool alloc_hook_page_decode_padding(const alloc_hook_page_t* page, const alloc_hook_block_t* block, size_t* delta, size_t* bsize) {
  *bsize = alloc_hook_page_usable_block_size(page);
  const alloc_hook_padding_t* const padding = (alloc_hook_padding_t*)((uint8_t*)block + *bsize);
  alloc_hook_track_mem_defined(padding,sizeof(alloc_hook_padding_t));
  *delta = padding->delta;
  uint32_t canary = padding->canary;
  uintptr_t keys[2];
  keys[0] = page->keys[0];
  keys[1] = page->keys[1];
  bool ok = ((uint32_t)alloc_hook_ptr_encode(page,block,keys) == canary && *delta <= *bsize);
  alloc_hook_track_mem_noaccess(padding,sizeof(alloc_hook_padding_t));
  return ok;
}

// Return the exact usable size of a block.
static size_t alloc_hook_page_usable_size_of(const alloc_hook_page_t* page, const alloc_hook_block_t* block) {
  size_t bsize;
  size_t delta;
  bool ok = alloc_hook_page_decode_padding(page, block, &delta, &bsize);
  alloc_hook_assert_internal(ok); alloc_hook_assert_internal(delta <= bsize);
  return (ok ? bsize - delta : 0);
}

// When a non-thread-local block is freed, it becomes part of the thread delayed free
// list that is freed later by the owning heap. If the exact usable size is too small to
// contain the pointer for the delayed list, then shrink the padding (by decreasing delta)
// so it will later not trigger an overflow error in `alloc_hook_free_block`.
void _alloc_hook_padding_shrink(const alloc_hook_page_t* page, const alloc_hook_block_t* block, const size_t min_size) {
  size_t bsize;
  size_t delta;
  bool ok = alloc_hook_page_decode_padding(page, block, &delta, &bsize);
  alloc_hook_assert_internal(ok);
  if (!ok || (bsize - delta) >= min_size) return;  // usually already enough space
  alloc_hook_assert_internal(bsize >= min_size);
  if (bsize < min_size) return;  // should never happen
  size_t new_delta = (bsize - min_size);
  alloc_hook_assert_internal(new_delta < bsize);
  alloc_hook_padding_t* padding = (alloc_hook_padding_t*)((uint8_t*)block + bsize);
  alloc_hook_track_mem_defined(padding,sizeof(alloc_hook_padding_t));
  padding->delta = (uint32_t)new_delta;
  alloc_hook_track_mem_noaccess(padding,sizeof(alloc_hook_padding_t));
}
#else
static size_t alloc_hook_page_usable_size_of(const alloc_hook_page_t* page, const alloc_hook_block_t* block) {
  ALLOC_HOOK_UNUSED(block);
  return alloc_hook_page_usable_block_size(page);
}

void _alloc_hook_padding_shrink(const alloc_hook_page_t* page, const alloc_hook_block_t* block, const size_t min_size) {
  ALLOC_HOOK_UNUSED(page);
  ALLOC_HOOK_UNUSED(block);
  ALLOC_HOOK_UNUSED(min_size);
}
#endif

#if ALLOC_HOOK_PADDING && ALLOC_HOOK_PADDING_CHECK

static bool alloc_hook_verify_padding(const alloc_hook_page_t* page, const alloc_hook_block_t* block, size_t* size, size_t* wrong) {
  size_t bsize;
  size_t delta;
  bool ok = alloc_hook_page_decode_padding(page, block, &delta, &bsize);
  *size = *wrong = bsize;
  if (!ok) return false;
  alloc_hook_assert_internal(bsize >= delta);
  *size = bsize - delta;
  if (!alloc_hook_page_is_huge(page)) {
    uint8_t* fill = (uint8_t*)block + bsize - delta;
    const size_t maxpad = (delta > ALLOC_HOOK_MAX_ALIGN_SIZE ? ALLOC_HOOK_MAX_ALIGN_SIZE : delta); // check at most the first N padding bytes
    alloc_hook_track_mem_defined(fill, maxpad);
    for (size_t i = 0; i < maxpad; i++) {
      if (fill[i] != ALLOC_HOOK_DEBUG_PADDING) {
        *wrong = bsize - delta + i;
        ok = false;
        break;
      }
    }
    alloc_hook_track_mem_noaccess(fill, maxpad);
  }
  return ok;
}

static void alloc_hook_check_padding(const alloc_hook_page_t* page, const alloc_hook_block_t* block) {
  size_t size;
  size_t wrong;
  if (!alloc_hook_verify_padding(page,block,&size,&wrong)) {
    _alloc_hook_error_message(EFAULT, "buffer overflow in heap block %p of size %zu: write after %zu bytes\n", block, size, wrong );
  }
}

#else

static void alloc_hook_check_padding(const alloc_hook_page_t* page, const alloc_hook_block_t* block) {
  ALLOC_HOOK_UNUSED(page);
  ALLOC_HOOK_UNUSED(block);
}

#endif

// only maintain stats for smaller objects if requested
#if (ALLOC_HOOK_STAT>0)
static void alloc_hook_stat_free(const alloc_hook_page_t* page, const alloc_hook_block_t* block) {
  #if (ALLOC_HOOK_STAT < 2)  
  ALLOC_HOOK_UNUSED(block);
  #endif
  alloc_hook_heap_t* const heap = alloc_hook_heap_get_default();
  const size_t bsize = alloc_hook_page_usable_block_size(page);
  #if (ALLOC_HOOK_STAT>1)
  const size_t usize = alloc_hook_page_usable_size_of(page, block);
  alloc_hook_heap_stat_decrease(heap, malloc, usize);
  #endif  
  if (bsize <= ALLOC_HOOK_MEDIUM_OBJ_SIZE_MAX) {
    alloc_hook_heap_stat_decrease(heap, normal, bsize);
    #if (ALLOC_HOOK_STAT > 1)
    alloc_hook_heap_stat_decrease(heap, normal_bins[_alloc_hook_bin(bsize)], 1);
    #endif
  }
  else if (bsize <= ALLOC_HOOK_LARGE_OBJ_SIZE_MAX) {
    alloc_hook_heap_stat_decrease(heap, large, bsize);
  }
  else {
    alloc_hook_heap_stat_decrease(heap, huge, bsize);
  }  
}
#else
static void alloc_hook_stat_free(const alloc_hook_page_t* page, const alloc_hook_block_t* block) {
  ALLOC_HOOK_UNUSED(page); ALLOC_HOOK_UNUSED(block);
}
#endif

#if ALLOC_HOOK_HUGE_PAGE_ABANDON
#if (ALLOC_HOOK_STAT>0)
// maintain stats for huge objects
static void alloc_hook_stat_huge_free(const alloc_hook_page_t* page) {
  alloc_hook_heap_t* const heap = alloc_hook_heap_get_default();
  const size_t bsize = alloc_hook_page_block_size(page); // to match stats in `page.c:alloc_hook_page_huge_alloc`
  if (bsize <= ALLOC_HOOK_LARGE_OBJ_SIZE_MAX) {
    alloc_hook_heap_stat_decrease(heap, large, bsize);
  }
  else {
    alloc_hook_heap_stat_decrease(heap, huge, bsize);
  }
}
#else
static void alloc_hook_stat_huge_free(const alloc_hook_page_t* page) {
  ALLOC_HOOK_UNUSED(page);
}
#endif
#endif

// ------------------------------------------------------
// Free
// ------------------------------------------------------

// multi-threaded free (or free in huge block if compiled with ALLOC_HOOK_HUGE_PAGE_ABANDON)
static alloc_hook_decl_noinline void _alloc_hook_free_block_mt(alloc_hook_page_t* page, alloc_hook_block_t* block)
{
  // The padding check may access the non-thread-owned page for the key values.
  // that is safe as these are constant and the page won't be freed (as the block is not freed yet).
  alloc_hook_check_padding(page, block);
  _alloc_hook_padding_shrink(page, block, sizeof(alloc_hook_block_t));       // for small size, ensure we can fit the delayed thread pointers without triggering overflow detection
  
  // huge page segments are always abandoned and can be freed immediately
  alloc_hook_segment_t* segment = _alloc_hook_page_segment(page);
  if (segment->kind == ALLOC_HOOK_SEGMENT_HUGE) {
    #if ALLOC_HOOK_HUGE_PAGE_ABANDON
    // huge page segments are always abandoned and can be freed immediately
    alloc_hook_stat_huge_free(page);
    _alloc_hook_segment_huge_page_free(segment, page, block);
    return;
    #else
    // huge pages are special as they occupy the entire segment
    // as these are large we reset the memory occupied by the page so it is available to other threads
    // (as the owning thread needs to actually free the memory later).
    _alloc_hook_segment_huge_page_reset(segment, page, block);
    #endif
  }
  
  #if (ALLOC_HOOK_DEBUG>0) && !ALLOC_HOOK_TRACK_ENABLED && !ALLOC_HOOK_TSAN        // note: when tracking, cannot use alloc_hook_usable_size with multi-threading
  if (segment->kind != ALLOC_HOOK_SEGMENT_HUGE) {                  // not for huge segments as we just reset the content
    memset(block, ALLOC_HOOK_DEBUG_FREED, alloc_hook_usable_size(block));
  }
  #endif

  // Try to put the block on either the page-local thread free list, or the heap delayed free list.
  alloc_hook_thread_free_t tfreex;
  bool use_delayed;
  alloc_hook_thread_free_t tfree = alloc_hook_atomic_load_relaxed(&page->xthread_free);
  do {
    use_delayed = (alloc_hook_tf_delayed(tfree) == ALLOC_HOOK_USE_DELAYED_FREE);
    if alloc_hook_unlikely(use_delayed) {
      // unlikely: this only happens on the first concurrent free in a page that is in the full list
      tfreex = alloc_hook_tf_set_delayed(tfree,ALLOC_HOOK_DELAYED_FREEING);
    }
    else {
      // usual: directly add to page thread_free list
      alloc_hook_block_set_next(page, block, alloc_hook_tf_block(tfree));
      tfreex = alloc_hook_tf_set_block(tfree,block);
    }
  } while (!alloc_hook_atomic_cas_weak_release(&page->xthread_free, &tfree, tfreex));

  if alloc_hook_unlikely(use_delayed) {
    // racy read on `heap`, but ok because ALLOC_HOOK_DELAYED_FREEING is set (see `alloc_hook_heap_delete` and `alloc_hook_heap_collect_abandon`)
    alloc_hook_heap_t* const heap = (alloc_hook_heap_t*)(alloc_hook_atomic_load_acquire(&page->xheap)); //alloc_hook_page_heap(page);
    alloc_hook_assert_internal(heap != NULL);
    if (heap != NULL) {
      // add to the delayed free list of this heap. (do this atomically as the lock only protects heap memory validity)
      alloc_hook_block_t* dfree = alloc_hook_atomic_load_ptr_relaxed(alloc_hook_block_t, &heap->thread_delayed_free);
      do {
        alloc_hook_block_set_nextx(heap,block,dfree, heap->keys);
      } while (!alloc_hook_atomic_cas_ptr_weak_release(alloc_hook_block_t,&heap->thread_delayed_free, &dfree, block));
    }

    // and reset the ALLOC_HOOK_DELAYED_FREEING flag
    tfree = alloc_hook_atomic_load_relaxed(&page->xthread_free);
    do {
      tfreex = tfree;
      alloc_hook_assert_internal(alloc_hook_tf_delayed(tfree) == ALLOC_HOOK_DELAYED_FREEING);
      tfreex = alloc_hook_tf_set_delayed(tfree,ALLOC_HOOK_NO_DELAYED_FREE);
    } while (!alloc_hook_atomic_cas_weak_release(&page->xthread_free, &tfree, tfreex));
  }
}

// regular free
static inline void _alloc_hook_free_block(alloc_hook_page_t* page, bool local, alloc_hook_block_t* block)
{
  // and push it on the free list
  //const size_t bsize = alloc_hook_page_block_size(page);
  if alloc_hook_likely(local) {
    // owning thread can free a block directly
    if alloc_hook_unlikely(alloc_hook_check_is_double_free(page, block)) return;
    alloc_hook_check_padding(page, block);
    #if (ALLOC_HOOK_DEBUG>0) && !ALLOC_HOOK_TRACK_ENABLED && !ALLOC_HOOK_TSAN
    if (!alloc_hook_page_is_huge(page)) {   // huge page content may be already decommitted
      memset(block, ALLOC_HOOK_DEBUG_FREED, alloc_hook_page_block_size(page));
    }
    #endif
    alloc_hook_block_set_next(page, block, page->local_free);
    page->local_free = block;
    page->used--;
    if alloc_hook_unlikely(alloc_hook_page_all_free(page)) {
      _alloc_hook_page_retire(page);
    }
    else if alloc_hook_unlikely(alloc_hook_page_is_in_full(page)) {
      _alloc_hook_page_unfull(page);
    }
  }
  else {
    _alloc_hook_free_block_mt(page,block);
  }
}


// Adjust a block that was allocated aligned, to the actual start of the block in the page.
alloc_hook_block_t* _alloc_hook_page_ptr_unalign(const alloc_hook_segment_t* segment, const alloc_hook_page_t* page, const void* p) {
  alloc_hook_assert_internal(page!=NULL && p!=NULL);
  const size_t diff   = (uint8_t*)p - _alloc_hook_page_start(segment, page, NULL);
  const size_t adjust = (diff % alloc_hook_page_block_size(page));
  return (alloc_hook_block_t*)((uintptr_t)p - adjust);
}


void alloc_hook_decl_noinline _alloc_hook_free_generic(const alloc_hook_segment_t* segment, alloc_hook_page_t* page, bool is_local, void* p) alloc_hook_attr_noexcept {
  alloc_hook_block_t* const block = (alloc_hook_page_has_aligned(page) ? _alloc_hook_page_ptr_unalign(segment, page, p) : (alloc_hook_block_t*)p);
  alloc_hook_stat_free(page, block);    // stat_free may access the padding
  alloc_hook_track_free_size(block, alloc_hook_page_usable_size_of(page,block));
  _alloc_hook_free_block(page, is_local, block);
}

// Get the segment data belonging to a pointer
// This is just a single `and` in assembly but does further checks in debug mode
// (and secure mode) if this was a valid pointer.
static inline alloc_hook_segment_t* alloc_hook_checked_ptr_segment(const void* p, const char* msg)
{
  ALLOC_HOOK_UNUSED(msg);
  alloc_hook_assert(p != NULL);

#if (ALLOC_HOOK_DEBUG>0)
  if alloc_hook_unlikely(((uintptr_t)p & (ALLOC_HOOK_INTPTR_SIZE - 1)) != 0) {
    _alloc_hook_error_message(EINVAL, "%s: invalid (unaligned) pointer: %p\n", msg, p);
    return NULL;
  }
#endif

  alloc_hook_segment_t* const segment = _alloc_hook_ptr_segment(p);
  alloc_hook_assert_internal(segment != NULL);

#if (ALLOC_HOOK_DEBUG>0)
  if alloc_hook_unlikely(!alloc_hook_is_in_heap_region(p)) {
  #if (ALLOC_HOOK_INTPTR_SIZE == 8 && defined(__linux__))
    if (((uintptr_t)p >> 40) != 0x7F) { // linux tends to align large blocks above 0x7F000000000 (issue #640)
  #else
    {
  #endif
      _alloc_hook_warning_message("%s: pointer might not point to a valid heap region: %p\n"
        "(this may still be a valid very large allocation (over 64MiB))\n", msg, p);
      if alloc_hook_likely(_alloc_hook_ptr_cookie(segment) == segment->cookie) {
        _alloc_hook_warning_message("(yes, the previous pointer %p was valid after all)\n", p);
      }
    }
  }
#endif
#if (ALLOC_HOOK_DEBUG>0 || ALLOC_HOOK_SECURE>=4)
  if alloc_hook_unlikely(_alloc_hook_ptr_cookie(segment) != segment->cookie) {
    _alloc_hook_error_message(EINVAL, "%s: pointer does not point to a valid heap space: %p\n", msg, p);
    return NULL;
  }
#endif

  return segment;
}

// Free a block
// fast path written carefully to prevent spilling on the stack
void alloc_hook_free(void* p) alloc_hook_attr_noexcept
{
  if alloc_hook_unlikely(p == NULL) return;
  alloc_hook_segment_t* const segment = alloc_hook_checked_ptr_segment(p,"alloc_hook_free");
  const bool          is_local= (_alloc_hook_prim_thread_id() == alloc_hook_atomic_load_relaxed(&segment->thread_id));
  alloc_hook_page_t* const    page    = _alloc_hook_segment_page_of(segment, p);

  if alloc_hook_likely(is_local) {                       // thread-local free?
    if alloc_hook_likely(page->flags.full_aligned == 0)  // and it is not a full page (full pages need to move from the full bin), nor has aligned blocks (aligned blocks need to be unaligned)
    {
      alloc_hook_block_t* const block = (alloc_hook_block_t*)p;
      if alloc_hook_unlikely(alloc_hook_check_is_double_free(page, block)) return;
      alloc_hook_check_padding(page, block);
      alloc_hook_stat_free(page, block);
      #if (ALLOC_HOOK_DEBUG>0) && !ALLOC_HOOK_TRACK_ENABLED  && !ALLOC_HOOK_TSAN
      memset(block, ALLOC_HOOK_DEBUG_FREED, alloc_hook_page_block_size(page));
      #endif
      alloc_hook_track_free_size(p, alloc_hook_page_usable_size_of(page,block)); // faster then alloc_hook_usable_size as we already know the page and that p is unaligned
      alloc_hook_block_set_next(page, block, page->local_free);
      page->local_free = block;
      if alloc_hook_unlikely(--page->used == 0) {   // using this expression generates better code than: page->used--; if (alloc_hook_page_all_free(page))
        _alloc_hook_page_retire(page);
      }
    }
    else {
      // page is full or contains (inner) aligned blocks; use generic path
      _alloc_hook_free_generic(segment, page, true, p);
    }
  }
  else {
    // not thread-local; use generic path
    _alloc_hook_free_generic(segment, page, false, p);
  }
}

// return true if successful
bool _alloc_hook_free_delayed_block(alloc_hook_block_t* block) {
  // get segment and page
  const alloc_hook_segment_t* const segment = _alloc_hook_ptr_segment(block);
  alloc_hook_assert_internal(_alloc_hook_ptr_cookie(segment) == segment->cookie);
  alloc_hook_assert_internal(_alloc_hook_thread_id() == segment->thread_id);
  alloc_hook_page_t* const page = _alloc_hook_segment_page_of(segment, block);

  // Clear the no-delayed flag so delayed freeing is used again for this page.
  // This must be done before collecting the free lists on this page -- otherwise
  // some blocks may end up in the page `thread_free` list with no blocks in the
  // heap `thread_delayed_free` list which may cause the page to be never freed!
  // (it would only be freed if we happen to scan it in `alloc_hook_page_queue_find_free_ex`)
  if (!_alloc_hook_page_try_use_delayed_free(page, ALLOC_HOOK_USE_DELAYED_FREE, false /* dont overwrite never delayed */)) {
    return false;
  }

  // collect all other non-local frees to ensure up-to-date `used` count
  _alloc_hook_page_free_collect(page, false);

  // and free the block (possibly freeing the page as well since used is updated)
  _alloc_hook_free_block(page, true, block);
  return true;
}

// Bytes available in a block
alloc_hook_decl_noinline static size_t alloc_hook_page_usable_aligned_size_of(const alloc_hook_segment_t* segment, const alloc_hook_page_t* page, const void* p) alloc_hook_attr_noexcept {
  const alloc_hook_block_t* block = _alloc_hook_page_ptr_unalign(segment, page, p);
  const size_t size = alloc_hook_page_usable_size_of(page, block);
  const ptrdiff_t adjust = (uint8_t*)p - (uint8_t*)block;
  alloc_hook_assert_internal(adjust >= 0 && (size_t)adjust <= size);
  return (size - adjust);
}

static inline size_t _alloc_hook_usable_size(const void* p, const char* msg) alloc_hook_attr_noexcept {
  if (p == NULL) return 0;
  const alloc_hook_segment_t* const segment = alloc_hook_checked_ptr_segment(p, msg);
  const alloc_hook_page_t* const page = _alloc_hook_segment_page_of(segment, p);
  if alloc_hook_likely(!alloc_hook_page_has_aligned(page)) {
    const alloc_hook_block_t* block = (const alloc_hook_block_t*)p;
    return alloc_hook_page_usable_size_of(page, block);
  }
  else {
    // split out to separate routine for improved code generation
    return alloc_hook_page_usable_aligned_size_of(segment, page, p);
  }
}

alloc_hook_decl_nodiscard size_t alloc_hook_usable_size(const void* p) alloc_hook_attr_noexcept {
  return _alloc_hook_usable_size(p, "alloc_hook_usable_size");
}


// ------------------------------------------------------
// Allocation extensions
// ------------------------------------------------------

void alloc_hook_free_size(void* p, size_t size) alloc_hook_attr_noexcept {
  ALLOC_HOOK_UNUSED_RELEASE(size);
  alloc_hook_assert(p == NULL || size <= _alloc_hook_usable_size(p,"alloc_hook_free_size"));
  alloc_hook_free(p);
}

void alloc_hook_free_size_aligned(void* p, size_t size, size_t alignment) alloc_hook_attr_noexcept {
  ALLOC_HOOK_UNUSED_RELEASE(alignment);
  alloc_hook_assert(((uintptr_t)p % alignment) == 0);
  alloc_hook_free_size(p,size);
}

void alloc_hook_free_aligned(void* p, size_t alignment) alloc_hook_attr_noexcept {
  ALLOC_HOOK_UNUSED_RELEASE(alignment);
  alloc_hook_assert(((uintptr_t)p % alignment) == 0);
  alloc_hook_free(p);
}

alloc_hook_decl_nodiscard extern inline alloc_hook_decl_restrict void* alloc_hook_heap_calloc(alloc_hook_heap_t* heap, size_t count, size_t size) alloc_hook_attr_noexcept {
  size_t total;
  if (alloc_hook_count_size_overflow(count,size,&total)) return NULL;
  return alloc_hook_heap_zalloc(heap,total);
}

alloc_hook_decl_nodiscard alloc_hook_decl_restrict void* alloc_hook_calloc(size_t count, size_t size) alloc_hook_attr_noexcept {
  return alloc_hook_heap_calloc(alloc_hook_prim_get_default_heap(),count,size);
}

// Uninitialized `calloc`
alloc_hook_decl_nodiscard extern alloc_hook_decl_restrict void* alloc_hook_heap_mallocn(alloc_hook_heap_t* heap, size_t count, size_t size) alloc_hook_attr_noexcept {
  size_t total;
  if (alloc_hook_count_size_overflow(count, size, &total)) return NULL;
  return alloc_hook_heap_malloc(heap, total);
}

alloc_hook_decl_nodiscard alloc_hook_decl_restrict void* alloc_hook_mallocn(size_t count, size_t size) alloc_hook_attr_noexcept {
  return alloc_hook_heap_mallocn(alloc_hook_prim_get_default_heap(),count,size);
}

// Expand (or shrink) in place (or fail)
void* alloc_hook_expand(void* p, size_t newsize) alloc_hook_attr_noexcept {
  #if ALLOC_HOOK_PADDING
  // we do not shrink/expand with padding enabled
  ALLOC_HOOK_UNUSED(p); ALLOC_HOOK_UNUSED(newsize);
  return NULL;
  #else
  if (p == NULL) return NULL;
  const size_t size = _alloc_hook_usable_size(p,"alloc_hook_expand");
  if (newsize > size) return NULL;
  return p; // it fits
  #endif
}

void* _alloc_hook_heap_realloc_zero(alloc_hook_heap_t* heap, void* p, size_t newsize, bool zero) alloc_hook_attr_noexcept {
  // if p == NULL then behave as malloc.
  // else if size == 0 then reallocate to a zero-sized block (and don't return NULL, just as alloc_hook_malloc(0)).
  // (this means that returning NULL always indicates an error, and `p` will not have been freed in that case.)
  const size_t size = _alloc_hook_usable_size(p,"alloc_hook_realloc"); // also works if p == NULL (with size 0)
  if alloc_hook_unlikely(newsize <= size && newsize >= (size / 2) && newsize > 0) {  // note: newsize must be > 0 or otherwise we return NULL for realloc(NULL,0)
    alloc_hook_assert_internal(p!=NULL);
    // todo: do not track as the usable size is still the same in the free; adjust potential padding?
    // alloc_hook_track_resize(p,size,newsize)
    // if (newsize < size) { alloc_hook_track_mem_noaccess((uint8_t*)p + newsize, size - newsize); }
    return p;  // reallocation still fits and not more than 50% waste
  }
  void* newp = alloc_hook_heap_malloc(heap,newsize);
  if alloc_hook_likely(newp != NULL) {
    if (zero && newsize > size) {
      // also set last word in the previous allocation to zero to ensure any padding is zero-initialized
      const size_t start = (size >= sizeof(intptr_t) ? size - sizeof(intptr_t) : 0);
      _alloc_hook_memzero((uint8_t*)newp + start, newsize - start);
    }
    else if (newsize == 0) {
      ((uint8_t*)newp)[0] = 0; // work around for applications that expect zero-reallocation to be zero initialized (issue #725)
    }
    if alloc_hook_likely(p != NULL) {
      const size_t copysize = (newsize > size ? size : newsize);
      alloc_hook_track_mem_defined(p,copysize);  // _alloc_hook_useable_size may be too large for byte precise memory tracking..
      _alloc_hook_memcpy(newp, p, copysize);
      alloc_hook_free(p); // only free the original pointer if successful
    }
  }
  return newp;
}

alloc_hook_decl_nodiscard void* alloc_hook_heap_realloc(alloc_hook_heap_t* heap, void* p, size_t newsize) alloc_hook_attr_noexcept {
  return _alloc_hook_heap_realloc_zero(heap, p, newsize, false);
}

alloc_hook_decl_nodiscard void* alloc_hook_heap_reallocn(alloc_hook_heap_t* heap, void* p, size_t count, size_t size) alloc_hook_attr_noexcept {
  size_t total;
  if (alloc_hook_count_size_overflow(count, size, &total)) return NULL;
  return alloc_hook_heap_realloc(heap, p, total);
}


// Reallocate but free `p` on errors
alloc_hook_decl_nodiscard void* alloc_hook_heap_reallocf(alloc_hook_heap_t* heap, void* p, size_t newsize) alloc_hook_attr_noexcept {
  void* newp = alloc_hook_heap_realloc(heap, p, newsize);
  if (newp==NULL && p!=NULL) alloc_hook_free(p);
  return newp;
}

alloc_hook_decl_nodiscard void* alloc_hook_heap_rezalloc(alloc_hook_heap_t* heap, void* p, size_t newsize) alloc_hook_attr_noexcept {
  return _alloc_hook_heap_realloc_zero(heap, p, newsize, true);
}

alloc_hook_decl_nodiscard void* alloc_hook_heap_recalloc(alloc_hook_heap_t* heap, void* p, size_t count, size_t size) alloc_hook_attr_noexcept {
  size_t total;
  if (alloc_hook_count_size_overflow(count, size, &total)) return NULL;
  return alloc_hook_heap_rezalloc(heap, p, total);
}


alloc_hook_decl_nodiscard void* alloc_hook_realloc(void* p, size_t newsize) alloc_hook_attr_noexcept {
  return alloc_hook_heap_realloc(alloc_hook_prim_get_default_heap(),p,newsize);
}

alloc_hook_decl_nodiscard void* alloc_hook_reallocn(void* p, size_t count, size_t size) alloc_hook_attr_noexcept {
  return alloc_hook_heap_reallocn(alloc_hook_prim_get_default_heap(),p,count,size);
}

// Reallocate but free `p` on errors
alloc_hook_decl_nodiscard void* alloc_hook_reallocf(void* p, size_t newsize) alloc_hook_attr_noexcept {
  return alloc_hook_heap_reallocf(alloc_hook_prim_get_default_heap(),p,newsize);
}

alloc_hook_decl_nodiscard void* alloc_hook_rezalloc(void* p, size_t newsize) alloc_hook_attr_noexcept {
  return alloc_hook_heap_rezalloc(alloc_hook_prim_get_default_heap(), p, newsize);
}

alloc_hook_decl_nodiscard void* alloc_hook_recalloc(void* p, size_t count, size_t size) alloc_hook_attr_noexcept {
  return alloc_hook_heap_recalloc(alloc_hook_prim_get_default_heap(), p, count, size);
}



// ------------------------------------------------------
// strdup, strndup, and realpath
// ------------------------------------------------------

// `strdup` using alloc_hook_malloc
alloc_hook_decl_nodiscard alloc_hook_decl_restrict char* alloc_hook_heap_strdup(alloc_hook_heap_t* heap, const char* s) alloc_hook_attr_noexcept {
  if (s == NULL) return NULL;
  size_t n = strlen(s);
  char* t = (char*)alloc_hook_heap_malloc(heap,n+1);
  if (t == NULL) return NULL;
  _alloc_hook_memcpy(t, s, n);
  t[n] = 0;
  return t;
}

alloc_hook_decl_nodiscard alloc_hook_decl_restrict char* alloc_hook_strdup(const char* s) alloc_hook_attr_noexcept {
  return alloc_hook_heap_strdup(alloc_hook_prim_get_default_heap(), s);
}

// `strndup` using alloc_hook_malloc
alloc_hook_decl_nodiscard alloc_hook_decl_restrict char* alloc_hook_heap_strndup(alloc_hook_heap_t* heap, const char* s, size_t n) alloc_hook_attr_noexcept {
  if (s == NULL) return NULL;
  const char* end = (const char*)memchr(s, 0, n);  // find end of string in the first `n` characters (returns NULL if not found)
  const size_t m = (end != NULL ? (size_t)(end - s) : n);  // `m` is the minimum of `n` or the end-of-string
  alloc_hook_assert_internal(m <= n);
  char* t = (char*)alloc_hook_heap_malloc(heap, m+1);
  if (t == NULL) return NULL;
  _alloc_hook_memcpy(t, s, m);
  t[m] = 0;
  return t;
}

alloc_hook_decl_nodiscard alloc_hook_decl_restrict char* alloc_hook_strndup(const char* s, size_t n) alloc_hook_attr_noexcept {
  return alloc_hook_heap_strndup(alloc_hook_prim_get_default_heap(),s,n);
}

#ifndef __wasi__
// `realpath` using alloc_hook_malloc
#ifdef _WIN32
#ifndef PATH_MAX
#define PATH_MAX MAX_PATH
#endif
#include <windows.h>
alloc_hook_decl_nodiscard alloc_hook_decl_restrict char* alloc_hook_heap_realpath(alloc_hook_heap_t* heap, const char* fname, char* resolved_name) alloc_hook_attr_noexcept {
  // todo: use GetFullPathNameW to allow longer file names
  char buf[PATH_MAX];
  DWORD res = GetFullPathNameA(fname, PATH_MAX, (resolved_name == NULL ? buf : resolved_name), NULL);
  if (res == 0) {
    errno = GetLastError(); return NULL;
  }
  else if (res > PATH_MAX) {
    errno = EINVAL; return NULL;
  }
  else if (resolved_name != NULL) {
    return resolved_name;
  }
  else {
    return alloc_hook_heap_strndup(heap, buf, PATH_MAX);
  }
}
#else
/*
#include <unistd.h>  // pathconf
static size_t alloc_hook_path_max(void) {
  static size_t path_max = 0;
  if (path_max <= 0) {
    long m = pathconf("/",_PC_PATH_MAX);
    if (m <= 0) path_max = 4096;      // guess
    else if (m < 256) path_max = 256; // at least 256
    else path_max = m;
  }
  return path_max;
}
*/
char* alloc_hook_heap_realpath(alloc_hook_heap_t* heap, const char* fname, char* resolved_name) alloc_hook_attr_noexcept {
  if (resolved_name != NULL) {
    return realpath(fname,resolved_name);
  }
  else {
    char* rname = realpath(fname, NULL);
    if (rname == NULL) return NULL;
    char* result = alloc_hook_heap_strdup(heap, rname);
    free(rname);  // use regular free! (which may be redirected to our free but that's ok)
    return result;
  }
  /*
    const size_t n  = alloc_hook_path_max();
    char* buf = (char*)alloc_hook_malloc(n+1);
    if (buf == NULL) {
      errno = ENOMEM;
      return NULL;
    }
    char* rname  = realpath(fname,buf);
    char* result = alloc_hook_heap_strndup(heap,rname,n); // ok if `rname==NULL`
    alloc_hook_free(buf);
    return result;
  }
  */
}
#endif

alloc_hook_decl_nodiscard alloc_hook_decl_restrict char* alloc_hook_realpath(const char* fname, char* resolved_name) alloc_hook_attr_noexcept {
  return alloc_hook_heap_realpath(alloc_hook_prim_get_default_heap(),fname,resolved_name);
}
#endif

/*-------------------------------------------------------
C++ new and new_aligned
The standard requires calling into `get_new_handler` and
throwing the bad_alloc exception on failure. If we compile
with a C++ compiler we can implement this precisely. If we
use a C compiler we cannot throw a `bad_alloc` exception
but we call `exit` instead (i.e. not returning).
-------------------------------------------------------*/

#ifdef __cplusplus
#include <new>
static bool alloc_hook_try_new_handler(bool nothrow) {
  #if defined(_MSC_VER) || (__cplusplus >= 201103L)
    std::new_handler h = std::get_new_handler();
  #else
    std::new_handler h = std::set_new_handler();
    std::set_new_handler(h);
  #endif
  if (h==NULL) {
    _alloc_hook_error_message(ENOMEM, "out of memory in 'new'");
    if (!nothrow) {
      throw std::bad_alloc();
    }
    return false;
  }
  else {
    h();
    return true;
  }
}
#else
typedef void (*std_new_handler_t)(void);

#if (defined(__GNUC__) || (defined(__clang__) && !defined(_MSC_VER)))  // exclude clang-cl, see issue #631
std_new_handler_t __attribute__((weak)) _ZSt15get_new_handlerv(void) {
  return NULL;
}
static std_new_handler_t alloc_hook_get_new_handler(void) {
  return _ZSt15get_new_handlerv();
}
#else
// note: on windows we could dynamically link to `?get_new_handler@std@@YAP6AXXZXZ`.
static std_new_handler_t alloc_hook_get_new_handler() {
  return NULL;
}
#endif

static bool alloc_hook_try_new_handler(bool nothrow) {
  std_new_handler_t h = alloc_hook_get_new_handler();
  if (h==NULL) {
    _alloc_hook_error_message(ENOMEM, "out of memory in 'new'");
    if (!nothrow) {
      abort();  // cannot throw in plain C, use abort
    }
    return false;
  }
  else {
    h();
    return true;
  }
}
#endif

alloc_hook_decl_export alloc_hook_decl_noinline void* alloc_hook_heap_try_new(alloc_hook_heap_t* heap, size_t size, bool nothrow ) {
  void* p = NULL;
  while(p == NULL && alloc_hook_try_new_handler(nothrow)) {
    p = alloc_hook_heap_malloc(heap,size);
  }
  return p;
}

static alloc_hook_decl_noinline void* alloc_hook_try_new(size_t size, bool nothrow) {
  return alloc_hook_heap_try_new(alloc_hook_prim_get_default_heap(), size, nothrow);
}


alloc_hook_decl_nodiscard alloc_hook_decl_restrict void* alloc_hook_heap_alloc_new(alloc_hook_heap_t* heap, size_t size) {
  void* p = alloc_hook_heap_malloc(heap,size);
  if alloc_hook_unlikely(p == NULL) return alloc_hook_heap_try_new(heap, size, false);
  return p;
}

alloc_hook_decl_nodiscard alloc_hook_decl_restrict void* alloc_hook_new(size_t size) {
  return alloc_hook_heap_alloc_new(alloc_hook_prim_get_default_heap(), size);
}


alloc_hook_decl_nodiscard alloc_hook_decl_restrict void* alloc_hook_heap_alloc_new_n(alloc_hook_heap_t* heap, size_t count, size_t size) {
  size_t total;
  if alloc_hook_unlikely(alloc_hook_count_size_overflow(count, size, &total)) {
    alloc_hook_try_new_handler(false);  // on overflow we invoke the try_new_handler once to potentially throw std::bad_alloc
    return NULL;
  }
  else {
    return alloc_hook_heap_alloc_new(heap,total);
  }
}

alloc_hook_decl_nodiscard alloc_hook_decl_restrict void* alloc_hook_new_n(size_t count, size_t size) {
  return alloc_hook_heap_alloc_new_n(alloc_hook_prim_get_default_heap(), size, count);
}


alloc_hook_decl_nodiscard alloc_hook_decl_restrict void* alloc_hook_new_nothrow(size_t size) alloc_hook_attr_noexcept {
  void* p = alloc_hook_malloc(size);
  if alloc_hook_unlikely(p == NULL) return alloc_hook_try_new(size, true);
  return p;
}

alloc_hook_decl_nodiscard alloc_hook_decl_restrict void* alloc_hook_new_aligned(size_t size, size_t alignment) {
  void* p;
  do {
    p = alloc_hook_malloc_aligned(size, alignment);
  }
  while(p == NULL && alloc_hook_try_new_handler(false));
  return p;
}

alloc_hook_decl_nodiscard alloc_hook_decl_restrict void* alloc_hook_new_aligned_nothrow(size_t size, size_t alignment) alloc_hook_attr_noexcept {
  void* p;
  do {
    p = alloc_hook_malloc_aligned(size, alignment);
  }
  while(p == NULL && alloc_hook_try_new_handler(true));
  return p;
}

alloc_hook_decl_nodiscard void* alloc_hook_new_realloc(void* p, size_t newsize) {
  void* q;
  do {
    q = alloc_hook_realloc(p, newsize);
  } while (q == NULL && alloc_hook_try_new_handler(false));
  return q;
}

alloc_hook_decl_nodiscard void* alloc_hook_new_reallocn(void* p, size_t newcount, size_t size) {
  size_t total;
  if alloc_hook_unlikely(alloc_hook_count_size_overflow(newcount, size, &total)) {
    alloc_hook_try_new_handler(false);  // on overflow we invoke the try_new_handler once to potentially throw std::bad_alloc
    return NULL;
  }
  else {
    return alloc_hook_new_realloc(p, total);
  }
}

// ------------------------------------------------------
// ensure explicit external inline definitions are emitted!
// ------------------------------------------------------

#ifdef __cplusplus
void* _alloc_hook_externs[] = {
  (void*)&_alloc_hook_page_malloc,
  (void*)&_alloc_hook_heap_malloc_zero,
  (void*)&_alloc_hook_heap_malloc_zero_ex,
  (void*)&alloc_hook_malloc,
  (void*)&alloc_hook_malloc_small,
  (void*)&alloc_hook_zalloc_small,
  (void*)&alloc_hook_heap_malloc,
  (void*)&alloc_hook_heap_zalloc,
  (void*)&alloc_hook_heap_malloc_small,
  // (void*)&alloc_hook_heap_alloc_new,
  // (void*)&alloc_hook_heap_alloc_new_n
};
#endif
