/* ----------------------------------------------------------------------------
Copyright (c) 2018-2021, Microsoft Research, Daan Leijen
This is free software; you can redistribute it and/or modify it under the
terms of the MIT license. A copy of the license can be found in the file
"LICENSE" at the root of this distribution.
-----------------------------------------------------------------------------*/
#include "alloc_hook.h"
#include "alloc_hook_internal.h"
#include "alloc_hook_atomic.h"
#include "alloc_hook_prim.h"  // alloc_hook_prim_out_stderr

#include <stdio.h>      // FILE
#include <stdlib.h>     // abort
#include <stdarg.h>


static long alloc_hook_max_error_count   = 16; // stop outputting errors after this (use < 0 for no limit)
static long alloc_hook_max_warning_count = 16; // stop outputting warnings after this (use < 0 for no limit)

static void alloc_hook_add_stderr_output(void);

int alloc_hook_version(void) alloc_hook_attr_noexcept {
  return ALLOC_HOOK_MALLOC_VERSION;
}


// --------------------------------------------------------
// Options
// These can be accessed by multiple threads and may be
// concurrently initialized, but an initializing data race
// is ok since they resolve to the same value.
// --------------------------------------------------------
typedef enum alloc_hook_init_e {
  UNINIT,       // not yet initialized
  DEFAULTED,    // not found in the environment, use default value
  INITIALIZED   // found in environment or set explicitly
} alloc_hook_init_t;

typedef struct alloc_hook_option_desc_s {
  long        value;  // the value
  alloc_hook_init_t   init;   // is it initialized yet? (from the environment)
  alloc_hook_option_t option; // for debugging: the option index should match the option
  const char* name;   // option name without `alloc_hook_` prefix
  const char* legacy_name; // potential legacy option name
} alloc_hook_option_desc_t;

#define ALLOC_HOOK_OPTION(opt)                  alloc_hook_option_##opt, #opt, NULL
#define ALLOC_HOOK_OPTION_LEGACY(opt,legacy)    alloc_hook_option_##opt, #opt, #legacy

