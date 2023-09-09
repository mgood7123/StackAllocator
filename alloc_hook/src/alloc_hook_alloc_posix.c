/* ----------------------------------------------------------------------------
Copyright (c) 2018-2021, Microsoft Research, Daan Leijen
This is free software; you can redistribute it and/or modify it under the
terms of the MIT license. A copy of the license can be found in the file
"LICENSE" at the root of this distribution.
-----------------------------------------------------------------------------*/

// ------------------------------------------------------------------------
// mi prefixed publi definitions of various Posix, Unix, and C++ functions
// for convenience and used when overriding these functions.
// ------------------------------------------------------------------------
#include "alloc_hook.h"
#include "alloc_hook_internal.h"

// ------------------------------------------------------
// Posix & Unix functions definitions
// ------------------------------------------------------

#include <errno.h>
#include <string.h>  // memset
#include <stdlib.h>  // getenv

#ifdef _MSC_VER
#pragma warning(disable:4996)  // getenv _wgetenv
#endif

#ifndef EINVAL
#define EINVAL 22
#endif
#ifndef ENOMEM
#define ENOMEM 12
#endif


alloc_hook_decl_nodiscard size_t alloc_hook_malloc_size(const void* p) alloc_hook_attr_noexcept {
  // if (!alloc_hook_is_in_heap_region(p)) return 0;
  return alloc_hook_usable_size(p);
}

alloc_hook_decl_nodiscard size_t alloc_hook_malloc_usable_size(const void *p) alloc_hook_attr_noexcept {
  // if (!alloc_hook_is_in_heap_region(p)) return 0;
  return alloc_hook_usable_size(p);
}

alloc_hook_decl_nodiscard size_t alloc_hook_malloc_good_size(size_t size) alloc_hook_attr_noexcept {
  return alloc_hook_good_size(size);
}

void alloc_hook_cfree(void* p) alloc_hook_attr_noexcept {
  if (alloc_hook_is_in_heap_region(p)) {
    alloc_hook_free(p);
  }
}

int alloc_hook_posix_memalign(void** p, size_t alignment, size_t size) alloc_hook_attr_noexcept {
  // Note: The spec dictates we should not modify `*p` on an error. (issue#27)
  // <http://man7.org/linux/man-pages/man3/posix_memalign.3.html>
  if (p == NULL) return EINVAL;
  if ((alignment % sizeof(void*)) != 0) return EINVAL;                 // natural alignment
  // it is also required that alignment is a power of 2 and > 0; this is checked in `alloc_hook_malloc_aligned`
  if (alignment==0 || !_alloc_hook_is_power_of_two(alignment)) return EINVAL;  // not a power of 2
  void* q = alloc_hook_malloc_aligned(size, alignment);
  if (q==NULL && size != 0) return ENOMEM;
  alloc_hook_assert_internal(((uintptr_t)q % alignment) == 0);
  *p = q;
  return 0;
}

alloc_hook_decl_nodiscard alloc_hook_decl_restrict void* alloc_hook_memalign(size_t alignment, size_t size) alloc_hook_attr_noexcept {
  void* p = alloc_hook_malloc_aligned(size, alignment);
  alloc_hook_assert_internal(((uintptr_t)p % alignment) == 0);
  return p;
}

alloc_hook_decl_nodiscard alloc_hook_decl_restrict void* alloc_hook_valloc(size_t size) alloc_hook_attr_noexcept {
  return alloc_hook_memalign( _alloc_hook_os_page_size(), size );
}

alloc_hook_decl_nodiscard alloc_hook_decl_restrict void* alloc_hook_pvalloc(size_t size) alloc_hook_attr_noexcept {
  size_t psize = _alloc_hook_os_page_size();
  if (size >= SIZE_MAX - psize) return NULL; // overflow
  size_t asize = _alloc_hook_align_up(size, psize);
  return alloc_hook_malloc_aligned(asize, psize);
}

