/* ----------------------------------------------------------------------------
Copyright (c) 2018-2023, Microsoft Research, Daan Leijen
This is free software; you can redistribute it and/or modify it under the
terms of the MIT license. A copy of the license can be found in the file
"LICENSE" at the root of this distribution.
-----------------------------------------------------------------------------*/
#include "alloc_hook.h"
#include "alloc_hook_internal.h"
#include "alloc_hook_atomic.h"
#include "alloc_hook_prim.h"


/* -----------------------------------------------------------
  Initialization.
  On windows initializes support for aligned allocation and
  large OS pages (if ALLOC_HOOK_LARGE_OS_PAGES is true).
----------------------------------------------------------- */

static alloc_hook_os_mem_config_t alloc_hook_os_mem_config = {
  4096,   // page size
  0,      // large page size (usually 2MiB)
  4096,   // allocation granularity
  true,   // has overcommit?  (if true we use MAP_NORESERVE on mmap systems)
  false,  // must free whole? (on mmap systems we can free anywhere in a mapped range, but on Windows we must free the entire span)
  true    // has virtual reserve? (if true we can reserve virtual address space without using commit or physical memory)
};

bool _alloc_hook_os_has_overcommit(void) {
  return alloc_hook_os_mem_config.has_overcommit;
}

bool _alloc_hook_os_has_virtual_reserve(void) { 
  return alloc_hook_os_mem_config.has_virtual_reserve;
}


// OS (small) page size
size_t _alloc_hook_os_page_size(void) {
  return alloc_hook_os_mem_config.page_size;
}

// if large OS pages are supported (2 or 4MiB), then return the size, otherwise return the small page size (4KiB)
size_t _alloc_hook_os_large_page_size(void) {
  return (alloc_hook_os_mem_config.large_page_size != 0 ? alloc_hook_os_mem_config.large_page_size : _alloc_hook_os_page_size());
}

bool _alloc_hook_os_use_large_page(size_t size, size_t alignment) {
  // if we have access, check the size and alignment requirements
  if (alloc_hook_os_mem_config.large_page_size == 0 || !alloc_hook_option_is_enabled(alloc_hook_option_allow_large_os_pages)) return false;
  return ((size % alloc_hook_os_mem_config.large_page_size) == 0 && (alignment % alloc_hook_os_mem_config.large_page_size) == 0);
}

// round to a good OS allocation size (bounded by max 12.5% waste)
size_t _alloc_hook_os_good_alloc_size(size_t size) {
  size_t align_size;
  if (size < 512*ALLOC_HOOK_KiB) align_size = _alloc_hook_os_page_size();
  else if (size < 2*ALLOC_HOOK_MiB) align_size = 64*ALLOC_HOOK_KiB;
  else if (size < 8*ALLOC_HOOK_MiB) align_size = 256*ALLOC_HOOK_KiB;
  else if (size < 32*ALLOC_HOOK_MiB) align_size = 1*ALLOC_HOOK_MiB;
  else align_size = 4*ALLOC_HOOK_MiB;
  if alloc_hook_unlikely(size >= (SIZE_MAX - align_size)) return size; // possible overflow?
  return _alloc_hook_align_up(size, align_size);
}

void _alloc_hook_os_init(void) {
  _alloc_hook_prim_mem_init(&alloc_hook_os_mem_config);
}


/* -----------------------------------------------------------
  Util
-------------------------------------------------------------- */
bool _alloc_hook_os_decommit(void* addr, size_t size, alloc_hook_stats_t* stats);
bool _alloc_hook_os_commit(void* addr, size_t size, bool* is_zero, alloc_hook_stats_t* tld_stats);

static void* alloc_hook_align_up_ptr(void* p, size_t alignment) {
  return (void*)_alloc_hook_align_up((uintptr_t)p, alignment);
}

static void* alloc_hook_align_down_ptr(void* p, size_t alignment) {
  return (void*)_alloc_hook_align_down((uintptr_t)p, alignment);
}


/* -----------------------------------------------------------
  aligned hinting
-------------------------------------------------------------- */

// On 64-bit systems, we can do efficient aligned allocation by using
// the 2TiB to 30TiB area to allocate those.
#if (ALLOC_HOOK_INTPTR_SIZE >= 8)
static alloc_hook_decl_cache_align _Atomic(uintptr_t)aligned_base;

