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
#include <stdio.h>   // stderr


//---------------------------------------------
// Dynamically bind Windows API points for portability
//---------------------------------------------

// We use VirtualAlloc2 for aligned allocation, but it is only supported on Windows 10 and Windows Server 2016.
// So, we need to look it up dynamically to run on older systems. (use __stdcall for 32-bit compatibility)
// NtAllocateVirtualAllocEx is used for huge OS page allocation (1GiB)
// We define a minimal MEM_EXTENDED_PARAMETER ourselves in order to be able to compile with older SDK's.
typedef enum ALLOC_HOOK_MEM_EXTENDED_PARAMETER_TYPE_E {
  MiMemExtendedParameterInvalidType = 0,
  MiMemExtendedParameterAddressRequirements,
  MiMemExtendedParameterNumaNode,
  MiMemExtendedParameterPartitionHandle,
  MiMemExtendedParameterUserPhysicalHandle,
  MiMemExtendedParameterAttributeFlags,
  MiMemExtendedParameterMax
} ALLOC_HOOK_MEM_EXTENDED_PARAMETER_TYPE;

typedef struct DECLSPEC_ALIGN(8) ALLOC_HOOK_MEM_EXTENDED_PARAMETER_S {
  struct { DWORD64 Type : 8; DWORD64 Reserved : 56; } Type;
  union  { DWORD64 ULong64; PVOID Pointer; SIZE_T Size; HANDLE Handle; DWORD ULong; } Arg;
} ALLOC_HOOK_MEM_EXTENDED_PARAMETER;

typedef struct ALLOC_HOOK_MEM_ADDRESS_REQUIREMENTS_S {
  PVOID  LowestStartingAddress;
  PVOID  HighestEndingAddress;
  SIZE_T Alignment;
} ALLOC_HOOK_MEM_ADDRESS_REQUIREMENTS;

#define ALLOC_HOOK_MEM_EXTENDED_PARAMETER_NONPAGED_HUGE   0x00000010

#include <winternl.h>
typedef PVOID    (__stdcall *PVirtualAlloc2)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, ALLOC_HOOK_MEM_EXTENDED_PARAMETER*, ULONG);
typedef NTSTATUS (__stdcall *PNtAllocateVirtualMemoryEx)(HANDLE, PVOID*, SIZE_T*, ULONG, ULONG, ALLOC_HOOK_MEM_EXTENDED_PARAMETER*, ULONG);
static PVirtualAlloc2 pVirtualAlloc2 = NULL;
static PNtAllocateVirtualMemoryEx pNtAllocateVirtualMemoryEx = NULL;

// Similarly, GetNumaProcesorNodeEx is only supported since Windows 7
typedef struct ALLOC_HOOK_PROCESSOR_NUMBER_S { WORD Group; BYTE Number; BYTE Reserved; } ALLOC_HOOK_PROCESSOR_NUMBER;

typedef VOID (__stdcall *PGetCurrentProcessorNumberEx)(ALLOC_HOOK_PROCESSOR_NUMBER* ProcNumber);
typedef BOOL (__stdcall *PGetNumaProcessorNodeEx)(ALLOC_HOOK_PROCESSOR_NUMBER* Processor, PUSHORT NodeNumber);
typedef BOOL (__stdcall* PGetNumaNodeProcessorMaskEx)(USHORT Node, PGROUP_AFFINITY ProcessorMask);
typedef BOOL (__stdcall *PGetNumaProcessorNode)(UCHAR Processor, PUCHAR NodeNumber);
static PGetCurrentProcessorNumberEx pGetCurrentProcessorNumberEx = NULL;
static PGetNumaProcessorNodeEx      pGetNumaProcessorNodeEx = NULL;
static PGetNumaNodeProcessorMaskEx  pGetNumaNodeProcessorMaskEx = NULL;
static PGetNumaProcessorNode        pGetNumaProcessorNode = NULL;

//---------------------------------------------
// Enable large page support dynamically (if possible)
//---------------------------------------------