static alloc_hook_option_desc_t options[_alloc_hook_option_last] =
{
  // stable options
  #if ALLOC_HOOK_DEBUG || defined(ALLOC_HOOK_SHOW_ERRORS)
  { 1, UNINIT, ALLOC_HOOK_OPTION(show_errors) },
  #else
  { 0, UNINIT, ALLOC_HOOK_OPTION(show_errors) },
  #endif
  { 0, UNINIT, ALLOC_HOOK_OPTION(show_stats) },
  { 0, UNINIT, ALLOC_HOOK_OPTION(verbose) },

  // the following options are experimental and not all combinations make sense.
  { 1, UNINIT, ALLOC_HOOK_OPTION(eager_commit) },               // commit per segment directly (4MiB)  (but see also `eager_commit_delay`)
  { 2, UNINIT, ALLOC_HOOK_OPTION_LEGACY(arena_eager_commit,eager_region_commit) }, // eager commit arena's? 2 is used to enable this only on an OS that has overcommit (i.e. linux)
  { 1, UNINIT, ALLOC_HOOK_OPTION_LEGACY(purge_decommits,reset_decommits) },        // purge decommits memory (instead of reset) (note: on linux this uses MADV_DONTNEED for decommit)
  { 0, UNINIT, ALLOC_HOOK_OPTION_LEGACY(allow_large_os_pages,large_os_pages) },    // use large OS pages, use only with eager commit to prevent fragmentation of VMA's
  { 0, UNINIT, ALLOC_HOOK_OPTION(reserve_huge_os_pages) },      // per 1GiB huge pages
  {-1, UNINIT, ALLOC_HOOK_OPTION(reserve_huge_os_pages_at) },   // reserve huge pages at node N
  { 0, UNINIT, ALLOC_HOOK_OPTION(reserve_os_memory)     },
  { 0, UNINIT, ALLOC_HOOK_OPTION(deprecated_segment_cache) },   // cache N segments per thread
  { 0, UNINIT, ALLOC_HOOK_OPTION(deprecated_page_reset) },      // reset page memory on free
  { 0, UNINIT, ALLOC_HOOK_OPTION_LEGACY(abandoned_page_purge,abandoned_page_reset) },       // reset free page memory when a thread terminates
  { 0, UNINIT, ALLOC_HOOK_OPTION(deprecated_segment_reset) },   // reset segment memory on free (needs eager commit)
#if defined(__NetBSD__)
  { 0, UNINIT, ALLOC_HOOK_OPTION(eager_commit_delay) },         // the first N segments per thread are not eagerly committed
#else
  { 1, UNINIT, ALLOC_HOOK_OPTION(eager_commit_delay) },         // the first N segments per thread are not eagerly committed (but per page in the segment on demand)
#endif
  { 10,  UNINIT, ALLOC_HOOK_OPTION_LEGACY(purge_delay,reset_delay) },  // purge delay in milli-seconds
  { 0,   UNINIT, ALLOC_HOOK_OPTION(use_numa_nodes) },           // 0 = use available numa nodes, otherwise use at most N nodes.
  { 0,   UNINIT, ALLOC_HOOK_OPTION(limit_os_alloc) },           // 1 = do not use OS memory for allocation (but only reserved arenas)
  { 100, UNINIT, ALLOC_HOOK_OPTION(os_tag) },                   // only apple specific for now but might serve more or less related purpose
  { 16,  UNINIT, ALLOC_HOOK_OPTION(max_errors) },               // maximum errors that are output
  { 16,  UNINIT, ALLOC_HOOK_OPTION(max_warnings) },             // maximum warnings that are output
  { 8,   UNINIT, ALLOC_HOOK_OPTION(max_segment_reclaim)},       // max. number of segment reclaims from the abandoned segments per try.
  { 0,   UNINIT, ALLOC_HOOK_OPTION(destroy_on_exit)},           // release all OS memory on process exit; careful with dangling pointer or after-exit frees!
  #if (ALLOC_HOOK_INTPTR_SIZE>4)
  { 1024L * 1024L, UNINIT, ALLOC_HOOK_OPTION(arena_reserve) },  // reserve memory N KiB at a time
  #else
  {  128L * 1024L, UNINIT, ALLOC_HOOK_OPTION(arena_reserve) },
  #endif
  { 10,  UNINIT, ALLOC_HOOK_OPTION(arena_purge_mult) },        // purge delay multiplier for arena's
  { 1,   UNINIT, ALLOC_HOOK_OPTION_LEGACY(purge_extend_delay, decommit_extend_delay) },
};

static void alloc_hook_option_init(alloc_hook_option_desc_t* desc);

void _alloc_hook_options_init(void) {
  // called on process load; should not be called before the CRT is initialized!
  // (e.g. do not call this from process_init as that may run before CRT initialization)
  alloc_hook_add_stderr_output(); // now it safe to use stderr for output
  for(int i = 0; i < _alloc_hook_option_last; i++ ) {
    alloc_hook_option_t option = (alloc_hook_option_t)i;
    long l = alloc_hook_option_get(option); ALLOC_HOOK_UNUSED(l); // initialize
    // if (option != alloc_hook_option_verbose)
    {
      alloc_hook_option_desc_t* desc = &options[option];
      _alloc_hook_verbose_message("option '%s': %ld\n", desc->name, desc->value);
    }
  }
  alloc_hook_max_error_count = alloc_hook_option_get(alloc_hook_option_max_errors);
  alloc_hook_max_warning_count = alloc_hook_option_get(alloc_hook_option_max_warnings);
}

alloc_hook_decl_nodiscard long alloc_hook_option_get(alloc_hook_option_t option) {
  alloc_hook_assert(option >= 0 && option < _alloc_hook_option_last);
  if (option < 0 || option >= _alloc_hook_option_last) return 0;
  alloc_hook_option_desc_t* desc = &options[option];
  alloc_hook_assert(desc->option == option);  // index should match the option
  if alloc_hook_unlikely(desc->init == UNINIT) {
    alloc_hook_option_init(desc);
  }
  return desc->value;
}

alloc_hook_decl_nodiscard long alloc_hook_option_get_clamp(alloc_hook_option_t option, long min, long max) {
  long x = alloc_hook_option_get(option);
  return (x < min ? min : (x > max ? max : x));
}

