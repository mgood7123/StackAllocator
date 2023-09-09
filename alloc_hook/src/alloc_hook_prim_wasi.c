/* ----------------------------------------------------------------------------
Copyright (c) 2018-2023, Microsoft Research, Daan Leijen
This is free software; you can redistribute it and/or modify it under the
terms of the MIT license. A copy of the license can be found in the file
"LICENSE" at the root of this distribution.
-----------------------------------------------------------------------------*/

// This file is included in `src/prim/prim.c`

#include "alloc_hook.h"
#include "alloc_hook_internal.h"
#include "alloc_hook_atomic.h"
#include "alloc_hook_prim.h"

//---------------------------------------------
// Initialize
//---------------------------------------------

void _alloc_hook_prim_mem_init( alloc_hook_os_mem_config_t* config ) {
  config->page_size = 64*ALLOC_HOOK_KiB; // WebAssembly has a fixed page size: 64KiB
  config->alloc_granularity = 16;
  config->has_overcommit = false;  
  config->must_free_whole = true;
  config->has_virtual_reserve = false;
}

//---------------------------------------------
// Free
//---------------------------------------------

int _alloc_hook_prim_free(void* addr, size_t size ) {
  ALLOC_HOOK_UNUSED(addr); ALLOC_HOOK_UNUSED(size);
  // wasi heap cannot be shrunk
  return 0;
}


//---------------------------------------------
// Allocation: sbrk or memory_grow
//---------------------------------------------

#if defined(ALLOC_HOOK_USE_SBRK)
  static void* alloc_hook_memory_grow( size_t size ) {
    void* p = sbrk(size);
    if (p == (void*)(-1)) return NULL;
    #if !defined(__wasi__) // on wasi this is always zero initialized already (?)
    memset(p,0,size);
    #endif
    return p;
  }
#elif defined(__wasi__)
  static void* alloc_hook_memory_grow( size_t size ) {
    size_t base = (size > 0 ? __builtin_wasm_memory_grow(0,_alloc_hook_divide_up(size, _alloc_hook_os_page_size()))
                            : __builtin_wasm_memory_size(0));
    if (base == SIZE_MAX) return NULL;
    return (void*)(base * _alloc_hook_os_page_size());
  }
#endif

#if defined(ALLOC_HOOK_USE_PTHREADS)
static pthread_mutex_t alloc_hook_heap_grow_mutex = PTHREAD_MUTEX_INITIALIZER;
#endif

static void* alloc_hook_prim_mem_grow(size_t size, size_t try_alignment) {
  void* p = NULL;
  if (try_alignment <= 1) {
    // `sbrk` is not thread safe in general so try to protect it (we could skip this on WASM but leave it in for now)
    #if defined(ALLOC_HOOK_USE_PTHREADS)
    pthread_mutex_lock(&alloc_hook_heap_grow_mutex);
    #endif
    p = alloc_hook_memory_grow(size);
    #if defined(ALLOC_HOOK_USE_PTHREADS)
    pthread_mutex_unlock(&alloc_hook_heap_grow_mutex);
    #endif
  }
  else {
    void* base = NULL;
    size_t alloc_size = 0;
    // to allocate aligned use a lock to try to avoid thread interaction
    // between getting the current size and actual allocation
    // (also, `sbrk` is not thread safe in general)
    #if defined(ALLOC_HOOK_USE_PTHREADS)
    pthread_mutex_lock(&alloc_hook_heap_grow_mutex);
    #endif
    {
      void* current = alloc_hook_memory_grow(0);  // get current size
      if (current != NULL) {
        void* aligned_current = alloc_hook_align_up_ptr(current, try_alignment);  // and align from there to minimize wasted space
        alloc_size = _alloc_hook_align_up( ((uint8_t*)aligned_current - (uint8_t*)current) + size, _alloc_hook_os_page_size());
        base = alloc_hook_memory_grow(alloc_size);
      }
    }
    #if defined(ALLOC_HOOK_USE_PTHREADS)
    pthread_mutex_unlock(&alloc_hook_heap_grow_mutex);
    #endif
    if (base != NULL) {
      p = alloc_hook_align_up_ptr(base, try_alignment);
      if ((uint8_t*)p + size > (uint8_t*)base + alloc_size) {
        // another thread used wasm_memory_grow/sbrk in-between and we do not have enough
        // space after alignment. Give up (and waste the space as we cannot shrink :-( )
        // (in `alloc_hook_os_mem_alloc_aligned` this will fall back to overallocation to align)
        p = NULL;
      }
    }
  }
  /*
  if (p == NULL) {
    _alloc_hook_warning_message("unable to allocate sbrk/wasm_memory_grow OS memory (%zu bytes, %zu alignment)\n", size, try_alignment);
    errno = ENOMEM;
    return NULL;
  }
  */
  alloc_hook_assert_internal( p == NULL || try_alignment == 0 || (uintptr_t)p % try_alignment == 0 );
  return p;
}

// Note: the `try_alignment` is just a hint and the returned pointer is not guaranteed to be aligned.
int _alloc_hook_prim_alloc(size_t size, size_t try_alignment, bool commit, bool allow_large, bool* is_large, bool* is_zero, void** addr) {
  ALLOC_HOOK_UNUSED(allow_large); ALLOC_HOOK_UNUSED(commit);
  *is_large = false;
  *is_zero = false;
  *addr = alloc_hook_prim_mem_grow(size, try_alignment);
  return (*addr != NULL ? 0 : ENOMEM);
}


