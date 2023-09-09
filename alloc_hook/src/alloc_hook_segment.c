/* ----------------------------------------------------------------------------
Copyright (c) 2018-2020, Microsoft Research, Daan Leijen
This is free software; you can redistribute it and/or modify it under the
terms of the MIT license. A copy of the license can be found in the file
"LICENSE" at the root of this distribution.
-----------------------------------------------------------------------------*/
#include "alloc_hook.h"
#include "alloc_hook_internal.h"
#include "alloc_hook_atomic.h"

#include <string.h>  // memset
#include <stdio.h>

#define ALLOC_HOOK_PAGE_HUGE_ALIGN   (256*1024)

static void alloc_hook_segment_try_purge(alloc_hook_segment_t* segment, bool force, alloc_hook_stats_t* stats);


// -------------------------------------------------------------------
// commit mask 
// -------------------------------------------------------------------

static bool alloc_hook_commit_mask_all_set(const alloc_hook_commit_mask_t* commit, const alloc_hook_commit_mask_t* cm) {
  for (size_t i = 0; i < ALLOC_HOOK_COMMIT_MASK_FIELD_COUNT; i++) {
    if ((commit->mask[i] & cm->mask[i]) != cm->mask[i]) return false;
  }
  return true;
}

static bool alloc_hook_commit_mask_any_set(const alloc_hook_commit_mask_t* commit, const alloc_hook_commit_mask_t* cm) {
  for (size_t i = 0; i < ALLOC_HOOK_COMMIT_MASK_FIELD_COUNT; i++) {
    if ((commit->mask[i] & cm->mask[i]) != 0) return true;
  }
  return false;
}

static void alloc_hook_commit_mask_create_intersect(const alloc_hook_commit_mask_t* commit, const alloc_hook_commit_mask_t* cm, alloc_hook_commit_mask_t* res) {
  for (size_t i = 0; i < ALLOC_HOOK_COMMIT_MASK_FIELD_COUNT; i++) {
    res->mask[i] = (commit->mask[i] & cm->mask[i]);
  }
}

static void alloc_hook_commit_mask_clear(alloc_hook_commit_mask_t* res, const alloc_hook_commit_mask_t* cm) {
  for (size_t i = 0; i < ALLOC_HOOK_COMMIT_MASK_FIELD_COUNT; i++) {
    res->mask[i] &= ~(cm->mask[i]);
  }
}

static void alloc_hook_commit_mask_set(alloc_hook_commit_mask_t* res, const alloc_hook_commit_mask_t* cm) {
  for (size_t i = 0; i < ALLOC_HOOK_COMMIT_MASK_FIELD_COUNT; i++) {
    res->mask[i] |= cm->mask[i];
  }
}

static void alloc_hook_commit_mask_create(size_t bitidx, size_t bitcount, alloc_hook_commit_mask_t* cm) {
  alloc_hook_assert_internal(bitidx < ALLOC_HOOK_COMMIT_MASK_BITS);
  alloc_hook_assert_internal((bitidx + bitcount) <= ALLOC_HOOK_COMMIT_MASK_BITS);
  if (bitcount == ALLOC_HOOK_COMMIT_MASK_BITS) {
    alloc_hook_assert_internal(bitidx==0);
    alloc_hook_commit_mask_create_full(cm);
  }
  else if (bitcount == 0) {
    alloc_hook_commit_mask_create_empty(cm);
  }
  else {
    alloc_hook_commit_mask_create_empty(cm);
    size_t i = bitidx / ALLOC_HOOK_COMMIT_MASK_FIELD_BITS;
    size_t ofs = bitidx % ALLOC_HOOK_COMMIT_MASK_FIELD_BITS;
    while (bitcount > 0) {
      alloc_hook_assert_internal(i < ALLOC_HOOK_COMMIT_MASK_FIELD_COUNT);
      size_t avail = ALLOC_HOOK_COMMIT_MASK_FIELD_BITS - ofs;
      size_t count = (bitcount > avail ? avail : bitcount);
      size_t mask = (count >= ALLOC_HOOK_COMMIT_MASK_FIELD_BITS ? ~((size_t)0) : (((size_t)1 << count) - 1) << ofs);
      cm->mask[i] = mask;
      bitcount -= count;
      ofs = 0;
      i++;
    }
  }
}

size_t _alloc_hook_commit_mask_committed_size(const alloc_hook_commit_mask_t* cm, size_t total) {
  alloc_hook_assert_internal((total%ALLOC_HOOK_COMMIT_MASK_BITS)==0);
  size_t count = 0;
  for (size_t i = 0; i < ALLOC_HOOK_COMMIT_MASK_FIELD_COUNT; i++) {
    size_t mask = cm->mask[i];
    if (~mask == 0) {
      count += ALLOC_HOOK_COMMIT_MASK_FIELD_BITS;
    }
    else {
      for (; mask != 0; mask >>= 1) {  // todo: use popcount
        if ((mask&1)!=0) count++;
      }
    }
  }
  // we use total since for huge segments each commit bit may represent a larger size
  return ((total / ALLOC_HOOK_COMMIT_MASK_BITS) * count);
}


size_t _alloc_hook_commit_mask_next_run(const alloc_hook_commit_mask_t* cm, size_t* idx) {
  size_t i = (*idx) / ALLOC_HOOK_COMMIT_MASK_FIELD_BITS;
  size_t ofs = (*idx) % ALLOC_HOOK_COMMIT_MASK_FIELD_BITS;
  size_t mask = 0;
  // find first ones
  while (i < ALLOC_HOOK_COMMIT_MASK_FIELD_COUNT) {
    mask = cm->mask[i];
    mask >>= ofs;
    if (mask != 0) {
      while ((mask&1) == 0) {
        mask >>= 1;
        ofs++;
      }
      break;
    }
    i++;
    ofs = 0;
  }
  if (i >= ALLOC_HOOK_COMMIT_MASK_FIELD_COUNT) {
    // not found
    *idx = ALLOC_HOOK_COMMIT_MASK_BITS;
    return 0;
  }
  else {
    // found, count ones
    size_t count = 0;
    *idx = (i*ALLOC_HOOK_COMMIT_MASK_FIELD_BITS) + ofs;
    do {
      alloc_hook_assert_internal(ofs < ALLOC_HOOK_COMMIT_MASK_FIELD_BITS && (mask&1) == 1);
      do {
        count++;
        mask >>= 1;
      } while ((mask&1) == 1);
      if ((((*idx + count) % ALLOC_HOOK_COMMIT_MASK_FIELD_BITS) == 0)) {
        i++;
        if (i >= ALLOC_HOOK_COMMIT_MASK_FIELD_COUNT) break;
        mask = cm->mask[i];
        ofs = 0;
      }
    } while ((mask&1) == 1);
    alloc_hook_assert_internal(count > 0);
    return count;
  }
}


/* --------------------------------------------------------------------------------
  Segment allocation

  If a  thread ends, it "abandons" pages with used blocks
  and there is an abandoned segment list whose segments can
  be reclaimed by still running threads, much like work-stealing.
-------------------------------------------------------------------------------- */


/* -----------------------------------------------------------
   Slices
----------------------------------------------------------- */


static const alloc_hook_slice_t* alloc_hook_segment_slices_end(const alloc_hook_segment_t* segment) {
  return &segment->slices[segment->slice_entries];
}

static uint8_t* alloc_hook_slice_start(const alloc_hook_slice_t* slice) {
  alloc_hook_segment_t* segment = _alloc_hook_ptr_segment(slice);
  alloc_hook_assert_internal(slice >= segment->slices && slice < alloc_hook_segment_slices_end(segment));
  return ((uint8_t*)segment + ((slice - segment->slices)*ALLOC_HOOK_SEGMENT_SLICE_SIZE));
}


/* -----------------------------------------------------------
   Bins
----------------------------------------------------------- */
// Use bit scan forward to quickly find the first zero bit if it is available

static inline size_t alloc_hook_slice_bin8(size_t slice_count) {
  if (slice_count<=1) return slice_count;
  alloc_hook_assert_internal(slice_count <= ALLOC_HOOK_SLICES_PER_SEGMENT);
  slice_count--;
  size_t s = alloc_hook_bsr(slice_count);  // slice_count > 1
  if (s <= 2) return slice_count + 1;
  size_t bin = ((s << 2) | ((slice_count >> (s - 2))&0x03)) - 4;
  return bin;
}

static inline size_t alloc_hook_slice_bin(size_t slice_count) {
  alloc_hook_assert_internal(slice_count*ALLOC_HOOK_SEGMENT_SLICE_SIZE <= ALLOC_HOOK_SEGMENT_SIZE);
  alloc_hook_assert_internal(alloc_hook_slice_bin8(ALLOC_HOOK_SLICES_PER_SEGMENT) <= ALLOC_HOOK_SEGMENT_BIN_MAX);
  size_t bin = alloc_hook_slice_bin8(slice_count);
  alloc_hook_assert_internal(bin <= ALLOC_HOOK_SEGMENT_BIN_MAX);
  return bin;
}

static inline size_t alloc_hook_slice_index(const alloc_hook_slice_t* slice) {
  alloc_hook_segment_t* segment = _alloc_hook_ptr_segment(slice);
  ptrdiff_t index = slice - segment->slices;
  alloc_hook_assert_internal(index >= 0 && index < (ptrdiff_t)segment->slice_entries);
  return index;
}


/* -----------------------------------------------------------
   Slice span queues
----------------------------------------------------------- */

static void alloc_hook_span_queue_push(alloc_hook_span_queue_t* sq, alloc_hook_slice_t* slice) {
  // todo: or push to the end?
  alloc_hook_assert_internal(slice->prev == NULL && slice->next==NULL);
  slice->prev = NULL; // paranoia
  slice->next = sq->first;
  sq->first = slice;
  if (slice->next != NULL) slice->next->prev = slice;
                     else sq->last = slice;
  slice->xblock_size = 0; // free
}

static alloc_hook_span_queue_t* alloc_hook_span_queue_for(size_t slice_count, alloc_hook_segments_tld_t* tld) {
  size_t bin = alloc_hook_slice_bin(slice_count);
  alloc_hook_span_queue_t* sq = &tld->spans[bin];
  alloc_hook_assert_internal(sq->slice_count >= slice_count);
  return sq;
}

static void alloc_hook_span_queue_delete(alloc_hook_span_queue_t* sq, alloc_hook_slice_t* slice) {
  alloc_hook_assert_internal(slice->xblock_size==0 && slice->slice_count>0 && slice->slice_offset==0);
  // should work too if the queue does not contain slice (which can happen during reclaim)
  if (slice->prev != NULL) slice->prev->next = slice->next;
  if (slice == sq->first) sq->first = slice->next;
  if (slice->next != NULL) slice->next->prev = slice->prev;
  if (slice == sq->last) sq->last = slice->prev;
  slice->prev = NULL;
  slice->next = NULL;
  slice->xblock_size = 1; // no more free
}


/* -----------------------------------------------------------
 Invariant checking
----------------------------------------------------------- */