alloc_hook_decl_nodiscard size_t alloc_hook_option_get_size(alloc_hook_option_t option) {
  alloc_hook_assert_internal(option == alloc_hook_option_reserve_os_memory || option == alloc_hook_option_arena_reserve);
  long x = alloc_hook_option_get(option);
  return (x < 0 ? 0 : (size_t)x * ALLOC_HOOK_KiB);
}

void alloc_hook_option_set(alloc_hook_option_t option, long value) {
  alloc_hook_assert(option >= 0 && option < _alloc_hook_option_last);
  if (option < 0 || option >= _alloc_hook_option_last) return;
  alloc_hook_option_desc_t* desc = &options[option];
  alloc_hook_assert(desc->option == option);  // index should match the option
  desc->value = value;
  desc->init = INITIALIZED;
}

void alloc_hook_option_set_default(alloc_hook_option_t option, long value) {
  alloc_hook_assert(option >= 0 && option < _alloc_hook_option_last);
  if (option < 0 || option >= _alloc_hook_option_last) return;
  alloc_hook_option_desc_t* desc = &options[option];
  if (desc->init != INITIALIZED) {
    desc->value = value;
  }
}

alloc_hook_decl_nodiscard bool alloc_hook_option_is_enabled(alloc_hook_option_t option) {
  return (alloc_hook_option_get(option) != 0);
}

void alloc_hook_option_set_enabled(alloc_hook_option_t option, bool enable) {
  alloc_hook_option_set(option, (enable ? 1 : 0));
}

void alloc_hook_option_set_enabled_default(alloc_hook_option_t option, bool enable) {
  alloc_hook_option_set_default(option, (enable ? 1 : 0));
}

void alloc_hook_option_enable(alloc_hook_option_t option) {
  alloc_hook_option_set_enabled(option,true);
}

void alloc_hook_option_disable(alloc_hook_option_t option) {
  alloc_hook_option_set_enabled(option,false);
}

static void alloc_hook_cdecl alloc_hook_out_stderr(const char* msg, void* arg) {
  ALLOC_HOOK_UNUSED(arg);
  if (msg != NULL && msg[0] != 0) {
    _alloc_hook_prim_out_stderr(msg);
  }
}

// Since an output function can be registered earliest in the `main`
// function we also buffer output that happens earlier. When
// an output function is registered it is called immediately with
// the output up to that point.
#ifndef ALLOC_HOOK_MAX_DELAY_OUTPUT
#define ALLOC_HOOK_MAX_DELAY_OUTPUT ((size_t)(32*1024))
#endif
static char out_buf[ALLOC_HOOK_MAX_DELAY_OUTPUT+1];
static _Atomic(size_t) out_len;

static void alloc_hook_cdecl alloc_hook_out_buf(const char* msg, void* arg) {
  ALLOC_HOOK_UNUSED(arg);
  if (msg==NULL) return;
  if (alloc_hook_atomic_load_relaxed(&out_len)>=ALLOC_HOOK_MAX_DELAY_OUTPUT) return;
  size_t n = _alloc_hook_strlen(msg);
  if (n==0) return;
  // claim space
  size_t start = alloc_hook_atomic_add_acq_rel(&out_len, n);
  if (start >= ALLOC_HOOK_MAX_DELAY_OUTPUT) return;
  // check bound
  if (start+n >= ALLOC_HOOK_MAX_DELAY_OUTPUT) {
    n = ALLOC_HOOK_MAX_DELAY_OUTPUT-start-1;
  }
  _alloc_hook_memcpy(&out_buf[start], msg, n);
}

static void alloc_hook_out_buf_flush(alloc_hook_output_fun* out, bool no_more_buf, void* arg) {
  if (out==NULL) return;
  // claim (if `no_more_buf == true`, no more output will be added after this point)
  size_t count = alloc_hook_atomic_add_acq_rel(&out_len, (no_more_buf ? ALLOC_HOOK_MAX_DELAY_OUTPUT : 1));
  // and output the current contents
  if (count>ALLOC_HOOK_MAX_DELAY_OUTPUT) count = ALLOC_HOOK_MAX_DELAY_OUTPUT;
  out_buf[count] = 0;
  out(out_buf,arg);
  if (!no_more_buf) {
    out_buf[count] = '\n'; // if continue with the buffer, insert a newline
  }
}


