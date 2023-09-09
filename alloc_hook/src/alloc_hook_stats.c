/* ----------------------------------------------------------------------------
Copyright (c) 2018-2021, Microsoft Research, Daan Leijen
This is free software; you can redistribute it and/or modify it under the
terms of the MIT license. A copy of the license can be found in the file
"LICENSE" at the root of this distribution.
-----------------------------------------------------------------------------*/
#include "alloc_hook.h"
#include "alloc_hook_internal.h"
#include "alloc_hook_atomic.h"
#include "alloc_hook_prim.h"

#include <stdio.h>  // snprintf
#include <string.h> // memset

#if defined(_MSC_VER) && (_MSC_VER < 1920)
#pragma warning(disable:4204)  // non-constant aggregate initializer
#endif

/* -----------------------------------------------------------
  Statistics operations
----------------------------------------------------------- */

static bool alloc_hook_is_in_main(void* stat) {
  return ((uint8_t*)stat >= (uint8_t*)&_alloc_hook_stats_main
         && (uint8_t*)stat < ((uint8_t*)&_alloc_hook_stats_main + sizeof(alloc_hook_stats_t)));
}

static void alloc_hook_stat_update(alloc_hook_stat_count_t* stat, int64_t amount) {
  if (amount == 0) return;
  if (alloc_hook_is_in_main(stat))
  {
    // add atomically (for abandoned pages)
    int64_t current = alloc_hook_atomic_addi64_relaxed(&stat->current, amount);
    alloc_hook_atomic_maxi64_relaxed(&stat->peak, current + amount);
    if (amount > 0) {
      alloc_hook_atomic_addi64_relaxed(&stat->allocated,amount);
    }
    else {
      alloc_hook_atomic_addi64_relaxed(&stat->freed, -amount);
    }
  }
  else {
    // add thread local
    stat->current += amount;
    if (stat->current > stat->peak) stat->peak = stat->current;
    if (amount > 0) {
      stat->allocated += amount;
    }
    else {
      stat->freed += -amount;
    }
  }
}

void _alloc_hook_stat_counter_increase(alloc_hook_stat_counter_t* stat, size_t amount) {
  if (alloc_hook_is_in_main(stat)) {
    alloc_hook_atomic_addi64_relaxed( &stat->count, 1 );
    alloc_hook_atomic_addi64_relaxed( &stat->total, (int64_t)amount );
  }
  else {
    stat->count++;
    stat->total += amount;
  }
}

void _alloc_hook_stat_increase(alloc_hook_stat_count_t* stat, size_t amount) {
  alloc_hook_stat_update(stat, (int64_t)amount);
}

void _alloc_hook_stat_decrease(alloc_hook_stat_count_t* stat, size_t amount) {
  alloc_hook_stat_update(stat, -((int64_t)amount));
}

// must be thread safe as it is called from stats_merge
static void alloc_hook_stat_add(alloc_hook_stat_count_t* stat, const alloc_hook_stat_count_t* src, int64_t unit) {
  if (stat==src) return;
  if (src->allocated==0 && src->freed==0) return;
  alloc_hook_atomic_addi64_relaxed( &stat->allocated, src->allocated * unit);
  alloc_hook_atomic_addi64_relaxed( &stat->current, src->current * unit);
  alloc_hook_atomic_addi64_relaxed( &stat->freed, src->freed * unit);
  // peak scores do not work across threads..
  alloc_hook_atomic_addi64_relaxed( &stat->peak, src->peak * unit);
}

static void alloc_hook_stat_counter_add(alloc_hook_stat_counter_t* stat, const alloc_hook_stat_counter_t* src, int64_t unit) {
  if (stat==src) return;
  alloc_hook_atomic_addi64_relaxed( &stat->total, src->total * unit);
  alloc_hook_atomic_addi64_relaxed( &stat->count, src->count * unit);
}