static bool alloc_hook_slice_is_used(const alloc_hook_slice_t* slice) {
  return (slice->xblock_size > 0);
}


#if (ALLOC_HOOK_DEBUG>=3)
static bool alloc_hook_span_queue_contains(alloc_hook_span_queue_t* sq, alloc_hook_slice_t* slice) {
  for (alloc_hook_slice_t* s = sq->first; s != NULL; s = s->next) {
    if (s==slice) return true;
  }
  return false;
}

static bool alloc_hook_segment_is_valid(alloc_hook_segment_t* segment, alloc_hook_segments_tld_t* tld) {
  alloc_hook_assert_internal(segment != NULL);
  alloc_hook_assert_internal(_alloc_hook_ptr_cookie(segment) == segment->cookie);
  alloc_hook_assert_internal(segment->abandoned <= segment->used);
  alloc_hook_assert_internal(segment->thread_id == 0 || segment->thread_id == _alloc_hook_thread_id());
  alloc_hook_assert_internal(alloc_hook_commit_mask_all_set(&segment->commit_mask, &segment->purge_mask)); // can only decommit committed blocks
  //alloc_hook_assert_internal(segment->segment_info_size % ALLOC_HOOK_SEGMENT_SLICE_SIZE == 0);
  alloc_hook_slice_t* slice = &segment->slices[0];
  const alloc_hook_slice_t* end = alloc_hook_segment_slices_end(segment);
  size_t used_count = 0;
  alloc_hook_span_queue_t* sq;
  while(slice < end) {
    alloc_hook_assert_internal(slice->slice_count > 0);
    alloc_hook_assert_internal(slice->slice_offset == 0);
    size_t index = alloc_hook_slice_index(slice);
    size_t maxindex = (index + slice->slice_count >= segment->slice_entries ? segment->slice_entries : index + slice->slice_count) - 1;
    if (alloc_hook_slice_is_used(slice)) { // a page in use, we need at least MAX_SLICE_OFFSET valid back offsets
      used_count++;
      for (size_t i = 0; i <= ALLOC_HOOK_MAX_SLICE_OFFSET && index + i <= maxindex; i++) {
        alloc_hook_assert_internal(segment->slices[index + i].slice_offset == i*sizeof(alloc_hook_slice_t));
        alloc_hook_assert_internal(i==0 || segment->slices[index + i].slice_count == 0);
        alloc_hook_assert_internal(i==0 || segment->slices[index + i].xblock_size == 1);
      }
      // and the last entry as well (for coalescing)
      const alloc_hook_slice_t* last = slice + slice->slice_count - 1;
      if (last > slice && last < alloc_hook_segment_slices_end(segment)) {
        alloc_hook_assert_internal(last->slice_offset == (slice->slice_count-1)*sizeof(alloc_hook_slice_t));
        alloc_hook_assert_internal(last->slice_count == 0);
        alloc_hook_assert_internal(last->xblock_size == 1);
      }
    }
    else {  // free range of slices; only last slice needs a valid back offset
      alloc_hook_slice_t* last = &segment->slices[maxindex];
      if (segment->kind != ALLOC_HOOK_SEGMENT_HUGE || slice->slice_count <= (segment->slice_entries - segment->segment_info_slices)) {
        alloc_hook_assert_internal((uint8_t*)slice == (uint8_t*)last - last->slice_offset);
      }
      alloc_hook_assert_internal(slice == last || last->slice_count == 0 );
      alloc_hook_assert_internal(last->xblock_size == 0 || (segment->kind==ALLOC_HOOK_SEGMENT_HUGE && last->xblock_size==1));
      if (segment->kind != ALLOC_HOOK_SEGMENT_HUGE && segment->thread_id != 0) { // segment is not huge or abandoned
        sq = alloc_hook_span_queue_for(slice->slice_count,tld);
        alloc_hook_assert_internal(alloc_hook_span_queue_contains(sq,slice));
      }
    }
    slice = &segment->slices[maxindex+1];
  }
  alloc_hook_assert_internal(slice == end);
  alloc_hook_assert_internal(used_count == segment->used + 1);
  return true;
}
#endif

/* -----------------------------------------------------------
 Segment size calculations
----------------------------------------------------------- */

static size_t alloc_hook_segment_info_size(alloc_hook_segment_t* segment) {
  return segment->segment_info_slices * ALLOC_HOOK_SEGMENT_SLICE_SIZE;
}

static uint8_t* _alloc_hook_segment_page_start_from_slice(const alloc_hook_segment_t* segment, const alloc_hook_slice_t* slice, size_t xblock_size, size_t* page_size)
{
  ptrdiff_t idx = slice - segment->slices;
  size_t psize = (size_t)slice->slice_count * ALLOC_HOOK_SEGMENT_SLICE_SIZE;
  // make the start not OS page aligned for smaller blocks to avoid page/cache effects
  // note: the offset must always be an xblock_size multiple since we assume small allocations
  // are aligned (see `alloc_hook_heap_malloc_aligned`).
  size_t start_offset = 0;
  if (xblock_size >= ALLOC_HOOK_INTPTR_SIZE) {
    if (xblock_size <= 64) { start_offset = 3*xblock_size; }
    else if (xblock_size <= 512) { start_offset = xblock_size; }
  }
  if (page_size != NULL) { *page_size = psize - start_offset; }
  return (uint8_t*)segment + ((idx*ALLOC_HOOK_SEGMENT_SLICE_SIZE) + start_offset);
}

// Start of the page available memory; can be used on uninitialized pages
uint8_t* _alloc_hook_segment_page_start(const alloc_hook_segment_t* segment, const alloc_hook_page_t* page, size_t* page_size)
{
  const alloc_hook_slice_t* slice = alloc_hook_page_to_slice((alloc_hook_page_t*)page);
  uint8_t* p = _alloc_hook_segment_page_start_from_slice(segment, slice, page->xblock_size, page_size);  
  alloc_hook_assert_internal(page->xblock_size > 0 || _alloc_hook_ptr_page(p) == page);
  alloc_hook_assert_internal(_alloc_hook_ptr_segment(p) == segment);
  return p;
}


static size_t alloc_hook_segment_calculate_slices(size_t required, size_t* pre_size, size_t* info_slices) {
  size_t page_size = _alloc_hook_os_page_size();
  size_t isize     = _alloc_hook_align_up(sizeof(alloc_hook_segment_t), page_size);
  size_t guardsize = 0;
  
  if (ALLOC_HOOK_SECURE>0) {
    // in secure mode, we set up a protected page in between the segment info
    // and the page data (and one at the end of the segment)
    guardsize = page_size;
    if (required > 0) {
      required = _alloc_hook_align_up(required, ALLOC_HOOK_SEGMENT_SLICE_SIZE) + page_size;
    }
  }

  if (pre_size != NULL) *pre_size = isize;
  isize = _alloc_hook_align_up(isize + guardsize, ALLOC_HOOK_SEGMENT_SLICE_SIZE);
  if (info_slices != NULL) *info_slices = isize / ALLOC_HOOK_SEGMENT_SLICE_SIZE;
  size_t segment_size = (required==0 ? ALLOC_HOOK_SEGMENT_SIZE : _alloc_hook_align_up( required + isize + guardsize, ALLOC_HOOK_SEGMENT_SLICE_SIZE) );  
  alloc_hook_assert_internal(segment_size % ALLOC_HOOK_SEGMENT_SLICE_SIZE == 0);
  return (segment_size / ALLOC_HOOK_SEGMENT_SLICE_SIZE);
}


/* ----------------------------------------------------------------------------
Segment caches
We keep a small segment cache per thread to increase local
reuse and avoid setting/clearing guard pages in secure mode.
------------------------------------------------------------------------------- */

static void alloc_hook_segments_track_size(long segment_size, alloc_hook_segments_tld_t* tld) {
  if (segment_size>=0) _alloc_hook_stat_increase(&tld->stats->segments,1);
                  else _alloc_hook_stat_decrease(&tld->stats->segments,1);
  tld->count += (segment_size >= 0 ? 1 : -1);
  if (tld->count > tld->peak_count) tld->peak_count = tld->count;
  tld->current_size += segment_size;
  if (tld->current_size > tld->peak_size) tld->peak_size = tld->current_size;
}

static void alloc_hook_segment_os_free(alloc_hook_segment_t* segment, alloc_hook_segments_tld_t* tld) {
  segment->thread_id = 0;
  _alloc_hook_segment_map_freed_at(segment);
  alloc_hook_segments_track_size(-((long)alloc_hook_segment_size(segment)),tld);
  if (ALLOC_HOOK_SECURE>0) {
    // _alloc_hook_os_unprotect(segment, alloc_hook_segment_size(segment)); // ensure no more guard pages are set
    // unprotect the guard pages; we cannot just unprotect the whole segment size as part may be decommitted
    size_t os_pagesize = _alloc_hook_os_page_size();
    _alloc_hook_os_unprotect((uint8_t*)segment + alloc_hook_segment_info_size(segment) - os_pagesize, os_pagesize);
    uint8_t* end = (uint8_t*)segment + alloc_hook_segment_size(segment) - os_pagesize;
    _alloc_hook_os_unprotect(end, os_pagesize);
  }

  // purge delayed decommits now? (no, leave it to the arena)
  // alloc_hook_segment_try_purge(segment,true,tld->stats);
  
  const size_t size = alloc_hook_segment_size(segment);
  const size_t csize = _alloc_hook_commit_mask_committed_size(&segment->commit_mask, size);

  _alloc_hook_abandoned_await_readers();  // wait until safe to free
  _alloc_hook_arena_free(segment, alloc_hook_segment_size(segment), csize, segment->memid, tld->stats);
}

// called by threads that are terminating 
void _alloc_hook_segment_thread_collect(alloc_hook_segments_tld_t* tld) {
  ALLOC_HOOK_UNUSED(tld);
  // nothing to do
}


/* -----------------------------------------------------------
   Commit/Decommit ranges
----------------------------------------------------------- */