static bool win_enable_large_os_pages(size_t* large_page_size)
{
  static bool large_initialized = false;
  if (large_initialized) return (_alloc_hook_os_large_page_size() > 0);
  large_initialized = true;

  // Try to see if large OS pages are supported
  // To use large pages on Windows, we first need access permission
  // Set "Lock pages in memory" permission in the group policy editor
  // <https://devblogs.microsoft.com/oldnewthing/20110128-00/?p=11643>
  unsigned long err = 0;
  HANDLE token = NULL;
  BOOL ok = OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token);
  if (ok) {
    TOKEN_PRIVILEGES tp;
    ok = LookupPrivilegeValue(NULL, TEXT("SeLockMemoryPrivilege"), &tp.Privileges[0].Luid);
    if (ok) {
      tp.PrivilegeCount = 1;
      tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
      ok = AdjustTokenPrivileges(token, FALSE, &tp, 0, (PTOKEN_PRIVILEGES)NULL, 0);
      if (ok) {
        err = GetLastError();
        ok = (err == ERROR_SUCCESS);
        if (ok && large_page_size != NULL) {
          *large_page_size = GetLargePageMinimum();
        }
      }
    }
    CloseHandle(token);
  }
  if (!ok) {
    if (err == 0) err = GetLastError();
    _alloc_hook_warning_message("cannot enable large OS page support, error %lu\n", err);
  }
  return (ok!=0);
}


//---------------------------------------------
// Initialize
//---------------------------------------------

void _alloc_hook_prim_mem_init( alloc_hook_os_mem_config_t* config )
{
  config->has_overcommit = false;
  config->must_free_whole = true;
  config->has_virtual_reserve = true;
  // get the page size
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  if (si.dwPageSize > 0) { config->page_size = si.dwPageSize; }
  if (si.dwAllocationGranularity > 0) { config->alloc_granularity = si.dwAllocationGranularity; }
  // get the VirtualAlloc2 function
  HINSTANCE  hDll;
  hDll = LoadLibrary(TEXT("kernelbase.dll"));
  if (hDll != NULL) {
    // use VirtualAlloc2FromApp if possible as it is available to Windows store apps
    pVirtualAlloc2 = (PVirtualAlloc2)(void (*)(void))GetProcAddress(hDll, "VirtualAlloc2FromApp");
    if (pVirtualAlloc2==NULL) pVirtualAlloc2 = (PVirtualAlloc2)(void (*)(void))GetProcAddress(hDll, "VirtualAlloc2");
    FreeLibrary(hDll);
  }
  // NtAllocateVirtualMemoryEx is used for huge page allocation
  hDll = LoadLibrary(TEXT("ntdll.dll"));
  if (hDll != NULL) {
    pNtAllocateVirtualMemoryEx = (PNtAllocateVirtualMemoryEx)(void (*)(void))GetProcAddress(hDll, "NtAllocateVirtualMemoryEx");
    FreeLibrary(hDll);
  }
  // Try to use Win7+ numa API
  hDll = LoadLibrary(TEXT("kernel32.dll"));
  if (hDll != NULL) {
    pGetCurrentProcessorNumberEx = (PGetCurrentProcessorNumberEx)(void (*)(void))GetProcAddress(hDll, "GetCurrentProcessorNumberEx");
    pGetNumaProcessorNodeEx = (PGetNumaProcessorNodeEx)(void (*)(void))GetProcAddress(hDll, "GetNumaProcessorNodeEx");
    pGetNumaNodeProcessorMaskEx = (PGetNumaNodeProcessorMaskEx)(void (*)(void))GetProcAddress(hDll, "GetNumaNodeProcessorMaskEx");
    pGetNumaProcessorNode = (PGetNumaProcessorNode)(void (*)(void))GetProcAddress(hDll, "GetNumaProcessorNode");
    FreeLibrary(hDll);
  }
  if (alloc_hook_option_is_enabled(alloc_hook_option_allow_large_os_pages) || alloc_hook_option_is_enabled(alloc_hook_option_reserve_huge_os_pages)) {
    win_enable_large_os_pages(&config->large_page_size);
  }
}


//---------------------------------------------
// Free
//---------------------------------------------

int _alloc_hook_prim_free(void* addr, size_t size ) {
  ALLOC_HOOK_UNUSED(size);
  DWORD errcode = 0;
  bool err = (VirtualFree(addr, 0, MEM_RELEASE) == 0);
  if (err) { errcode = GetLastError(); }
  if (errcode == ERROR_INVALID_ADDRESS) {
    // In alloc_hook_os_mem_alloc_aligned the fallback path may have returned a pointer inside
    // the memory region returned by VirtualAlloc; in that case we need to free using
    // the start of the region.
    MEMORY_BASIC_INFORMATION info = { 0 };
    VirtualQuery(addr, &info, sizeof(info));
    if (info.AllocationBase < addr && ((uint8_t*)addr - (uint8_t*)info.AllocationBase) < (ptrdiff_t)ALLOC_HOOK_SEGMENT_SIZE) {
      errcode = 0;
      err = (VirtualFree(info.AllocationBase, 0, MEM_RELEASE) == 0);
      if (err) { errcode = GetLastError(); }
    }
  }
  return (int)errcode;
}


