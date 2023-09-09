/* ----------------------------------------------------------------------------
Copyright (c) 2018-2021, Microsoft Research, Daan Leijen
This is free software; you can redistribute it and/or modify it under the
terms of the MIT license. A copy of the license can be found in the file
"LICENSE" at the root of this distribution.
-----------------------------------------------------------------------------*/

#include "alloc_hook.h"
#include "alloc_hook_internal.h"
#include "alloc_hook_prim.h"  // alloc_hook_prim_get_default_heap

#include <string.h>     // memset

// ------------------------------------------------------
// Aligned Allocation
// ------------------------------------------------------

// Fallback primitive aligned allocation -- split out for better codegen
static alloc_hook_decl_noinline void* alloc_hook_heap_malloc_zero_aligned_at_fallback(alloc_hook_heap_t* const heap, const size_t size, const size_t alignment, const size_t offset, const bool zero) alloc_hook_attr_noexcept
{
  alloc_hook_assert_internal(size <= PTRDIFF_MAX);
  alloc_hook_assert_internal(alignment != 0 && _alloc_hook_is_power_of_two(alignment));

  const uintptr_t align_mask = alignment - 1;  // for any x, `(x & align_mask) == (x % alignment)`
  const size_t padsize = size + ALLOC_HOOK_PADDING_SIZE;

  // use regular allocation if it is guaranteed to fit the alignment constraints
  if (offset==0 && alignment<=padsize && padsize<=ALLOC_HOOK_MAX_ALIGN_GUARANTEE && (padsize&align_mask)==0) {
    void* p = _alloc_hook_heap_malloc_zero(heap, size, zero);
    alloc_hook_assert_internal(p == NULL || ((uintptr_t)p % alignment) == 0);
    return p;
  }

  void* p;
  size_t oversize;
  if alloc_hook_unlikely(alignment > ALLOC_HOOK_ALIGNMENT_MAX) {
    // use OS allocation for very large alignment and allocate inside a huge page (dedicated segment with 1 page)
    // This can support alignments >= ALLOC_HOOK_SEGMENT_SIZE by ensuring the object can be aligned at a point in the
    // first (and single) page such that the segment info is `ALLOC_HOOK_SEGMENT_SIZE` bytes before it (so it can be found by aligning the pointer down)
    if alloc_hook_unlikely(offset != 0) {
      // todo: cannot support offset alignment for very large alignments yet
      #if ALLOC_HOOK_DEBUG > 0
      _alloc_hook_error_message(EOVERFLOW, "aligned allocation with a very large alignment cannot be used with an alignment offset (size %zu, alignment %zu, offset %zu)\n", size, alignment, offset);
      #endif
      return NULL;
    }
    oversize = (size <= ALLOC_HOOK_SMALL_SIZE_MAX ? ALLOC_HOOK_SMALL_SIZE_MAX + 1 /* ensure we use generic malloc path */ : size);
    p = _alloc_hook_heap_malloc_zero_ex(heap, oversize, false, alignment); // the page block size should be large enough to align in the single huge page block
    // zero afterwards as only the area from the aligned_p may be committed!
    if (p == NULL) return NULL;    
  }
  else {
    // otherwise over-allocate
    oversize = size + alignment - 1;
    p = _alloc_hook_heap_malloc_zero(heap, oversize, zero);
    if (p == NULL) return NULL;
  }

  // .. and align within the allocation
  const uintptr_t poffset = ((uintptr_t)p + offset) & align_mask;
  const uintptr_t adjust  = (poffset == 0 ? 0 : alignment - poffset);
  alloc_hook_assert_internal(adjust < alignment);
  void* aligned_p = (void*)((uintptr_t)p + adjust);
  if (aligned_p != p) {
    alloc_hook_page_t* page = _alloc_hook_ptr_page(p);
    alloc_hook_page_set_has_aligned(page, true);
    _alloc_hook_padding_shrink(page, (alloc_hook_block_t*)p, adjust + size);
  }
  // todo: expand padding if overallocated ?

  alloc_hook_assert_internal(alloc_hook_page_usable_block_size(_alloc_hook_ptr_page(p)) >= adjust + size);
  alloc_hook_assert_internal(p == _alloc_hook_page_ptr_unalign(_alloc_hook_ptr_segment(aligned_p), _alloc_hook_ptr_page(aligned_p), aligned_p));
  alloc_hook_assert_internal(((uintptr_t)aligned_p + offset) % alignment == 0);
  alloc_hook_assert_internal(alloc_hook_usable_size(aligned_p)>=size);
  alloc_hook_assert_internal(alloc_hook_usable_size(p) == alloc_hook_usable_size(aligned_p)+adjust);
    
  // now zero the block if needed
  if (alignment > ALLOC_HOOK_ALIGNMENT_MAX) {
    // for the tracker, on huge aligned allocations only from the start of the large block is defined
    alloc_hook_track_mem_undefined(aligned_p, size);
    if (zero) {
      _alloc_hook_memzero_aligned(aligned_p, alloc_hook_usable_size(aligned_p));
    }
  }

  if (p != aligned_p) {
    alloc_hook_track_align(p,aligned_p,adjust,alloc_hook_usable_size(aligned_p));
  }  
  return aligned_p;
}