// Return a ALLOC_HOOK_SEGMENT_SIZE aligned address that is probably available.
// If this returns NULL, the OS will determine the address but on some OS's that may not be
// properly aligned which can be more costly as it needs to be adjusted afterwards.
// For a size > 1GiB this always returns NULL in order to guarantee good ASLR randomization;
// (otherwise an initial large allocation of say 2TiB has a 50% chance to include (known) addresses
//  in the middle of the 2TiB - 6TiB address range (see issue #372))

#define ALLOC_HOOK_HINT_BASE ((uintptr_t)2 << 40)  // 2TiB start
#define ALLOC_HOOK_HINT_AREA ((uintptr_t)4 << 40)  // upto 6TiB   (since before win8 there is "only" 8TiB available to processes)
#define ALLOC_HOOK_HINT_MAX  ((uintptr_t)30 << 40) // wrap after 30TiB (area after 32TiB is used for huge OS pages)

void* _alloc_hook_os_get_aligned_hint(size_t try_alignment, size_t size)
{
  if (try_alignment <= 1 || try_alignment > ALLOC_HOOK_SEGMENT_SIZE) return NULL;
  size = _alloc_hook_align_up(size, ALLOC_HOOK_SEGMENT_SIZE);
  if (size > 1*ALLOC_HOOK_GiB) return NULL;  // guarantee the chance of fixed valid address is at most 1/(ALLOC_HOOK_HINT_AREA / 1<<30) = 1/4096.
  #if (ALLOC_HOOK_SECURE>0)
  size += ALLOC_HOOK_SEGMENT_SIZE;        // put in `ALLOC_HOOK_SEGMENT_SIZE` virtual gaps between hinted blocks; this splits VLA's but increases guarded areas.
  #endif

  uintptr_t hint = alloc_hook_atomic_add_acq_rel(&aligned_base, size);
  if (hint == 0 || hint > ALLOC_HOOK_HINT_MAX) {   // wrap or initialize
    uintptr_t init = ALLOC_HOOK_HINT_BASE;
    #if (ALLOC_HOOK_SECURE>0 || ALLOC_HOOK_DEBUG==0)       // security: randomize start of aligned allocations unless in debug mode
    uintptr_t r = _alloc_hook_heap_random_next(alloc_hook_prim_get_default_heap());
    init = init + ((ALLOC_HOOK_SEGMENT_SIZE * ((r>>17) & 0xFFFFF)) % ALLOC_HOOK_HINT_AREA);  // (randomly 20 bits)*4MiB == 0 to 4TiB
    #endif
    uintptr_t expected = hint + size;
    alloc_hook_atomic_cas_strong_acq_rel(&aligned_base, &expected, init);
    hint = alloc_hook_atomic_add_acq_rel(&aligned_base, size); // this may still give 0 or > ALLOC_HOOK_HINT_MAX but that is ok, it is a hint after all
  }
  if (hint%try_alignment != 0) return NULL;
  return (void*)hint;
}
#else
void* _alloc_hook_os_get_aligned_hint(size_t try_alignment, size_t size) {
  ALLOC_HOOK_UNUSED(try_alignment); ALLOC_HOOK_UNUSED(size);
  return NULL;
}
#endif


/* -----------------------------------------------------------
  Free memory
-------------------------------------------------------------- */

static void alloc_hook_os_free_huge_os_pages(void* p, size_t size, alloc_hook_stats_t* stats);

static void alloc_hook_os_prim_free(void* addr, size_t size, bool still_committed, alloc_hook_stats_t* tld_stats) {
  ALLOC_HOOK_UNUSED(tld_stats);
  alloc_hook_assert_internal((size % _alloc_hook_os_page_size()) == 0);
  if (addr == NULL || size == 0) return; // || _alloc_hook_os_is_huge_reserved(addr)
  int err = _alloc_hook_prim_free(addr, size);
  if (err != 0) {
    _alloc_hook_warning_message("unable to free OS memory (error: %d (0x%x), size: 0x%zx bytes, address: %p)\n", err, err, size, addr);
  }
  alloc_hook_stats_t* stats = &_alloc_hook_stats_main;
  if (still_committed) { _alloc_hook_stat_decrease(&stats->committed, size); }
  _alloc_hook_stat_decrease(&stats->reserved, size);
}