//---------------------------------------------
// VirtualAlloc
//---------------------------------------------

static void* win_virtual_alloc_prim(void* addr, size_t size, size_t try_alignment, DWORD flags) {
  #if (ALLOC_HOOK_INTPTR_SIZE >= 8)
  // on 64-bit systems, try to use the virtual address area after 2TiB for 4MiB aligned allocations
  if (addr == NULL) {
    void* hint = _alloc_hook_os_get_aligned_hint(try_alignment,size);
    if (hint != NULL) {
      void* p = VirtualAlloc(hint, size, flags, PAGE_READWRITE);
      if (p != NULL) return p;
      _alloc_hook_verbose_message("warning: unable to allocate hinted aligned OS memory (%zu bytes, error code: 0x%x, address: %p, alignment: %zu, flags: 0x%x)\n", size, GetLastError(), hint, try_alignment, flags);
      // fall through on error
    }
  }
  #endif
  // on modern Windows try use VirtualAlloc2 for aligned allocation
  if (try_alignment > 1 && (try_alignment % _alloc_hook_os_page_size()) == 0 && pVirtualAlloc2 != NULL) {
    ALLOC_HOOK_MEM_ADDRESS_REQUIREMENTS reqs = { 0, 0, 0 };
    reqs.Alignment = try_alignment;
    ALLOC_HOOK_MEM_EXTENDED_PARAMETER param = { {0, 0}, {0} };
    param.Type.Type = MiMemExtendedParameterAddressRequirements;
    param.Arg.Pointer = &reqs;
    void* p = (*pVirtualAlloc2)(GetCurrentProcess(), addr, size, flags, PAGE_READWRITE, &param, 1);
    if (p != NULL) return p;
    _alloc_hook_warning_message("unable to allocate aligned OS memory (%zu bytes, error code: 0x%x, address: %p, alignment: %zu, flags: 0x%x)\n", size, GetLastError(), addr, try_alignment, flags);
    // fall through on error
  }
  // last resort
  return VirtualAlloc(addr, size, flags, PAGE_READWRITE);
}

static void* win_virtual_alloc(void* addr, size_t size, size_t try_alignment, DWORD flags, bool large_only, bool allow_large, bool* is_large) {
  alloc_hook_assert_internal(!(large_only && !allow_large));
  static _Atomic(size_t) large_page_try_ok; // = 0;
  void* p = NULL;
  // Try to allocate large OS pages (2MiB) if allowed or required.
  if ((large_only || _alloc_hook_os_use_large_page(size, try_alignment))
      && allow_large && (flags&MEM_COMMIT)!=0 && (flags&MEM_RESERVE)!=0) {
    size_t try_ok = alloc_hook_atomic_load_acquire(&large_page_try_ok);
    if (!large_only && try_ok > 0) {
      // if a large page allocation fails, it seems the calls to VirtualAlloc get very expensive.
      // therefore, once a large page allocation failed, we don't try again for `large_page_try_ok` times.
      alloc_hook_atomic_cas_strong_acq_rel(&large_page_try_ok, &try_ok, try_ok - 1);
    }
    else {
      // large OS pages must always reserve and commit.
      *is_large = true;
      p = win_virtual_alloc_prim(addr, size, try_alignment, flags | MEM_LARGE_PAGES);
      if (large_only) return p;
      // fall back to non-large page allocation on error (`p == NULL`).
      if (p == NULL) {
        alloc_hook_atomic_store_release(&large_page_try_ok,10UL);  // on error, don't try again for the next N allocations
      }
    }
  }
  // Fall back to regular page allocation
  if (p == NULL) {
    *is_large = ((flags&MEM_LARGE_PAGES) != 0);
    p = win_virtual_alloc_prim(addr, size, try_alignment, flags);
  }
  //if (p == NULL) { _alloc_hook_warning_message("unable to allocate OS memory (%zu bytes, error code: 0x%x, address: %p, alignment: %zu, flags: 0x%x, large only: %d, allow large: %d)\n", size, GetLastError(), addr, try_alignment, flags, large_only, allow_large); }
  return p;
}