// Primitive aligned allocation
static void* alloc_hook_heap_malloc_zero_aligned_at(alloc_hook_heap_t* const heap, const size_t size, const size_t alignment, const size_t offset, const bool zero) alloc_hook_attr_noexcept
{
  // note: we don't require `size > offset`, we just guarantee that the address at offset is aligned regardless of the allocated size.
  if alloc_hook_unlikely(alignment == 0 || !_alloc_hook_is_power_of_two(alignment)) { // require power-of-two (see <https://en.cppreference.com/w/c/memory/aligned_alloc>)
    #if ALLOC_HOOK_DEBUG > 0
    _alloc_hook_error_message(EOVERFLOW, "aligned allocation requires the alignment to be a power-of-two (size %zu, alignment %zu)\n", size, alignment);
    #endif
    return NULL;
  }

  if alloc_hook_unlikely(size > PTRDIFF_MAX) {          // we don't allocate more than PTRDIFF_MAX (see <https://sourceware.org/ml/libc-announce/2019/msg00001.html>)
    #if ALLOC_HOOK_DEBUG > 0
    _alloc_hook_error_message(EOVERFLOW, "aligned allocation request is too large (size %zu, alignment %zu)\n", size, alignment);
    #endif
    return NULL;
  }
  const uintptr_t align_mask = alignment-1;       // for any x, `(x & align_mask) == (x % alignment)`
  const size_t padsize = size + ALLOC_HOOK_PADDING_SIZE;  // note: cannot overflow due to earlier size > PTRDIFF_MAX check

  // try first if there happens to be a small block available with just the right alignment
  if alloc_hook_likely(padsize <= ALLOC_HOOK_SMALL_SIZE_MAX && alignment <= padsize) {
    alloc_hook_page_t* page = _alloc_hook_heap_get_free_small_page(heap, padsize);
    const bool is_aligned = (((uintptr_t)page->free+offset) & align_mask)==0;
    if alloc_hook_likely(page->free != NULL && is_aligned)
    {
      #if ALLOC_HOOK_STAT>1
      alloc_hook_heap_stat_increase(heap, malloc, size);
      #endif
      void* p = _alloc_hook_page_malloc(heap, page, padsize, zero); // TODO: inline _alloc_hook_page_malloc
      alloc_hook_assert_internal(p != NULL);
      alloc_hook_assert_internal(((uintptr_t)p + offset) % alignment == 0);
      alloc_hook_track_malloc(p,size,zero);
      return p;
    }
  }
  // fallback
  return alloc_hook_heap_malloc_zero_aligned_at_fallback(heap, size, alignment, offset, zero);
}