void _alloc_hook_os_free_ex(void* addr, size_t size, bool still_committed, alloc_hook_memid_t memid, alloc_hook_stats_t* tld_stats) {
  if (alloc_hook_memkind_is_os(memid.memkind)) {
    size_t csize = _alloc_hook_os_good_alloc_size(size);
    void* base = addr;
    // different base? (due to alignment)
    if (memid.mem.os.base != NULL) {
      alloc_hook_assert(memid.mem.os.base <= addr);
      alloc_hook_assert((uint8_t*)memid.mem.os.base + memid.mem.os.alignment >= (uint8_t*)addr);
      base = memid.mem.os.base;
      csize += ((uint8_t*)addr - (uint8_t*)memid.mem.os.base);
    }
    // free it
    if (memid.memkind == ALLOC_HOOK_MEM_OS_HUGE) {
      alloc_hook_assert(memid.is_pinned);
      alloc_hook_os_free_huge_os_pages(base, csize, tld_stats);
    }
    else {
      alloc_hook_os_prim_free(base, csize, still_committed, tld_stats);
    }
  }
  else {
    // nothing to do 
    alloc_hook_assert(memid.memkind < ALLOC_HOOK_MEM_OS);
  }
}

void  _alloc_hook_os_free(void* p, size_t size, alloc_hook_memid_t memid, alloc_hook_stats_t* tld_stats) {
  _alloc_hook_os_free_ex(p, size, true, memid, tld_stats);
}


/* -----------------------------------------------------------
   Primitive allocation from the OS.
-------------------------------------------------------------- */

// Note: the `try_alignment` is just a hint and the returned pointer is not guaranteed to be aligned.
static void* alloc_hook_os_prim_alloc(size_t size, size_t try_alignment, bool commit, bool allow_large, bool* is_large, bool* is_zero, alloc_hook_stats_t* stats) {
  alloc_hook_assert_internal(size > 0 && (size % _alloc_hook_os_page_size()) == 0);
  alloc_hook_assert_internal(is_zero != NULL);
  alloc_hook_assert_internal(is_large != NULL);
  if (size == 0) return NULL;
  if (!commit) { allow_large = false; }
  if (try_alignment == 0) { try_alignment = 1; } // avoid 0 to ensure there will be no divide by zero when aligning

  *is_zero = false;
  void* p = NULL; 
  int err = _alloc_hook_prim_alloc(size, try_alignment, commit, allow_large, is_large, is_zero, &p);
  if (err != 0) {
    _alloc_hook_warning_message("unable to allocate OS memory (error: %d (0x%x), size: 0x%zx bytes, align: 0x%zx, commit: %d, allow large: %d)\n", err, err, size, try_alignment, commit, allow_large);
  }
  alloc_hook_stat_counter_increase(stats->mmap_calls, 1);
  if (p != NULL) {
    _alloc_hook_stat_increase(&stats->reserved, size);
    if (commit) { 
      _alloc_hook_stat_increase(&stats->committed, size); 
      // seems needed for asan (or `alloc_hook-test-api` fails)
      #ifdef ALLOC_HOOK_TRACK_ASAN
      if (*is_zero) { alloc_hook_track_mem_defined(p,size); }
               else { alloc_hook_track_mem_undefined(p,size); }
      #endif
    }    
  }
  return p;
}