static void alloc_hook_segment_commit_mask(alloc_hook_segment_t* segment, bool conservative, uint8_t* p, size_t size, uint8_t** start_p, size_t* full_size, alloc_hook_commit_mask_t* cm) {
  alloc_hook_assert_internal(_alloc_hook_ptr_segment(p + 1) == segment);
  alloc_hook_assert_internal(segment->kind != ALLOC_HOOK_SEGMENT_HUGE);
  alloc_hook_commit_mask_create_empty(cm);
  if (size == 0 || size > ALLOC_HOOK_SEGMENT_SIZE || segment->kind == ALLOC_HOOK_SEGMENT_HUGE) return;
  const size_t segstart = alloc_hook_segment_info_size(segment);
  const size_t segsize = alloc_hook_segment_size(segment);
  if (p >= (uint8_t*)segment + segsize) return;

  size_t pstart = (p - (uint8_t*)segment);
  alloc_hook_assert_internal(pstart + size <= segsize);

  size_t start;
  size_t end;
  if (conservative) {
    // decommit conservative
    start = _alloc_hook_align_up(pstart, ALLOC_HOOK_COMMIT_SIZE);
    end   = _alloc_hook_align_down(pstart + size, ALLOC_HOOK_COMMIT_SIZE);
    alloc_hook_assert_internal(start >= segstart);
    alloc_hook_assert_internal(end <= segsize);
  }
  else {
    // commit liberal
    start = _alloc_hook_align_down(pstart, ALLOC_HOOK_MINIMAL_COMMIT_SIZE);
    end   = _alloc_hook_align_up(pstart + size, ALLOC_HOOK_MINIMAL_COMMIT_SIZE);
  }
  if (pstart >= segstart && start < segstart) {  // note: the mask is also calculated for an initial commit of the info area
    start = segstart;
  }
  if (end > segsize) {
    end = segsize;
  }

  alloc_hook_assert_internal(start <= pstart && (pstart + size) <= end);
  alloc_hook_assert_internal(start % ALLOC_HOOK_COMMIT_SIZE==0 && end % ALLOC_HOOK_COMMIT_SIZE == 0);
  *start_p   = (uint8_t*)segment + start;
  *full_size = (end > start ? end - start : 0);
  if (*full_size == 0) return;

  size_t bitidx = start / ALLOC_HOOK_COMMIT_SIZE;
  alloc_hook_assert_internal(bitidx < ALLOC_HOOK_COMMIT_MASK_BITS);
  
  size_t bitcount = *full_size / ALLOC_HOOK_COMMIT_SIZE; // can be 0
  if (bitidx + bitcount > ALLOC_HOOK_COMMIT_MASK_BITS) {
    _alloc_hook_warning_message("commit mask overflow: idx=%zu count=%zu start=%zx end=%zx p=0x%p size=%zu fullsize=%zu\n", bitidx, bitcount, start, end, p, size, *full_size);
  }
  alloc_hook_assert_internal((bitidx + bitcount) <= ALLOC_HOOK_COMMIT_MASK_BITS);
  alloc_hook_commit_mask_create(bitidx, bitcount, cm);
}

static bool alloc_hook_segment_commit(alloc_hook_segment_t* segment, uint8_t* p, size_t size, alloc_hook_stats_t* stats) {
  alloc_hook_assert_internal(alloc_hook_commit_mask_all_set(&segment->commit_mask, &segment->purge_mask));

  // commit liberal
  uint8_t* start = NULL;
  size_t   full_size = 0;
  alloc_hook_commit_mask_t mask;
  alloc_hook_segment_commit_mask(segment, false /* conservative? */, p, size, &start, &full_size, &mask);
  if (alloc_hook_commit_mask_is_empty(&mask) || full_size == 0) return true;

  if (!alloc_hook_commit_mask_all_set(&segment->commit_mask, &mask)) {
    // committing
    bool is_zero = false;
    alloc_hook_commit_mask_t cmask;
    alloc_hook_commit_mask_create_intersect(&segment->commit_mask, &mask, &cmask);
    _alloc_hook_stat_decrease(&_alloc_hook_stats_main.committed, _alloc_hook_commit_mask_committed_size(&cmask, ALLOC_HOOK_SEGMENT_SIZE)); // adjust for overlap
    if (!_alloc_hook_os_commit(start, full_size, &is_zero, stats)) return false;
    alloc_hook_commit_mask_set(&segment->commit_mask, &mask);
  }
  
  // increase purge expiration when using part of delayed purges -- we assume more allocations are coming soon.
  if (alloc_hook_commit_mask_any_set(&segment->purge_mask, &mask)) {
    segment->purge_expire = _alloc_hook_clock_now() + alloc_hook_option_get(alloc_hook_option_purge_delay);
  }

  // always clear any delayed purges in our range (as they are either committed now)
  alloc_hook_commit_mask_clear(&segment->purge_mask, &mask);
  return true;
}

static bool alloc_hook_segment_ensure_committed(alloc_hook_segment_t* segment, uint8_t* p, size_t size, alloc_hook_stats_t* stats) {
  alloc_hook_assert_internal(alloc_hook_commit_mask_all_set(&segment->commit_mask, &segment->purge_mask));
  // note: assumes commit_mask is always full for huge segments as otherwise the commit mask bits can overflow
  if (alloc_hook_commit_mask_is_full(&segment->commit_mask) && alloc_hook_commit_mask_is_empty(&segment->purge_mask)) return true; // fully committed
  alloc_hook_assert_internal(segment->kind != ALLOC_HOOK_SEGMENT_HUGE);
  return alloc_hook_segment_commit(segment, p, size, stats);
}

static bool alloc_hook_segment_purge(alloc_hook_segment_t* segment, uint8_t* p, size_t size, alloc_hook_stats_t* stats) {    
  alloc_hook_assert_internal(alloc_hook_commit_mask_all_set(&segment->commit_mask, &segment->purge_mask));
  if (!segment->allow_purge) return true;

  // purge conservative
  uint8_t* start = NULL;
  size_t   full_size = 0;
  alloc_hook_commit_mask_t mask;
  alloc_hook_segment_commit_mask(segment, true /* conservative? */, p, size, &start, &full_size, &mask);
  if (alloc_hook_commit_mask_is_empty(&mask) || full_size==0) return true;

  if (alloc_hook_commit_mask_any_set(&segment->commit_mask, &mask)) {
    // purging
    alloc_hook_assert_internal((void*)start != (void*)segment);
    alloc_hook_assert_internal(segment->allow_decommit);
    const bool decommitted = _alloc_hook_os_purge(start, full_size, stats);  // reset or decommit
    if (decommitted) {
      alloc_hook_commit_mask_t cmask;
      alloc_hook_commit_mask_create_intersect(&segment->commit_mask, &mask, &cmask);
      _alloc_hook_stat_increase(&_alloc_hook_stats_main.committed, full_size - _alloc_hook_commit_mask_committed_size(&cmask, ALLOC_HOOK_SEGMENT_SIZE)); // adjust for double counting 
      alloc_hook_commit_mask_clear(&segment->commit_mask, &mask);
    }        
  }
  
  // always clear any scheduled purges in our range
  alloc_hook_commit_mask_clear(&segment->purge_mask, &mask);
  return true;
}

static void alloc_hook_segment_schedule_purge(alloc_hook_segment_t* segment, uint8_t* p, size_t size, alloc_hook_stats_t* stats) {
  if (!segment->allow_purge) return;

  if (alloc_hook_option_get(alloc_hook_option_purge_delay) == 0) {
    alloc_hook_segment_purge(segment, p, size, stats);
  }
  else {
    // register for future purge in the purge mask
    uint8_t* start = NULL;
    size_t   full_size = 0;
    alloc_hook_commit_mask_t mask; 
    alloc_hook_segment_commit_mask(segment, true /*conservative*/, p, size, &start, &full_size, &mask);
    if (alloc_hook_commit_mask_is_empty(&mask) || full_size==0) return;
    
    // update delayed commit
    alloc_hook_assert_internal(segment->purge_expire > 0 || alloc_hook_commit_mask_is_empty(&segment->purge_mask));      
    alloc_hook_commit_mask_t cmask;
    alloc_hook_commit_mask_create_intersect(&segment->commit_mask, &mask, &cmask);  // only purge what is committed; span_free may try to decommit more
    alloc_hook_commit_mask_set(&segment->purge_mask, &cmask);
    alloc_hook_msecs_t now = _alloc_hook_clock_now();    
    if (segment->purge_expire == 0) {
      // no previous purgess, initialize now
      segment->purge_expire = now + alloc_hook_option_get(alloc_hook_option_purge_delay);
    }
    else if (segment->purge_expire <= now) {
      // previous purge mask already expired
      if (segment->purge_expire + alloc_hook_option_get(alloc_hook_option_purge_extend_delay) <= now) {
        alloc_hook_segment_try_purge(segment, true, stats);
      }
      else {
        segment->purge_expire = now + alloc_hook_option_get(alloc_hook_option_purge_extend_delay); // (alloc_hook_option_get(alloc_hook_option_purge_delay) / 8); // wait a tiny bit longer in case there is a series of free's
      }
    }
    else {
      // previous purge mask is not yet expired, increase the expiration by a bit.
      segment->purge_expire += alloc_hook_option_get(alloc_hook_option_purge_extend_delay);
    }
  }  
}

static void alloc_hook_segment_try_purge(alloc_hook_segment_t* segment, bool force, alloc_hook_stats_t* stats) {
  if (!segment->allow_purge || alloc_hook_commit_mask_is_empty(&segment->purge_mask)) return;
  alloc_hook_msecs_t now = _alloc_hook_clock_now();
  if (!force && now < segment->purge_expire) return;

  alloc_hook_commit_mask_t mask = segment->purge_mask;
  segment->purge_expire = 0;
  alloc_hook_commit_mask_create_empty(&segment->purge_mask);

  size_t idx;
  size_t count;
  alloc_hook_commit_mask_foreach(&mask, idx, count) {
    // if found, decommit that sequence
    if (count > 0) {
      uint8_t* p = (uint8_t*)segment + (idx*ALLOC_HOOK_COMMIT_SIZE);
      size_t size = count * ALLOC_HOOK_COMMIT_SIZE;
      alloc_hook_segment_purge(segment, p, size, stats);
    }
  }
  alloc_hook_commit_mask_foreach_end()
  alloc_hook_assert_internal(alloc_hook_commit_mask_is_empty(&segment->purge_mask));
}


/* -----------------------------------------------------------
   Span free
----------------------------------------------------------- */

static bool alloc_hook_segment_is_abandoned(alloc_hook_segment_t* segment) {
  return (segment->thread_id == 0);
}

// note: can be called on abandoned segments
static void alloc_hook_segment_span_free(alloc_hook_segment_t* segment, size_t slice_index, size_t slice_count, bool allow_purge, alloc_hook_segments_tld_t* tld) {
  alloc_hook_assert_internal(slice_index < segment->slice_entries);
  alloc_hook_span_queue_t* sq = (segment->kind == ALLOC_HOOK_SEGMENT_HUGE || alloc_hook_segment_is_abandoned(segment) 
                          ? NULL : alloc_hook_span_queue_for(slice_count,tld));
  if (slice_count==0) slice_count = 1;
  alloc_hook_assert_internal(slice_index + slice_count - 1 < segment->slice_entries);

  // set first and last slice (the intermediates can be undetermined)
  alloc_hook_slice_t* slice = &segment->slices[slice_index];
  slice->slice_count = (uint32_t)slice_count;
  alloc_hook_assert_internal(slice->slice_count == slice_count); // no overflow?
  slice->slice_offset = 0;
  if (slice_count > 1) {
    alloc_hook_slice_t* last = &segment->slices[slice_index + slice_count - 1];
    last->slice_count = 0;
    last->slice_offset = (uint32_t)(sizeof(alloc_hook_page_t)*(slice_count - 1));
    last->xblock_size = 0;
  }

  // perhaps decommit
  if (allow_purge) {
    alloc_hook_segment_schedule_purge(segment, alloc_hook_slice_start(slice), slice_count * ALLOC_HOOK_SEGMENT_SLICE_SIZE, tld->stats);
  }
  
  // and push it on the free page queue (if it was not a huge page)
  if (sq != NULL) alloc_hook_span_queue_push( sq, slice );
             else slice->xblock_size = 0; // mark huge page as free anyways
}