//---------------------------------------------
// Commit/Reset/Protect
//---------------------------------------------

int _alloc_hook_prim_commit(void* addr, size_t size, bool* is_zero) {
  ALLOC_HOOK_UNUSED(addr); ALLOC_HOOK_UNUSED(size); 
  *is_zero = false;
  return 0;
}

int _alloc_hook_prim_decommit(void* addr, size_t size, bool* needs_recommit) {
  ALLOC_HOOK_UNUSED(addr); ALLOC_HOOK_UNUSED(size);
  *needs_recommit = false;
  return 0;
}

int _alloc_hook_prim_reset(void* addr, size_t size) {
  ALLOC_HOOK_UNUSED(addr); ALLOC_HOOK_UNUSED(size);
  return 0;
}

int _alloc_hook_prim_protect(void* addr, size_t size, bool protect) {
  ALLOC_HOOK_UNUSED(addr); ALLOC_HOOK_UNUSED(size); ALLOC_HOOK_UNUSED(protect);
  return 0;
}


//---------------------------------------------
// Huge pages and NUMA nodes
//---------------------------------------------

int _alloc_hook_prim_alloc_huge_os_pages(void* hint_addr, size_t size, int numa_node, bool* is_zero, void** addr) {
  ALLOC_HOOK_UNUSED(hint_addr); ALLOC_HOOK_UNUSED(size); ALLOC_HOOK_UNUSED(numa_node);
  *is_zero = true;
  *addr = NULL;
  return ENOSYS;
}

size_t _alloc_hook_prim_numa_node(void) {
  return 0;
}

size_t _alloc_hook_prim_numa_node_count(void) {
  return 1;
}


//----------------------------------------------------------------
// Clock
//----------------------------------------------------------------

#include <time.h>

#if defined(CLOCK_REALTIME) || defined(CLOCK_MONOTONIC)

alloc_hook_msecs_t _alloc_hook_prim_clock_now(void) {
  struct timespec t;
  #ifdef CLOCK_MONOTONIC
  clock_gettime(CLOCK_MONOTONIC, &t);
  #else
  clock_gettime(CLOCK_REALTIME, &t);
  #endif
  return ((alloc_hook_msecs_t)t.tv_sec * 1000) + ((alloc_hook_msecs_t)t.tv_nsec / 1000000);
}

#else

// low resolution timer
alloc_hook_msecs_t _alloc_hook_prim_clock_now(void) {
  #if !defined(CLOCKS_PER_SEC) || (CLOCKS_PER_SEC == 1000) || (CLOCKS_PER_SEC == 0)
  return (alloc_hook_msecs_t)clock();  
  #elif (CLOCKS_PER_SEC < 1000)
  return (alloc_hook_msecs_t)clock() * (1000 / (alloc_hook_msecs_t)CLOCKS_PER_SEC);  
  #else
  return (alloc_hook_msecs_t)clock() / ((alloc_hook_msecs_t)CLOCKS_PER_SEC / 1000);
  #endif
}

#endif


//----------------------------------------------------------------
// Process info
//----------------------------------------------------------------

void _alloc_hook_prim_process_info(alloc_hook_process_info_t* pinfo)
{
  // use defaults
  ALLOC_HOOK_UNUSED(pinfo);
}

int alloc_hook_prim_dup(int fd) {
  return dup(fd);
}
int alloc_hook_prim_open(const char* fpath, int open_flags) {
  return open(fpath,open_flags);
}
ssize_t alloc_hook_prim_read(int fd, void* buf, size_t bufsize) {
  return read(fd,buf,bufsize);
}
ssize_t alloc_hook_prim_write(int fd, void* buf, size_t bufsize) {
  return write(fd,buf,bufsize);
}
int alloc_hook_prim_close(int fd) {
  return close(fd);
}
int alloc_hook_prim_access(const char *fpath, int mode) {
  return access(fpath,mode);
}

//----------------------------------------------------------------
// Output
//----------------------------------------------------------------

void _alloc_hook_prim_out_stderr( const char* msg ) {
  _alloc_hook_write_stderr(msg);
}


//----------------------------------------------------------------
// Environment
//----------------------------------------------------------------

bool _alloc_hook_prim_getenv(const char* name, char* result, size_t result_size) {
  // cannot call getenv() when still initializing the C runtime.
  if (_alloc_hook_preloading()) return false;
  const char* s = getenv(name);
  if (s == NULL) {
    // we check the upper case name too.
    char buf[64+1];
    size_t len = _alloc_hook_strnlen(name,sizeof(buf)-1);
    for (size_t i = 0; i < len; i++) {
      buf[i] = _alloc_hook_toupper(name[i]);
    }
    buf[len] = 0;
    s = getenv(buf);
  }
  if (s == NULL || _alloc_hook_strnlen(s,result_size) >= result_size)  return false;
  _alloc_hook_strlcpy(result, s, result_size);
  return true;
}


//----------------------------------------------------------------
// Random
//----------------------------------------------------------------

bool _alloc_hook_prim_random_buf(void* buf, size_t buf_len) {
  return false;
}


//----------------------------------------------------------------
// Thread init/done
//----------------------------------------------------------------

void _alloc_hook_prim_thread_init_auto_done(void) {
  // nothing
}

void _alloc_hook_prim_thread_done_auto_done(void) {
  // nothing
}

void _alloc_hook_prim_thread_associate_default_heap(alloc_hook_heap_t* heap) {
  ALLOC_HOOK_UNUSED(heap);
}