// ------------------------------------------------------
// Optimized alloc_hook_heap_malloc_aligned / alloc_hook_malloc_aligned
// ------------------------------------------------------

alloc_hook_decl_nodiscard alloc_hook_decl_restrict void* alloc_hook_heap_malloc_aligned_at(alloc_hook_heap_t* heap, size_t size, size_t alignment, size_t offset) alloc_hook_attr_noexcept {
  return alloc_hook_heap_malloc_zero_aligned_at(heap, size, alignment, offset, false);
}

alloc_hook_decl_nodiscard alloc_hook_decl_restrict void* alloc_hook_heap_malloc_aligned(alloc_hook_heap_t* heap, size_t size, size_t alignment) alloc_hook_attr_noexcept {
  if alloc_hook_unlikely(alignment == 0 || !_alloc_hook_is_power_of_two(alignment)) return NULL;
  #if !ALLOC_HOOK_PADDING
  // without padding, any small sized allocation is naturally aligned (see also `_alloc_hook_segment_page_start`)
  if alloc_hook_likely(_alloc_hook_is_power_of_two(size) && size >= alignment && size <= ALLOC_HOOK_SMALL_SIZE_MAX)
  #else
  // with padding, we can only guarantee this for fixed alignments
  if alloc_hook_likely((alignment == sizeof(void*) || (alignment == ALLOC_HOOK_MAX_ALIGN_SIZE && size > (ALLOC_HOOK_MAX_ALIGN_SIZE/2)))
                && size <= ALLOC_HOOK_SMALL_SIZE_MAX)
  #endif
  {
    // fast path for common alignment and size
    return alloc_hook_heap_malloc_small(heap, size);
  }
  else {
    return alloc_hook_heap_malloc_aligned_at(heap, size, alignment, 0);
  }
}

// ensure a definition is emitted
#if defined(__cplusplus)
static void* _alloc_hook_heap_malloc_aligned = (void*)&alloc_hook_heap_malloc_aligned;
#endif

// ------------------------------------------------------
// Aligned Allocation
// ------------------------------------------------------

alloc_hook_decl_nodiscard alloc_hook_decl_restrict void* alloc_hook_heap_zalloc_aligned_at(alloc_hook_heap_t* heap, size_t size, size_t alignment, size_t offset) alloc_hook_attr_noexcept {
  return alloc_hook_heap_malloc_zero_aligned_at(heap, size, alignment, offset, true);
}

alloc_hook_decl_nodiscard alloc_hook_decl_restrict void* alloc_hook_heap_zalloc_aligned(alloc_hook_heap_t* heap, size_t size, size_t alignment) alloc_hook_attr_noexcept {
  return alloc_hook_heap_zalloc_aligned_at(heap, size, alignment, 0);
}

alloc_hook_decl_nodiscard alloc_hook_decl_restrict void* alloc_hook_heap_calloc_aligned_at(alloc_hook_heap_t* heap, size_t count, size_t size, size_t alignment, size_t offset) alloc_hook_attr_noexcept {
  size_t total;
  if (alloc_hook_count_size_overflow(count, size, &total)) return NULL;
  return alloc_hook_heap_zalloc_aligned_at(heap, total, alignment, offset);
}

alloc_hook_decl_nodiscard alloc_hook_decl_restrict void* alloc_hook_heap_calloc_aligned(alloc_hook_heap_t* heap, size_t count, size_t size, size_t alignment) alloc_hook_attr_noexcept {
  return alloc_hook_heap_calloc_aligned_at(heap,count,size,alignment,0);
}

alloc_hook_decl_nodiscard alloc_hook_decl_restrict void* alloc_hook_malloc_aligned_at(size_t size, size_t alignment, size_t offset) alloc_hook_attr_noexcept {
  return alloc_hook_heap_malloc_aligned_at(alloc_hook_prim_get_default_heap(), size, alignment, offset);
}

