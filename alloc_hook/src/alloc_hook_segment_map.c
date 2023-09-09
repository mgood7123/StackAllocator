/* ----------------------------------------------------------------------------
Copyright (c) 2019-2023, Microsoft Research, Daan Leijen
This is free software; you can redistribute it and/or modify it under the
terms of the MIT license. A copy of the license can be found in the file
"LICENSE" at the root of this distribution.
-----------------------------------------------------------------------------*/

/* -----------------------------------------------------------
  The following functions are to reliably find the segment or
  block that encompasses any pointer p (or NULL if it is not
  in any of our segments).
  We maintain a bitmap of all memory with 1 bit per ALLOC_HOOK_SEGMENT_SIZE (64MiB)
  set to 1 if it contains the segment meta data.
----------------------------------------------------------- */
#include "alloc_hook.h"
#include "alloc_hook_internal.h"
#include "alloc_hook_atomic.h"

#if (ALLOC_HOOK_INTPTR_SIZE==8)
#define ALLOC_HOOK_MAX_ADDRESS    ((size_t)40 << 40)  // 40TB (to include huge page areas)
#else
#define ALLOC_HOOK_MAX_ADDRESS    ((size_t)2 << 30)   // 2Gb
#endif

#define ALLOC_HOOK_SEGMENT_MAP_BITS  (ALLOC_HOOK_MAX_ADDRESS / ALLOC_HOOK_SEGMENT_SIZE)
#define ALLOC_HOOK_SEGMENT_MAP_SIZE  (ALLOC_HOOK_SEGMENT_MAP_BITS / 8)
#define ALLOC_HOOK_SEGMENT_MAP_WSIZE (ALLOC_HOOK_SEGMENT_MAP_SIZE / ALLOC_HOOK_INTPTR_SIZE)

static _Atomic(uintptr_t) alloc_hook_segment_map[ALLOC_HOOK_SEGMENT_MAP_WSIZE + 1];  // 2KiB per TB with 64MiB segments

static size_t alloc_hook_segment_map_index_of(const alloc_hook_segment_t* segment, size_t* bitidx) {
  alloc_hook_assert_internal(_alloc_hook_ptr_segment(segment + 1) == segment); // is it aligned on ALLOC_HOOK_SEGMENT_SIZE?
  if ((uintptr_t)segment >= ALLOC_HOOK_MAX_ADDRESS) {
    *bitidx = 0;
    return ALLOC_HOOK_SEGMENT_MAP_WSIZE;
  }
  else {
    const uintptr_t segindex = ((uintptr_t)segment) / ALLOC_HOOK_SEGMENT_SIZE;
    *bitidx = segindex % ALLOC_HOOK_INTPTR_BITS;
    const size_t mapindex = segindex / ALLOC_HOOK_INTPTR_BITS;
    alloc_hook_assert_internal(mapindex < ALLOC_HOOK_SEGMENT_MAP_WSIZE);
    return mapindex;
  }
}

void _alloc_hook_segment_map_allocated_at(const alloc_hook_segment_t* segment) {
  size_t bitidx;
  size_t index = alloc_hook_segment_map_index_of(segment, &bitidx);
  alloc_hook_assert_internal(index <= ALLOC_HOOK_SEGMENT_MAP_WSIZE);
  if (index==ALLOC_HOOK_SEGMENT_MAP_WSIZE) return;
  uintptr_t mask = alloc_hook_atomic_load_relaxed(&alloc_hook_segment_map[index]);
  uintptr_t newmask;
  do {
    newmask = (mask | ((uintptr_t)1 << bitidx));
  } while (!alloc_hook_atomic_cas_weak_release(&alloc_hook_segment_map[index], &mask, newmask));
}

void _alloc_hook_segment_map_freed_at(const alloc_hook_segment_t* segment) {
  size_t bitidx;
  size_t index = alloc_hook_segment_map_index_of(segment, &bitidx);
  alloc_hook_assert_internal(index <= ALLOC_HOOK_SEGMENT_MAP_WSIZE);
  if (index == ALLOC_HOOK_SEGMENT_MAP_WSIZE) return;
  uintptr_t mask = alloc_hook_atomic_load_relaxed(&alloc_hook_segment_map[index]);
  uintptr_t newmask;
  do {
    newmask = (mask & ~((uintptr_t)1 << bitidx));
  } while (!alloc_hook_atomic_cas_weak_release(&alloc_hook_segment_map[index], &mask, newmask));
}