// must be thread safe as it is called from stats_merge
static void alloc_hook_stats_add(alloc_hook_stats_t* stats, const alloc_hook_stats_t* src) {
  if (stats==src) return;
  alloc_hook_stat_add(&stats->segments, &src->segments,1);
  alloc_hook_stat_add(&stats->pages, &src->pages,1);
  alloc_hook_stat_add(&stats->reserved, &src->reserved, 1);
  alloc_hook_stat_add(&stats->committed, &src->committed, 1);
  alloc_hook_stat_add(&stats->reset, &src->reset, 1);
  alloc_hook_stat_add(&stats->purged, &src->purged, 1);
  alloc_hook_stat_add(&stats->page_committed, &src->page_committed, 1);

  alloc_hook_stat_add(&stats->pages_abandoned, &src->pages_abandoned, 1);
  alloc_hook_stat_add(&stats->segments_abandoned, &src->segments_abandoned, 1);
  alloc_hook_stat_add(&stats->threads, &src->threads, 1);

  alloc_hook_stat_add(&stats->malloc, &src->malloc, 1);
  alloc_hook_stat_add(&stats->segments_cache, &src->segments_cache, 1);
  alloc_hook_stat_add(&stats->normal, &src->normal, 1);
  alloc_hook_stat_add(&stats->huge, &src->huge, 1);
  alloc_hook_stat_add(&stats->large, &src->large, 1);

  alloc_hook_stat_counter_add(&stats->pages_extended, &src->pages_extended, 1);
  alloc_hook_stat_counter_add(&stats->mmap_calls, &src->mmap_calls, 1);
  alloc_hook_stat_counter_add(&stats->commit_calls, &src->commit_calls, 1);
  alloc_hook_stat_counter_add(&stats->reset_calls, &src->reset_calls, 1);
  alloc_hook_stat_counter_add(&stats->purge_calls, &src->purge_calls, 1);

  alloc_hook_stat_counter_add(&stats->page_no_retire, &src->page_no_retire, 1);
  alloc_hook_stat_counter_add(&stats->searches, &src->searches, 1);
  alloc_hook_stat_counter_add(&stats->normal_count, &src->normal_count, 1);
  alloc_hook_stat_counter_add(&stats->huge_count, &src->huge_count, 1);
  alloc_hook_stat_counter_add(&stats->large_count, &src->large_count, 1);
#if ALLOC_HOOK_STAT>1
  for (size_t i = 0; i <= ALLOC_HOOK_BIN_HUGE; i++) {
    if (src->normal_bins[i].allocated > 0 || src->normal_bins[i].freed > 0) {
      alloc_hook_stat_add(&stats->normal_bins[i], &src->normal_bins[i], 1);
    }
  }
#endif
}

/* -----------------------------------------------------------
  Display statistics
----------------------------------------------------------- */

// unit > 0 : size in binary bytes
// unit == 0: count as decimal
// unit < 0 : count in binary
static void alloc_hook_printf_amount(int64_t n, int64_t unit, alloc_hook_output_fun* out, void* arg, const char* fmt) {
  char buf[32]; buf[0] = 0;
  int  len = 32;
  const char* suffix = (unit <= 0 ? " " : "B");
  const int64_t base = (unit == 0 ? 1000 : 1024);
  if (unit>0) n *= unit;

  const int64_t pos = (n < 0 ? -n : n);
  if (pos < base) {
    if (n!=1 || suffix[0] != 'B') {  // skip printing 1 B for the unit column
      snprintf(buf, len, "%d   %-3s", (int)n, (n==0 ? "" : suffix));
    }
  }
  else {
    int64_t divider = base;
    const char* magnitude = "K";
    if (pos >= divider*base) { divider *= base; magnitude = "M"; }
    if (pos >= divider*base) { divider *= base; magnitude = "G"; }
    const int64_t tens = (n / (divider/10));
    const long whole = (long)(tens/10);
    const long frac1 = (long)(tens%10);
    char unitdesc[8];
    snprintf(unitdesc, 8, "%s%s%s", magnitude, (base==1024 ? "i" : ""), suffix);
    snprintf(buf, len, "%ld.%ld %-3s", whole, (frac1 < 0 ? -frac1 : frac1), unitdesc);
  }
  _alloc_hook_fprintf(out, arg, (fmt==NULL ? "%12s" : fmt), buf);
}


static void alloc_hook_print_amount(int64_t n, int64_t unit, alloc_hook_output_fun* out, void* arg) {
  alloc_hook_printf_amount(n,unit,out,arg,NULL);
}