alloc_hook_decl_nodiscard alloc_hook_decl_restrict void* alloc_hook_malloc_aligned(size_t size, size_t alignment) alloc_hook_attr_noexcept {
  return alloc_hook_heap_malloc_aligned(alloc_hook_prim_get_default_heap(), size, alignment);
}

alloc_hook_decl_nodiscard alloc_hook_decl_restrict void* alloc_hook_zalloc_aligned_at(size_t size, size_t alignment, size_t offset) alloc_hook_attr_noexcept {
  return alloc_hook_heap_zalloc_aligned_at(alloc_hook_prim_get_default_heap(), size, alignment, offset);
}

alloc_hook_decl_nodiscard alloc_hook_decl_restrict void* alloc_hook_zalloc_aligned(size_t size, size_t alignment) alloc_hook_attr_noexcept {
  return alloc_hook_heap_zalloc_aligned(alloc_hook_prim_get_default_heap(), size, alignment);
}

alloc_hook_decl_nodiscard alloc_hook_decl_restrict void* alloc_hook_calloc_aligned_at(size_t count, size_t size, size_t alignment, size_t offset) alloc_hook_attr_noexcept {
  return alloc_hook_heap_calloc_aligned_at(alloc_hook_prim_get_default_heap(), count, size, alignment, offset);
}

alloc_hook_decl_nodiscard alloc_hook_decl_restrict void* alloc_hook_calloc_aligned(size_t count, size_t size, size_t alignment) alloc_hook_attr_noexcept {
  return alloc_hook_heap_calloc_aligned(alloc_hook_prim_get_default_heap(), count, size, alignment);
}


// ------------------------------------------------------
// Aligned re-allocation
// ------------------------------------------------------

static void* alloc_hook_heap_realloc_zero_aligned_at(alloc_hook_heap_t* heap, void* p, size_t newsize, size_t alignment, size_t offset, bool zero) alloc_hook_attr_noexcept {
  alloc_hook_assert(alignment > 0);
  if (alignment <= sizeof(uintptr_t)) return _alloc_hook_heap_realloc_zero(heap,p,newsize,zero);
  if (p == NULL) return alloc_hook_heap_malloc_zero_aligned_at(heap,newsize,alignment,offset,zero);
  size_t size = alloc_hook_usable_size(p);
  if (newsize <= size && newsize >= (size - (size / 2))
      && (((uintptr_t)p + offset) % alignment) == 0) {
    return p;  // reallocation still fits, is aligned and not more than 50% waste
  }
  else {
    // note: we don't zero allocate upfront so we only zero initialize the expanded part
    void* newp = alloc_hook_heap_malloc_aligned_at(heap,newsize,alignment,offset);
    if (newp != NULL) {
      if (zero && newsize > size) {
        // also set last word in the previous allocation to zero to ensure any padding is zero-initialized
        size_t start = (size >= sizeof(intptr_t) ? size - sizeof(intptr_t) : 0);
        _alloc_hook_memzero((uint8_t*)newp + start, newsize - start);
      }
      _alloc_hook_memcpy_aligned(newp, p, (newsize > size ? size : newsize));
      alloc_hook_free(p); // only free if successful
    }
    return newp;
  }
}

static void* alloc_hook_heap_realloc_zero_aligned(alloc_hook_heap_t* heap, void* p, size_t newsize, size_t alignment, bool zero) alloc_hook_attr_noexcept {
  alloc_hook_assert(alignment > 0);
  if (alignment <= sizeof(uintptr_t)) return _alloc_hook_heap_realloc_zero(heap,p,newsize,zero);
  size_t offset = ((uintptr_t)p % alignment); // use offset of previous allocation (p can be NULL)
  return alloc_hook_heap_realloc_zero_aligned_at(heap,p,newsize,alignment,offset,zero);
}

alloc_hook_decl_nodiscard void* alloc_hook_heap_realloc_aligned_at(alloc_hook_heap_t* heap, void* p, size_t newsize, size_t alignment, size_t offset) alloc_hook_attr_noexcept {
  return alloc_hook_heap_realloc_zero_aligned_at(heap,p,newsize,alignment,offset,false);
}