/*
// called from reclaim to add existing free spans
static void alloc_hook_segment_span_add_free(alloc_hook_slice_t* slice, alloc_hook_segments_tld_t* tld) {
  alloc_hook_segment_t* segment = _alloc_hook_ptr_segment(slice);
  alloc_hook_assert_internal(slice->xblock_size==0 && slice->slice_count>0 && slice->slice_offset==0);
  size_t slice_index = alloc_hook_slice_index(slice);
  alloc_hook_segment_span_free(segment,slice_index,slice->slice_count,tld);
}
*/

static void alloc_hook_segment_span_remove_from_queue(alloc_hook_slice_t* slice, alloc_hook_segments_tld_t* tld) {
  alloc_hook_assert_internal(slice->slice_count > 0 && slice->slice_offset==0 && slice->xblock_size==0);
  alloc_hook_assert_internal(_alloc_hook_ptr_segment(slice)->kind != ALLOC_HOOK_SEGMENT_HUGE);
  alloc_hook_span_queue_t* sq = alloc_hook_span_queue_for(slice->slice_count, tld);
  alloc_hook_span_queue_delete(sq, slice);
}

// note: can be called on abandoned segments
static alloc_hook_slice_t* alloc_hook_segment_span_free_coalesce(alloc_hook_slice_t* slice, alloc_hook_segments_tld_t* tld) {
  alloc_hook_assert_internal(slice != NULL && slice->slice_count > 0 && slice->slice_offset == 0);
  alloc_hook_segment_t* segment = _alloc_hook_ptr_segment(slice);
  bool is_abandoned = alloc_hook_segment_is_abandoned(segment);

  // for huge pages, just mark as free but don't add to the queues
  if (segment->kind == ALLOC_HOOK_SEGMENT_HUGE) {
    // issue #691: segment->used can be 0 if the huge page block was freed while abandoned (reclaim will get here in that case)
    alloc_hook_assert_internal((segment->used==0 && slice->xblock_size==0) || segment->used == 1);  // decreased right after this call in `alloc_hook_segment_page_clear`
    slice->xblock_size = 0;  // mark as free anyways
    // we should mark the last slice `xblock_size=0` now to maintain invariants but we skip it to 
    // avoid a possible cache miss (and the segment is about to be freed)
    return slice;
  }

  // otherwise coalesce the span and add to the free span queues
  size_t slice_count = slice->slice_count;
  alloc_hook_slice_t* next = slice + slice->slice_count;
  alloc_hook_assert_internal(next <= alloc_hook_segment_slices_end(segment));
  if (next < alloc_hook_segment_slices_end(segment) && next->xblock_size==0) {
    // free next block -- remove it from free and merge
    alloc_hook_assert_internal(next->slice_count > 0 && next->slice_offset==0);
    slice_count += next->slice_count; // extend
    if (!is_abandoned) { alloc_hook_segment_span_remove_from_queue(next, tld); }
  }
  if (slice > segment->slices) {
    alloc_hook_slice_t* prev = alloc_hook_slice_first(slice - 1);
    alloc_hook_assert_internal(prev >= segment->slices);
    if (prev->xblock_size==0) {
      // free previous slice -- remove it from free and merge
      alloc_hook_assert_internal(prev->slice_count > 0 && prev->slice_offset==0);
      slice_count += prev->slice_count;
      if (!is_abandoned) { alloc_hook_segment_span_remove_from_queue(prev, tld); }
      slice = prev;
    }
  }

  // and add the new free page
  alloc_hook_segment_span_free(segment, alloc_hook_slice_index(slice), slice_count, true, tld);
  return slice;
}



/* -----------------------------------------------------------
   Page allocation
----------------------------------------------------------- */

// Note: may still return NULL if committing the memory failed
static alloc_hook_page_t* alloc_hook_segment_span_allocate(alloc_hook_segment_t* segment, size_t slice_index, size_t slice_count, alloc_hook_segments_tld_t* tld) {
  alloc_hook_assert_internal(slice_index < segment->slice_entries);
  alloc_hook_slice_t* const slice = &segment->slices[slice_index];
  alloc_hook_assert_internal(slice->xblock_size==0 || slice->xblock_size==1);

  // commit before changing the slice data
  if (!alloc_hook_segment_ensure_committed(segment, _alloc_hook_segment_page_start_from_slice(segment, slice, 0, NULL), slice_count * ALLOC_HOOK_SEGMENT_SLICE_SIZE, tld->stats)) {
    return NULL;  // commit failed!
  }

  // convert the slices to a page
  slice->slice_offset = 0;
  slice->slice_count = (uint32_t)slice_count;
  alloc_hook_assert_internal(slice->slice_count == slice_count);
  const size_t bsize = slice_count * ALLOC_HOOK_SEGMENT_SLICE_SIZE;
  slice->xblock_size = (uint32_t)(bsize >= ALLOC_HOOK_HUGE_BLOCK_SIZE ? ALLOC_HOOK_HUGE_BLOCK_SIZE : bsize);
  alloc_hook_page_t*  page = alloc_hook_slice_to_page(slice);
  alloc_hook_assert_internal(alloc_hook_page_block_size(page) == bsize);

  // set slice back pointers for the first ALLOC_HOOK_MAX_SLICE_OFFSET entries
  size_t extra = slice_count-1;
  if (extra > ALLOC_HOOK_MAX_SLICE_OFFSET) extra = ALLOC_HOOK_MAX_SLICE_OFFSET;
  if (slice_index + extra >= segment->slice_entries) extra = segment->slice_entries - slice_index - 1;  // huge objects may have more slices than avaiable entries in the segment->slices
  
  alloc_hook_slice_t* slice_next = slice + 1;
  for (size_t i = 1; i <= extra; i++, slice_next++) {
    slice_next->slice_offset = (uint32_t)(sizeof(alloc_hook_slice_t)*i);
    slice_next->slice_count = 0;
    slice_next->xblock_size = 1;
  }

  // and also for the last one (if not set already) (the last one is needed for coalescing and for large alignments)
  // note: the cast is needed for ubsan since the index can be larger than ALLOC_HOOK_SLICES_PER_SEGMENT for huge allocations (see #543)
  alloc_hook_slice_t* last = slice + slice_count - 1;
  alloc_hook_slice_t* end = (alloc_hook_slice_t*)alloc_hook_segment_slices_end(segment);
  if (last > end) last = end;
  if (last > slice) {
    last->slice_offset = (uint32_t)(sizeof(alloc_hook_slice_t) * (last - slice));
    last->slice_count = 0;
    last->xblock_size = 1;
  }
  
  // and initialize the page
  page->is_committed = true;
  segment->used++;
  return page;
}

static void alloc_hook_segment_slice_split(alloc_hook_segment_t* segment, alloc_hook_slice_t* slice, size_t slice_count, alloc_hook_segments_tld_t* tld) {
  alloc_hook_assert_internal(_alloc_hook_ptr_segment(slice) == segment);
  alloc_hook_assert_internal(slice->slice_count >= slice_count);
  alloc_hook_assert_internal(slice->xblock_size > 0); // no more in free queue
  if (slice->slice_count <= slice_count) return;
  alloc_hook_assert_internal(segment->kind != ALLOC_HOOK_SEGMENT_HUGE);
  size_t next_index = alloc_hook_slice_index(slice) + slice_count;
  size_t next_count = slice->slice_count - slice_count;
  alloc_hook_segment_span_free(segment, next_index, next_count, false /* don't purge left-over part */, tld);
  slice->slice_count = (uint32_t)slice_count;
}

static alloc_hook_page_t* alloc_hook_segments_page_find_and_allocate(size_t slice_count, alloc_hook_arena_id_t req_arena_id, alloc_hook_segments_tld_t* tld) {
  alloc_hook_assert_internal(slice_count*ALLOC_HOOK_SEGMENT_SLICE_SIZE <= ALLOC_HOOK_LARGE_OBJ_SIZE_MAX);
  // search from best fit up
  alloc_hook_span_queue_t* sq = alloc_hook_span_queue_for(slice_count, tld);
  if (slice_count == 0) slice_count = 1;
  while (sq <= &tld->spans[ALLOC_HOOK_SEGMENT_BIN_MAX]) {
    for (alloc_hook_slice_t* slice = sq->first; slice != NULL; slice = slice->next) {
      if (slice->slice_count >= slice_count) {
        // found one
        alloc_hook_segment_t* segment = _alloc_hook_ptr_segment(slice);
        if (_alloc_hook_arena_memid_is_suitable(segment->memid, req_arena_id)) {
          // found a suitable page span
          alloc_hook_span_queue_delete(sq, slice);

          if (slice->slice_count > slice_count) {
            alloc_hook_segment_slice_split(segment, slice, slice_count, tld);
          }
          alloc_hook_assert_internal(slice != NULL && slice->slice_count == slice_count && slice->xblock_size > 0);
          alloc_hook_page_t* page = alloc_hook_segment_span_allocate(segment, alloc_hook_slice_index(slice), slice->slice_count, tld);
          if (page == NULL) {
            // commit failed; return NULL but first restore the slice
            alloc_hook_segment_span_free_coalesce(slice, tld);
            return NULL;
          }
          return page;
        }
      }
    }
    sq++;
  }
  // could not find a page..
  return NULL;
}


/* -----------------------------------------------------------
   Segment allocation
----------------------------------------------------------- */

static alloc_hook_segment_t* alloc_hook_segment_os_alloc( size_t required, size_t page_alignment, bool eager_delayed, alloc_hook_arena_id_t req_arena_id,
                                          size_t* psegment_slices, size_t* ppre_size, size_t* pinfo_slices, 
                                          bool commit, alloc_hook_segments_tld_t* tld, alloc_hook_os_tld_t* os_tld)