// Once this module is loaded, switch to this routine
// which outputs to stderr and the delayed output buffer.
static void alloc_hook_cdecl alloc_hook_out_buf_stderr(const char* msg, void* arg) {
  alloc_hook_out_stderr(msg,arg);
  alloc_hook_out_buf(msg,arg);
}



// --------------------------------------------------------
// Default output handler
// --------------------------------------------------------

// Should be atomic but gives errors on many platforms as generally we cannot cast a function pointer to a uintptr_t.
// For now, don't register output from multiple threads.
static alloc_hook_output_fun* volatile alloc_hook_out_default; // = NULL
static _Atomic(void*) alloc_hook_out_arg; // = NULL

static alloc_hook_output_fun* alloc_hook_out_get_default(void** parg) {
  if (parg != NULL) { *parg = alloc_hook_atomic_load_ptr_acquire(void,&alloc_hook_out_arg); }
  alloc_hook_output_fun* out = alloc_hook_out_default;
  return (out == NULL ? &alloc_hook_out_buf : out);
}

void alloc_hook_register_output(alloc_hook_output_fun* out, void* arg) alloc_hook_attr_noexcept {
  alloc_hook_out_default = (out == NULL ? &alloc_hook_out_stderr : out); // stop using the delayed output buffer
  alloc_hook_atomic_store_ptr_release(void,&alloc_hook_out_arg, arg);
  if (out!=NULL) alloc_hook_out_buf_flush(out,true,arg);         // output all the delayed output now
}

// add stderr to the delayed output after the module is loaded
static void alloc_hook_add_stderr_output(void) {
  alloc_hook_assert_internal(alloc_hook_out_default == NULL);
  alloc_hook_out_buf_flush(&alloc_hook_out_stderr, false, NULL); // flush current contents to stderr
  alloc_hook_out_default = &alloc_hook_out_buf_stderr;           // and add stderr to the delayed output
}

// --------------------------------------------------------
// Messages, all end up calling `_alloc_hook_fputs`.
// --------------------------------------------------------
static _Atomic(size_t) error_count;   // = 0;  // when >= max_error_count stop emitting errors
static _Atomic(size_t) warning_count; // = 0;  // when >= max_warning_count stop emitting warnings

// When overriding malloc, we may recurse into alloc_hook_vfprintf if an allocation
// inside the C runtime causes another message.
// In some cases (like on macOS) the loader already allocates which
// calls into alloc_hook; if we then access thread locals (like `recurse`)
// this may crash as the access may call _tlv_bootstrap that tries to
// (recursively) invoke malloc again to allocate space for the thread local
// variables on demand. This is why we use a _alloc_hook_preloading test on such
// platforms. However, C code generator may move the initial thread local address
// load before the `if` and we therefore split it out in a separate funcion.
static alloc_hook_decl_thread bool recurse = false;

static alloc_hook_decl_noinline bool alloc_hook_recurse_enter_prim(void) {
  if (recurse) return false;
  recurse = true;
  return true;
}

static alloc_hook_decl_noinline void alloc_hook_recurse_exit_prim(void) {
  recurse = false;
}

static bool alloc_hook_recurse_enter(void) {
  #if defined(__APPLE__) || defined(ALLOC_HOOK_TLS_RECURSE_GUARD)
  if (_alloc_hook_preloading()) return false;
  #endif
  return alloc_hook_recurse_enter_prim();
}

static void alloc_hook_recurse_exit(void) {
  #if defined(__APPLE__) || defined(ALLOC_HOOK_TLS_RECURSE_GUARD)
  if (_alloc_hook_preloading()) return;
  #endif
  alloc_hook_recurse_exit_prim();
}

