/* ----------------------------------------------------------------------------
Copyright (c) 2019-2023 Microsoft Research, Daan Leijen
This is free software; you can redistribute it and/or modify it under the
terms of the MIT license. A copy of the license can be found in the file
"LICENSE" at the root of this distribution.
-----------------------------------------------------------------------------*/

/* ----------------------------------------------------------------------------
Concurrent bitmap that can set/reset sequences of bits atomically,
represeted as an array of fields where each field is a machine word (`size_t`)

There are two api's; the standard one cannot have sequences that cross
between the bitmap fields (and a sequence must be <= ALLOC_HOOK_BITMAP_FIELD_BITS).
(this is used in region allocation)

The `_across` postfixed functions do allow sequences that can cross over
between the fields. (This is used in arena allocation)
---------------------------------------------------------------------------- */

#ifndef ALLOC_HOOK_BITMAP_H
#define ALLOC_HOOK_BITMAP_H

/* -----------------------------------------------------------
  Bitmap definition
----------------------------------------------------------- */

#define ALLOC_HOOK_BITMAP_FIELD_BITS   (8*ALLOC_HOOK_SIZE_SIZE)
#define ALLOC_HOOK_BITMAP_FIELD_FULL   (~((size_t)0))   // all bits set

// An atomic bitmap of `size_t` fields
typedef _Atomic(size_t)  alloc_hook_bitmap_field_t;
typedef alloc_hook_bitmap_field_t*  alloc_hook_bitmap_t;

// A bitmap index is the index of the bit in a bitmap.
typedef size_t alloc_hook_bitmap_index_t;

// Create a bit index.
static inline alloc_hook_bitmap_index_t alloc_hook_bitmap_index_create(size_t idx, size_t bitidx) {
  alloc_hook_assert_internal(bitidx < ALLOC_HOOK_BITMAP_FIELD_BITS);
  return (idx*ALLOC_HOOK_BITMAP_FIELD_BITS) + bitidx;
}

// Create a bit index.
static inline alloc_hook_bitmap_index_t alloc_hook_bitmap_index_create_from_bit(size_t full_bitidx) {  
  return alloc_hook_bitmap_index_create(full_bitidx / ALLOC_HOOK_BITMAP_FIELD_BITS, full_bitidx % ALLOC_HOOK_BITMAP_FIELD_BITS);
}

// Get the field index from a bit index.
static inline size_t alloc_hook_bitmap_index_field(alloc_hook_bitmap_index_t bitmap_idx) {
  return (bitmap_idx / ALLOC_HOOK_BITMAP_FIELD_BITS);
}

// Get the bit index in a bitmap field
static inline size_t alloc_hook_bitmap_index_bit_in_field(alloc_hook_bitmap_index_t bitmap_idx) {
  return (bitmap_idx % ALLOC_HOOK_BITMAP_FIELD_BITS);
}

// Get the full bit index
static inline size_t alloc_hook_bitmap_index_bit(alloc_hook_bitmap_index_t bitmap_idx) {
  return bitmap_idx;
}

/* -----------------------------------------------------------
  Claim a bit sequence atomically
----------------------------------------------------------- */

// Try to atomically claim a sequence of `count` bits in a single
// field at `idx` in `bitmap`. Returns `true` on success.
bool _alloc_hook_bitmap_try_find_claim_field(alloc_hook_bitmap_t bitmap, size_t idx, const size_t count, alloc_hook_bitmap_index_t* bitmap_idx);

// Starts at idx, and wraps around to search in all `bitmap_fields` fields.
// For now, `count` can be at most ALLOC_HOOK_BITMAP_FIELD_BITS and will never cross fields.
bool _alloc_hook_bitmap_try_find_from_claim(alloc_hook_bitmap_t bitmap, const size_t bitmap_fields, const size_t start_field_idx, const size_t count, alloc_hook_bitmap_index_t* bitmap_idx);

// Like _alloc_hook_bitmap_try_find_from_claim but with an extra predicate that must be fullfilled
typedef bool (alloc_hook_cdecl *alloc_hook_bitmap_pred_fun_t)(alloc_hook_bitmap_index_t bitmap_idx, void* pred_arg);
bool _alloc_hook_bitmap_try_find_from_claim_pred(alloc_hook_bitmap_t bitmap, const size_t bitmap_fields, const size_t start_field_idx, const size_t count, alloc_hook_bitmap_pred_fun_t pred_fun, void* pred_arg, alloc_hook_bitmap_index_t* bitmap_idx);

// Set `count` bits at `bitmap_idx` to 0 atomically
// Returns `true` if all `count` bits were 1 previously.
bool _alloc_hook_bitmap_unclaim(alloc_hook_bitmap_t bitmap, size_t bitmap_fields, size_t count, alloc_hook_bitmap_index_t bitmap_idx);

// Try to set `count` bits at `bitmap_idx` from 0 to 1 atomically. 
// Returns `true` if successful when all previous `count` bits were 0.
bool _alloc_hook_bitmap_try_claim(alloc_hook_bitmap_t bitmap, size_t bitmap_fields, size_t count, alloc_hook_bitmap_index_t bitmap_idx);

// Set `count` bits at `bitmap_idx` to 1 atomically
// Returns `true` if all `count` bits were 0 previously. `any_zero` is `true` if there was at least one zero bit.
bool _alloc_hook_bitmap_claim(alloc_hook_bitmap_t bitmap, size_t bitmap_fields, size_t count, alloc_hook_bitmap_index_t bitmap_idx, bool* any_zero);

bool _alloc_hook_bitmap_is_claimed(alloc_hook_bitmap_t bitmap, size_t bitmap_fields, size_t count, alloc_hook_bitmap_index_t bitmap_idx);
bool _alloc_hook_bitmap_is_any_claimed(alloc_hook_bitmap_t bitmap, size_t bitmap_fields, size_t count, alloc_hook_bitmap_index_t bitmap_idx);


//--------------------------------------------------------------------------
// the `_across` functions work on bitmaps where sequences can cross over
// between the fields. This is used in arena allocation
//--------------------------------------------------------------------------

// Find `count` bits of zeros and set them to 1 atomically; returns `true` on success.
// Starts at idx, and wraps around to search in all `bitmap_fields` fields.
bool _alloc_hook_bitmap_try_find_from_claim_across(alloc_hook_bitmap_t bitmap, const size_t bitmap_fields, const size_t start_field_idx, const size_t count, alloc_hook_bitmap_index_t* bitmap_idx);

// Set `count` bits at `bitmap_idx` to 0 atomically
// Returns `true` if all `count` bits were 1 previously.
bool _alloc_hook_bitmap_unclaim_across(alloc_hook_bitmap_t bitmap, size_t bitmap_fields, size_t count, alloc_hook_bitmap_index_t bitmap_idx);

// Set `count` bits at `bitmap_idx` to 1 atomically
// Returns `true` if all `count` bits were 0 previously. `any_zero` is `true` if there was at least one zero bit.
bool _alloc_hook_bitmap_claim_across(alloc_hook_bitmap_t bitmap, size_t bitmap_fields, size_t count, alloc_hook_bitmap_index_t bitmap_idx, bool* pany_zero);

bool _alloc_hook_bitmap_is_claimed_across(alloc_hook_bitmap_t bitmap, size_t bitmap_fields, size_t count, alloc_hook_bitmap_index_t bitmap_idx);
bool _alloc_hook_bitmap_is_any_claimed_across(alloc_hook_bitmap_t bitmap, size_t bitmap_fields, size_t count, alloc_hook_bitmap_index_t bitmap_idx);

#endif