{
  alloc_hook_memid_t memid;
  bool   allow_large = (!eager_delayed && (ALLOC_HOOK_SECURE == 0)); // only allow large OS pages once we are no longer lazy
  size_t align_offset = 0;
  size_t alignment = ALLOC_HOOK_SEGMENT_ALIGN;
  
  if (page_alignment > 0) {
    // alloc_hook_assert_internal(huge_page != NULL);
    alloc_hook_assert_internal(page_alignment >= ALLOC_HOOK_SEGMENT_ALIGN);
    alignment = page_alignment;
    const size_t info_size = (*pinfo_slices) * ALLOC_HOOK_SEGMENT_SLICE_SIZE;
    align_offset = _alloc_hook_align_up( info_size, ALLOC_HOOK_SEGMENT_ALIGN );
    const size_t extra = align_offset - info_size;
    // recalculate due to potential guard pages
    *psegment_slices = alloc_hook_segment_calculate_slices(required + extra, ppre_size, pinfo_slices);
  }

  const size_t segment_size = (*psegment_slices) * ALLOC_HOOK_SEGMENT_SLICE_SIZE;
  alloc_hook_segment_t* segment = (alloc_hook_segment_t*)_alloc_hook_arena_alloc_aligned(segment_size, alignment, align_offset, commit, allow_large, req_arena_id, &memid, os_tld);
  if (segment == NULL) {
    return NULL;  // failed to allocate
  }

  // ensure metadata part of the segment is committed  
  alloc_hook_commit_mask_t commit_mask; 
  if (memid.initially_committed) { 
    alloc_hook_commit_mask_create_full(&commit_mask);  
  }
  else { 
    // at least commit the info slices
    const size_t commit_needed = _alloc_hook_divide_up((*pinfo_slices)*ALLOC_HOOK_SEGMENT_SLICE_SIZE, ALLOC_HOOK_COMMIT_SIZE);
    alloc_hook_assert_internal(commit_needed>0);
    alloc_hook_commit_mask_create(0, commit_needed, &commit_mask);    
    alloc_hook_assert_internal(commit_needed*ALLOC_HOOK_COMMIT_SIZE >= (*pinfo_slices)*ALLOC_HOOK_SEGMENT_SLICE_SIZE);
    if (!_alloc_hook_os_commit(segment, commit_needed*ALLOC_HOOK_COMMIT_SIZE, NULL, tld->stats)) {
      _alloc_hook_arena_free(segment,segment_size,0,memid,tld->stats);
      return NULL;
    }    
  }
  alloc_hook_assert_internal(segment != NULL && (uintptr_t)segment % ALLOC_HOOK_SEGMENT_SIZE == 0);

  segment->memid = memid;
  segment->allow_decommit = !memid.is_pinned;
  segment->allow_purge = segment->allow_decommit && (alloc_hook_option_get(alloc_hook_option_purge_delay) >= 0);
  segment->segment_size = segment_size;
  segment->commit_mask = commit_mask;
  segment->purge_expire = 0;
  alloc_hook_commit_mask_create_empty(&segment->purge_mask);
  alloc_hook_atomic_store_ptr_release(alloc_hook_segment_t, &segment->abandoned_next, NULL);  // tsan
  
  alloc_hook_segments_track_size((long)(segment_size), tld);
  _alloc_hook_segment_map_allocated_at(segment);
  return segment;
}


// Allocate a segment from the OS aligned to `ALLOC_HOOK_SEGMENT_SIZE` .
static alloc_hook_segment_t* alloc_hook_segment_alloc(size_t required, size_t page_alignment, alloc_hook_arena_id_t req_arena_id, alloc_hook_segments_tld_t* tld, alloc_hook_os_tld_t* os_tld, alloc_hook_page_t** huge_page)
{
  alloc_hook_assert_internal((required==0 && huge_page==NULL) || (required>0 && huge_page != NULL));
  
  // calculate needed sizes first
  size_t info_slices;
  size_t pre_size;
  size_t segment_slices = alloc_hook_segment_calculate_slices(required, &pre_size, &info_slices);
  
  // Commit eagerly only if not the first N lazy segments (to reduce impact of many threads that allocate just a little)
  const bool eager_delay = (// !_alloc_hook_os_has_overcommit() &&             // never delay on overcommit systems
                            _alloc_hook_current_thread_count() > 1 &&       // do not delay for the first N threads
                            tld->count < (size_t)alloc_hook_option_get(alloc_hook_option_eager_commit_delay));
  const bool eager = !eager_delay && alloc_hook_option_is_enabled(alloc_hook_option_eager_commit);
  bool commit = eager || (required > 0);   
  
  // Allocate the segment from the OS  
  alloc_hook_segment_t* segment = alloc_hook_segment_os_alloc(required, page_alignment, eager_delay, req_arena_id, 
                                              &segment_slices, &pre_size, &info_slices, commit, tld, os_tld);
  if (segment == NULL) return NULL;
  
  // zero the segment info? -- not always needed as it may be zero initialized from the OS   
  if (!segment->memid.initially_zero) {
    ptrdiff_t ofs    = offsetof(alloc_hook_segment_t, next);
    size_t    prefix = offsetof(alloc_hook_segment_t, slices) - ofs;
    size_t    zsize  = prefix + (sizeof(alloc_hook_slice_t) * (segment_slices + 1)); // one more  
    _alloc_hook_memzero((uint8_t*)segment + ofs, zsize);
  }
  
  // initialize the rest of the segment info
  const size_t slice_entries = (segment_slices > ALLOC_HOOK_SLICES_PER_SEGMENT ? ALLOC_HOOK_SLICES_PER_SEGMENT : segment_slices);
  segment->segment_slices = segment_slices;
  segment->segment_info_slices = info_slices;
  segment->thread_id = _alloc_hook_thread_id();
  segment->cookie = _alloc_hook_ptr_cookie(segment);
  segment->slice_entries = slice_entries;
  segment->kind = (required == 0 ? ALLOC_HOOK_SEGMENT_NORMAL : ALLOC_HOOK_SEGMENT_HUGE);

  // _alloc_hook_memzero(segment->slices, sizeof(alloc_hook_slice_t)*(info_slices+1));
  _alloc_hook_stat_increase(&tld->stats->page_committed, alloc_hook_segment_info_size(segment));

  // set up guard pages
  size_t guard_slices = 0;
  if (ALLOC_HOOK_SECURE>0) {
    // in secure mode, we set up a protected page in between the segment info
    // and the page data, and at the end of the segment.
    size_t os_pagesize = _alloc_hook_os_page_size();    
    alloc_hook_assert_internal(alloc_hook_segment_info_size(segment) - os_pagesize >= pre_size);
    _alloc_hook_os_protect((uint8_t*)segment + alloc_hook_segment_info_size(segment) - os_pagesize, os_pagesize);
    uint8_t* end = (uint8_t*)segment + alloc_hook_segment_size(segment) - os_pagesize;
    alloc_hook_segment_ensure_committed(segment, end, os_pagesize, tld->stats);
    _alloc_hook_os_protect(end, os_pagesize);
    if (slice_entries == segment_slices) segment->slice_entries--; // don't use the last slice :-(
    guard_slices = 1;
  }

  // reserve first slices for segment info
  alloc_hook_page_t* page0 = alloc_hook_segment_span_allocate(segment, 0, info_slices, tld);
  alloc_hook_assert_internal(page0!=NULL); if (page0==NULL) return NULL; // cannot fail as we always commit in advance  
  alloc_hook_assert_internal(segment->used == 1);
  segment->used = 0; // don't count our internal slices towards usage
  
  // initialize initial free pages
  if (segment->kind == ALLOC_HOOK_SEGMENT_NORMAL) { // not a huge page
    alloc_hook_assert_internal(huge_page==NULL);
    alloc_hook_segment_span_free(segment, info_slices, segment->slice_entries - info_slices, false /* don't purge */, tld);
  }
  else {
    alloc_hook_assert_internal(huge_page!=NULL);
    alloc_hook_assert_internal(alloc_hook_commit_mask_is_empty(&segment->purge_mask));
    alloc_hook_assert_internal(alloc_hook_commit_mask_is_full(&segment->commit_mask));
    *huge_page = alloc_hook_segment_span_allocate(segment, info_slices, segment_slices - info_slices - guard_slices, tld);
    alloc_hook_assert_internal(*huge_page != NULL); // cannot fail as we commit in advance 
  }

  alloc_hook_assert_expensive(alloc_hook_segment_is_valid(segment,tld));
  return segment;
}


static void alloc_hook_segment_free(alloc_hook_segment_t* segment, bool force, alloc_hook_segments_tld_t* tld) {
  ALLOC_HOOK_UNUSED(force);
  alloc_hook_assert_internal(segment != NULL);
  alloc_hook_assert_internal(segment->next == NULL);
  alloc_hook_assert_internal(segment->used == 0);

  // Remove the free pages
  alloc_hook_slice_t* slice = &segment->slices[0];
  const alloc_hook_slice_t* end = alloc_hook_segment_slices_end(segment);
  #if ALLOC_HOOK_DEBUG>1
  size_t page_count = 0;
  #endif
  while (slice < end) {
    alloc_hook_assert_internal(slice->slice_count > 0);
    alloc_hook_assert_internal(slice->slice_offset == 0);
    alloc_hook_assert_internal(alloc_hook_slice_index(slice)==0 || slice->xblock_size == 0); // no more used pages ..
    if (slice->xblock_size == 0 && segment->kind != ALLOC_HOOK_SEGMENT_HUGE) {
      alloc_hook_segment_span_remove_from_queue(slice, tld);
    }
    #if ALLOC_HOOK_DEBUG>1
    page_count++;
    #endif
    slice = slice + slice->slice_count;
  }
  alloc_hook_assert_internal(page_count == 2); // first page is allocated by the segment itself

  // stats
  _alloc_hook_stat_decrease(&tld->stats->page_committed, alloc_hook_segment_info_size(segment));

  // return it to the OS
  alloc_hook_segment_os_free(segment, tld);
}


/* -----------------------------------------------------------
   Page Free
----------------------------------------------------------- */

static void alloc_hook_segment_abandon(alloc_hook_segment_t* segment, alloc_hook_segments_tld_t* tld);

// note: can be called on abandoned pages
static alloc_hook_slice_t* alloc_hook_segment_page_clear(alloc_hook_page_t* page, alloc_hook_segments_tld_t* tld) {
  alloc_hook_assert_internal(page->xblock_size > 0);
  alloc_hook_assert_internal(alloc_hook_page_all_free(page));
  alloc_hook_segment_t* segment = _alloc_hook_ptr_segment(page);
  alloc_hook_assert_internal(segment->used > 0);
  
  size_t inuse = page->capacity * alloc_hook_page_block_size(page);
  _alloc_hook_stat_decrease(&tld->stats->page_committed, inuse);
  _alloc_hook_stat_decrease(&tld->stats->pages, 1);

  // reset the page memory to reduce memory pressure?
  if (segment->allow_decommit && alloc_hook_option_is_enabled(alloc_hook_option_deprecated_page_reset)) {
    size_t psize;
    uint8_t* start = _alloc_hook_page_start(segment, page, &psize);    
    _alloc_hook_os_reset(start, psize, tld->stats);
  }

  // zero the page data, but not the segment fields
  page->is_zero_init = false;
  ptrdiff_t ofs = offsetof(alloc_hook_page_t, capacity);
  _alloc_hook_memzero((uint8_t*)page + ofs, sizeof(*page) - ofs);
  page->xblock_size = 1;

  // and free it
  alloc_hook_slice_t* slice = alloc_hook_segment_span_free_coalesce(alloc_hook_page_to_slice(page), tld);  
  segment->used--;
  // cannot assert segment valid as it is called during reclaim
  // alloc_hook_assert_expensive(alloc_hook_segment_is_valid(segment, tld));
  return slice;
}