void _alloc_hook_fputs(alloc_hook_output_fun* out, void* arg, const char* prefix, const char* message) {
  if (out==NULL || (void*)out==(void*)stdout || (void*)out==(void*)stderr) { // TODO: use alloc_hook_out_stderr for stderr?
    if (!alloc_hook_recurse_enter()) return;
    out = alloc_hook_out_get_default(&arg);
    if (prefix != NULL) out(prefix, arg);
    out(message, arg);
    alloc_hook_recurse_exit();
  }
  else {
    if (prefix != NULL) out(prefix, arg);
    out(message, arg);
  }
}

// Define our own limited `fprintf` that avoids memory allocation.
// We do this using `snprintf` with a limited buffer.
static void alloc_hook_vfprintf( alloc_hook_output_fun* out, void* arg, const char* prefix, const char* fmt, va_list args ) {
  char buf[512];
  if (fmt==NULL) return;
  if (!alloc_hook_recurse_enter()) return;
  vsnprintf(buf,sizeof(buf)-1,fmt,args);
  alloc_hook_recurse_exit();
  _alloc_hook_fputs(out,arg,prefix,buf);
}

void _alloc_hook_fprintf( alloc_hook_output_fun* out, void* arg, const char* fmt, ... ) {
  va_list args;
  va_start(args,fmt);
  alloc_hook_vfprintf(out,arg,NULL,fmt,args);
  va_end(args);
}

static void alloc_hook_vfprintf_thread(alloc_hook_output_fun* out, void* arg, const char* prefix, const char* fmt, va_list args) {
  if (prefix != NULL && _alloc_hook_strnlen(prefix,33) <= 32 && !_alloc_hook_is_main_thread()) {
    char tprefix[64];
    snprintf(tprefix, sizeof(tprefix), "%sthread 0x%llx: ", prefix, (unsigned long long)_alloc_hook_thread_id());
    alloc_hook_vfprintf(out, arg, tprefix, fmt, args);
  }
  else {
    alloc_hook_vfprintf(out, arg, prefix, fmt, args);
  }
}

void _alloc_hook_trace_message(const char* fmt, ...) {
  if (alloc_hook_option_get(alloc_hook_option_verbose) <= 1) return;  // only with verbose level 2 or higher
  va_list args;
  va_start(args, fmt);
  alloc_hook_vfprintf_thread(NULL, NULL, "alloc_hook: ", fmt, args);
  va_end(args);
}

void alloc_hook_trace_message(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  alloc_hook_vfprintf_thread(NULL, NULL, "alloc_hook: ", fmt, args);
  va_end(args);
}

void _alloc_hook_verbose_message(const char* fmt, ...) {
  if (!alloc_hook_option_is_enabled(alloc_hook_option_verbose)) return;
  va_list args;
  va_start(args,fmt);
  alloc_hook_vfprintf(NULL, NULL, "alloc_hook: ", fmt, args);
  va_end(args);
}

void alloc_hook_verbose_message(const char* fmt, ...) {
  va_list args;
  va_start(args,fmt);
  alloc_hook_vfprintf(NULL, NULL, "alloc_hook: ", fmt, args);
  va_end(args);
}

static void alloc_hook_show_error_message(const char* fmt, va_list args) {
  if (!alloc_hook_option_is_enabled(alloc_hook_option_verbose)) {
    if (!alloc_hook_option_is_enabled(alloc_hook_option_show_errors)) return;
    if (alloc_hook_max_error_count >= 0 && (long)alloc_hook_atomic_increment_acq_rel(&error_count) > alloc_hook_max_error_count) return;
  }
  alloc_hook_vfprintf_thread(NULL, NULL, "alloc_hook: error: ", fmt, args);
}

void _alloc_hook_warning_message(const char* fmt, ...) {
  if (!alloc_hook_option_is_enabled(alloc_hook_option_verbose)) {
    if (!alloc_hook_option_is_enabled(alloc_hook_option_show_errors)) return;
    if (alloc_hook_max_warning_count >= 0 && (long)alloc_hook_atomic_increment_acq_rel(&warning_count) > alloc_hook_max_warning_count) return;
  }
  va_list args;
  va_start(args,fmt);
  alloc_hook_vfprintf_thread(NULL, NULL, "alloc_hook: warning: ", fmt, args);
  va_end(args);
}