int _alloc_hook_prim_alloc(size_t size, size_t try_alignment, bool commit, bool allow_large, bool* is_large, bool* is_zero, void** addr) {
  alloc_hook_assert_internal(size > 0 && (size % _alloc_hook_os_page_size()) == 0);
  alloc_hook_assert_internal(commit || !allow_large);
  alloc_hook_assert_internal(try_alignment > 0);
  *is_zero = true;
  int flags = MEM_RESERVE;
  if (commit) { flags |= MEM_COMMIT; }
  *addr = win_virtual_alloc(NULL, size, try_alignment, flags, false, allow_large, is_large);
  return (*addr != NULL ? 0 : (int)GetLastError());
}


//---------------------------------------------
// Commit/Reset/Protect
//---------------------------------------------
#ifdef _MSC_VER
#pragma warning(disable:6250)   // suppress warning calling VirtualFree without MEM_RELEASE (for decommit)
#endif

int _alloc_hook_prim_commit(void* addr, size_t size, bool* is_zero) {
  *is_zero = false;
  /*
  // zero'ing only happens on an initial commit... but checking upfront seems expensive..
  _MEMORY_BASIC_INFORMATION meminfo; _alloc_hook_memzero_var(meminfo);
  if (VirtualQuery(addr, &meminfo, size) > 0) {
    if ((meminfo.State & MEM_COMMIT) == 0) {
      *is_zero = true;
    }
  }
  */
  // commit
  void* p = VirtualAlloc(addr, size, MEM_COMMIT, PAGE_READWRITE);
  if (p == NULL) return (int)GetLastError();
  return 0;
}

int _alloc_hook_prim_decommit(void* addr, size_t size, bool* needs_recommit) {  
  BOOL ok = VirtualFree(addr, size, MEM_DECOMMIT);
  *needs_recommit = true;  // for safety, assume always decommitted even in the case of an error.
  return (ok ? 0 : (int)GetLastError());
}

int _alloc_hook_prim_reset(void* addr, size_t size) {
  void* p = VirtualAlloc(addr, size, MEM_RESET, PAGE_READWRITE);
  alloc_hook_assert_internal(p == addr);
  #if 0
  if (p != NULL) {
    VirtualUnlock(addr,size); // VirtualUnlock after MEM_RESET removes the memory directly from the working set
  }
  #endif
  return (p != NULL ? 0 : (int)GetLastError());
}

int _alloc_hook_prim_protect(void* addr, size_t size, bool protect) {
  DWORD oldprotect = 0;
  BOOL ok = VirtualProtect(addr, size, protect ? PAGE_NOACCESS : PAGE_READWRITE, &oldprotect);
  return (ok ? 0 : (int)GetLastError());
}


//---------------------------------------------
// Huge page allocation
//---------------------------------------------

static void* _alloc_hook_prim_alloc_huge_os_pagesx(void* hint_addr, size_t size, int numa_node)
{
  const DWORD flags = MEM_LARGE_PAGES | MEM_COMMIT | MEM_RESERVE;

  win_enable_large_os_pages(NULL);

  ALLOC_HOOK_MEM_EXTENDED_PARAMETER params[3] = { {{0,0},{0}},{{0,0},{0}},{{0,0},{0}} };
  // on modern Windows try use NtAllocateVirtualMemoryEx for 1GiB huge pages
  static bool alloc_hook_huge_pages_available = true;
  if (pNtAllocateVirtualMemoryEx != NULL && alloc_hook_huge_pages_available) {
    params[0].Type.Type = MiMemExtendedParameterAttributeFlags;
    params[0].Arg.ULong64 = ALLOC_HOOK_MEM_EXTENDED_PARAMETER_NONPAGED_HUGE;
    ULONG param_count = 1;
    if (numa_node >= 0) {
      param_count++;
      params[1].Type.Type = MiMemExtendedParameterNumaNode;
      params[1].Arg.ULong = (unsigned)numa_node;
    }
    SIZE_T psize = size;
    void* base = hint_addr;
    NTSTATUS err = (*pNtAllocateVirtualMemoryEx)(GetCurrentProcess(), &base, &psize, flags, PAGE_READWRITE, params, param_count);
    if (err == 0 && base != NULL) {
      return base;
    }
    else {
      // fall back to regular large pages
      alloc_hook_huge_pages_available = false; // don't try further huge pages
      _alloc_hook_warning_message("unable to allocate using huge (1GiB) pages, trying large (2MiB) pages instead (status 0x%lx)\n", err);
    }
  }
  // on modern Windows try use VirtualAlloc2 for numa aware large OS page allocation
  if (pVirtualAlloc2 != NULL && numa_node >= 0) {
    params[0].Type.Type = MiMemExtendedParameterNumaNode;
    params[0].Arg.ULong = (unsigned)numa_node;
    return (*pVirtualAlloc2)(GetCurrentProcess(), hint_addr, size, flags, PAGE_READWRITE, params, 1);
  }

  // otherwise use regular virtual alloc on older windows
  return VirtualAlloc(hint_addr, size, flags, PAGE_READWRITE);
}

