/* ----------------------------------------------------------------------------
Copyright (c) 2018-2020 Microsoft Research, Daan Leijen
This is free software; you can redistribute it and/or modify it under the
terms of the MIT license. A copy of the license can be found in the file
"LICENSE" at the root of this distribution.
-----------------------------------------------------------------------------*/

#ifndef ALLOC_HOOK_NEW_DELETE_H
#define ALLOC_HOOK_NEW_DELETE_H

// ----------------------------------------------------------------------------
// This header provides convenient overrides for the new and
// delete operations in C++.
//
// This header should be included in only one source file!
//
// On Windows, or when linking dynamically with alloc_hook, these
// can be more performant than the standard new-delete operations.
// See <https://en.cppreference.com/w/cpp/memory/new/operator_new>
// ---------------------------------------------------------------------------
#if defined(__cplusplus)
  #include <new>
  #include <alloc_hook.h>

  #if defined(_MSC_VER) && defined(_Ret_notnull_) && defined(_Post_writable_byte_size_)
  // stay consistent with VCRT definitions
  #define alloc_hook_decl_new(n)          alloc_hook_decl_nodiscard alloc_hook_decl_restrict _Ret_notnull_ _Post_writable_byte_size_(n)
  #define alloc_hook_decl_new_nothrow(n)  alloc_hook_decl_nodiscard alloc_hook_decl_restrict _Ret_maybenull_ _Success_(return != NULL) _Post_writable_byte_size_(n)
  #else
  #define alloc_hook_decl_new(n)          alloc_hook_decl_nodiscard alloc_hook_decl_restrict
  #define alloc_hook_decl_new_nothrow(n)  alloc_hook_decl_nodiscard alloc_hook_decl_restrict
  #endif

  void operator delete(void* p) noexcept              { alloc_hook_free(p); };
  void operator delete[](void* p) noexcept            { alloc_hook_free(p); };

  void operator delete  (void* p, const std::nothrow_t&) noexcept { alloc_hook_free(p); }
  void operator delete[](void* p, const std::nothrow_t&) noexcept { alloc_hook_free(p); }

  alloc_hook_decl_new(n) void* operator new(std::size_t n) noexcept(false) { return alloc_hook_new(n); }
  alloc_hook_decl_new(n) void* operator new[](std::size_t n) noexcept(false) { return alloc_hook_new(n); }

  alloc_hook_decl_new_nothrow(n) void* operator new  (std::size_t n, const std::nothrow_t& tag) noexcept { (void)(tag); return alloc_hook_new_nothrow(n); }
  alloc_hook_decl_new_nothrow(n) void* operator new[](std::size_t n, const std::nothrow_t& tag) noexcept { (void)(tag); return alloc_hook_new_nothrow(n); }

  #if (__cplusplus >= 201402L || _MSC_VER >= 1916)
  void operator delete  (void* p, std::size_t n) noexcept { alloc_hook_free_size(p,n); };
  void operator delete[](void* p, std::size_t n) noexcept { alloc_hook_free_size(p,n); };
  #endif

  #if (__cplusplus > 201402L || defined(__cpp_aligned_new))
  void operator delete  (void* p, std::align_val_t al) noexcept { alloc_hook_free_aligned(p, static_cast<size_t>(al)); }
  void operator delete[](void* p, std::align_val_t al) noexcept { alloc_hook_free_aligned(p, static_cast<size_t>(al)); }
  void operator delete  (void* p, std::size_t n, std::align_val_t al) noexcept { alloc_hook_free_size_aligned(p, n, static_cast<size_t>(al)); };
  void operator delete[](void* p, std::size_t n, std::align_val_t al) noexcept { alloc_hook_free_size_aligned(p, n, static_cast<size_t>(al)); };
  void operator delete  (void* p, std::align_val_t al, const std::nothrow_t&) noexcept { alloc_hook_free_aligned(p, static_cast<size_t>(al)); }
  void operator delete[](void* p, std::align_val_t al, const std::nothrow_t&) noexcept { alloc_hook_free_aligned(p, static_cast<size_t>(al)); }

  void* operator new  (std::size_t n, std::align_val_t al) noexcept(false) { return alloc_hook_new_aligned(n, static_cast<size_t>(al)); }
  void* operator new[](std::size_t n, std::align_val_t al) noexcept(false) { return alloc_hook_new_aligned(n, static_cast<size_t>(al)); }
  void* operator new  (std::size_t n, std::align_val_t al, const std::nothrow_t&) noexcept { return alloc_hook_new_aligned_nothrow(n, static_cast<size_t>(al)); }
  void* operator new[](std::size_t n, std::align_val_t al, const std::nothrow_t&) noexcept { return alloc_hook_new_aligned_nothrow(n, static_cast<size_t>(al)); }
  #endif
#endif

#endif // ALLOC_HOOK_NEW_DELETE_H