void alloc_hook_warning_message(const char* fmt, ...) {
  va_list args;
  va_start(args,fmt);
  alloc_hook_vfprintf_thread(NULL, NULL, "alloc_hook: warning: ", fmt, args);
  va_end(args);
}

#if ALLOC_HOOK_DEBUG
void _alloc_hook_assert_fail(const char* assertion, const char* fname, unsigned line, const char* func ) {
  _alloc_hook_fprintf(NULL, NULL, "alloc_hook: assertion failed: at \"%s\":%u, %s\n  assertion: \"%s\"\n", fname, line, (func==NULL?"":func), assertion);
  abort();
}
#endif

// --------------------------------------------------------
// Errors
// --------------------------------------------------------

static alloc_hook_error_fun* volatile  alloc_hook_error_handler; // = NULL
static _Atomic(void*) alloc_hook_error_arg;     // = NULL

static void alloc_hook_error_default(int err) {
  ALLOC_HOOK_UNUSED(err);
#if (ALLOC_HOOK_DEBUG>0)
  if (err==EFAULT) {
    #ifdef _MSC_VER
    __debugbreak();
    #endif
    abort();
  }
#endif
#if (ALLOC_HOOK_SECURE>0)
  if (err==EFAULT) {  // abort on serious errors in secure mode (corrupted meta-data)
    abort();
  }
#endif
#if defined(ALLOC_HOOK_XMALLOC)
  if (err==ENOMEM || err==EOVERFLOW) { // abort on memory allocation fails in xmalloc mode
    abort();
  }
#endif
}

void alloc_hook_register_error(alloc_hook_error_fun* fun, void* arg) {
  alloc_hook_error_handler = fun;  // can be NULL
  alloc_hook_atomic_store_ptr_release(void,&alloc_hook_error_arg, arg);
}

void _alloc_hook_error_message(int err, const char* fmt, ...) {
  // show detailed error message
  va_list args;
  va_start(args, fmt);
  alloc_hook_show_error_message(fmt, args);
  va_end(args);
  // and call the error handler which may abort (or return normally)
  if (alloc_hook_error_handler != NULL) {
    alloc_hook_error_handler(err, alloc_hook_atomic_load_ptr_acquire(void,&alloc_hook_error_arg));
  }
  else {
    alloc_hook_error_default(err);
  }
}

void alloc_hook_error_message(int err, const char* fmt, ...) {
  // show detailed error message
  va_list args;
  va_start(args, fmt);
  alloc_hook_show_error_message(fmt, args);
  va_end(args);
  // and call the error handler which may abort (or return normally)
  if (alloc_hook_error_handler != NULL) {
    alloc_hook_error_handler(err, alloc_hook_atomic_load_ptr_acquire(void,&alloc_hook_error_arg));
  }
  else {
    alloc_hook_error_default(err);
  }
}


// --------------------------------------------------------
// Initialize options by checking the environment
// --------------------------------------------------------
char _alloc_hook_toupper(char c) {
  if (c >= 'a' && c <= 'z') return (c - 'a' + 'A');
                       else return c;
}

int _alloc_hook_strnicmp(const char* s, const char* t, size_t n) {
  if (n == 0) return 0;
  for (; *s != 0 && *t != 0 && n > 0; s++, t++, n--) {
    if (_alloc_hook_toupper(*s) != _alloc_hook_toupper(*t)) break;
  }
  return (n == 0 ? 0 : *s - *t);
}

void _alloc_hook_strlcpy(char* dest, const char* src, size_t dest_size) {
  if (dest==NULL || src==NULL || dest_size == 0) return;
  // copy until end of src, or when dest is (almost) full
  while (*src != 0 && dest_size > 1) {
    *dest++ = *src++;
    dest_size--;
  }
  // always zero terminate
  *dest = 0;
}

void _alloc_hook_strlcat(char* dest, const char* src, size_t dest_size) {
  if (dest==NULL || src==NULL || dest_size == 0) return;
  // find end of string in the dest buffer
  while (*dest != 0 && dest_size > 1) {
    dest++;
    dest_size--;
  }
  // and catenate
  _alloc_hook_strlcpy(dest, src, dest_size);
}