int _alloc_hook_prim_alloc_huge_os_pages(void* hint_addr, size_t size, int numa_node, bool* is_zero, void** addr) {
  *is_zero = true;
  *addr = _alloc_hook_prim_alloc_huge_os_pagesx(hint_addr,size,numa_node);
  return (*addr != NULL ? 0 : (int)GetLastError());
}


//---------------------------------------------
// Numa nodes
//---------------------------------------------

size_t _alloc_hook_prim_numa_node(void) {
  USHORT numa_node = 0;
  if (pGetCurrentProcessorNumberEx != NULL && pGetNumaProcessorNodeEx != NULL) {
    // Extended API is supported
    ALLOC_HOOK_PROCESSOR_NUMBER pnum;
    (*pGetCurrentProcessorNumberEx)(&pnum);
    USHORT nnode = 0;
    BOOL ok = (*pGetNumaProcessorNodeEx)(&pnum, &nnode);
    if (ok) { numa_node = nnode; }
  }
  else if (pGetNumaProcessorNode != NULL) {
    // Vista or earlier, use older API that is limited to 64 processors. Issue #277
    DWORD pnum = GetCurrentProcessorNumber();
    UCHAR nnode = 0;
    BOOL ok = pGetNumaProcessorNode((UCHAR)pnum, &nnode);
    if (ok) { numa_node = nnode; }
  }
  return numa_node;
}

size_t _alloc_hook_prim_numa_node_count(void) {
  ULONG numa_max = 0;
  GetNumaHighestNodeNumber(&numa_max);
  // find the highest node number that has actual processors assigned to it. Issue #282
  while(numa_max > 0) {
    if (pGetNumaNodeProcessorMaskEx != NULL) {
      // Extended API is supported
      GROUP_AFFINITY affinity;
      if ((*pGetNumaNodeProcessorMaskEx)((USHORT)numa_max, &affinity)) {
        if (affinity.Mask != 0) break;  // found the maximum non-empty node
      }
    }
    else {
      // Vista or earlier, use older API that is limited to 64 processors.
      ULONGLONG mask;
      if (GetNumaNodeProcessorMask((UCHAR)numa_max, &mask)) {
        if (mask != 0) break; // found the maximum non-empty node
      };
    }
    // max node was invalid or had no processor assigned, try again
    numa_max--;
  }
  return ((size_t)numa_max + 1);
}


//----------------------------------------------------------------
// Clock
//----------------------------------------------------------------

static alloc_hook_msecs_t alloc_hook_to_msecs(LARGE_INTEGER t) {
  static LARGE_INTEGER mfreq; // = 0
  if (mfreq.QuadPart == 0LL) {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    mfreq.QuadPart = f.QuadPart/1000LL;
    if (mfreq.QuadPart == 0) mfreq.QuadPart = 1;
  }
  return (alloc_hook_msecs_t)(t.QuadPart / mfreq.QuadPart);
}

alloc_hook_msecs_t _alloc_hook_prim_clock_now(void) {
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return alloc_hook_to_msecs(t);
}


//----------------------------------------------------------------
// Process Info
//----------------------------------------------------------------

#include <windows.h>
#include <psapi.h>

static alloc_hook_msecs_t filetime_msecs(const FILETIME* ftime) {
  ULARGE_INTEGER i;
  i.LowPart = ftime->dwLowDateTime;
  i.HighPart = ftime->dwHighDateTime;
  alloc_hook_msecs_t msecs = (i.QuadPart / 10000); // FILETIME is in 100 nano seconds
  return msecs;
}

