/* ----------------------------------------------------------------------------
Copyright (c) 2018-2023 Microsoft Research, Daan Leijen
This is free software; you can redistribute it and/or modify it under the
terms of the MIT license. A copy of the license can be found in the file
"LICENSE" at the root of this distribution.
-----------------------------------------------------------------------------*/

#ifndef ALLOC_HOOK_ATOMIC_H
#define ALLOC_HOOK_ATOMIC_H

// --------------------------------------------------------------------------------------------
// Atomics
// We need to be portable between C, C++, and MSVC.
// We base the primitives on the C/C++ atomics and create a mimimal wrapper for MSVC in C compilation mode.
// This is why we try to use only `uintptr_t` and `<type>*` as atomic types.
// To gain better insight in the range of used atomics, we use explicitly named memory order operations
// instead of passing the memory order as a parameter.
// -----------------------------------------------------------------------------------------------

#if defined(__cplusplus)
// Use C++ atomics
#include <atomic>
#define  _Atomic(tp)            std::atomic<tp>
#define  alloc_hook_atomic(name)        std::atomic_##name
#define  alloc_hook_memory_order(name)  std::memory_order_##name
#if !defined(ATOMIC_VAR_INIT) || (__cplusplus >= 202002L) // c++20, see issue #571
 #define ALLOC_HOOK_ATOMIC_VAR_INIT(x)  x
#else
 #define ALLOC_HOOK_ATOMIC_VAR_INIT(x)  ATOMIC_VAR_INIT(x)
#endif
#elif defined(_MSC_VER)
// Use MSVC C wrapper for C11 atomics
#define  _Atomic(tp)            tp
#define  ALLOC_HOOK_ATOMIC_VAR_INIT(x)  x
#define  alloc_hook_atomic(name)        alloc_hook_atomic_##name
#define  alloc_hook_memory_order(name)  alloc_hook_memory_order_##name
#else
// Use C11 atomics
#include <stdatomic.h>
#define  alloc_hook_atomic(name)        atomic_##name
#define  alloc_hook_memory_order(name)  memory_order_##name
#if !defined(ATOMIC_VAR_INIT) || (__STDC_VERSION__ >= 201710L) // c17, see issue #735
 #define ALLOC_HOOK_ATOMIC_VAR_INIT(x) x
#else
 #define ALLOC_HOOK_ATOMIC_VAR_INIT(x) ATOMIC_VAR_INIT(x)
#endif
#endif

// Various defines for all used memory orders in alloc_hook
#define alloc_hook_atomic_cas_weak(p,expected,desired,mem_success,mem_fail)  \
  alloc_hook_atomic(compare_exchange_weak_explicit)(p,expected,desired,mem_success,mem_fail)

#define alloc_hook_atomic_cas_strong(p,expected,desired,mem_success,mem_fail)  \
  alloc_hook_atomic(compare_exchange_strong_explicit)(p,expected,desired,mem_success,mem_fail)

#define alloc_hook_atomic_load_acquire(p)                alloc_hook_atomic(load_explicit)(p,alloc_hook_memory_order(acquire))
#define alloc_hook_atomic_load_relaxed(p)                alloc_hook_atomic(load_explicit)(p,alloc_hook_memory_order(relaxed))
#define alloc_hook_atomic_store_release(p,x)             alloc_hook_atomic(store_explicit)(p,x,alloc_hook_memory_order(release))
#define alloc_hook_atomic_store_relaxed(p,x)             alloc_hook_atomic(store_explicit)(p,x,alloc_hook_memory_order(relaxed))
#define alloc_hook_atomic_exchange_release(p,x)          alloc_hook_atomic(exchange_explicit)(p,x,alloc_hook_memory_order(release))
#define alloc_hook_atomic_exchange_acq_rel(p,x)          alloc_hook_atomic(exchange_explicit)(p,x,alloc_hook_memory_order(acq_rel))
#define alloc_hook_atomic_cas_weak_release(p,exp,des)    alloc_hook_atomic_cas_weak(p,exp,des,alloc_hook_memory_order(release),alloc_hook_memory_order(relaxed))
#define alloc_hook_atomic_cas_weak_acq_rel(p,exp,des)    alloc_hook_atomic_cas_weak(p,exp,des,alloc_hook_memory_order(acq_rel),alloc_hook_memory_order(acquire))
#define alloc_hook_atomic_cas_strong_release(p,exp,des)  alloc_hook_atomic_cas_strong(p,exp,des,alloc_hook_memory_order(release),alloc_hook_memory_order(relaxed))
#define alloc_hook_atomic_cas_strong_acq_rel(p,exp,des)  alloc_hook_atomic_cas_strong(p,exp,des,alloc_hook_memory_order(acq_rel),alloc_hook_memory_order(acquire))