size_t _alloc_hook_strlen(const char* s) {
  if (s==NULL) return 0;
  size_t len = 0;
  while(s[len] != 0) { len++; }
  return len;
}

size_t _alloc_hook_strnlen(const char* s, size_t max_len) {
  if (s==NULL) return 0;
  size_t len = 0;
  while(s[len] != 0 && len < max_len) { len++; }
  return len;
}

#ifdef ALLOC_HOOK_NO_GETENV
static bool alloc_hook_getenv(const char* name, char* result, size_t result_size) {
  ALLOC_HOOK_UNUSED(name);
  ALLOC_HOOK_UNUSED(result);
  ALLOC_HOOK_UNUSED(result_size);
  return false;
}
#else
static bool alloc_hook_getenv(const char* name, char* result, size_t result_size) {
  if (name==NULL || result == NULL || result_size < 64) return false;
  return _alloc_hook_prim_getenv(name,result,result_size);
}
#endif

// TODO: implement ourselves to reduce dependencies on the C runtime
#include <stdlib.h> // strtol
#include <string.h> // strstr


static void alloc_hook_option_init(alloc_hook_option_desc_t* desc) {
  // Read option value from the environment
  char s[64 + 1];
  char buf[64+1];
  _alloc_hook_strlcpy(buf, "alloc_hook_", sizeof(buf));
  _alloc_hook_strlcat(buf, desc->name, sizeof(buf));
  bool found = alloc_hook_getenv(buf, s, sizeof(s));
  if (!found && desc->legacy_name != NULL) {
    _alloc_hook_strlcpy(buf, "alloc_hook_", sizeof(buf));
    _alloc_hook_strlcat(buf, desc->legacy_name, sizeof(buf));
    found = alloc_hook_getenv(buf, s, sizeof(s));
    if (found) {
      _alloc_hook_warning_message("environment option \"alloc_hook_%s\" is deprecated -- use \"alloc_hook_%s\" instead.\n", desc->legacy_name, desc->name);
    }
  }

  if (found) {
    size_t len = _alloc_hook_strnlen(s, sizeof(buf) - 1);
    for (size_t i = 0; i < len; i++) {
      buf[i] = _alloc_hook_toupper(s[i]);
    }
    buf[len] = 0;
    if (buf[0] == 0 || strstr("1;TRUE;YES;ON", buf) != NULL) {
      desc->value = 1;
      desc->init = INITIALIZED;
    }
    else if (strstr("0;FALSE;NO;OFF", buf) != NULL) {
      desc->value = 0;
      desc->init = INITIALIZED;
    }
    else {
      char* end = buf;
      long value = strtol(buf, &end, 10);
      if (desc->option == alloc_hook_option_reserve_os_memory || desc->option == alloc_hook_option_arena_reserve) {
        // this option is interpreted in KiB to prevent overflow of `long`
        if (*end == 'K') { end++; }
        else if (*end == 'M') { value *= ALLOC_HOOK_KiB; end++; }
        else if (*end == 'G') { value *= ALLOC_HOOK_MiB; end++; }
        else { value = (value + ALLOC_HOOK_KiB - 1) / ALLOC_HOOK_KiB; }
        if (end[0] == 'I' && end[1] == 'B') { end += 2; }
        else if (*end == 'B') { end++; }
      }
      if (*end == 0) {
        desc->value = value;
        desc->init = INITIALIZED;
      }
      else {
        // set `init` first to avoid recursion through _alloc_hook_warning_message on alloc_hook_verbose.
        desc->init = DEFAULTED;
        if (desc->option == alloc_hook_option_verbose && desc->value == 0) {
          // if the 'alloc_hook_verbose' env var has a bogus value we'd never know
          // (since the value defaults to 'off') so in that case briefly enable verbose
          desc->value = 1;
          _alloc_hook_warning_message("environment option alloc_hook_%s has an invalid value.\n", desc->name);
          desc->value = 0;
        }
        else {
          _alloc_hook_warning_message("environment option alloc_hook_%s has an invalid value.\n", desc->name);
        }
      }
    }
    alloc_hook_assert_internal(desc->init != UNINIT);
  }
  else if (!_alloc_hook_preloading()) {
    desc->init = DEFAULTED;
  }
}