static void alloc_hook_print_count(int64_t n, int64_t unit, alloc_hook_output_fun* out, void* arg) {
  if (unit==1) _alloc_hook_fprintf(out, arg, "%12s"," ");
          else alloc_hook_print_amount(n,0,out,arg);
}

static void alloc_hook_stat_print_ex(const alloc_hook_stat_count_t* stat, const char* msg, int64_t unit, alloc_hook_output_fun* out, void* arg, const char* notok ) {
  _alloc_hook_fprintf(out, arg,"%10s:", msg);
  if (unit > 0) {
    alloc_hook_print_amount(stat->peak, unit, out, arg);
    alloc_hook_print_amount(stat->allocated, unit, out, arg);
    alloc_hook_print_amount(stat->freed, unit, out, arg);
    alloc_hook_print_amount(stat->current, unit, out, arg);
    alloc_hook_print_amount(unit, 1, out, arg);
    alloc_hook_print_count(stat->allocated, unit, out, arg);
    if (stat->allocated > stat->freed) {
      _alloc_hook_fprintf(out, arg, "  ");
      _alloc_hook_fprintf(out, arg, (notok == NULL ? "not all freed" : notok));
      _alloc_hook_fprintf(out, arg, "\n");
    }
    else {
      _alloc_hook_fprintf(out, arg, "  ok\n");
    }
  }
  else if (unit<0) {
    alloc_hook_print_amount(stat->peak, -1, out, arg);
    alloc_hook_print_amount(stat->allocated, -1, out, arg);
    alloc_hook_print_amount(stat->freed, -1, out, arg);
    alloc_hook_print_amount(stat->current, -1, out, arg);
    if (unit==-1) {
      _alloc_hook_fprintf(out, arg, "%24s", "");
    }
    else {
      alloc_hook_print_amount(-unit, 1, out, arg);
      alloc_hook_print_count((stat->allocated / -unit), 0, out, arg);
    }
    if (stat->allocated > stat->freed)
      _alloc_hook_fprintf(out, arg, "  not all freed!\n");
    else
      _alloc_hook_fprintf(out, arg, "  ok\n");
  }
  else {
    alloc_hook_print_amount(stat->peak, 1, out, arg);
    alloc_hook_print_amount(stat->allocated, 1, out, arg);
    _alloc_hook_fprintf(out, arg, "%11s", " ");  // no freed
    alloc_hook_print_amount(stat->current, 1, out, arg);
    _alloc_hook_fprintf(out, arg, "\n");
  }
}

static void alloc_hook_stat_print(const alloc_hook_stat_count_t* stat, const char* msg, int64_t unit, alloc_hook_output_fun* out, void* arg) {
  alloc_hook_stat_print_ex(stat, msg, unit, out, arg, NULL);
}

static void alloc_hook_stat_peak_print(const alloc_hook_stat_count_t* stat, const char* msg, int64_t unit, alloc_hook_output_fun* out, void* arg) {
  _alloc_hook_fprintf(out, arg, "%10s:", msg);
  alloc_hook_print_amount(stat->peak, unit, out, arg);
  _alloc_hook_fprintf(out, arg, "\n");
}

static void alloc_hook_stat_counter_print(const alloc_hook_stat_counter_t* stat, const char* msg, alloc_hook_output_fun* out, void* arg ) {
  _alloc_hook_fprintf(out, arg, "%10s:", msg);
  alloc_hook_print_amount(stat->total, -1, out, arg);
  _alloc_hook_fprintf(out, arg, "\n");
}


static void alloc_hook_stat_counter_print_avg(const alloc_hook_stat_counter_t* stat, const char* msg, alloc_hook_output_fun* out, void* arg) {
  const int64_t avg_tens = (stat->count == 0 ? 0 : (stat->total*10 / stat->count));
  const long avg_whole = (long)(avg_tens/10);
  const long avg_frac1 = (long)(avg_tens%10);
  _alloc_hook_fprintf(out, arg, "%10s: %5ld.%ld avg\n", msg, avg_whole, avg_frac1);
}


