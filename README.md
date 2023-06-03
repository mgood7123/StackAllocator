# StackAllocator

this was going to be a stack based allocator but instead this turned into an allocation tracker instead

use namespace SA

includes `Log` i, w, e, and a for logging

`Loga` logs an error then calls `std::terminate`

`SA::Allocator` is used to allocate objects

`SA::DefaultAllocator` is a type alias for `SA::Allocator<SA::PageList<4096>>`

`alloc<T>()` allocates an object large enough to hold that type, and returns a pointer to it as `T*`

the destructor `~Allocator` deallocates all allocated memory via the `alloc<T>()` function

`dealloc(void*)` deallocates an object obtained via `alloc<T>()`, passing `nullptr`, `NULL`, or `0` does nothing, `please note that, due to address recycling by allocators, it is UB to pass an object that has previously been deallocated, or an object not obtained by this instance`

NOTE: `dealloc(void*)` can be used to `manually free memory immediately` instead of `freeing all at once at`, this `may` improve performance if we have too many objects being freed at one time

multiple instances of `SA::Allocator` can co-exist and hold their own allocations

`SA::AllocatorWithMemUsage` is an allocator that additionally keeps track of memory usage and provides pointer logging

`allocWithVerboseAllocationTracking` additionally instructs the `Page` to log its allocation tracking

`allocWithVerboseAllocationTrackingAndVerboseContents` additionally instructs the `Page` to `output the contents` of the `entire allocation` upon `deletion`, in addition to logging its allocation tracking

`SA::DefaultAllocatorWithMemUsage` is a type alias for `SA::AllocatorWithMemUsage<SA::PageList<4096>>`

multiple instances of `SA::AllocatorWithMemUsage` can co-exist and hold their own allocations

`SA::AllocatorBase` is the base class of both `SA::Allocator` and `SA::AllocatorWithMemUsage`

`SA::AllocatorBase` is just an empty structure

## example

```c
#include <SA.h>
namespace A {
    struct A {
        A * real;
        A() {}
        A(A* sub) {
            real = sub;
        }
        virtual ~A() {
            puts("A");
            printf("sub: %p\n", real);
        }
    };
}
int main () {
    SA::Logi("hi");
    SA::AllocatorWithMemUsage a;
    SA::DefaultAllocatorWithMemUsage * ap = a.allocWithVerboseAllocationTrackingAndVerboseContents<SA::DefaultAllocatorWithMemUsage>();
    ap->allocWithVerboseAllocationTrackingAndVerboseContents<int>()[0] = 567884;
    auto * str = ap->allocWithVerboseAllocationTrackingAndVerboseContents<std::string>();
    str[0] = "hello";
    SA::Logi(str[0]);
    a.allocWithVerboseAllocationTrackingAndVerboseContents<A::A>(a.allocWithVerboseAllocationTrackingAndVerboseContents<A::A>());
    return 0;
}
```
```
hi
allocating page: 0xb4000075f7c2c010 with size 16
allocating page: 0xb4000075f7c2b010 with size 4
allocating page: 0xb4000075f7c35000 with size 24
hello
allocating page: 0xb4000075f7c2c020 with size 16
allocating page: 0xb4000075f7c2c030 with size 16
deallocating page: 0xb4000075f7c2c030 with size 16
ptr: 0x000000: 98 6e c9 c6 59 00 00 00  .n..Y...
ptr: 0x000008: 20 c0 c2 f7 75 00 00 b4   ...u...

A
sub: 0xb4000075f7c2c020
deallocating page: 0xb4000075f7c2c020 with size 16
ptr: 0x000000: 98 6e c9 c6 59 00 00 00  .n..Y...
ptr: 0x000008: 00 00 00 00 00 00 00 00  ........

A
sub: 0x0
deallocating page: 0xb4000075f7c2c010 with size 16
ptr: 0x000000: c0 20 c3 f7 75 00 00 b4  . ..u...
ptr: 0x000008: 02 00 00 00 00 00 00 00  ........

deallocating page: 0xb4000075f7c35000 with size 24
ptr: 0x000000: 0a 68 65 6c 6c 6f 00 00  .hello..
ptr: 0x000008: 00 00 00 00 00 00 00 00  ........
ptr: 0x000010: 00 00 00 00 00 00 00 00  ........

deallocating page: 0xb4000075f7c2b010 with size 4
ptr: 0x000000: 4c aa 08 00              L...
```