#define alloc_hook_atomic_add_relaxed(p,x)               alloc_hook_atomic(fetch_add_explicit)(p,x,alloc_hook_memory_order(relaxed))
#define alloc_hook_atomic_sub_relaxed(p,x)               alloc_hook_atomic(fetch_sub_explicit)(p,x,alloc_hook_memory_order(relaxed))
#define alloc_hook_atomic_add_acq_rel(p,x)               alloc_hook_atomic(fetch_add_explicit)(p,x,alloc_hook_memory_order(acq_rel))
#define alloc_hook_atomic_sub_acq_rel(p,x)               alloc_hook_atomic(fetch_sub_explicit)(p,x,alloc_hook_memory_order(acq_rel))
#define alloc_hook_atomic_and_acq_rel(p,x)               alloc_hook_atomic(fetch_and_explicit)(p,x,alloc_hook_memory_order(acq_rel))
#define alloc_hook_atomic_or_acq_rel(p,x)                alloc_hook_atomic(fetch_or_explicit)(p,x,alloc_hook_memory_order(acq_rel))

#define alloc_hook_atomic_increment_relaxed(p)           alloc_hook_atomic_add_relaxed(p,(uintptr_t)1)
#define alloc_hook_atomic_decrement_relaxed(p)           alloc_hook_atomic_sub_relaxed(p,(uintptr_t)1)
#define alloc_hook_atomic_increment_acq_rel(p)           alloc_hook_atomic_add_acq_rel(p,(uintptr_t)1)
#define alloc_hook_atomic_decrement_acq_rel(p)           alloc_hook_atomic_sub_acq_rel(p,(uintptr_t)1)

static inline void alloc_hook_atomic_yield(void);
static inline intptr_t alloc_hook_atomic_addi(_Atomic(intptr_t)*p, intptr_t add);
static inline intptr_t alloc_hook_atomic_subi(_Atomic(intptr_t)*p, intptr_t sub);


#if defined(__cplusplus) || !defined(_MSC_VER)

// In C++/C11 atomics we have polymorphic atomics so can use the typed `ptr` variants (where `tp` is the type of atomic value)
// We use these macros so we can provide a typed wrapper in MSVC in C compilation mode as well
#define alloc_hook_atomic_load_ptr_acquire(tp,p)                alloc_hook_atomic_load_acquire(p)
#define alloc_hook_atomic_load_ptr_relaxed(tp,p)                alloc_hook_atomic_load_relaxed(p)