static void alloc_hook_print_header(alloc_hook_output_fun* out, void* arg ) {
  _alloc_hook_fprintf(out, arg, "%10s: %11s %11s %11s %11s %11s %11s\n", "heap stats", "peak   ", "total   ", "freed   ", "current   ", "unit   ", "count   ");
}

#if ALLOC_HOOK_STAT>1
static void alloc_hook_stats_print_bins(const alloc_hook_stat_count_t* bins, size_t max, const char* fmt, alloc_hook_output_fun* out, void* arg) {
  bool found = false;
  char buf[64];
  for (size_t i = 0; i <= max; i++) {
    if (bins[i].allocated > 0) {
      found = true;
      int64_t unit = _alloc_hook_bin_size((uint8_t)i);
      snprintf(buf, 64, "%s %3lu", fmt, (long)i);
      alloc_hook_stat_print(&bins[i], buf, unit, out, arg);
    }
  }
  if (found) {
    _alloc_hook_fprintf(out, arg, "\n");
    alloc_hook_print_header(out, arg);
  }
}
#endif



//------------------------------------------------------------
// Use an output wrapper for line-buffered output
// (which is nice when using loggers etc.)
//------------------------------------------------------------
typedef struct buffered_s {
  alloc_hook_output_fun* out;   // original output function
  void*          arg;   // and state
  char*          buf;   // local buffer of at least size `count+1`
  size_t         used;  // currently used chars `used <= count`
  size_t         count; // total chars available for output
} buffered_t;

static void alloc_hook_buffered_flush(buffered_t* buf) {
  buf->buf[buf->used] = 0;
  _alloc_hook_fputs(buf->out, buf->arg, NULL, buf->buf);
  buf->used = 0;
}

static void alloc_hook_cdecl alloc_hook_buffered_out(const char* msg, void* arg) {
  buffered_t* buf = (buffered_t*)arg;
  if (msg==NULL || buf==NULL) return;
  for (const char* src = msg; *src != 0; src++) {
    char c = *src;
    if (buf->used >= buf->count) alloc_hook_buffered_flush(buf);
    alloc_hook_assert_internal(buf->used < buf->count);
    buf->buf[buf->used++] = c;
    if (c == '\n') alloc_hook_buffered_flush(buf);
  }
}

//------------------------------------------------------------
// Print statistics
//------------------------------------------------------------