void _alloc_hook_segment_page_free(alloc_hook_page_t* page, bool force, alloc_hook_segments_tld_t* tld)
{
  alloc_hook_assert(page != NULL);

  alloc_hook_segment_t* segment = _alloc_hook_page_segment(page);
  alloc_hook_assert_expensive(alloc_hook_segment_is_valid(segment,tld));

  // mark it as free now
  alloc_hook_segment_page_clear(page, tld);
  alloc_hook_assert_expensive(alloc_hook_segment_is_valid(segment, tld));

  if (segment->used == 0) {
    // no more used pages; remove from the free list and free the segment
    alloc_hook_segment_free(segment, force, tld);
  }
  else if (segment->used == segment->abandoned) {
    // only abandoned pages; remove from free list and abandon
    alloc_hook_segment_abandon(segment,tld);
  }
}


/* -----------------------------------------------------------
Abandonment

When threads terminate, they can leave segments with
live blocks (reachable through other threads). Such segments
are "abandoned" and will be reclaimed by other threads to
reuse their pages and/or free them eventually

We maintain a global list of abandoned segments that are
reclaimed on demand. Since this is shared among threads
the implementation needs to avoid the A-B-A problem on
popping abandoned segments: <https://en.wikipedia.org/wiki/ABA_problem>
We use tagged pointers to avoid accidentally identifying
reused segments, much like stamped references in Java.
Secondly, we maintain a reader counter to avoid resetting
or decommitting segments that have a pending read operation.

Note: the current implementation is one possible design;
another way might be to keep track of abandoned segments
in the arenas/segment_cache's. This would have the advantage of keeping
all concurrent code in one place and not needing to deal
with ABA issues. The drawback is that it is unclear how to
scan abandoned segments efficiently in that case as they
would be spread among all other segments in the arenas.
----------------------------------------------------------- */

// Use the bottom 20-bits (on 64-bit) of the aligned segment pointers
// to put in a tag that increments on update to avoid the A-B-A problem.
#define ALLOC_HOOK_TAGGED_MASK   ALLOC_HOOK_SEGMENT_MASK
typedef uintptr_t        alloc_hook_tagged_segment_t;

static alloc_hook_segment_t* alloc_hook_tagged_segment_ptr(alloc_hook_tagged_segment_t ts) {
  return (alloc_hook_segment_t*)(ts & ~ALLOC_HOOK_TAGGED_MASK);
}

static alloc_hook_tagged_segment_t alloc_hook_tagged_segment(alloc_hook_segment_t* segment, alloc_hook_tagged_segment_t ts) {
  alloc_hook_assert_internal(((uintptr_t)segment & ALLOC_HOOK_TAGGED_MASK) == 0);
  uintptr_t tag = ((ts & ALLOC_HOOK_TAGGED_MASK) + 1) & ALLOC_HOOK_TAGGED_MASK;
  return ((uintptr_t)segment | tag);
}

// This is a list of visited abandoned pages that were full at the time.
// this list migrates to `abandoned` when that becomes NULL. The use of
// this list reduces contention and the rate at which segments are visited.
static alloc_hook_decl_cache_align _Atomic(alloc_hook_segment_t*)       abandoned_visited; // = NULL

// The abandoned page list (tagged as it supports pop)
static alloc_hook_decl_cache_align _Atomic(alloc_hook_tagged_segment_t) abandoned;         // = NULL

// Maintain these for debug purposes (these counts may be a bit off)
static alloc_hook_decl_cache_align _Atomic(size_t)           abandoned_count;
static alloc_hook_decl_cache_align _Atomic(size_t)           abandoned_visited_count;

// We also maintain a count of current readers of the abandoned list
// in order to prevent resetting/decommitting segment memory if it might
// still be read.
static alloc_hook_decl_cache_align _Atomic(size_t)           abandoned_readers; // = 0

// Push on the visited list
static void alloc_hook_abandoned_visited_push(alloc_hook_segment_t* segment) {
  alloc_hook_assert_internal(segment->thread_id == 0);
  alloc_hook_assert_internal(alloc_hook_atomic_load_ptr_relaxed(alloc_hook_segment_t,&segment->abandoned_next) == NULL);
  alloc_hook_assert_internal(segment->next == NULL);
  alloc_hook_assert_internal(segment->used > 0);
  alloc_hook_segment_t* anext = alloc_hook_atomic_load_ptr_relaxed(alloc_hook_segment_t, &abandoned_visited);
  do {
    alloc_hook_atomic_store_ptr_release(alloc_hook_segment_t, &segment->abandoned_next, anext);
  } while (!alloc_hook_atomic_cas_ptr_weak_release(alloc_hook_segment_t, &abandoned_visited, &anext, segment));
  alloc_hook_atomic_increment_relaxed(&abandoned_visited_count);
}

// Move the visited list to the abandoned list.
static bool alloc_hook_abandoned_visited_revisit(void)
{
  // quick check if the visited list is empty
  if (alloc_hook_atomic_load_ptr_relaxed(alloc_hook_segment_t, &abandoned_visited) == NULL) return false;

  // grab the whole visited list
  alloc_hook_segment_t* first = alloc_hook_atomic_exchange_ptr_acq_rel(alloc_hook_segment_t, &abandoned_visited, NULL);
  if (first == NULL) return false;

  // first try to swap directly if the abandoned list happens to be NULL
  alloc_hook_tagged_segment_t afirst;
  alloc_hook_tagged_segment_t ts = alloc_hook_atomic_load_relaxed(&abandoned);
  if (alloc_hook_tagged_segment_ptr(ts)==NULL) {
    size_t count = alloc_hook_atomic_load_relaxed(&abandoned_visited_count);
    afirst = alloc_hook_tagged_segment(first, ts);
    if (alloc_hook_atomic_cas_strong_acq_rel(&abandoned, &ts, afirst)) {
      alloc_hook_atomic_add_relaxed(&abandoned_count, count);
      alloc_hook_atomic_sub_relaxed(&abandoned_visited_count, count);
      return true;
    }
  }

  // find the last element of the visited list: O(n)
  alloc_hook_segment_t* last = first;
  alloc_hook_segment_t* next;
  while ((next = alloc_hook_atomic_load_ptr_relaxed(alloc_hook_segment_t, &last->abandoned_next)) != NULL) {
    last = next;
  }

  // and atomically prepend to the abandoned list
  // (no need to increase the readers as we don't access the abandoned segments)
  alloc_hook_tagged_segment_t anext = alloc_hook_atomic_load_relaxed(&abandoned);
  size_t count;
  do {
    count = alloc_hook_atomic_load_relaxed(&abandoned_visited_count);
    alloc_hook_atomic_store_ptr_release(alloc_hook_segment_t, &last->abandoned_next, alloc_hook_tagged_segment_ptr(anext));
    afirst = alloc_hook_tagged_segment(first, anext);
  } while (!alloc_hook_atomic_cas_weak_release(&abandoned, &anext, afirst));
  alloc_hook_atomic_add_relaxed(&abandoned_count, count);
  alloc_hook_atomic_sub_relaxed(&abandoned_visited_count, count);
  return true;
}

// Push on the abandoned list.
static void alloc_hook_abandoned_push(alloc_hook_segment_t* segment) {
  alloc_hook_assert_internal(segment->thread_id == 0);
  alloc_hook_assert_internal(alloc_hook_atomic_load_ptr_relaxed(alloc_hook_segment_t, &segment->abandoned_next) == NULL);
  alloc_hook_assert_internal(segment->next == NULL);
  alloc_hook_assert_internal(segment->used > 0);
  alloc_hook_tagged_segment_t next;
  alloc_hook_tagged_segment_t ts = alloc_hook_atomic_load_relaxed(&abandoned);
  do {
    alloc_hook_atomic_store_ptr_release(alloc_hook_segment_t, &segment->abandoned_next, alloc_hook_tagged_segment_ptr(ts));
    next = alloc_hook_tagged_segment(segment, ts);
  } while (!alloc_hook_atomic_cas_weak_release(&abandoned, &ts, next));
  alloc_hook_atomic_increment_relaxed(&abandoned_count);
}

// Wait until there are no more pending reads on segments that used to be in the abandoned list
// called for example from `arena.c` before decommitting
void _alloc_hook_abandoned_await_readers(void) {
  size_t n;
  do {
    n = alloc_hook_atomic_load_acquire(&abandoned_readers);
    if (n != 0) alloc_hook_atomic_yield();
  } while (n != 0);
}

// Pop from the abandoned list
static alloc_hook_segment_t* alloc_hook_abandoned_pop(void) {
  alloc_hook_segment_t* segment;
  // Check efficiently if it is empty (or if the visited list needs to be moved)
  alloc_hook_tagged_segment_t ts = alloc_hook_atomic_load_relaxed(&abandoned);
  segment = alloc_hook_tagged_segment_ptr(ts);
  if alloc_hook_likely(segment == NULL) {
    if alloc_hook_likely(!alloc_hook_abandoned_visited_revisit()) { // try to swap in the visited list on NULL
      return NULL;
    }
  }

  // Do a pop. We use a reader count to prevent
  // a segment to be decommitted while a read is still pending,
  // and a tagged pointer to prevent A-B-A link corruption.
  // (this is called from `region.c:_alloc_hook_mem_free` for example)
  alloc_hook_atomic_increment_relaxed(&abandoned_readers);  // ensure no segment gets decommitted
  alloc_hook_tagged_segment_t next = 0;
  ts = alloc_hook_atomic_load_acquire(&abandoned);
  do {
    segment = alloc_hook_tagged_segment_ptr(ts);
    if (segment != NULL) {
      alloc_hook_segment_t* anext = alloc_hook_atomic_load_ptr_relaxed(alloc_hook_segment_t, &segment->abandoned_next);
      next = alloc_hook_tagged_segment(anext, ts); // note: reads the segment's `abandoned_next` field so should not be decommitted
    }
  } while (segment != NULL && !alloc_hook_atomic_cas_weak_acq_rel(&abandoned, &ts, next));
  alloc_hook_atomic_decrement_relaxed(&abandoned_readers);  // release reader lock
  if (segment != NULL) {
    alloc_hook_atomic_store_ptr_release(alloc_hook_segment_t, &segment->abandoned_next, NULL);
    alloc_hook_atomic_decrement_relaxed(&abandoned_count);
  }
  return segment;
}

/* -----------------------------------------------------------
   Abandon segment/page
----------------------------------------------------------- */