// Primitive aligned allocation from the OS.
// This function guarantees the allocated memory is aligned.
static void* alloc_hook_os_prim_alloc_aligned(size_t size, size_t alignment, bool commit, bool allow_large, bool* is_large, bool* is_zero, void** base, alloc_hook_stats_t* stats) {
  alloc_hook_assert_internal(alignment >= _alloc_hook_os_page_size() && ((alignment & (alignment - 1)) == 0));
  alloc_hook_assert_internal(size > 0 && (size % _alloc_hook_os_page_size()) == 0);
  alloc_hook_assert_internal(is_large != NULL);
  alloc_hook_assert_internal(is_zero != NULL);
  alloc_hook_assert_internal(base != NULL);
  if (!commit) allow_large = false;
  if (!(alignment >= _alloc_hook_os_page_size() && ((alignment & (alignment - 1)) == 0))) return NULL;
  size = _alloc_hook_align_up(size, _alloc_hook_os_page_size());

  // try first with a hint (this will be aligned directly on Win 10+ or BSD)
  void* p = alloc_hook_os_prim_alloc(size, alignment, commit, allow_large, is_large, is_zero, stats);
  if (p == NULL) return NULL;

  // aligned already?
  if (((uintptr_t)p % alignment) == 0) {
    *base = p;
  }
  else {
    // if not aligned, free it, overallocate, and unmap around it
    _alloc_hook_warning_message("unable to allocate aligned OS memory directly, fall back to over-allocation (size: 0x%zx bytes, address: %p, alignment: 0x%zx, commit: %d)\n", size, p, alignment, commit);
    alloc_hook_os_prim_free(p, size, commit, stats);
    if (size >= (SIZE_MAX - alignment)) return NULL; // overflow
    const size_t over_size = size + alignment;

    if (alloc_hook_os_mem_config.must_free_whole) {  // win32 virtualAlloc cannot free parts of an allocate block
      // over-allocate uncommitted (virtual) memory
      p = alloc_hook_os_prim_alloc(over_size, 1 /*alignment*/, false /* commit? */, false /* allow_large */, is_large, is_zero, stats);
      if (p == NULL) return NULL;
      
      // set p to the aligned part in the full region
      // note: this is dangerous on Windows as VirtualFree needs the actual base pointer
      // this is handled though by having the `base` field in the memid's
      *base = p; // remember the base
      p = alloc_hook_align_up_ptr(p, alignment);

      // explicitly commit only the aligned part
      if (commit) {
        _alloc_hook_os_commit(p, size, NULL, stats);
      }
    }
    else  { // mmap can free inside an allocation
      // overallocate...
      p = alloc_hook_os_prim_alloc(over_size, 1, commit, false, is_large, is_zero, stats);
      if (p == NULL) return NULL;
      
      // and selectively unmap parts around the over-allocated area. (noop on sbrk)
      void* aligned_p = alloc_hook_align_up_ptr(p, alignment);
      size_t pre_size = (uint8_t*)aligned_p - (uint8_t*)p;
      size_t mid_size = _alloc_hook_align_up(size, _alloc_hook_os_page_size());
      size_t post_size = over_size - pre_size - mid_size;
      alloc_hook_assert_internal(pre_size < over_size&& post_size < over_size&& mid_size >= size);
      if (pre_size > 0)  { alloc_hook_os_prim_free(p, pre_size, commit, stats); }
      if (post_size > 0) { alloc_hook_os_prim_free((uint8_t*)aligned_p + mid_size, post_size, commit, stats); }
      // we can return the aligned pointer on `mmap` (and sbrk) systems
      p = aligned_p;
      *base = aligned_p; // since we freed the pre part, `*base == p`.      
    }
  }

  alloc_hook_assert_internal(p == NULL || (p != NULL && *base != NULL && ((uintptr_t)p % alignment) == 0));
  return p;
}


/* -----------------------------------------------------------
  OS API: alloc and alloc_aligned
----------------------------------------------------------- */

void* _alloc_hook_os_alloc(size_t size, alloc_hook_memid_t* memid, alloc_hook_stats_t* tld_stats) {
  ALLOC_HOOK_UNUSED(tld_stats);
  *memid = _alloc_hook_memid_none();
  alloc_hook_stats_t* stats = &_alloc_hook_stats_main;
  if (size == 0) return NULL;
  size = _alloc_hook_os_good_alloc_size(size);
  bool os_is_large = false;
  bool os_is_zero  = false;
  void* p = alloc_hook_os_prim_alloc(size, 0, true, false, &os_is_large, &os_is_zero, stats);
  if (p != NULL) {
    *memid = _alloc_hook_memid_create_os(true, os_is_zero, os_is_large);
  }  
  return p;
}

void* _alloc_hook_os_alloc_aligned(size_t size, size_t alignment, bool commit, bool allow_large, alloc_hook_memid_t* memid, alloc_hook_stats_t* tld_stats)
{
  ALLOC_HOOK_UNUSED(&_alloc_hook_os_get_aligned_hint); // suppress unused warnings
  ALLOC_HOOK_UNUSED(tld_stats);
  *memid = _alloc_hook_memid_none();
  if (size == 0) return NULL;
  size = _alloc_hook_os_good_alloc_size(size);
  alignment = _alloc_hook_align_up(alignment, _alloc_hook_os_page_size());
  
  bool os_is_large = false;
  bool os_is_zero  = false;
  void* os_base = NULL;
  void* p = alloc_hook_os_prim_alloc_aligned(size, alignment, commit, allow_large, &os_is_large, &os_is_zero, &os_base, &_alloc_hook_stats_main /*tld->stats*/ );
  if (p != NULL) {
    *memid = _alloc_hook_memid_create_os(commit, os_is_zero, os_is_large);
    memid->mem.os.base = os_base;
    memid->mem.os.alignment = alignment;
  }
  return p;
}