static void _alloc_hook_stats_print(alloc_hook_stats_t* stats, alloc_hook_output_fun* out0, void* arg0) alloc_hook_attr_noexcept {
  // wrap the output function to be line buffered
  char buf[256];
  buffered_t buffer = { out0, arg0, NULL, 0, 255 };
  buffer.buf = buf;
  alloc_hook_output_fun* out = &alloc_hook_buffered_out;
  void* arg = &buffer;

  // and print using that
  alloc_hook_print_header(out,arg);
  #if ALLOC_HOOK_STAT>1
  alloc_hook_stats_print_bins(stats->normal_bins, ALLOC_HOOK_BIN_HUGE, "normal",out,arg);
  #endif
  #if ALLOC_HOOK_STAT
  alloc_hook_stat_print(&stats->normal, "normal", (stats->normal_count.count == 0 ? 1 : -(stats->normal.allocated / stats->normal_count.count)), out, arg);
  alloc_hook_stat_print(&stats->large, "large", (stats->large_count.count == 0 ? 1 : -(stats->large.allocated / stats->large_count.count)), out, arg);
  alloc_hook_stat_print(&stats->huge, "huge", (stats->huge_count.count == 0 ? 1 : -(stats->huge.allocated / stats->huge_count.count)), out, arg);
  alloc_hook_stat_count_t total = { 0,0,0,0 };
  alloc_hook_stat_add(&total, &stats->normal, 1);
  alloc_hook_stat_add(&total, &stats->large, 1);
  alloc_hook_stat_add(&total, &stats->huge, 1);
  alloc_hook_stat_print(&total, "total", 1, out, arg);
  #endif
  #if ALLOC_HOOK_STAT>1
  alloc_hook_stat_print(&stats->malloc, "malloc req", 1, out, arg);
  _alloc_hook_fprintf(out, arg, "\n");
  #endif
  alloc_hook_stat_print_ex(&stats->reserved, "reserved", 1, out, arg, "");
  alloc_hook_stat_print_ex(&stats->committed, "committed", 1, out, arg, "");
  alloc_hook_stat_peak_print(&stats->reset, "reset", 1, out, arg );
  alloc_hook_stat_peak_print(&stats->purged, "purged", 1, out, arg );
  alloc_hook_stat_print(&stats->page_committed, "touched", 1, out, arg);
  alloc_hook_stat_print(&stats->segments, "segments", -1, out, arg);
  alloc_hook_stat_print(&stats->segments_abandoned, "-abandoned", -1, out, arg);
  alloc_hook_stat_print(&stats->segments_cache, "-cached", -1, out, arg);
  alloc_hook_stat_print(&stats->pages, "pages", -1, out, arg);
  alloc_hook_stat_print(&stats->pages_abandoned, "-abandoned", -1, out, arg);
  alloc_hook_stat_counter_print(&stats->pages_extended, "-extended", out, arg);
  alloc_hook_stat_counter_print(&stats->page_no_retire, "-noretire", out, arg);
  alloc_hook_stat_counter_print(&stats->mmap_calls, "mmaps", out, arg);
  alloc_hook_stat_counter_print(&stats->commit_calls, "commits", out, arg);
  alloc_hook_stat_counter_print(&stats->reset_calls, "resets", out, arg);
  alloc_hook_stat_counter_print(&stats->purge_calls, "purges", out, arg);
  alloc_hook_stat_print(&stats->threads, "threads", -1, out, arg);
  alloc_hook_stat_counter_print_avg(&stats->searches, "searches", out, arg);
  _alloc_hook_fprintf(out, arg, "%10s: %5zu\n", "numa nodes", _alloc_hook_os_numa_node_count());

  size_t elapsed;
  size_t user_time;
  size_t sys_time;
  size_t current_rss;
  size_t peak_rss;
  size_t current_commit;
  size_t peak_commit;
  size_t page_faults;
  alloc_hook_process_info(&elapsed, &user_time, &sys_time, &current_rss, &peak_rss, &current_commit, &peak_commit, &page_faults);
  _alloc_hook_fprintf(out, arg, "%10s: %5ld.%03ld s\n", "elapsed", elapsed/1000, elapsed%1000);
  _alloc_hook_fprintf(out, arg, "%10s: user: %ld.%03ld s, system: %ld.%03ld s, faults: %lu, rss: ", "process",
              user_time/1000, user_time%1000, sys_time/1000, sys_time%1000, (unsigned long)page_faults );
  alloc_hook_printf_amount((int64_t)peak_rss, 1, out, arg, "%s");
  if (peak_commit > 0) {
    _alloc_hook_fprintf(out, arg, ", commit: ");
    alloc_hook_printf_amount((int64_t)peak_commit, 1, out, arg, "%s");
  }
  _alloc_hook_fprintf(out, arg, "\n");
}

static alloc_hook_msecs_t alloc_hook_process_start; // = 0

static alloc_hook_stats_t* alloc_hook_stats_get_default(void) {
  alloc_hook_heap_t* heap = alloc_hook_heap_get_default();
  return &heap->tld->stats;
}

static void alloc_hook_stats_merge_from(alloc_hook_stats_t* stats) {
  if (stats != &_alloc_hook_stats_main) {
    alloc_hook_stats_add(&_alloc_hook_stats_main, stats);
    memset(stats, 0, sizeof(alloc_hook_stats_t));
  }
}

void alloc_hook_stats_reset(void) alloc_hook_attr_noexcept {
  alloc_hook_stats_t* stats = alloc_hook_stats_get_default();
  if (stats != &_alloc_hook_stats_main) { memset(stats, 0, sizeof(alloc_hook_stats_t)); }
  memset(&_alloc_hook_stats_main, 0, sizeof(alloc_hook_stats_t));
  if (alloc_hook_process_start == 0) { alloc_hook_process_start = _alloc_hook_clock_start(); };
}

void alloc_hook_stats_merge(void) alloc_hook_attr_noexcept {
  alloc_hook_stats_merge_from( alloc_hook_stats_get_default() );
}

void _alloc_hook_stats_done(alloc_hook_stats_t* stats) {  // called from `alloc_hook_thread_done`
  alloc_hook_stats_merge_from(stats);
}