// In C++ we need to add casts to help resolve templates if NULL is passed
#if defined(__cplusplus)
#define alloc_hook_atomic_store_ptr_release(tp,p,x)             alloc_hook_atomic_store_release(p,(tp*)x)
#define alloc_hook_atomic_store_ptr_relaxed(tp,p,x)             alloc_hook_atomic_store_relaxed(p,(tp*)x)
#define alloc_hook_atomic_cas_ptr_weak_release(tp,p,exp,des)    alloc_hook_atomic_cas_weak_release(p,exp,(tp*)des)
#define alloc_hook_atomic_cas_ptr_weak_acq_rel(tp,p,exp,des)    alloc_hook_atomic_cas_weak_acq_rel(p,exp,(tp*)des)
#define alloc_hook_atomic_cas_ptr_strong_release(tp,p,exp,des)  alloc_hook_atomic_cas_strong_release(p,exp,(tp*)des)
#define alloc_hook_atomic_exchange_ptr_release(tp,p,x)          alloc_hook_atomic_exchange_release(p,(tp*)x)
#define alloc_hook_atomic_exchange_ptr_acq_rel(tp,p,x)          alloc_hook_atomic_exchange_acq_rel(p,(tp*)x)
#else
#define alloc_hook_atomic_store_ptr_release(tp,p,x)             alloc_hook_atomic_store_release(p,x)
#define alloc_hook_atomic_store_ptr_relaxed(tp,p,x)             alloc_hook_atomic_store_relaxed(p,x)
#define alloc_hook_atomic_cas_ptr_weak_release(tp,p,exp,des)    alloc_hook_atomic_cas_weak_release(p,exp,des)
#define alloc_hook_atomic_cas_ptr_weak_acq_rel(tp,p,exp,des)    alloc_hook_atomic_cas_weak_acq_rel(p,exp,des)
#define alloc_hook_atomic_cas_ptr_strong_release(tp,p,exp,des)  alloc_hook_atomic_cas_strong_release(p,exp,des)
#define alloc_hook_atomic_exchange_ptr_release(tp,p,x)          alloc_hook_atomic_exchange_release(p,x)
#define alloc_hook_atomic_exchange_ptr_acq_rel(tp,p,x)          alloc_hook_atomic_exchange_acq_rel(p,x)
#endif

// These are used by the statistics
static inline int64_t alloc_hook_atomic_addi64_relaxed(volatile int64_t* p, int64_t add) {
  return alloc_hook_atomic(fetch_add_explicit)((_Atomic(int64_t)*)p, add, alloc_hook_memory_order(relaxed));
}
static inline void alloc_hook_atomic_maxi64_relaxed(volatile int64_t* p, int64_t x) {
  int64_t current = alloc_hook_atomic_load_relaxed((_Atomic(int64_t)*)p);
  while (current < x && !alloc_hook_atomic_cas_weak_release((_Atomic(int64_t)*)p, &current, x)) { /* nothing */ };
}

// Used by timers
#define alloc_hook_atomic_loadi64_acquire(p)            alloc_hook_atomic(load_explicit)(p,alloc_hook_memory_order(acquire))
#define alloc_hook_atomic_loadi64_relaxed(p)            alloc_hook_atomic(load_explicit)(p,alloc_hook_memory_order(relaxed))
#define alloc_hook_atomic_storei64_release(p,x)         alloc_hook_atomic(store_explicit)(p,x,alloc_hook_memory_order(release))
#define alloc_hook_atomic_storei64_relaxed(p,x)         alloc_hook_atomic(store_explicit)(p,x,alloc_hook_memory_order(relaxed))

#define alloc_hook_atomic_casi64_strong_acq_rel(p,e,d)  alloc_hook_atomic_cas_strong_acq_rel(p,e,d)
#define alloc_hook_atomic_addi64_acq_rel(p,i)           alloc_hook_atomic_add_acq_rel(p,i)


#elif defined(_MSC_VER)

// MSVC C compilation wrapper that uses Interlocked operations to model C11 atomics.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <intrin.h>
#ifdef _WIN64
typedef LONG64   msc_intptr_t;
#define ALLOC_HOOK_64(f) f##64
#else
typedef LONG     msc_intptr_t;
#define ALLOC_HOOK_64(f) f
#endif

typedef enum alloc_hook_memory_order_e {
  alloc_hook_memory_order_relaxed,
  alloc_hook_memory_order_consume,
  alloc_hook_memory_order_acquire,
  alloc_hook_memory_order_release,
  alloc_hook_memory_order_acq_rel,
  alloc_hook_memory_order_seq_cst
} alloc_hook_memory_order;