/* -----------------------------------------------------------
  OS aligned allocation with an offset. This is used
  for large alignments > ALLOC_HOOK_ALIGNMENT_MAX. We use a large alloc_hook
  page where the object can be aligned at an offset from the start of the segment.
  As we may need to overallocate, we need to free such pointers using `alloc_hook_free_aligned`
  to use the actual start of the memory region.
----------------------------------------------------------- */

void* _alloc_hook_os_alloc_aligned_at_offset(size_t size, size_t alignment, size_t offset, bool commit, bool allow_large, alloc_hook_memid_t* memid, alloc_hook_stats_t* tld_stats) {
  alloc_hook_assert(offset <= ALLOC_HOOK_SEGMENT_SIZE);
  alloc_hook_assert(offset <= size);
  alloc_hook_assert((alignment % _alloc_hook_os_page_size()) == 0);
  *memid = _alloc_hook_memid_none();
  if (offset > ALLOC_HOOK_SEGMENT_SIZE) return NULL;
  if (offset == 0) {
    // regular aligned allocation
    return _alloc_hook_os_alloc_aligned(size, alignment, commit, allow_large, memid, tld_stats);
  }
  else {
    // overallocate to align at an offset
    const size_t extra = _alloc_hook_align_up(offset, alignment) - offset;
    const size_t oversize = size + extra;
    void* const start = _alloc_hook_os_alloc_aligned(oversize, alignment, commit, allow_large, memid, tld_stats);
    if (start == NULL) return NULL;

    void* const p = (uint8_t*)start + extra;
    alloc_hook_assert(_alloc_hook_is_aligned((uint8_t*)p + offset, alignment));
    // decommit the overallocation at the start
    if (commit && extra > _alloc_hook_os_page_size()) {
      _alloc_hook_os_decommit(start, extra, tld_stats);
    }
    return p;
  }
}

/* -----------------------------------------------------------
  OS memory API: reset, commit, decommit, protect, unprotect.
----------------------------------------------------------- */

// OS page align within a given area, either conservative (pages inside the area only),
// or not (straddling pages outside the area is possible)
static void* alloc_hook_os_page_align_areax(bool conservative, void* addr, size_t size, size_t* newsize) {
  alloc_hook_assert(addr != NULL && size > 0);
  if (newsize != NULL) *newsize = 0;
  if (size == 0 || addr == NULL) return NULL;

  // page align conservatively within the range
  void* start = (conservative ? alloc_hook_align_up_ptr(addr, _alloc_hook_os_page_size())
    : alloc_hook_align_down_ptr(addr, _alloc_hook_os_page_size()));
  void* end = (conservative ? alloc_hook_align_down_ptr((uint8_t*)addr + size, _alloc_hook_os_page_size())
    : alloc_hook_align_up_ptr((uint8_t*)addr + size, _alloc_hook_os_page_size()));
  ptrdiff_t diff = (uint8_t*)end - (uint8_t*)start;
  if (diff <= 0) return NULL;

  alloc_hook_assert_internal((conservative && (size_t)diff <= size) || (!conservative && (size_t)diff >= size));
  if (newsize != NULL) *newsize = (size_t)diff;
  return start;
}

static void* alloc_hook_os_page_align_area_conservative(void* addr, size_t size, size_t* newsize) {
  return alloc_hook_os_page_align_areax(true, addr, size, newsize);
}

bool _alloc_hook_os_commit(void* addr, size_t size, bool* is_zero, alloc_hook_stats_t* tld_stats) {
  ALLOC_HOOK_UNUSED(tld_stats);
  alloc_hook_stats_t* stats = &_alloc_hook_stats_main;  
  if (is_zero != NULL) { *is_zero = false; }
  _alloc_hook_stat_increase(&stats->committed, size);  // use size for precise commit vs. decommit
  _alloc_hook_stat_counter_increase(&stats->commit_calls, 1);

  // page align range
  size_t csize;
  void* start = alloc_hook_os_page_align_areax(false /* conservative? */, addr, size, &csize);
  if (csize == 0) return true;

  // commit  
  bool os_is_zero = false;
  int err = _alloc_hook_prim_commit(start, csize, &os_is_zero); 
  if (err != 0) {
    _alloc_hook_warning_message("cannot commit OS memory (error: %d (0x%x), address: %p, size: 0x%zx bytes)\n", err, err, start, csize);
    return false;
  }
  if (os_is_zero && is_zero != NULL) { 
    *is_zero = true;
    alloc_hook_assert_expensive(alloc_hook_mem_is_zero(start, csize));
  }
  // note: the following seems required for asan (otherwise `alloc_hook-test-stress` fails)
  #ifdef ALLOC_HOOK_TRACK_ASAN
  if (os_is_zero) { alloc_hook_track_mem_defined(start,csize); }
             else { alloc_hook_track_mem_undefined(start,csize); } 
  #endif
  return true;
}