alloc_hook_decl_nodiscard alloc_hook_decl_restrict void* alloc_hook_aligned_alloc(size_t alignment, size_t size) alloc_hook_attr_noexcept {
  // C11 requires the size to be an integral multiple of the alignment, see <https://en.cppreference.com/w/c/memory/aligned_alloc>.
  // unfortunately, it turns out quite some programs pass a size that is not an integral multiple so skip this check..
  /* if alloc_hook_unlikely((size & (alignment - 1)) != 0) { // C11 requires alignment>0 && integral multiple, see <https://en.cppreference.com/w/c/memory/aligned_alloc>
      #if ALLOC_HOOK_DEBUG > 0
      _alloc_hook_error_message(EOVERFLOW, "(alloc_hook_)aligned_alloc requires the size to be an integral multiple of the alignment (size %zu, alignment %zu)\n", size, alignment);
      #endif
      return NULL;
    }
  */
  // C11 also requires alignment to be a power-of-two (and > 0) which is checked in alloc_hook_malloc_aligned
  void* p = alloc_hook_malloc_aligned(size, alignment);
  alloc_hook_assert_internal(((uintptr_t)p % alignment) == 0);
  return p;
}

alloc_hook_decl_nodiscard void* alloc_hook_reallocarray( void* p, size_t count, size_t size ) alloc_hook_attr_noexcept {  // BSD
  void* newp = alloc_hook_reallocn(p,count,size);
  if (newp==NULL) { errno = ENOMEM; }
  return newp;
}

alloc_hook_decl_nodiscard int alloc_hook_reallocarr( void* p, size_t count, size_t size ) alloc_hook_attr_noexcept { // NetBSD
  alloc_hook_assert(p != NULL);
  if (p == NULL) {
    errno = EINVAL;
    return EINVAL;
  }
  void** op = (void**)p;
  void* newp = alloc_hook_reallocarray(*op, count, size);
  if alloc_hook_unlikely(newp == NULL) { return errno; }
  *op = newp;
  return 0;
}

void* alloc_hook__expand(void* p, size_t newsize) alloc_hook_attr_noexcept {  // Microsoft
  void* res = alloc_hook_expand(p, newsize);
  if (res == NULL) { errno = ENOMEM; }
  return res;
}

alloc_hook_decl_nodiscard alloc_hook_decl_restrict unsigned short* alloc_hook_wcsdup(const unsigned short* s) alloc_hook_attr_noexcept {
  if (s==NULL) return NULL;
  size_t len;
  for(len = 0; s[len] != 0; len++) { }
  size_t size = (len+1)*sizeof(unsigned short);
  unsigned short* p = (unsigned short*)alloc_hook_malloc(size);
  if (p != NULL) {
    _alloc_hook_memcpy(p,s,size);
  }
  return p;
}

alloc_hook_decl_nodiscard alloc_hook_decl_restrict unsigned char* alloc_hook_mbsdup(const unsigned char* s)  alloc_hook_attr_noexcept {
  return (unsigned char*)alloc_hook_strdup((const char*)s);
}

int alloc_hook_dupenv_s(char** buf, size_t* size, const char* name) alloc_hook_attr_noexcept {
  if (buf==NULL || name==NULL) return EINVAL;
  if (size != NULL) *size = 0;
  char* p = getenv(name);        // mscver warning 4996
  if (p==NULL) {
    *buf = NULL;
  }
  else {
    *buf = alloc_hook_strdup(p);
    if (*buf==NULL) return ENOMEM;
    if (size != NULL) *size = _alloc_hook_strlen(p);
  }
  return 0;
}

int alloc_hook_wdupenv_s(unsigned short** buf, size_t* size, const unsigned short* name) alloc_hook_attr_noexcept {
  if (buf==NULL || name==NULL) return EINVAL;
  if (size != NULL) *size = 0;
#if !defined(_WIN32) || (defined(WINAPI_FAMILY) && (WINAPI_FAMILY != WINAPI_FAMILY_DESKTOP_APP))
  // not supported
  *buf = NULL;
  return EINVAL;
#else
  unsigned short* p = (unsigned short*)_wgetenv((const wchar_t*)name);  // msvc warning 4996
  if (p==NULL) {
    *buf = NULL;
  }
  else {
    *buf = alloc_hook_wcsdup(p);
    if (*buf==NULL) return ENOMEM;
    if (size != NULL) *size = wcslen((const wchar_t*)p);
  }
  return 0;
#endif
}

alloc_hook_decl_nodiscard void* alloc_hook_aligned_offset_recalloc(void* p, size_t newcount, size_t size, size_t alignment, size_t offset) alloc_hook_attr_noexcept { // Microsoft
  return alloc_hook_recalloc_aligned_at(p, newcount, size, alignment, offset);
}

alloc_hook_decl_nodiscard void* alloc_hook_aligned_recalloc(void* p, size_t newcount, size_t size, size_t alignment) alloc_hook_attr_noexcept { // Microsoft
  return alloc_hook_recalloc_aligned(p, newcount, size, alignment);
}