static inline uintptr_t alloc_hook_atomic_fetch_add_explicit(_Atomic(uintptr_t)*p, uintptr_t add, alloc_hook_memory_order mo) {
  (void)(mo);
  return (uintptr_t)ALLOC_HOOK_64(_InterlockedExchangeAdd)((volatile msc_intptr_t*)p, (msc_intptr_t)add);
}
static inline uintptr_t alloc_hook_atomic_fetch_sub_explicit(_Atomic(uintptr_t)*p, uintptr_t sub, alloc_hook_memory_order mo) {
  (void)(mo);
  return (uintptr_t)ALLOC_HOOK_64(_InterlockedExchangeAdd)((volatile msc_intptr_t*)p, -((msc_intptr_t)sub));
}
static inline uintptr_t alloc_hook_atomic_fetch_and_explicit(_Atomic(uintptr_t)*p, uintptr_t x, alloc_hook_memory_order mo) {
  (void)(mo);
  return (uintptr_t)ALLOC_HOOK_64(_InterlockedAnd)((volatile msc_intptr_t*)p, (msc_intptr_t)x);
}
static inline uintptr_t alloc_hook_atomic_fetch_or_explicit(_Atomic(uintptr_t)*p, uintptr_t x, alloc_hook_memory_order mo) {
  (void)(mo);
  return (uintptr_t)ALLOC_HOOK_64(_InterlockedOr)((volatile msc_intptr_t*)p, (msc_intptr_t)x);
}
static inline bool alloc_hook_atomic_compare_exchange_strong_explicit(_Atomic(uintptr_t)*p, uintptr_t* expected, uintptr_t desired, alloc_hook_memory_order mo1, alloc_hook_memory_order mo2) {
  (void)(mo1); (void)(mo2);
  uintptr_t read = (uintptr_t)ALLOC_HOOK_64(_InterlockedCompareExchange)((volatile msc_intptr_t*)p, (msc_intptr_t)desired, (msc_intptr_t)(*expected));
  if (read == *expected) {
    return true;
  }
  else {
    *expected = read;
    return false;
  }
}
static inline bool alloc_hook_atomic_compare_exchange_weak_explicit(_Atomic(uintptr_t)*p, uintptr_t* expected, uintptr_t desired, alloc_hook_memory_order mo1, alloc_hook_memory_order mo2) {
  return alloc_hook_atomic_compare_exchange_strong_explicit(p, expected, desired, mo1, mo2);
}
static inline uintptr_t alloc_hook_atomic_exchange_explicit(_Atomic(uintptr_t)*p, uintptr_t exchange, alloc_hook_memory_order mo) {
  (void)(mo);
  return (uintptr_t)ALLOC_HOOK_64(_InterlockedExchange)((volatile msc_intptr_t*)p, (msc_intptr_t)exchange);
}
static inline void alloc_hook_atomic_thread_fence(alloc_hook_memory_order mo) {
  (void)(mo);
  _Atomic(uintptr_t) x = 0;
  alloc_hook_atomic_exchange_explicit(&x, 1, mo);
}
static inline uintptr_t alloc_hook_atomic_load_explicit(_Atomic(uintptr_t) const* p, alloc_hook_memory_order mo) {
  (void)(mo);
#if defined(_M_IX86) || defined(_M_X64)
  return *p;
#else
  uintptr_t x = *p;
  if (mo > alloc_hook_memory_order_relaxed) {
    while (!alloc_hook_atomic_compare_exchange_weak_explicit(p, &x, x, mo, alloc_hook_memory_order_relaxed)) { /* nothing */ };
  }
  return x;
#endif
}
static inline void alloc_hook_atomic_store_explicit(_Atomic(uintptr_t)*p, uintptr_t x, alloc_hook_memory_order mo) {
  (void)(mo);
#if defined(_M_IX86) || defined(_M_X64)
  *p = x;
#else
  alloc_hook_atomic_exchange_explicit(p, x, mo);
#endif
}
static inline int64_t alloc_hook_atomic_loadi64_explicit(_Atomic(int64_t)*p, alloc_hook_memory_order mo) {
  (void)(mo);
#if defined(_M_X64)
  return *p;
#else
  int64_t old = *p;
  int64_t x = old;
  while ((old = InterlockedCompareExchange64(p, x, old)) != x) {
    x = old;
  }
  return x;
#endif
}
static inline void alloc_hook_atomic_storei64_explicit(_Atomic(int64_t)*p, int64_t x, alloc_hook_memory_order mo) {
  (void)(mo);
#if defined(x_M_IX86) || defined(_M_X64)
  *p = x;
#else
  InterlockedExchange64(p, x);
#endif
}