static bool alloc_hook_os_decommit_ex(void* addr, size_t size, bool* needs_recommit, alloc_hook_stats_t* tld_stats) {
  ALLOC_HOOK_UNUSED(tld_stats);
  alloc_hook_stats_t* stats = &_alloc_hook_stats_main;
  alloc_hook_assert_internal(needs_recommit!=NULL);
  _alloc_hook_stat_decrease(&stats->committed, size);

  // page align
  size_t csize;
  void* start = alloc_hook_os_page_align_area_conservative(addr, size, &csize);
  if (csize == 0) return true; 

  // decommit
  *needs_recommit = true;
  int err = _alloc_hook_prim_decommit(start,csize,needs_recommit);  
  if (err != 0) {
    _alloc_hook_warning_message("cannot decommit OS memory (error: %d (0x%x), address: %p, size: 0x%zx bytes)\n", err, err, start, csize);
  }
  alloc_hook_assert_internal(err == 0);
  return (err == 0);
}

bool _alloc_hook_os_decommit(void* addr, size_t size, alloc_hook_stats_t* tld_stats) {
  bool needs_recommit;
  return alloc_hook_os_decommit_ex(addr, size, &needs_recommit, tld_stats);
}


// Signal to the OS that the address range is no longer in use
// but may be used later again. This will release physical memory
// pages and reduce swapping while keeping the memory committed.
// We page align to a conservative area inside the range to reset.
bool _alloc_hook_os_reset(void* addr, size_t size, alloc_hook_stats_t* stats) { 
  // page align conservatively within the range
  size_t csize;
  void* start = alloc_hook_os_page_align_area_conservative(addr, size, &csize);
  if (csize == 0) return true;  // || _alloc_hook_os_is_huge_reserved(addr)
  _alloc_hook_stat_increase(&stats->reset, csize);
  _alloc_hook_stat_counter_increase(&stats->reset_calls, 1);

  #if (ALLOC_HOOK_DEBUG>1) && !ALLOC_HOOK_SECURE && !ALLOC_HOOK_TRACK_ENABLED // && !ALLOC_HOOK_TSAN
  memset(start, 0, csize); // pretend it is eagerly reset
  #endif

  int err = _alloc_hook_prim_reset(start, csize);
  if (err != 0) {
    _alloc_hook_warning_message("cannot reset OS memory (error: %d (0x%x), address: %p, size: 0x%zx bytes)\n", err, err, start, csize);
  }
  return (err == 0);
}


// either resets or decommits memory, returns true if the memory needs 
// to be recommitted if it is to be re-used later on.
bool _alloc_hook_os_purge_ex(void* p, size_t size, bool allow_reset, alloc_hook_stats_t* stats)
{
  if (alloc_hook_option_get(alloc_hook_option_purge_delay) < 0) return false;  // is purging allowed?
  _alloc_hook_stat_counter_increase(&stats->purge_calls, 1);
  _alloc_hook_stat_increase(&stats->purged, size);

  if (alloc_hook_option_is_enabled(alloc_hook_option_purge_decommits) &&   // should decommit?
      !_alloc_hook_preloading())                                   // don't decommit during preloading (unsafe)
  {
    bool needs_recommit = true;
    alloc_hook_os_decommit_ex(p, size, &needs_recommit, stats);
    return needs_recommit;   
  }
  else {
    if (allow_reset) {  // this can sometimes be not allowed if the range is not fully committed
      _alloc_hook_os_reset(p, size, stats);
    }
    return false;  // needs no recommit
  }
}

// either resets or decommits memory, returns true if the memory needs 
// to be recommitted if it is to be re-used later on.
bool _alloc_hook_os_purge(void* p, size_t size, alloc_hook_stats_t * stats) {
  return _alloc_hook_os_purge_ex(p, size, true, stats);
}

// Protect a region in memory to be not accessible.
static  bool alloc_hook_os_protectx(void* addr, size_t size, bool protect) {
  // page align conservatively within the range
  size_t csize = 0;
  void* start = alloc_hook_os_page_align_area_conservative(addr, size, &csize);
  if (csize == 0) return false;
  /*
  if (_alloc_hook_os_is_huge_reserved(addr)) {
	  _alloc_hook_warning_message("cannot mprotect memory allocated in huge OS pages\n");
  }
  */
  int err = _alloc_hook_prim_protect(start,csize,protect);
  if (err != 0) {
    _alloc_hook_warning_message("cannot %s OS memory (error: %d (0x%x), address: %p, size: 0x%zx bytes)\n", (protect ? "protect" : "unprotect"), err, err, start, csize);
  }
  return (err == 0);
}