typedef BOOL (WINAPI *PGetProcessMemoryInfo)(HANDLE, PPROCESS_MEMORY_COUNTERS, DWORD);
static PGetProcessMemoryInfo pGetProcessMemoryInfo = NULL;

void _alloc_hook_prim_process_info(alloc_hook_process_info_t* pinfo)
{
  FILETIME ct;
  FILETIME ut;
  FILETIME st;
  FILETIME et;
  GetProcessTimes(GetCurrentProcess(), &ct, &et, &st, &ut);
  pinfo->utime = filetime_msecs(&ut);
  pinfo->stime = filetime_msecs(&st);
  
  // load psapi on demand
  if (pGetProcessMemoryInfo == NULL) {
    HINSTANCE hDll = LoadLibrary(TEXT("psapi.dll"));
    if (hDll != NULL) {
      pGetProcessMemoryInfo = (PGetProcessMemoryInfo)(void (*)(void))GetProcAddress(hDll, "GetProcessMemoryInfo");
    }
  }

  // get process info
  PROCESS_MEMORY_COUNTERS info;
  memset(&info, 0, sizeof(info));
  if (pGetProcessMemoryInfo != NULL) {
    pGetProcessMemoryInfo(GetCurrentProcess(), &info, sizeof(info));
  } 
  pinfo->current_rss    = (size_t)info.WorkingSetSize;
  pinfo->peak_rss       = (size_t)info.PeakWorkingSetSize;
  pinfo->current_commit = (size_t)info.PagefileUsage;
  pinfo->peak_commit    = (size_t)info.PeakPagefileUsage;
  pinfo->page_faults    = (size_t)info.PageFaultCount;
}

//----------------------------------------------------------------
// Output
//----------------------------------------------------------------

void _alloc_hook_prim_out_stderr( const char* msg ) 
{
  // on windows with redirection, the C runtime cannot handle locale dependent output
  // after the main thread closes so we use direct console output.
  if (!_alloc_hook_preloading()) {
    // _cputs(msg);  // _cputs cannot be used at is aborts if it fails to lock the console
    static HANDLE hcon = INVALID_HANDLE_VALUE;
    static bool hconIsConsole;
    if (hcon == INVALID_HANDLE_VALUE) {
      CONSOLE_SCREEN_BUFFER_INFO sbi;
      hcon = GetStdHandle(STD_ERROR_HANDLE);
      hconIsConsole = ((hcon != INVALID_HANDLE_VALUE) && GetConsoleScreenBufferInfo(hcon, &sbi));
    }
    const size_t len = _alloc_hook_strlen(msg);
    if (len > 0 && len < UINT32_MAX) {
      DWORD written = 0;
      if (hconIsConsole) {
        WriteConsoleA(hcon, msg, (DWORD)len, &written, NULL);
      }
      else if (hcon != INVALID_HANDLE_VALUE) {
        // use direct write if stderr was redirected
        WriteFile(hcon, msg, (DWORD)len, &written, NULL);
      }
      else {
        // finally fall back to fputs after all
        _alloc_hook_write_stderr(msg);
      }
    }
  }
}

int alloc_hook_prim_dup(int fd) {
  return _dup(fd);
}
int alloc_hook_prim_open(const char* fpath, int open_flags) {
  return _open(fpath,open_flags);
}
ssize_t alloc_hook_prim_read(int fd, void* buf, size_t bufsize) {
  return _read(fd,buf,bufsize);
}
ssize_t alloc_hook_prim_write(int fd, void* buf, size_t bufsize) {
  return _write(fd,buf,bufsize);
}
int alloc_hook_prim_close(int fd) {
  return _close(fd);
}
int alloc_hook_prim_access(const char *fpath, int mode) {
  return _access(fpath,mode);
}

//----------------------------------------------------------------
// Environment
//----------------------------------------------------------------

// On Windows use GetEnvironmentVariable instead of getenv to work
// reliably even when this is invoked before the C runtime is initialized.
// i.e. when `_alloc_hook_preloading() == true`.
// Note: on windows, environment names are not case sensitive.
bool _alloc_hook_prim_getenv(const char* name, char* result, size_t result_size) {
  result[0] = 0;
  size_t len = GetEnvironmentVariableA(name, result, (DWORD)result_size);
  return (len > 0 && len < result_size);
}