void alloc_hook_stats_print_out(alloc_hook_output_fun* out, void* arg) alloc_hook_attr_noexcept {
  alloc_hook_stats_merge_from(alloc_hook_stats_get_default());
  _alloc_hook_stats_print(&_alloc_hook_stats_main, out, arg);
}

void alloc_hook_stats_print(void* out) alloc_hook_attr_noexcept {
  // for compatibility there is an `out` parameter (which can be `stdout` or `stderr`)
  alloc_hook_stats_print_out((alloc_hook_output_fun*)out, NULL);
}

void alloc_hook_thread_stats_print_out(alloc_hook_output_fun* out, void* arg) alloc_hook_attr_noexcept {
  _alloc_hook_stats_print(alloc_hook_stats_get_default(), out, arg);
}


// ----------------------------------------------------------------
// Basic timer for convenience; use milli-seconds to avoid doubles
// ----------------------------------------------------------------

static alloc_hook_msecs_t alloc_hook_clock_diff;

alloc_hook_msecs_t _alloc_hook_clock_now(void) {
  return _alloc_hook_prim_clock_now();
}

alloc_hook_msecs_t _alloc_hook_clock_start(void) {
  if (alloc_hook_clock_diff == 0.0) {
    alloc_hook_msecs_t t0 = _alloc_hook_clock_now();
    alloc_hook_clock_diff = _alloc_hook_clock_now() - t0;
  }
  return _alloc_hook_clock_now();
}

alloc_hook_msecs_t _alloc_hook_clock_end(alloc_hook_msecs_t start) {
  alloc_hook_msecs_t end = _alloc_hook_clock_now();
  return (end - start - alloc_hook_clock_diff);
}


// --------------------------------------------------------
// Basic process statistics
// --------------------------------------------------------

alloc_hook_decl_export void alloc_hook_process_info(size_t* elapsed_msecs, size_t* user_msecs, size_t* system_msecs, size_t* current_rss, size_t* peak_rss, size_t* current_commit, size_t* peak_commit, size_t* page_faults) alloc_hook_attr_noexcept
{
  alloc_hook_process_info_t pinfo;
  _alloc_hook_memzero_var(pinfo);
  pinfo.elapsed        = _alloc_hook_clock_end(alloc_hook_process_start);
  pinfo.current_commit = (size_t)(alloc_hook_atomic_loadi64_relaxed((_Atomic(int64_t)*)&_alloc_hook_stats_main.committed.current));
  pinfo.peak_commit    = (size_t)(alloc_hook_atomic_loadi64_relaxed((_Atomic(int64_t)*)&_alloc_hook_stats_main.committed.peak));
  pinfo.current_rss    = pinfo.current_commit;
  pinfo.peak_rss       = pinfo.peak_commit;
  pinfo.utime          = 0;
  pinfo.stime          = 0;
  pinfo.page_faults    = 0;

  _alloc_hook_prim_process_info(&pinfo);
  
  if (elapsed_msecs!=NULL)  *elapsed_msecs  = (pinfo.elapsed < 0 ? 0 : (pinfo.elapsed < (alloc_hook_msecs_t)PTRDIFF_MAX ? (size_t)pinfo.elapsed : PTRDIFF_MAX));
  if (user_msecs!=NULL)     *user_msecs     = (pinfo.utime < 0 ? 0 : (pinfo.utime < (alloc_hook_msecs_t)PTRDIFF_MAX ? (size_t)pinfo.utime : PTRDIFF_MAX));
  if (system_msecs!=NULL)   *system_msecs   = (pinfo.stime < 0 ? 0 : (pinfo.stime < (alloc_hook_msecs_t)PTRDIFF_MAX ? (size_t)pinfo.stime : PTRDIFF_MAX));
  if (current_rss!=NULL)    *current_rss    = pinfo.current_rss;
  if (peak_rss!=NULL)       *peak_rss       = pinfo.peak_rss;
  if (current_commit!=NULL) *current_commit = pinfo.current_commit;
  if (peak_commit!=NULL)    *peak_commit    = pinfo.peak_commit;
  if (page_faults!=NULL)    *page_faults    = pinfo.page_faults;
}