bool _alloc_hook_os_protect(void* addr, size_t size) {
  return alloc_hook_os_protectx(addr, size, true);
}

bool _alloc_hook_os_unprotect(void* addr, size_t size) {
  return alloc_hook_os_protectx(addr, size, false);
}



/* ----------------------------------------------------------------------------
Support for allocating huge OS pages (1Gib) that are reserved up-front
and possibly associated with a specific NUMA node. (use `numa_node>=0`)
-----------------------------------------------------------------------------*/
#define ALLOC_HOOK_HUGE_OS_PAGE_SIZE  (ALLOC_HOOK_GiB)


#if (ALLOC_HOOK_INTPTR_SIZE >= 8)
// To ensure proper alignment, use our own area for huge OS pages
static alloc_hook_decl_cache_align _Atomic(uintptr_t)  alloc_hook_huge_start; // = 0

// Claim an aligned address range for huge pages
static uint8_t* alloc_hook_os_claim_huge_pages(size_t pages, size_t* total_size) {
  if (total_size != NULL) *total_size = 0;
  const size_t size = pages * ALLOC_HOOK_HUGE_OS_PAGE_SIZE;

  uintptr_t start = 0;
  uintptr_t end = 0;
  uintptr_t huge_start = alloc_hook_atomic_load_relaxed(&alloc_hook_huge_start);
  do {
    start = huge_start;
    if (start == 0) {
      // Initialize the start address after the 32TiB area
      start = ((uintptr_t)32 << 40);  // 32TiB virtual start address
    #if (ALLOC_HOOK_SECURE>0 || ALLOC_HOOK_DEBUG==0)      // security: randomize start of huge pages unless in debug mode
      uintptr_t r = _alloc_hook_heap_random_next(alloc_hook_prim_get_default_heap());
      start = start + ((uintptr_t)ALLOC_HOOK_HUGE_OS_PAGE_SIZE * ((r>>17) & 0x0FFF));  // (randomly 12bits)*1GiB == between 0 to 4TiB
    #endif
    }
    end = start + size;
    alloc_hook_assert_internal(end % ALLOC_HOOK_SEGMENT_SIZE == 0);
  } while (!alloc_hook_atomic_cas_strong_acq_rel(&alloc_hook_huge_start, &huge_start, end));

  if (total_size != NULL) *total_size = size;
  return (uint8_t*)start;
}
#else
static uint8_t* alloc_hook_os_claim_huge_pages(size_t pages, size_t* total_size) {
  ALLOC_HOOK_UNUSED(pages);
  if (total_size != NULL) *total_size = 0;
  return NULL;
}
#endif