// These are used by the statistics
static inline int64_t alloc_hook_atomic_addi64_relaxed(volatile _Atomic(int64_t)*p, int64_t add) {
#ifdef _WIN64
  return (int64_t)alloc_hook_atomic_addi((int64_t*)p, add);
#else
  int64_t current;
  int64_t sum;
  do {
    current = *p;
    sum = current + add;
  } while (_InterlockedCompareExchange64(p, sum, current) != current);
  return current;
#endif
}
static inline void alloc_hook_atomic_maxi64_relaxed(volatile _Atomic(int64_t)*p, int64_t x) {
  int64_t current;
  do {
    current = *p;
  } while (current < x && _InterlockedCompareExchange64(p, x, current) != current);
}

static inline void alloc_hook_atomic_addi64_acq_rel(volatile _Atomic(int64_t*)p, int64_t i) {
  alloc_hook_atomic_addi64_relaxed(p, i);
}

static inline bool alloc_hook_atomic_casi64_strong_acq_rel(volatile _Atomic(int64_t*)p, int64_t* exp, int64_t des) {
  int64_t read = _InterlockedCompareExchange64(p, des, *exp);
  if (read == *exp) {
    return true;
  }
  else {
    *exp = read;
    return false;
  }
}

// The pointer macros cast to `uintptr_t`.
#define alloc_hook_atomic_load_ptr_acquire(tp,p)                (tp*)alloc_hook_atomic_load_acquire((_Atomic(uintptr_t)*)(p))
#define alloc_hook_atomic_load_ptr_relaxed(tp,p)                (tp*)alloc_hook_atomic_load_relaxed((_Atomic(uintptr_t)*)(p))
#define alloc_hook_atomic_store_ptr_release(tp,p,x)             alloc_hook_atomic_store_release((_Atomic(uintptr_t)*)(p),(uintptr_t)(x))
#define alloc_hook_atomic_store_ptr_relaxed(tp,p,x)             alloc_hook_atomic_store_relaxed((_Atomic(uintptr_t)*)(p),(uintptr_t)(x))
#define alloc_hook_atomic_cas_ptr_weak_release(tp,p,exp,des)    alloc_hook_atomic_cas_weak_release((_Atomic(uintptr_t)*)(p),(uintptr_t*)exp,(uintptr_t)des)
#define alloc_hook_atomic_cas_ptr_weak_acq_rel(tp,p,exp,des)    alloc_hook_atomic_cas_weak_acq_rel((_Atomic(uintptr_t)*)(p),(uintptr_t*)exp,(uintptr_t)des)
#define alloc_hook_atomic_cas_ptr_strong_release(tp,p,exp,des)  alloc_hook_atomic_cas_strong_release((_Atomic(uintptr_t)*)(p),(uintptr_t*)exp,(uintptr_t)des)
#define alloc_hook_atomic_exchange_ptr_release(tp,p,x)          (tp*)alloc_hook_atomic_exchange_release((_Atomic(uintptr_t)*)(p),(uintptr_t)x)
#define alloc_hook_atomic_exchange_ptr_acq_rel(tp,p,x)          (tp*)alloc_hook_atomic_exchange_acq_rel((_Atomic(uintptr_t)*)(p),(uintptr_t)x)

#define alloc_hook_atomic_loadi64_acquire(p)    alloc_hook_atomic(loadi64_explicit)(p,alloc_hook_memory_order(acquire))
#define alloc_hook_atomic_loadi64_relaxed(p)    alloc_hook_atomic(loadi64_explicit)(p,alloc_hook_memory_order(relaxed))
#define alloc_hook_atomic_storei64_release(p,x) alloc_hook_atomic(storei64_explicit)(p,x,alloc_hook_memory_order(release))
#define alloc_hook_atomic_storei64_relaxed(p,x) alloc_hook_atomic(storei64_explicit)(p,x,alloc_hook_memory_order(relaxed))


