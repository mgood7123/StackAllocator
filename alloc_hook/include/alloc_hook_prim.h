/* ----------------------------------------------------------------------------
Copyright (c) 2018-2023, Microsoft Research, Daan Leijen
This is free software; you can redistribute it and/or modify it under the
terms of the MIT license. A copy of the license can be found in the file
"LICENSE" at the root of this distribution.
-----------------------------------------------------------------------------*/

#ifndef ALLOC_HOOK_PRIM_H
#define ALLOC_HOOK_PRIM_H


// --------------------------------------------------------------------------
// This file specifies the primitive portability API.
// Each OS/host needs to implement these primitives, see `src/prim`
// for implementations on Window, macOS, WASI, and Linux/Unix.
//
// note: on all primitive functions, we always have result parameters != NUL, and:
//  addr != NULL and page aligned
//  size > 0     and page aligned
//  return value is an error code an int where 0 is success.
// --------------------------------------------------------------------------

// OS memory configuration
typedef struct alloc_hook_os_mem_config_s {
  size_t  page_size;            // 4KiB
  size_t  large_page_size;      // 2MiB
  size_t  alloc_granularity;    // smallest allocation size (on Windows 64KiB)
  bool    has_overcommit;       // can we reserve more memory than can be actually committed?
  bool    must_free_whole;      // must allocated blocks be freed as a whole (false for mmap, true for VirtualAlloc)
  bool    has_virtual_reserve;  // supports virtual address space reservation? (if true we can reserve virtual address space without using commit or physical memory)
} alloc_hook_os_mem_config_t;

// Initialize
void _alloc_hook_prim_mem_init( alloc_hook_os_mem_config_t* config );

// Free OS memory
int _alloc_hook_prim_free(void* addr, size_t size );
  
// Allocate OS memory. Return NULL on error.
// The `try_alignment` is just a hint and the returned pointer does not have to be aligned.
// If `commit` is false, the virtual memory range only needs to be reserved (with no access) 
// which will later be committed explicitly using `_alloc_hook_prim_commit`.
// `is_zero` is set to true if the memory was zero initialized (as on most OS's)
// pre: !commit => !allow_large
//      try_alignment >= _alloc_hook_os_page_size() and a power of 2
int _alloc_hook_prim_alloc(size_t size, size_t try_alignment, bool commit, bool allow_large, bool* is_large, bool* is_zero, void** addr);

// Commit memory. Returns error code or 0 on success.
// For example, on Linux this would make the memory PROT_READ|PROT_WRITE.
// `is_zero` is set to true if the memory was zero initialized (e.g. on Windows)
int _alloc_hook_prim_commit(void* addr, size_t size, bool* is_zero);

// Decommit memory. Returns error code or 0 on success. The `needs_recommit` result is true
// if the memory would need to be re-committed. For example, on Windows this is always true,
// but on Linux we could use MADV_DONTNEED to decommit which does not need a recommit.
// pre: needs_recommit != NULL
int _alloc_hook_prim_decommit(void* addr, size_t size, bool* needs_recommit);

// Reset memory. The range keeps being accessible but the content might be reset.
// Returns error code or 0 on success.
int _alloc_hook_prim_reset(void* addr, size_t size);

// Protect memory. Returns error code or 0 on success.
int _alloc_hook_prim_protect(void* addr, size_t size, bool protect);

// Allocate huge (1GiB) pages possibly associated with a NUMA node.
// `is_zero` is set to true if the memory was zero initialized (as on most OS's)
// pre: size > 0  and a multiple of 1GiB.
//      numa_node is either negative (don't care), or a numa node number.
int _alloc_hook_prim_alloc_huge_os_pages(void* hint_addr, size_t size, int numa_node, bool* is_zero, void** addr);

// Return the current NUMA node
size_t _alloc_hook_prim_numa_node(void);

// Return the number of logical NUMA nodes
size_t _alloc_hook_prim_numa_node_count(void);

// Clock ticks
alloc_hook_msecs_t _alloc_hook_prim_clock_now(void);

// Return process information (only for statistics)
typedef struct alloc_hook_process_info_s {
  alloc_hook_msecs_t  elapsed;
  alloc_hook_msecs_t  utime;
  alloc_hook_msecs_t  stime; 
  size_t      current_rss; 
  size_t      peak_rss;  
  size_t      current_commit;
  size_t      peak_commit; 
  size_t      page_faults;
} alloc_hook_process_info_t;

void _alloc_hook_prim_process_info(alloc_hook_process_info_t* pinfo);