alloc_hook_decl_nodiscard void* alloc_hook_heap_realloc_aligned(alloc_hook_heap_t* heap, void* p, size_t newsize, size_t alignment) alloc_hook_attr_noexcept {
  return alloc_hook_heap_realloc_zero_aligned(heap,p,newsize,alignment,false);
}

alloc_hook_decl_nodiscard void* alloc_hook_heap_rezalloc_aligned_at(alloc_hook_heap_t* heap, void* p, size_t newsize, size_t alignment, size_t offset) alloc_hook_attr_noexcept {
  return alloc_hook_heap_realloc_zero_aligned_at(heap, p, newsize, alignment, offset, true);
}

alloc_hook_decl_nodiscard void* alloc_hook_heap_rezalloc_aligned(alloc_hook_heap_t* heap, void* p, size_t newsize, size_t alignment) alloc_hook_attr_noexcept {
  return alloc_hook_heap_realloc_zero_aligned(heap, p, newsize, alignment, true);
}

alloc_hook_decl_nodiscard void* alloc_hook_heap_recalloc_aligned_at(alloc_hook_heap_t* heap, void* p, size_t newcount, size_t size, size_t alignment, size_t offset) alloc_hook_attr_noexcept {
  size_t total;
  if (alloc_hook_count_size_overflow(newcount, size, &total)) return NULL;
  return alloc_hook_heap_rezalloc_aligned_at(heap, p, total, alignment, offset);
}

alloc_hook_decl_nodiscard void* alloc_hook_heap_recalloc_aligned(alloc_hook_heap_t* heap, void* p, size_t newcount, size_t size, size_t alignment) alloc_hook_attr_noexcept {
  size_t total;
  if (alloc_hook_count_size_overflow(newcount, size, &total)) return NULL;
  return alloc_hook_heap_rezalloc_aligned(heap, p, total, alignment);
}

alloc_hook_decl_nodiscard void* alloc_hook_realloc_aligned_at(void* p, size_t newsize, size_t alignment, size_t offset) alloc_hook_attr_noexcept {
  return alloc_hook_heap_realloc_aligned_at(alloc_hook_prim_get_default_heap(), p, newsize, alignment, offset);
}

alloc_hook_decl_nodiscard void* alloc_hook_realloc_aligned(void* p, size_t newsize, size_t alignment) alloc_hook_attr_noexcept {
  return alloc_hook_heap_realloc_aligned(alloc_hook_prim_get_default_heap(), p, newsize, alignment);
}

alloc_hook_decl_nodiscard void* alloc_hook_rezalloc_aligned_at(void* p, size_t newsize, size_t alignment, size_t offset) alloc_hook_attr_noexcept {
  return alloc_hook_heap_rezalloc_aligned_at(alloc_hook_prim_get_default_heap(), p, newsize, alignment, offset);
}

alloc_hook_decl_nodiscard void* alloc_hook_rezalloc_aligned(void* p, size_t newsize, size_t alignment) alloc_hook_attr_noexcept {
  return alloc_hook_heap_rezalloc_aligned(alloc_hook_prim_get_default_heap(), p, newsize, alignment);
}

alloc_hook_decl_nodiscard void* alloc_hook_recalloc_aligned_at(void* p, size_t newcount, size_t size, size_t alignment, size_t offset) alloc_hook_attr_noexcept {
  return alloc_hook_heap_recalloc_aligned_at(alloc_hook_prim_get_default_heap(), p, newcount, size, alignment, offset);
}

alloc_hook_decl_nodiscard void* alloc_hook_recalloc_aligned(void* p, size_t newcount, size_t size, size_t alignment) alloc_hook_attr_noexcept {
  return alloc_hook_heap_recalloc_aligned(alloc_hook_prim_get_default_heap(), p, newcount, size, alignment);
}