// Allocate ALLOC_HOOK_SEGMENT_SIZE aligned huge pages
void* _alloc_hook_os_alloc_huge_os_pages(size_t pages, int numa_node, alloc_hook_msecs_t max_msecs, size_t* pages_reserved, size_t* psize, alloc_hook_memid_t* memid) {
  *memid = _alloc_hook_memid_none();
  if (psize != NULL) *psize = 0;
  if (pages_reserved != NULL) *pages_reserved = 0;
  size_t size = 0;
  uint8_t* start = alloc_hook_os_claim_huge_pages(pages, &size);
  if (start == NULL) return NULL; // or 32-bit systems

  // Allocate one page at the time but try to place them contiguously
  // We allocate one page at the time to be able to abort if it takes too long
  // or to at least allocate as many as available on the system.
  alloc_hook_msecs_t start_t = _alloc_hook_clock_start();
  size_t page = 0;
  bool all_zero = true;
  while (page < pages) {
    // allocate a page
    bool is_zero = false;
    void* addr = start + (page * ALLOC_HOOK_HUGE_OS_PAGE_SIZE);
    void* p = NULL;
    int err = _alloc_hook_prim_alloc_huge_os_pages(addr, ALLOC_HOOK_HUGE_OS_PAGE_SIZE, numa_node, &is_zero, &p);
    if (!is_zero) { all_zero = false;  }
    if (err != 0) {
      _alloc_hook_warning_message("unable to allocate huge OS page (error: %d (0x%x), address: %p, size: %zx bytes)\n", err, err, addr, ALLOC_HOOK_HUGE_OS_PAGE_SIZE);
      break;
    }

    // Did we succeed at a contiguous address?
    if (p != addr) {
      // no success, issue a warning and break
      if (p != NULL) {
        _alloc_hook_warning_message("could not allocate contiguous huge OS page %zu at %p\n", page, addr);
        alloc_hook_os_prim_free(p, ALLOC_HOOK_HUGE_OS_PAGE_SIZE, true, &_alloc_hook_stats_main);
      }
      break;
    }

    // success, record it
    page++;  // increase before timeout check (see issue #711)
    _alloc_hook_stat_increase(&_alloc_hook_stats_main.committed, ALLOC_HOOK_HUGE_OS_PAGE_SIZE);
    _alloc_hook_stat_increase(&_alloc_hook_stats_main.reserved, ALLOC_HOOK_HUGE_OS_PAGE_SIZE);

    // check for timeout
    if (max_msecs > 0) {
      alloc_hook_msecs_t elapsed = _alloc_hook_clock_end(start_t);
      if (page >= 1) {
        alloc_hook_msecs_t estimate = ((elapsed / (page+1)) * pages);
        if (estimate > 2*max_msecs) { // seems like we are going to timeout, break
          elapsed = max_msecs + 1;
        }
      }
      if (elapsed > max_msecs) {
        _alloc_hook_warning_message("huge OS page allocation timed out (after allocating %zu page(s))\n", page);
        break;
      }
    }
  }
  alloc_hook_assert_internal(page*ALLOC_HOOK_HUGE_OS_PAGE_SIZE <= size);
  if (pages_reserved != NULL) { *pages_reserved = page; }
  if (psize != NULL) { *psize = page * ALLOC_HOOK_HUGE_OS_PAGE_SIZE; }
  if (page != 0) {
    alloc_hook_assert(start != NULL);
    *memid = _alloc_hook_memid_create_os(true /* is committed */, all_zero, true /* is_large */);
    memid->memkind = ALLOC_HOOK_MEM_OS_HUGE;
    alloc_hook_assert(memid->is_pinned);
    #ifdef ALLOC_HOOK_TRACK_ASAN
    if (all_zero) { alloc_hook_track_mem_defined(start,size); }
    #endif
  }
  return (page == 0 ? NULL : start);
}

// free every huge page in a range individually (as we allocated per page)
// note: needed with VirtualAlloc but could potentially be done in one go on mmap'd systems.
static void alloc_hook_os_free_huge_os_pages(void* p, size_t size, alloc_hook_stats_t* stats) {
  if (p==NULL || size==0) return;
  uint8_t* base = (uint8_t*)p;
  while (size >= ALLOC_HOOK_HUGE_OS_PAGE_SIZE) {
    alloc_hook_os_prim_free(base, ALLOC_HOOK_HUGE_OS_PAGE_SIZE, true, stats);
    size -= ALLOC_HOOK_HUGE_OS_PAGE_SIZE;
    base += ALLOC_HOOK_HUGE_OS_PAGE_SIZE;
  }
}

/* ----------------------------------------------------------------------------
Support NUMA aware allocation
-----------------------------------------------------------------------------*/

_Atomic(size_t)  _alloc_hook_numa_node_count; // = 0   // cache the node count

size_t _alloc_hook_os_numa_node_count_get(void) {
  size_t count = alloc_hook_atomic_load_acquire(&_alloc_hook_numa_node_count);
  if (count <= 0) {
    long ncount = alloc_hook_option_get(alloc_hook_option_use_numa_nodes); // given explicitly?
    if (ncount > 0) {
      count = (size_t)ncount;
    }
    else {
      count = _alloc_hook_prim_numa_node_count(); // or detect dynamically
      if (count == 0) count = 1;
    }
    alloc_hook_atomic_store_release(&_alloc_hook_numa_node_count, count); // save it
    _alloc_hook_verbose_message("using %zd numa regions\n", count);
  }
  return count;
}

int _alloc_hook_os_numa_node_get(alloc_hook_os_tld_t* tld) {
  ALLOC_HOOK_UNUSED(tld);
  size_t numa_count = _alloc_hook_os_numa_node_count();
  if (numa_count<=1) return 0; // optimize on single numa node systems: always node 0
  // never more than the node count and >= 0
  size_t numa_node = _alloc_hook_prim_numa_node();
  if (numa_node >= numa_count) { numa_node = numa_node % numa_count; }
  return (int)numa_node;
}