//----------------------------------------------------------------
// Random
//----------------------------------------------------------------

#if defined(ALLOC_HOOK_USE_RTLGENRANDOM) // || defined(__cplusplus)
// We prefer to use BCryptGenRandom instead of (the unofficial) RtlGenRandom but when using
// dynamic overriding, we observed it can raise an exception when compiled with C++, and
// sometimes deadlocks when also running under the VS debugger.
// In contrast, issue #623 implies that on Windows Server 2019 we need to use BCryptGenRandom.
// To be continued..
#pragma comment (lib,"advapi32.lib")
#define RtlGenRandom  SystemFunction036
alloc_hook_decl_externc BOOLEAN NTAPI RtlGenRandom(PVOID RandomBuffer, ULONG RandomBufferLength);

bool _alloc_hook_prim_random_buf(void* buf, size_t buf_len) {
  return (RtlGenRandom(buf, (ULONG)buf_len) != 0);
}

#else

#ifndef BCRYPT_USE_SYSTEM_PREFERRED_RNG
#define BCRYPT_USE_SYSTEM_PREFERRED_RNG 0x00000002
#endif

typedef LONG (NTAPI *PBCryptGenRandom)(HANDLE, PUCHAR, ULONG, ULONG);
static  PBCryptGenRandom pBCryptGenRandom = NULL;

bool _alloc_hook_prim_random_buf(void* buf, size_t buf_len) {
  if (pBCryptGenRandom == NULL) {
    HINSTANCE hDll = LoadLibrary(TEXT("bcrypt.dll"));
    if (hDll != NULL) {
      pBCryptGenRandom = (PBCryptGenRandom)(void (*)(void))GetProcAddress(hDll, "BCryptGenRandom");
    }
    if (pBCryptGenRandom == NULL) return false;
  }
  return (pBCryptGenRandom(NULL, (PUCHAR)buf, (ULONG)buf_len, BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0);  
}

#endif  // ALLOC_HOOK_USE_RTLGENRANDOM

//----------------------------------------------------------------
// Thread init/done
//----------------------------------------------------------------

#if !defined(ALLOC_HOOK_SHARED_LIB)

// use thread local storage keys to detect thread ending
#include <fibersapi.h>
#if (_WIN32_WINNT < 0x600)  // before Windows Vista
WINBASEAPI DWORD WINAPI FlsAlloc( _In_opt_ PFLS_CALLBACK_FUNCTION lpCallback );
WINBASEAPI PVOID WINAPI FlsGetValue( _In_ DWORD dwFlsIndex );
WINBASEAPI BOOL  WINAPI FlsSetValue( _In_ DWORD dwFlsIndex, _In_opt_ PVOID lpFlsData );
WINBASEAPI BOOL  WINAPI FlsFree(_In_ DWORD dwFlsIndex);
#endif

static DWORD alloc_hook_fls_key = (DWORD)(-1);

static void NTAPI alloc_hook_fls_done(PVOID value) {
  alloc_hook_heap_t* heap = (alloc_hook_heap_t*)value;
  if (heap != NULL) {
    _alloc_hook_thread_done(heap);
    FlsSetValue(alloc_hook_fls_key, NULL);  // prevent recursion as _alloc_hook_thread_done may set it back to the main heap, issue #672
  }
}

void _alloc_hook_prim_thread_init_auto_done(void) {
  alloc_hook_fls_key = FlsAlloc(&alloc_hook_fls_done);
}

void _alloc_hook_prim_thread_done_auto_done(void) {
  // call thread-done on all threads (except the main thread) to prevent 
  // dangling callback pointer if statically linked with a DLL; Issue #208
  FlsFree(alloc_hook_fls_key);  
}

void _alloc_hook_prim_thread_associate_default_heap(alloc_hook_heap_t* heap) {
  alloc_hook_assert_internal(alloc_hook_fls_key != (DWORD)(-1));
  FlsSetValue(alloc_hook_fls_key, heap);
}

#else

// Dll; nothing to do as in that case thread_done is handled through the DLL_THREAD_DETACH event.

void _alloc_hook_prim_thread_init_auto_done(void) {
}

void _alloc_hook_prim_thread_done_auto_done(void) {
}

void _alloc_hook_prim_thread_associate_default_heap(alloc_hook_heap_t* heap) {
  ALLOC_HOOK_UNUSED(heap);
}

#endif