// Default stderr output. (only for warnings etc. with verbose enabled)
// msg != NULL && _alloc_hook_strlen(msg) > 0
void _alloc_hook_prim_out_stderr( const char* msg );

// Get an environment variable. (only for options)
// name != NULL, result != NULL, result_size >= 64
bool _alloc_hook_prim_getenv(const char* name, char* result, size_t result_size);


// Fill a buffer with strong randomness; return `false` on error or if
// there is no strong randomization available.
bool _alloc_hook_prim_random_buf(void* buf, size_t buf_len);

// Called on the first thread start, and should ensure `_alloc_hook_thread_done` is called on thread termination.
void _alloc_hook_prim_thread_init_auto_done(void);

// Called on process exit and may take action to clean up resources associated with the thread auto done.
void _alloc_hook_prim_thread_done_auto_done(void);

// Called when the default heap for a thread changes
void _alloc_hook_prim_thread_associate_default_heap(alloc_hook_heap_t* heap);


//-------------------------------------------------------------------
// Thread id: `_alloc_hook_prim_thread_id()`
// 
// Getting the thread id should be performant as it is called in the
// fast path of `_alloc_hook_free` and we specialize for various platforms as
// inlined definitions. Regular code should call `init.c:_alloc_hook_thread_id()`.
// We only require _alloc_hook_prim_thread_id() to return a unique id
// for each thread (unequal to zero).
//-------------------------------------------------------------------

// defined in `init.c`; do not use these directly
extern alloc_hook_decl_thread alloc_hook_heap_t* _alloc_hook_heap_default;  // default heap to allocate from
extern bool _alloc_hook_process_is_initialized;             // has alloc_hook_process_init been called?

static inline alloc_hook_threadid_t _alloc_hook_prim_thread_id(void) alloc_hook_attr_noexcept;

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static inline alloc_hook_threadid_t _alloc_hook_prim_thread_id(void) alloc_hook_attr_noexcept {
  // Windows: works on Intel and ARM in both 32- and 64-bit
  return (uintptr_t)NtCurrentTeb();
}

// We use assembly for a fast thread id on the main platforms. The TLS layout depends on
// both the OS and libc implementation so we use specific tests for each main platform.
// If you test on another platform and it works please send a PR :-)
// see also https://akkadia.org/drepper/tls.pdf for more info on the TLS register.
#elif defined(__GNUC__) && ( \
           (defined(__GLIBC__)   && (defined(__x86_64__) || defined(__i386__) || defined(__arm__) || defined(__aarch64__))) \
        || (defined(__APPLE__)   && (defined(__x86_64__) || defined(__aarch64__))) \
        || (defined(__BIONIC__)  && (defined(__x86_64__) || defined(__i386__) || defined(__arm__) || defined(__aarch64__))) \
        || (defined(__FreeBSD__) && (defined(__x86_64__) || defined(__i386__) || defined(__aarch64__))) \
        || (defined(__OpenBSD__) && (defined(__x86_64__) || defined(__i386__) || defined(__aarch64__))) \
      )

static inline void* alloc_hook_prim_tls_slot(size_t slot) alloc_hook_attr_noexcept {
  void* res;
  const size_t ofs = (slot*sizeof(void*));
  #if defined(__i386__)
    __asm__("movl %%gs:%1, %0" : "=r" (res) : "m" (*((void**)ofs)) : );  // x86 32-bit always uses GS
  #elif defined(__APPLE__) && defined(__x86_64__)
    __asm__("movq %%gs:%1, %0" : "=r" (res) : "m" (*((void**)ofs)) : );  // x86_64 macOSX uses GS
  #elif defined(__x86_64__) && (ALLOC_HOOK_INTPTR_SIZE==4)
    __asm__("movl %%fs:%1, %0" : "=r" (res) : "m" (*((void**)ofs)) : );  // x32 ABI
  #elif defined(__x86_64__)
    __asm__("movq %%fs:%1, %0" : "=r" (res) : "m" (*((void**)ofs)) : );  // x86_64 Linux, BSD uses FS
  #elif defined(__arm__)
    void** tcb; ALLOC_HOOK_UNUSED(ofs);
    __asm__ volatile ("mrc p15, 0, %0, c13, c0, 3\nbic %0, %0, #3" : "=r" (tcb));
    res = tcb[slot];
  #elif defined(__aarch64__)
    void** tcb; ALLOC_HOOK_UNUSED(ofs);
    #if defined(__APPLE__) // M1, issue #343
    __asm__ volatile ("mrs %0, tpidrro_el0\nbic %0, %0, #7" : "=r" (tcb));
    #else
    __asm__ volatile ("mrs %0, tpidr_el0" : "=r" (tcb));
    #endif
    res = tcb[slot];
  #endif
  return res;
}