// Determine the segment belonging to a pointer or NULL if it is not in a valid segment.
static alloc_hook_segment_t* _alloc_hook_segment_of(const void* p) {
  if (p == NULL) return NULL;
  alloc_hook_segment_t* segment = _alloc_hook_ptr_segment(p);
  alloc_hook_assert_internal(segment != NULL);
  size_t bitidx;
  size_t index = alloc_hook_segment_map_index_of(segment, &bitidx);
  // fast path: for any pointer to valid small/medium/large object or first ALLOC_HOOK_SEGMENT_SIZE in huge
  const uintptr_t mask = alloc_hook_atomic_load_relaxed(&alloc_hook_segment_map[index]);
  if alloc_hook_likely((mask & ((uintptr_t)1 << bitidx)) != 0) {
    return segment; // yes, allocated by us
  }
  if (index==ALLOC_HOOK_SEGMENT_MAP_WSIZE) return NULL;

  // TODO: maintain max/min allocated range for efficiency for more efficient rejection of invalid pointers?

  // search downwards for the first segment in case it is an interior pointer
  // could be slow but searches in ALLOC_HOOK_INTPTR_SIZE * ALLOC_HOOK_SEGMENT_SIZE (512MiB) steps trough
  // valid huge objects
  // note: we could maintain a lowest index to speed up the path for invalid pointers?
  size_t lobitidx;
  size_t loindex;
  uintptr_t lobits = mask & (((uintptr_t)1 << bitidx) - 1);
  if (lobits != 0) {
    loindex = index;
    lobitidx = alloc_hook_bsr(lobits);    // lobits != 0
  }
  else if (index == 0) {
    return NULL;
  }
  else {
    alloc_hook_assert_internal(index > 0);
    uintptr_t lomask = mask;
    loindex = index;
    do {
      loindex--;  
      lomask = alloc_hook_atomic_load_relaxed(&alloc_hook_segment_map[loindex]);      
    } while (lomask != 0 && loindex > 0);
    if (lomask == 0) return NULL;
    lobitidx = alloc_hook_bsr(lomask);    // lomask != 0
  }
  alloc_hook_assert_internal(loindex < ALLOC_HOOK_SEGMENT_MAP_WSIZE);
  // take difference as the addresses could be larger than the MAX_ADDRESS space.
  size_t diff = (((index - loindex) * (8*ALLOC_HOOK_INTPTR_SIZE)) + bitidx - lobitidx) * ALLOC_HOOK_SEGMENT_SIZE;
  segment = (alloc_hook_segment_t*)((uint8_t*)segment - diff);

  if (segment == NULL) return NULL;
  alloc_hook_assert_internal((void*)segment < p);
  bool cookie_ok = (_alloc_hook_ptr_cookie(segment) == segment->cookie);
  alloc_hook_assert_internal(cookie_ok);
  if alloc_hook_unlikely(!cookie_ok) return NULL;
  if (((uint8_t*)segment + alloc_hook_segment_size(segment)) <= (uint8_t*)p) return NULL; // outside the range
  alloc_hook_assert_internal(p >= (void*)segment && (uint8_t*)p < (uint8_t*)segment + alloc_hook_segment_size(segment));
  return segment;
}

// Is this a valid pointer in our heap?
static bool  alloc_hook_is_valid_pointer(const void* p) {
  return ((_alloc_hook_segment_of(p) != NULL) || (_alloc_hook_arena_contains(p)));
}

alloc_hook_decl_nodiscard alloc_hook_decl_export bool alloc_hook_is_in_heap_region(const void* p) alloc_hook_attr_noexcept {
  return alloc_hook_is_valid_pointer(p);
}

/*
// Return the full segment range belonging to a pointer
static void* alloc_hook_segment_range_of(const void* p, size_t* size) {
  alloc_hook_segment_t* segment = _alloc_hook_segment_of(p);
  if (segment == NULL) {
    if (size != NULL) *size = 0;
    return NULL;
  }
  else {
    if (size != NULL) *size = segment->segment_size;
    return segment;
  }
  alloc_hook_assert_expensive(page == NULL || alloc_hook_segment_is_valid(_alloc_hook_page_segment(page),tld));
  alloc_hook_assert_internal(page == NULL || (alloc_hook_segment_page_size(_alloc_hook_page_segment(page)) - (ALLOC_HOOK_SECURE == 0 ? 0 : _alloc_hook_os_page_size())) >= block_size);
  alloc_hook_reset_delayed(tld);
  alloc_hook_assert_internal(page == NULL || alloc_hook_page_not_in_queue(page, tld));
  return page;
}
*/
