#include <SA.h>

#ifndef SA_STACK_ALLOCATOR__SA_OVERRIDE_NEW
#ifdef SA_STACK_ALLOCATOR__LOGGING
#warning STACK ALLOCATOR LOGGING ENABLED
bool SA::log = true;
#else
bool SA::log = false;
#endif
#endif

SA::SINGLETONS & SA::GET_SINGLETONS() {
    static SA::SINGLETONS global;
    return global;
}

SA::TrackedAllocator * SA::GET_GLOBAL() {
    // ensure singleton is initialized
    GET_SINGLETONS();
#ifdef SA_STACK_ALLOCATOR__SA_OVERRIDE_NEW
    static SA::TrackedAllocator global;
    return &global;
#else
    return nullptr;
#endif
}
bool SA::IS_GLOBAL(SA::AllocatorBase * allocator) {
    return allocator == GET_GLOBAL();
}