static void alloc_hook_segment_abandon(alloc_hook_segment_t* segment, alloc_hook_segments_tld_t* tld) {
  alloc_hook_assert_internal(segment->used == segment->abandoned);
  alloc_hook_assert_internal(segment->used > 0);
  alloc_hook_assert_internal(alloc_hook_atomic_load_ptr_relaxed(alloc_hook_segment_t, &segment->abandoned_next) == NULL);
  alloc_hook_assert_internal(segment->abandoned_visits == 0);
  alloc_hook_assert_expensive(alloc_hook_segment_is_valid(segment,tld));
  
  // remove the free pages from the free page queues
  alloc_hook_slice_t* slice = &segment->slices[0];
  const alloc_hook_slice_t* end = alloc_hook_segment_slices_end(segment);
  while (slice < end) {
    alloc_hook_assert_internal(slice->slice_count > 0);
    alloc_hook_assert_internal(slice->slice_offset == 0);
    if (slice->xblock_size == 0) { // a free page
      alloc_hook_segment_span_remove_from_queue(slice,tld);
      slice->xblock_size = 0; // but keep it free
    }
    slice = slice + slice->slice_count;
  }

  // perform delayed decommits (forcing is much slower on mstress)
  alloc_hook_segment_try_purge(segment, alloc_hook_option_is_enabled(alloc_hook_option_abandoned_page_purge) /* force? */, tld->stats);    
  
  // all pages in the segment are abandoned; add it to the abandoned list
  _alloc_hook_stat_increase(&tld->stats->segments_abandoned, 1);
  alloc_hook_segments_track_size(-((long)alloc_hook_segment_size(segment)), tld);
  segment->thread_id = 0;
  alloc_hook_atomic_store_ptr_release(alloc_hook_segment_t, &segment->abandoned_next, NULL);
  segment->abandoned_visits = 1;   // from 0 to 1 to signify it is abandoned
  alloc_hook_abandoned_push(segment);
}

void _alloc_hook_segment_page_abandon(alloc_hook_page_t* page, alloc_hook_segments_tld_t* tld) {
  alloc_hook_assert(page != NULL);
  alloc_hook_assert_internal(alloc_hook_page_thread_free_flag(page)==ALLOC_HOOK_NEVER_DELAYED_FREE);
  alloc_hook_assert_internal(alloc_hook_page_heap(page) == NULL);
  alloc_hook_segment_t* segment = _alloc_hook_page_segment(page);

  alloc_hook_assert_expensive(alloc_hook_segment_is_valid(segment,tld));
  segment->abandoned++;  

  _alloc_hook_stat_increase(&tld->stats->pages_abandoned, 1);
  alloc_hook_assert_internal(segment->abandoned <= segment->used);
  if (segment->used == segment->abandoned) {
    // all pages are abandoned, abandon the entire segment
    alloc_hook_segment_abandon(segment, tld);
  }
}

/* -----------------------------------------------------------
  Reclaim abandoned pages
----------------------------------------------------------- */

static alloc_hook_slice_t* alloc_hook_slices_start_iterate(alloc_hook_segment_t* segment, const alloc_hook_slice_t** end) {
  alloc_hook_slice_t* slice = &segment->slices[0];
  *end = alloc_hook_segment_slices_end(segment);
  alloc_hook_assert_internal(slice->slice_count>0 && slice->xblock_size>0); // segment allocated page
  slice = slice + slice->slice_count; // skip the first segment allocated page
  return slice;
}

// Possibly free pages and check if free space is available
static bool alloc_hook_segment_check_free(alloc_hook_segment_t* segment, size_t slices_needed, size_t block_size, alloc_hook_segments_tld_t* tld) 
{
  alloc_hook_assert_internal(block_size < ALLOC_HOOK_HUGE_BLOCK_SIZE);
  alloc_hook_assert_internal(alloc_hook_segment_is_abandoned(segment));
  bool has_page = false;
  
  // for all slices
  const alloc_hook_slice_t* end;
  alloc_hook_slice_t* slice = alloc_hook_slices_start_iterate(segment, &end);
  while (slice < end) {
    alloc_hook_assert_internal(slice->slice_count > 0);
    alloc_hook_assert_internal(slice->slice_offset == 0);
    if (alloc_hook_slice_is_used(slice)) { // used page
      // ensure used count is up to date and collect potential concurrent frees
      alloc_hook_page_t* const page = alloc_hook_slice_to_page(slice);
      _alloc_hook_page_free_collect(page, false);
      if (alloc_hook_page_all_free(page)) {
        // if this page is all free now, free it without adding to any queues (yet) 
        alloc_hook_assert_internal(page->next == NULL && page->prev==NULL);
        _alloc_hook_stat_decrease(&tld->stats->pages_abandoned, 1);
        segment->abandoned--;
        slice = alloc_hook_segment_page_clear(page, tld); // re-assign slice due to coalesce!
        alloc_hook_assert_internal(!alloc_hook_slice_is_used(slice));
        if (slice->slice_count >= slices_needed) {
          has_page = true;
        }
      }
      else {
        if (page->xblock_size == block_size && alloc_hook_page_has_any_available(page)) {
          // a page has available free blocks of the right size
          has_page = true;
        }
      }      
    }
    else {
      // empty span
      if (slice->slice_count >= slices_needed) {
        has_page = true;
      }
    }
    slice = slice + slice->slice_count;
  }
  return has_page;
}

// Reclaim an abandoned segment; returns NULL if the segment was freed
// set `right_page_reclaimed` to `true` if it reclaimed a page of the right `block_size` that was not full.
static alloc_hook_segment_t* alloc_hook_segment_reclaim(alloc_hook_segment_t* segment, alloc_hook_heap_t* heap, size_t requested_block_size, bool* right_page_reclaimed, alloc_hook_segments_tld_t* tld) {
  alloc_hook_assert_internal(alloc_hook_atomic_load_ptr_relaxed(alloc_hook_segment_t, &segment->abandoned_next) == NULL);
  alloc_hook_assert_expensive(alloc_hook_segment_is_valid(segment, tld));
  if (right_page_reclaimed != NULL) { *right_page_reclaimed = false; }

  segment->thread_id = _alloc_hook_thread_id();
  segment->abandoned_visits = 0;
  alloc_hook_segments_track_size((long)alloc_hook_segment_size(segment), tld);
  alloc_hook_assert_internal(segment->next == NULL);
  _alloc_hook_stat_decrease(&tld->stats->segments_abandoned, 1);
  
  // for all slices
  const alloc_hook_slice_t* end;
  alloc_hook_slice_t* slice = alloc_hook_slices_start_iterate(segment, &end);
  while (slice < end) {
    alloc_hook_assert_internal(slice->slice_count > 0);
    alloc_hook_assert_internal(slice->slice_offset == 0);
    if (alloc_hook_slice_is_used(slice)) {
      // in use: reclaim the page in our heap
      alloc_hook_page_t* page = alloc_hook_slice_to_page(slice);
      alloc_hook_assert_internal(page->is_committed);
      alloc_hook_assert_internal(alloc_hook_page_thread_free_flag(page)==ALLOC_HOOK_NEVER_DELAYED_FREE);
      alloc_hook_assert_internal(alloc_hook_page_heap(page) == NULL);
      alloc_hook_assert_internal(page->next == NULL && page->prev==NULL);
      _alloc_hook_stat_decrease(&tld->stats->pages_abandoned, 1);
      segment->abandoned--;
      // set the heap again and allow delayed free again
      alloc_hook_page_set_heap(page, heap);
      _alloc_hook_page_use_delayed_free(page, ALLOC_HOOK_USE_DELAYED_FREE, true); // override never (after heap is set)
      _alloc_hook_page_free_collect(page, false); // ensure used count is up to date
      if (alloc_hook_page_all_free(page)) {
        // if everything free by now, free the page
        slice = alloc_hook_segment_page_clear(page, tld);   // set slice again due to coalesceing
      }
      else {
        // otherwise reclaim it into the heap
        _alloc_hook_page_reclaim(heap, page);
        if (requested_block_size == page->xblock_size && alloc_hook_page_has_any_available(page)) {
          if (right_page_reclaimed != NULL) { *right_page_reclaimed = true; }
        }
      }
    }
    else {
      // the span is free, add it to our page queues
      slice = alloc_hook_segment_span_free_coalesce(slice, tld); // set slice again due to coalesceing
    }
    alloc_hook_assert_internal(slice->slice_count>0 && slice->slice_offset==0);
    slice = slice + slice->slice_count;
  }

  alloc_hook_assert(segment->abandoned == 0);
  if (segment->used == 0) {  // due to page_clear
    alloc_hook_assert_internal(right_page_reclaimed == NULL || !(*right_page_reclaimed));
    alloc_hook_segment_free(segment, false, tld);
    return NULL;
  }
  else {
    return segment;
  }
}


void _alloc_hook_abandoned_reclaim_all(alloc_hook_heap_t* heap, alloc_hook_segments_tld_t* tld) {
  alloc_hook_segment_t* segment;
  while ((segment = alloc_hook_abandoned_pop()) != NULL) {
    alloc_hook_segment_reclaim(segment, heap, 0, NULL, tld);
  }
}

static alloc_hook_segment_t* alloc_hook_segment_try_reclaim(alloc_hook_heap_t* heap, size_t needed_slices, size_t block_size, bool* reclaimed, alloc_hook_segments_tld_t* tld)
{
  *reclaimed = false;
  alloc_hook_segment_t* segment;
  long max_tries = alloc_hook_option_get_clamp(alloc_hook_option_max_segment_reclaim, 8, 1024);     // limit the work to bound allocation times
  while ((max_tries-- > 0) && ((segment = alloc_hook_abandoned_pop()) != NULL)) {
    segment->abandoned_visits++;
    // todo: an arena exclusive heap will potentially visit many abandoned unsuitable segments
    // and push them into the visited list and use many tries. Perhaps we can skip non-suitable ones in a better way?
    bool is_suitable = _alloc_hook_heap_memid_is_suitable(heap, segment->memid);
    bool has_page = alloc_hook_segment_check_free(segment,needed_slices,block_size,tld); // try to free up pages (due to concurrent frees)
    if (segment->used == 0) {
      // free the segment (by forced reclaim) to make it available to other threads.
      // note1: we prefer to free a segment as that might lead to reclaiming another
      // segment that is still partially used.
      // note2: we could in principle optimize this by skipping reclaim and directly
      // freeing but that would violate some invariants temporarily)
      alloc_hook_segment_reclaim(segment, heap, 0, NULL, tld);
    }
    else if (has_page && is_suitable) {
      // found a large enough free span, or a page of the right block_size with free space 
      // we return the result of reclaim (which is usually `segment`) as it might free
      // the segment due to concurrent frees (in which case `NULL` is returned).
      return alloc_hook_segment_reclaim(segment, heap, block_size, reclaimed, tld);
    }
    else if (segment->abandoned_visits > 3 && is_suitable) {  
      // always reclaim on 3rd visit to limit the abandoned queue length.
      alloc_hook_segment_reclaim(segment, heap, 0, NULL, tld);
    }
    else {
      // otherwise, push on the visited list so it gets not looked at too quickly again
      alloc_hook_segment_try_purge(segment, true /* force? */, tld->stats); // force purge if needed as we may not visit soon again
      alloc_hook_abandoned_visited_push(segment);
    }
  }
  return NULL;
}