#endif


// Atomically add a signed value; returns the previous value.
static inline intptr_t alloc_hook_atomic_addi(_Atomic(intptr_t)*p, intptr_t add) {
  return (intptr_t)alloc_hook_atomic_add_acq_rel((_Atomic(uintptr_t)*)p, (uintptr_t)add);
}

// Atomically subtract a signed value; returns the previous value.
static inline intptr_t alloc_hook_atomic_subi(_Atomic(intptr_t)*p, intptr_t sub) {
  return (intptr_t)alloc_hook_atomic_addi(p, -sub);
}

typedef _Atomic(uintptr_t) alloc_hook_atomic_once_t;

// Returns true only on the first invocation
static inline bool alloc_hook_atomic_once( alloc_hook_atomic_once_t* once ) {
  if (alloc_hook_atomic_load_relaxed(once) != 0) return false;     // quick test 
  uintptr_t expected = 0;
  return alloc_hook_atomic_cas_strong_acq_rel(once, &expected, (uintptr_t)1); // try to set to 1
}

typedef _Atomic(uintptr_t) alloc_hook_atomic_guard_t;

// Allows only one thread to execute at a time
#define alloc_hook_atomic_guard(guard) \
  uintptr_t _alloc_hook_guard_expected = 0; \
  for(bool _alloc_hook_guard_once = true; \
      _alloc_hook_guard_once && alloc_hook_atomic_cas_strong_acq_rel(guard,&_alloc_hook_guard_expected,(uintptr_t)1); \
      (alloc_hook_atomic_store_release(guard,(uintptr_t)0), _alloc_hook_guard_once = false) )



// Yield
#if defined(__cplusplus)
#include <thread>
static inline void alloc_hook_atomic_yield(void) {
  std::this_thread::yield();
}
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static inline void alloc_hook_atomic_yield(void) {
  YieldProcessor();
}
#elif defined(__SSE2__)
#include <emmintrin.h>
static inline void alloc_hook_atomic_yield(void) {
  _mm_pause();
}
#elif (defined(__GNUC__) || defined(__clang__)) && \
      (defined(__x86_64__) || defined(__i386__) || defined(__arm__) || defined(__armel__) || defined(__ARMEL__) || \
       defined(__aarch64__) || defined(__powerpc__) || defined(__ppc__) || defined(__PPC__)) || defined(__POWERPC__)
#if defined(__x86_64__) || defined(__i386__)
static inline void alloc_hook_atomic_yield(void) {
  __asm__ volatile ("pause" ::: "memory");
}
#elif defined(__aarch64__)
static inline void alloc_hook_atomic_yield(void) {
  __asm__ volatile("wfe");
}
#elif (defined(__arm__) && __ARM_ARCH__ >= 7)
static inline void alloc_hook_atomic_yield(void) {
  __asm__ volatile("yield" ::: "memory");
}
#elif defined(__powerpc__) || defined(__ppc__) || defined(__PPC__) || defined(__POWERPC__)
#ifdef __APPLE__
static inline void alloc_hook_atomic_yield(void) {
  __asm__ volatile ("or r27,r27,r27" ::: "memory");
}
#else
static inline void alloc_hook_atomic_yield(void) {
  __asm__ __volatile__ ("or 27,27,27" ::: "memory");
}
#endif
#elif defined(__armel__) || defined(__ARMEL__)
static inline void alloc_hook_atomic_yield(void) {
  __asm__ volatile ("nop" ::: "memory");
}
#endif
#elif defined(__sun)
// Fallback for other archs
#include <synch.h>
static inline void alloc_hook_atomic_yield(void) {
  smt_pause();
}
#elif defined(__wasi__)
#include <sched.h>
static inline void alloc_hook_atomic_yield(void) {
  sched_yield();
}
#else
#include <unistd.h>
static inline void alloc_hook_atomic_yield(void) {
  sleep(0);
}
#endif


#endif // __ALLOC_HOOK_ATOMIC_H