// setting a tls slot is only used on macOS for now
static inline void alloc_hook_prim_tls_slot_set(size_t slot, void* value) alloc_hook_attr_noexcept {
  const size_t ofs = (slot*sizeof(void*));
  #if defined(__i386__)
    __asm__("movl %1,%%gs:%0" : "=m" (*((void**)ofs)) : "rn" (value) : );  // 32-bit always uses GS
  #elif defined(__APPLE__) && defined(__x86_64__)
    __asm__("movq %1,%%gs:%0" : "=m" (*((void**)ofs)) : "rn" (value) : );  // x86_64 macOS uses GS
  #elif defined(__x86_64__) && (ALLOC_HOOK_INTPTR_SIZE==4)
    __asm__("movl %1,%%fs:%0" : "=m" (*((void**)ofs)) : "rn" (value) : );  // x32 ABI
  #elif defined(__x86_64__)
    __asm__("movq %1,%%fs:%0" : "=m" (*((void**)ofs)) : "rn" (value) : );  // x86_64 Linux, BSD uses FS
  #elif defined(__arm__)
    void** tcb; ALLOC_HOOK_UNUSED(ofs);
    __asm__ volatile ("mrc p15, 0, %0, c13, c0, 3\nbic %0, %0, #3" : "=r" (tcb));
    tcb[slot] = value;
  #elif defined(__aarch64__)
    void** tcb; ALLOC_HOOK_UNUSED(ofs);
    #if defined(__APPLE__) // M1, issue #343
    __asm__ volatile ("mrs %0, tpidrro_el0\nbic %0, %0, #7" : "=r" (tcb));
    #else
    __asm__ volatile ("mrs %0, tpidr_el0" : "=r" (tcb));
    #endif
    tcb[slot] = value;
  #endif
}

static inline alloc_hook_threadid_t _alloc_hook_prim_thread_id(void) alloc_hook_attr_noexcept {
  #if defined(__BIONIC__)
    // issue #384, #495: on the Bionic libc (Android), slot 1 is the thread id
    // see: https://github.com/aosp-mirror/platform_bionic/blob/c44b1d0676ded732df4b3b21c5f798eacae93228/libc/platform/bionic/tls_defines.h#L86
    return (uintptr_t)alloc_hook_prim_tls_slot(1);
  #else
    // in all our other targets, slot 0 is the thread id
    // glibc: https://sourceware.org/git/?p=glibc.git;a=blob_plain;f=sysdeps/x86_64/nptl/tls.h
    // apple: https://github.com/apple/darwin-xnu/blob/main/libsyscall/os/tsd.h#L36
    return (uintptr_t)alloc_hook_prim_tls_slot(0);
  #endif
}

#else

// otherwise use portable C, taking the address of a thread local variable (this is still very fast on most platforms).
static inline alloc_hook_threadid_t _alloc_hook_prim_thread_id(void) alloc_hook_attr_noexcept {
  return (uintptr_t)&_alloc_hook_heap_default;
}

#endif



/* ----------------------------------------------------------------------------------------
The thread local default heap: `_alloc_hook_prim_get_default_heap()`
This is inlined here as it is on the fast path for allocation functions.

On most platforms (Windows, Linux, FreeBSD, NetBSD, etc), this just returns a
__thread local variable (`_alloc_hook_heap_default`). With the initial-exec TLS model this ensures
that the storage will always be available (allocated on the thread stacks).

On some platforms though we cannot use that when overriding `malloc` since the underlying
TLS implementation (or the loader) will call itself `malloc` on a first access and recurse.
We try to circumvent this in an efficient way:
- macOSX : we use an unused TLS slot from the OS allocated slots (ALLOC_HOOK_TLS_SLOT). On OSX, the
           loader itself calls `malloc` even before the modules are initialized.
- OpenBSD: we use an unused slot from the pthread block (ALLOC_HOOK_TLS_PTHREAD_SLOT_OFS).
- DragonFly: defaults are working but seem slow compared to freeBSD (see PR #323)
------------------------------------------------------------------------------------------- */

static inline alloc_hook_heap_t* alloc_hook_prim_get_default_heap(void);