void _alloc_hook_abandoned_collect(alloc_hook_heap_t* heap, bool force, alloc_hook_segments_tld_t* tld)
{
  alloc_hook_segment_t* segment;
  int max_tries = (force ? 16*1024 : 1024); // limit latency
  if (force) {
    alloc_hook_abandoned_visited_revisit(); 
  }
  while ((max_tries-- > 0) && ((segment = alloc_hook_abandoned_pop()) != NULL)) {
    alloc_hook_segment_check_free(segment,0,0,tld); // try to free up pages (due to concurrent frees)
    if (segment->used == 0) {
      // free the segment (by forced reclaim) to make it available to other threads.
      // note: we could in principle optimize this by skipping reclaim and directly
      // freeing but that would violate some invariants temporarily)
      alloc_hook_segment_reclaim(segment, heap, 0, NULL, tld);
    }
    else {
      // otherwise, purge if needed and push on the visited list 
      // note: forced purge can be expensive if many threads are destroyed/created as in mstress.
      alloc_hook_segment_try_purge(segment, force, tld->stats);
      alloc_hook_abandoned_visited_push(segment);
    }
  }
}

/* -----------------------------------------------------------
   Reclaim or allocate
----------------------------------------------------------- */

static alloc_hook_segment_t* alloc_hook_segment_reclaim_or_alloc(alloc_hook_heap_t* heap, size_t needed_slices, size_t block_size, alloc_hook_segments_tld_t* tld, alloc_hook_os_tld_t* os_tld)
{
  alloc_hook_assert_internal(block_size < ALLOC_HOOK_HUGE_BLOCK_SIZE);
  alloc_hook_assert_internal(block_size <= ALLOC_HOOK_LARGE_OBJ_SIZE_MAX);
  
  // 1. try to reclaim an abandoned segment
  bool reclaimed;
  alloc_hook_segment_t* segment = alloc_hook_segment_try_reclaim(heap, needed_slices, block_size, &reclaimed, tld);
  if (reclaimed) {
    // reclaimed the right page right into the heap
    alloc_hook_assert_internal(segment != NULL);
    return NULL; // pretend out-of-memory as the page will be in the page queue of the heap with available blocks
  }
  else if (segment != NULL) {
    // reclaimed a segment with a large enough empty span in it
    return segment;
  }
  // 2. otherwise allocate a fresh segment
  return alloc_hook_segment_alloc(0, 0, heap->arena_id, tld, os_tld, NULL);  
}


/* -----------------------------------------------------------
   Page allocation
----------------------------------------------------------- */

static alloc_hook_page_t* alloc_hook_segments_page_alloc(alloc_hook_heap_t* heap, alloc_hook_page_kind_t page_kind, size_t required, size_t block_size, alloc_hook_segments_tld_t* tld, alloc_hook_os_tld_t* os_tld)
{
  alloc_hook_assert_internal(required <= ALLOC_HOOK_LARGE_OBJ_SIZE_MAX && page_kind <= ALLOC_HOOK_PAGE_LARGE);

  // find a free page
  size_t page_size = _alloc_hook_align_up(required, (required > ALLOC_HOOK_MEDIUM_PAGE_SIZE ? ALLOC_HOOK_MEDIUM_PAGE_SIZE : ALLOC_HOOK_SEGMENT_SLICE_SIZE));
  size_t slices_needed = page_size / ALLOC_HOOK_SEGMENT_SLICE_SIZE;
  alloc_hook_assert_internal(slices_needed * ALLOC_HOOK_SEGMENT_SLICE_SIZE == page_size);
  alloc_hook_page_t* page = alloc_hook_segments_page_find_and_allocate(slices_needed, heap->arena_id, tld); //(required <= ALLOC_HOOK_SMALL_SIZE_MAX ? 0 : slices_needed), tld);
  if (page==NULL) {
    // no free page, allocate a new segment and try again
    if (alloc_hook_segment_reclaim_or_alloc(heap, slices_needed, block_size, tld, os_tld) == NULL) {
      // OOM or reclaimed a good page in the heap
      return NULL;  
    }
    else {
      // otherwise try again
      return alloc_hook_segments_page_alloc(heap, page_kind, required, block_size, tld, os_tld);
    }
  }
  alloc_hook_assert_internal(page != NULL && page->slice_count*ALLOC_HOOK_SEGMENT_SLICE_SIZE == page_size);
  alloc_hook_assert_internal(_alloc_hook_ptr_segment(page)->thread_id == _alloc_hook_thread_id());
  alloc_hook_segment_try_purge(_alloc_hook_ptr_segment(page), false, tld->stats);
  return page;
}



/* -----------------------------------------------------------
   Huge page allocation
----------------------------------------------------------- */

static alloc_hook_page_t* alloc_hook_segment_huge_page_alloc(size_t size, size_t page_alignment, alloc_hook_arena_id_t req_arena_id, alloc_hook_segments_tld_t* tld, alloc_hook_os_tld_t* os_tld)
{
  alloc_hook_page_t* page = NULL;
  alloc_hook_segment_t* segment = alloc_hook_segment_alloc(size,page_alignment,req_arena_id,tld,os_tld,&page);
  if (segment == NULL || page==NULL) return NULL;
  alloc_hook_assert_internal(segment->used==1);
  alloc_hook_assert_internal(alloc_hook_page_block_size(page) >= size);  
  #if ALLOC_HOOK_HUGE_PAGE_ABANDON
  segment->thread_id = 0; // huge segments are immediately abandoned
  #endif  

  // for huge pages we initialize the xblock_size as we may
  // overallocate to accommodate large alignments.
  size_t psize;
  uint8_t* start = _alloc_hook_segment_page_start(segment, page, &psize);
  page->xblock_size = (psize > ALLOC_HOOK_HUGE_BLOCK_SIZE ? ALLOC_HOOK_HUGE_BLOCK_SIZE : (uint32_t)psize);
  
  // decommit the part of the prefix of a page that will not be used; this can be quite large (close to ALLOC_HOOK_SEGMENT_SIZE)
  if (page_alignment > 0 && segment->allow_decommit) {
    uint8_t* aligned_p = (uint8_t*)_alloc_hook_align_up((uintptr_t)start, page_alignment);
    alloc_hook_assert_internal(_alloc_hook_is_aligned(aligned_p, page_alignment));
    alloc_hook_assert_internal(psize - (aligned_p - start) >= size);      
    uint8_t* decommit_start = start + sizeof(alloc_hook_block_t);              // for the free list
    ptrdiff_t decommit_size = aligned_p - decommit_start;
    _alloc_hook_os_reset(decommit_start, decommit_size, &_alloc_hook_stats_main);   // note: cannot use segment_decommit on huge segments    
  }
  
  return page;
}

#if ALLOC_HOOK_HUGE_PAGE_ABANDON
// free huge block from another thread
void _alloc_hook_segment_huge_page_free(alloc_hook_segment_t* segment, alloc_hook_page_t* page, alloc_hook_block_t* block) {
  // huge page segments are always abandoned and can be freed immediately by any thread
  alloc_hook_assert_internal(segment->kind==ALLOC_HOOK_SEGMENT_HUGE);
  alloc_hook_assert_internal(segment == _alloc_hook_page_segment(page));
  alloc_hook_assert_internal(alloc_hook_atomic_load_relaxed(&segment->thread_id)==0);

  // claim it and free
  alloc_hook_heap_t* heap = alloc_hook_heap_get_default(); // issue #221; don't use the internal get_default_heap as we need to ensure the thread is initialized.
  // paranoia: if this it the last reference, the cas should always succeed
  size_t expected_tid = 0;
  if (alloc_hook_atomic_cas_strong_acq_rel(&segment->thread_id, &expected_tid, heap->thread_id)) {
    alloc_hook_block_set_next(page, block, page->free);
    page->free = block;
    page->used--;
    page->is_zero = false;
    alloc_hook_assert(page->used == 0);
    alloc_hook_tld_t* tld = heap->tld;
    _alloc_hook_segment_page_free(page, true, &tld->segments);
  }
#if (ALLOC_HOOK_DEBUG!=0)
  else {
    alloc_hook_assert_internal(false);
  }
#endif
}

#else
// reset memory of a huge block from another thread
void _alloc_hook_segment_huge_page_reset(alloc_hook_segment_t* segment, alloc_hook_page_t* page, alloc_hook_block_t* block) {
  ALLOC_HOOK_UNUSED(page);
  alloc_hook_assert_internal(segment->kind == ALLOC_HOOK_SEGMENT_HUGE);
  alloc_hook_assert_internal(segment == _alloc_hook_page_segment(page));
  alloc_hook_assert_internal(page->used == 1); // this is called just before the free
  alloc_hook_assert_internal(page->free == NULL);
  if (segment->allow_decommit) {
    size_t csize = alloc_hook_usable_size(block);
    if (csize > sizeof(alloc_hook_block_t)) {
      csize = csize - sizeof(alloc_hook_block_t);
      uint8_t* p = (uint8_t*)block + sizeof(alloc_hook_block_t);
      _alloc_hook_os_reset(p, csize, &_alloc_hook_stats_main);  // note: cannot use segment_decommit on huge segments
    }
  }
}
#endif

/* -----------------------------------------------------------
   Page allocation and free
----------------------------------------------------------- */
alloc_hook_page_t* _alloc_hook_segment_page_alloc(alloc_hook_heap_t* heap, size_t block_size, size_t page_alignment, alloc_hook_segments_tld_t* tld, alloc_hook_os_tld_t* os_tld) {
  alloc_hook_page_t* page;
  if alloc_hook_unlikely(page_alignment > ALLOC_HOOK_ALIGNMENT_MAX) {
    alloc_hook_assert_internal(_alloc_hook_is_power_of_two(page_alignment));
    alloc_hook_assert_internal(page_alignment >= ALLOC_HOOK_SEGMENT_SIZE);
    if (page_alignment < ALLOC_HOOK_SEGMENT_SIZE) { page_alignment = ALLOC_HOOK_SEGMENT_SIZE; }
    page = alloc_hook_segment_huge_page_alloc(block_size,page_alignment,heap->arena_id,tld,os_tld);
  }
  else if (block_size <= ALLOC_HOOK_SMALL_OBJ_SIZE_MAX) {
    page = alloc_hook_segments_page_alloc(heap,ALLOC_HOOK_PAGE_SMALL,block_size,block_size,tld,os_tld);
  }
  else if (block_size <= ALLOC_HOOK_MEDIUM_OBJ_SIZE_MAX) {
    page = alloc_hook_segments_page_alloc(heap,ALLOC_HOOK_PAGE_MEDIUM,ALLOC_HOOK_MEDIUM_PAGE_SIZE,block_size,tld, os_tld);
  }
  else if (block_size <= ALLOC_HOOK_LARGE_OBJ_SIZE_MAX) {
    page = alloc_hook_segments_page_alloc(heap,ALLOC_HOOK_PAGE_LARGE,block_size,block_size,tld, os_tld);
  }
  else {
    page = alloc_hook_segment_huge_page_alloc(block_size,page_alignment,heap->arena_id,tld,os_tld);    
  }
  alloc_hook_assert_internal(page == NULL || _alloc_hook_heap_memid_is_suitable(heap, _alloc_hook_page_segment(page)->memid));
  alloc_hook_assert_expensive(page == NULL || alloc_hook_segment_is_valid(_alloc_hook_page_segment(page),tld));
  return page;
}