#if defined(ALLOC_HOOK_MALLOC_OVERRIDE)
#if defined(__APPLE__) // macOS
  #define ALLOC_HOOK_TLS_SLOT               89  // seems unused?
  // #define ALLOC_HOOK_TLS_RECURSE_GUARD 1
  // other possible unused ones are 9, 29, __PTK_FRAMEWORK_JAVASCRIPTCORE_KEY4 (94), __PTK_FRAMEWORK_GC_KEY9 (112) and __PTK_FRAMEWORK_OLDGC_KEY9 (89)
  // see <https://github.com/rweichler/substrate/blob/master/include/pthread_machdep.h>
#elif defined(__OpenBSD__)
  // use end bytes of a name; goes wrong if anyone uses names > 23 characters (ptrhread specifies 16)
  // see <https://github.com/openbsd/src/blob/master/lib/libc/include/thread_private.h#L371>
  #define ALLOC_HOOK_TLS_PTHREAD_SLOT_OFS   (6*sizeof(int) + 4*sizeof(void*) + 24)
  // #elif defined(__DragonFly__)
  // #warning "alloc_hook is not working correctly on DragonFly yet."
  // #define ALLOC_HOOK_TLS_PTHREAD_SLOT_OFS   (4 + 1*sizeof(void*))  // offset `uniqueid` (also used by gdb?) <https://github.com/DragonFlyBSD/DragonFlyBSD/blob/master/lib/libthread_xu/thread/thr_private.h#L458>
#elif defined(__ANDROID__)
  // See issue #381
  #define ALLOC_HOOK_TLS_PTHREAD
#endif
#endif


#if defined(ALLOC_HOOK_TLS_SLOT)

static inline alloc_hook_heap_t* alloc_hook_prim_get_default_heap(void) {
  alloc_hook_heap_t* heap = (alloc_hook_heap_t*)alloc_hook_prim_tls_slot(ALLOC_HOOK_TLS_SLOT);
  if alloc_hook_unlikely(heap == NULL) {
    #ifdef __GNUC__
    __asm(""); // prevent conditional load of the address of _alloc_hook_heap_empty
    #endif
    heap = (alloc_hook_heap_t*)&_alloc_hook_heap_empty;
  }
  return heap;
}

#elif defined(ALLOC_HOOK_TLS_PTHREAD_SLOT_OFS)

static inline alloc_hook_heap_t** alloc_hook_prim_tls_pthread_heap_slot(void) {
  pthread_t self = pthread_self();
  #if defined(__DragonFly__)
  if (self==NULL) return NULL;
  #endif
  return (alloc_hook_heap_t**)((uint8_t*)self + ALLOC_HOOK_TLS_PTHREAD_SLOT_OFS);
}

static inline alloc_hook_heap_t* alloc_hook_prim_get_default_heap(void) {
  alloc_hook_heap_t** pheap = alloc_hook_prim_tls_pthread_heap_slot();
  if alloc_hook_unlikely(pheap == NULL) return _alloc_hook_heap_main_get();
  alloc_hook_heap_t* heap = *pheap;
  if alloc_hook_unlikely(heap == NULL) return (alloc_hook_heap_t*)&_alloc_hook_heap_empty;
  return heap;
}

#elif defined(ALLOC_HOOK_TLS_PTHREAD)

extern pthread_key_t _alloc_hook_heap_default_key;
static inline alloc_hook_heap_t* alloc_hook_prim_get_default_heap(void) {
  alloc_hook_heap_t* heap = (alloc_hook_unlikely(_alloc_hook_heap_default_key == (pthread_key_t)(-1)) ? _alloc_hook_heap_main_get() : (alloc_hook_heap_t*)pthread_getspecific(_alloc_hook_heap_default_key));
  return (alloc_hook_unlikely(heap == NULL) ? (alloc_hook_heap_t*)&_alloc_hook_heap_empty : heap);
}

#else // default using a thread local variable; used on most platforms.

static inline alloc_hook_heap_t* alloc_hook_prim_get_default_heap(void) {
  #if defined(ALLOC_HOOK_TLS_RECURSE_GUARD)
  if (alloc_hook_unlikely(!_alloc_hook_process_is_initialized)) return _alloc_hook_heap_main_get();
  #endif
  return _alloc_hook_heap_default;
}

#endif  // alloc_hook_prim_get_default_heap()

int alloc_hook_prim_dup(int fd);
int alloc_hook_prim_open(const char* fpath, int open_flags);
ssize_t alloc_hook_prim_read(int fd, void* buf, size_t bufsize);
ssize_t alloc_hook_prim_write(int fd, const void* buf, size_t bufsize);
int alloc_hook_prim_close(int fd);
int alloc_hook_prim_access(const char *fpath, int mode);

#endif  // ALLOC_HOOK_PRIM_H
