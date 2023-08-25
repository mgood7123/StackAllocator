# StackAllocator

an Arena Allocator powerful enough to be used as a global C++ allocator

underlying allocations are done via `calloc` and `free`

API USAGE:

```cpp
// create instance
SA::Allocator a;

// i4 will be collected at end of scope
auto * i4 = a.alloc<int>(4);

auto * i5 = new int(5);

// i5 will be collected, a custom deleter may be supplied, defaults to `delete`
a.adopt(i5);

// i5 will not be collected
a.release(i5);

// manually collect i5
delete i5;

// i6 will be collected
a.adopt(new int(6));

{
    SA::DefaultAllocator b;
    // float will be collected by outer scoped allocator
    a.adopt(b.alloc<float>(5.7));
}
```

```cpp
struct A2 {
    SA::DefaultAllocatorWithMemUsage allocator;
    A2 * real;
    A2() {}
    A2(A2* sub) {
        real = sub;
        allocator.adopt(real, [](auto p) {delete static_cast<A2*>(p);});
    }
    virtual ~A2() {
        puts("A2");
        printf("sub: %p\n", real);
    }
};
void t() {
    SA::DefaultAllocator a;
    a.alloc<A::A2>(a.alloc<A::A2>()); // cyclic references cannot occur even if explicitly made as A2 does
}
```

TODO: update this readme


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

NOTE: `dealloc`, `CANNOT` and `WILL NOT` recursively free any memory that may be contained `within the pointer`

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


# EXAMPLE

```cpp
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
    struct A2 {
        SA::DefaultAllocatorWithMemUsage allocator;
        A2 * real;
        A2() {}
        A2(A2* sub) {
            real = sub;
            allocator.adopt(real);
        }
        virtual ~A2() {
            puts("A2");
            printf("sub: %p\n", real);
        }
    };
}

struct V {
    V(int g) {
        puts("V(int)");
    }
    ~V() {
        puts("~V()");
    }
};

int main () {
    {
        SA::Logi("hi");
        SA::Allocator a;
        SA::DefaultAllocatorWithMemUsage * ap = a.alloc<SA::DefaultAllocatorWithMemUsage>();
        ap->alloc<int>()[0] = 567884;
        auto * str = ap->alloc<std::string>();
        str[0] = "hello";
        SA::Logi(str[0]);
        a.alloc<A::A>(a.alloc<A::A>());
        a.alloc<A::A2>(a.alloc<A::A2>());
        V * v = new V(8);
        delete v;
        auto x = std::move(a);
        auto p = x.alloc<V>(8);
        x.dealloc(p);
        auto p2 = x.alloc<V>(8);
        V v2(8);

        auto ad = malloc(5);
        x.adopt(ad, [](auto p){free(p);});
        x.adopt(ad, [](auto p){free(p);});
        x.adopt(ad, [](auto p){free(p);});
        x.adopt(ad, [](auto p){free(p);});
        x.adopt(ad, [](auto p){free(p);});
        x.adopt(ad, [](auto p){free(p);});
        x.adopt(ad, [](auto p){free(p);});
        x.adopt(ad, [](auto p){free(p);});
        x.adopt(ad, [](auto p){free(p);});
        auto ad2 = malloc(5);
        x.adopt(ad2, [](auto p){free(p);});
        x.adopt(ad2, [](auto p){free(p);});
        x.release(ad2);
        free(ad2);
        ad2 = malloc(5);
        x.adopt(ad2, [](auto p){free(p);});
        x.adopt(ad2, [](auto p){free(p);});
        x.release(ad2);
        x.adopt(ad2, [](auto p){free(p);});
        x.adopt(ad2, [](auto p){free(p);});
        x.adopt(ad2, [](auto p){free(p);});
        x.release(ad2);
        x.release(ad2);
        x.adopt(ad2, [](auto p){free(p);});
        x.adopt(ad2, [](auto p){free(p);});
        x.adopt(ad2, [](auto p){free(p);});
        x.adopt(ad2, [](auto p){free(p);});
        x.adopt(ad2, [](auto p){free(p);});
        x.adopt(ad2, [](auto p){free(p);});
    }

    {
        // create instance
        SA::Allocator a;

        // i4 will be collected at end of scope
        auto * i4 = a.alloc<int>(4);

        auto * i5 = new int(5);

        // i5 will be collected
        a.adopt(i5);

        // i5 will not be collected
        a.release(i5);

        // manually collect i5
        delete i5;

        // i6 will be collected
        a.adopt(new int(6));

        {
            SA::DefaultAllocator b;
            // float will be collected outer by scoped allocator
            a.adopt(b.alloc<float>(5.7));
        }
    }
    
    return 0;
}
```

# COMPLETE OUTPUT

```cpp
$ ./debug_EXECUTABLE/stack_exe
LL()
LLP()
LLPI()
SINGLETONS()
hi
PER_TYPE<SA::TrackedAllocatorWithMemUsage>()
calloc returned 0x21092c0
allocated 40 bytes of memory, total memory usage for 'SA::TrackedAllocatorWithMemUsage': 40 bytes. total memory usage: 40 bytes
LLP()
PointerInfo()
LL()
PER_TYPE<std::mutex>()
calloc returned 0x2109450
allocated 40 bytes of memory, total memory usage for 'std::mutex': 40 bytes. total memory usage: 80 bytes
LLP()
LLPI()
LLP()
PointerInfo()
LL()
PER_TYPE<int>()
calloc returned 0x2109600
allocated 4 bytes of memory, total memory usage for 'int': 4 bytes. total memory usage: 84 bytes
LLP()
LLPI()
LLP()
PointerInfo()
LL()
PER_TYPE<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >>()
calloc returned 0x2109780
allocated 32 bytes of memory, total memory usage for 'std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >': 32 bytes. total memory usage: 116 bytes
LLP()
LLPI()
LLP()
PointerInfo()
hello
LL()
PER_TYPE<A::A>()
calloc returned 0x21099e0
allocated 16 bytes of memory, total memory usage for 'A::A': 16 bytes. total memory usage: 132 bytes
LLP()
LLPI()
LLP()
PointerInfo()
calloc returned 0x2109b60
allocated 16 bytes of memory, total memory usage for 'A::A': 32 bytes. total memory usage: 148 bytes
LLP()
LLPI()
LLP()
PointerInfo()
LL()
PER_TYPE<A::A2>()
calloc returned 0x2109c40
allocated 56 bytes of memory, total memory usage for 'A::A2': 56 bytes. total memory usage: 204 bytes
LLP()
LLPI()
LLP()
PointerInfo()
calloc returned 0x2109de0
allocated 40 bytes of memory, total memory usage for 'std::mutex': 80 bytes. total memory usage: 244 bytes
LLP()
LLPI()
LLP()
PointerInfo()
calloc returned 0x2109ed0
allocated 56 bytes of memory, total memory usage for 'A::A2': 112 bytes. total memory usage: 300 bytes
LLP()
LLPI()
LLP()
PointerInfo()
calloc returned 0x2109fd0
allocated 40 bytes of memory, total memory usage for 'std::mutex': 120 bytes. total memory usage: 340 bytes
LLP()
LLPI()
LLP()
PointerInfo()
LLP()
LL()
PER_TYPE<unsigned char>()
calloc returned 0x210a0e0
allocated 1 bytes of memory, total memory usage for 'unsigned char': 1 bytes. total memory usage: 341 bytes
LLP()
LLPI()
LLP()
PointerInfo()
V(int)
~V()
FIND: comparing tracked pointer 0x434358 with wanted pointer 0x434358
deallocate called with p = 0x210a0e0, n = 1
REMOVE: comparing tracked pointer 0x21092c0 with wanted pointer 0x210a0e0
REMOVE: comparing tracked pointer 0x2109450 with wanted pointer 0x210a0e0
REMOVE: comparing tracked pointer 0x2109600 with wanted pointer 0x210a0e0
REMOVE: comparing tracked pointer 0x2109780 with wanted pointer 0x210a0e0
REMOVE: comparing tracked pointer 0x21099e0 with wanted pointer 0x210a0e0
REMOVE: comparing tracked pointer 0x2109b60 with wanted pointer 0x210a0e0
REMOVE: comparing tracked pointer 0x2109c40 with wanted pointer 0x210a0e0
REMOVE: comparing tracked pointer 0x2109de0 with wanted pointer 0x210a0e0
REMOVE: comparing tracked pointer 0x2109ed0 with wanted pointer 0x210a0e0
REMOVE: comparing tracked pointer 0x2109fd0 with wanted pointer 0x210a0e0
REMOVE: comparing tracked pointer 0x210a0e0 with wanted pointer 0x210a0e0
deallocating 1 bytes of memory, total memory usage for 'unsigned char': 1 bytes. total memory usage: 341 bytes
logging contents
ptr: 0x000000: 00                                               .

REMOVE: comparing tracked pointer 0x434358 with wanted pointer 0x434358
~PointerInfo()
~LLP()
LL()
PER_TYPE<V>()
calloc returned 0x210a260
allocated 1 bytes of memory, total memory usage for 'V': 1 bytes. total memory usage: 341 bytes
LLP()
PointerInfo()
V(int)
FIND: comparing tracked pointer 0x7ffe04699000 with wanted pointer 0x7ffe04699000
~V()
deallocate called with p = 0x210a260, n = 1
REMOVE: comparing tracked pointer 0x21092c0 with wanted pointer 0x210a260
REMOVE: comparing tracked pointer 0x2109450 with wanted pointer 0x210a260
REMOVE: comparing tracked pointer 0x2109600 with wanted pointer 0x210a260
REMOVE: comparing tracked pointer 0x2109780 with wanted pointer 0x210a260
REMOVE: comparing tracked pointer 0x21099e0 with wanted pointer 0x210a260
REMOVE: comparing tracked pointer 0x2109b60 with wanted pointer 0x210a260
REMOVE: comparing tracked pointer 0x2109c40 with wanted pointer 0x210a260
REMOVE: comparing tracked pointer 0x2109de0 with wanted pointer 0x210a260
REMOVE: comparing tracked pointer 0x2109ed0 with wanted pointer 0x210a260
REMOVE: comparing tracked pointer 0x2109fd0 with wanted pointer 0x210a260
REMOVE: comparing tracked pointer 0x210a260 with wanted pointer 0x210a260
deallocating 1 bytes of memory, total memory usage for 'V': 1 bytes. total memory usage: 341 bytes
logging contents
ptr: 0x000000: 00                                               .

REMOVE: comparing tracked pointer 0x7ffe04699000 with wanted pointer 0x7ffe04699000
~PointerInfo()
~LLP()
calloc returned 0x210a380
allocated 1 bytes of memory, total memory usage for 'V': 1 bytes. total memory usage: 341 bytes
LLP()
PointerInfo()
V(int)
V(int)
LLPI()
LLP()
PointerInfo()
LLP()
LLP()
LLP()
LLP()
LLP()
LLP()
LLP()
LLP()
LLPI()
LLP()
PointerInfo()
LLP()
~PointerInfo()
~LLP()
~LLP()
LLP()
PointerInfo()
LLP()
~PointerInfo()
~LLP()
~LLP()
LLP()
PointerInfo()
LLP()
LLP()
~PointerInfo()
~LLP()
~LLP()
~LLP()
LLP()
PointerInfo()
LLP()
LLP()
LLP()
LLP()
LLP()
~V()
FIND: comparing tracked pointer 0x7ffe04699040 with wanted pointer 0x7ffe04699000
FIND: comparing tracked pointer 0x7ffe04699040 with wanted pointer 0x7ffe04699040
deallocating 0 bytes of memory
FIND: comparing tracked pointer 0x7ffe04699040 with wanted pointer 0x21092c0
deallocated 0 bytes of memory
FIND: comparing tracked pointer 0x7ffe04699040 with wanted pointer 0x21092d8
FIND: comparing tracked pointer 0x7ffe04699040 with wanted pointer 0x21092c0
deallocate called with p = 0x21092c0, n = 1
REMOVE: comparing tracked pointer 0x21092c0 with wanted pointer 0x21092c0
~LLP()
deallocating 40 bytes of memory, total memory usage for 'SA::TrackedAllocatorWithMemUsage': 40 bytes. total memory usage: 341 bytes
logging contents
ptr: 0x000000: a0 0f 42 00 00 00 00 00 00 00 00 00 00 00 00 00  ..B.............
ptr: 0x000010: f0 13 42 00 00 00 00 00 a0 0f 42 00 00 00 00 00  ..B.......B.....
ptr: 0x000020: 50 94 10 02 00 00 00 00                          P.......

REMOVE: comparing tracked pointer 0x7ffe04699040 with wanted pointer 0x7ffe04699040
~PointerInfo()
~LLP()
~LLPI()
calloc returned 0x210a900
allocated 4 bytes of memory, total memory usage for 'int': 8 bytes. total memory usage: 305 bytes
LLP()
LLPI()
LLP()
PointerInfo()
calloc returned 0x210a9e0
allocated 4 bytes of memory, total memory usage for 'unsigned char': 4 bytes. total memory usage: 309 bytes
LLP()
LLPI()
LLP()
PointerInfo()
LLP()
~PointerInfo()
~LLP()
~LLP()
calloc returned 0x210aae0
allocated 4 bytes of memory, total memory usage for 'unsigned char': 8 bytes. total memory usage: 313 bytes
LLP()
LLP()
PointerInfo()
LLP()
LL()
PER_TYPE<float>()
calloc returned 0x210abc0
allocated 4 bytes of memory, total memory usage for 'float': 4 bytes. total memory usage: 317 bytes
LLP()
LLPI()
LLP()
PointerInfo()
LLP()
FIND: comparing tracked pointer 0x21092d8 with wanted pointer 0x7ffe04698be8
FIND: comparing tracked pointer 0x21092d8 with wanted pointer 0x7ffe04698c58
FIND: comparing tracked pointer 0x21092d8 with wanted pointer 0x434358
~SINGLETONS()
~LLPI()
deallocate called with p = 0x2109450, n = 1
REMOVE: comparing tracked pointer 0x2109450 with wanted pointer 0x2109450
~LLP()
deallocating 40 bytes of memory, total memory usage for 'std::mutex': 120 bytes. total memory usage: 317 bytes
logging contents
ptr: 0x000000: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
ptr: 0x000010: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
ptr: 0x000020: 00 00 00 00 00 00 00 00                          ........

~PointerInfo()
~LLP()
~LLPI()
deallocate called with p = 0x2109600, n = 1
REMOVE: comparing tracked pointer 0x2109600 with wanted pointer 0x2109600
~LLP()
deallocating 4 bytes of memory, total memory usage for 'int': 8 bytes. total memory usage: 277 bytes
logging contents
ptr: 0x000000: 4c aa 08 00                                      L...

~PointerInfo()
~LLP()
~LLPI()
deallocate called with p = 0x2109780, n = 1
REMOVE: comparing tracked pointer 0x2109780 with wanted pointer 0x2109780
~LLP()
deallocating 32 bytes of memory, total memory usage for 'std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >': 32 bytes. total memory usage: 273 bytes
logging contents
ptr: 0x000000: 90 97 10 02 00 00 00 00 05 00 00 00 00 00 00 00  ................
ptr: 0x000010: 68 65 6c 6c 6f 00 00 00 00 00 00 00 00 00 00 00  hello...........

~PointerInfo()
~LLP()
~LLPI()
A
sub: (nil)
deallocate called with p = 0x21099e0, n = 1
REMOVE: comparing tracked pointer 0x21099e0 with wanted pointer 0x21099e0
~LLP()
deallocating 16 bytes of memory, total memory usage for 'A::A': 32 bytes. total memory usage: 241 bytes
logging contents
ptr: 0x000000: 10 1d 42 00 00 00 00 00 00 00 00 00 00 00 00 00  ..B.............

~PointerInfo()
~LLP()
~LLPI()
A
sub: 0x21099e0
deallocate called with p = 0x2109b60, n = 1
REMOVE: comparing tracked pointer 0x2109b60 with wanted pointer 0x2109b60
~LLP()
deallocating 16 bytes of memory, total memory usage for 'A::A': 16 bytes. total memory usage: 225 bytes
logging contents
ptr: 0x000000: 10 1d 42 00 00 00 00 00 e0 99 10 02 00 00 00 00  ..B.............

~PointerInfo()
~LLP()
~LLPI()
A2
sub: (nil)
deallocating 0 bytes of memory
FIND: comparing tracked pointer 0x7ffe04699040 with wanted pointer 0x2109c48
FIND: comparing tracked pointer 0x2109ed8 with wanted pointer 0x2109c48
deallocated 0 bytes of memory
FIND: comparing tracked pointer 0x7ffe04699040 with wanted pointer 0x2109c60
FIND: comparing tracked pointer 0x2109ed8 with wanted pointer 0x2109c60
FIND: comparing tracked pointer 0x7ffe04699040 with wanted pointer 0x2109c48
FIND: comparing tracked pointer 0x2109ed8 with wanted pointer 0x2109c48
deallocate called with p = 0x2109c40, n = 1
REMOVE: comparing tracked pointer 0x2109c40 with wanted pointer 0x2109c40
~LLP()
deallocating 56 bytes of memory, total memory usage for 'A::A2': 112 bytes. total memory usage: 209 bytes
logging contents
ptr: 0x000000: 70 1f 42 00 00 00 00 00 a0 0f 42 00 00 00 00 00  p.B.......B.....
ptr: 0x000010: 00 00 00 00 00 00 00 00 f0 13 42 00 00 00 00 00  ..........B.....
ptr: 0x000020: a0 0f 42 00 00 00 00 00 e0 9d 10 02 00 00 00 00  ..B.............
ptr: 0x000030: 00 00 00 00 00 00 00 00                          ........

~PointerInfo()
~LLP()
~LLP()
~LLPI()
deallocate called with p = 0x2109de0, n = 1
REMOVE: comparing tracked pointer 0x2109de0 with wanted pointer 0x2109de0
~LLP()
deallocating 40 bytes of memory, total memory usage for 'std::mutex': 80 bytes. total memory usage: 153 bytes
logging contents
ptr: 0x000000: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
ptr: 0x000010: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
ptr: 0x000020: 00 00 00 00 00 00 00 00                          ........

~PointerInfo()
~LLP()
~LLPI()
A2
sub: 0x2109c40
deallocating 0 bytes of memory
FIND: comparing tracked pointer 0x7ffe04699040 with wanted pointer 0x2109ed8
deallocated 0 bytes of memory
FIND: comparing tracked pointer 0x7ffe04699040 with wanted pointer 0x2109ef0
FIND: comparing tracked pointer 0x7ffe04699040 with wanted pointer 0x2109ed8
deallocate called with p = 0x2109ed0, n = 1
REMOVE: comparing tracked pointer 0x2109ed0 with wanted pointer 0x2109ed0
~LLP()
deallocating 56 bytes of memory, total memory usage for 'A::A2': 56 bytes. total memory usage: 113 bytes
logging contents
ptr: 0x000000: 70 1f 42 00 00 00 00 00 a0 0f 42 00 00 00 00 00  p.B.......B.....
ptr: 0x000010: 00 00 00 00 00 00 00 00 f0 13 42 00 00 00 00 00  ..........B.....
ptr: 0x000020: a0 0f 42 00 00 00 00 00 d0 9f 10 02 00 00 00 00  ..B.............
ptr: 0x000030: 40 9c 10 02 00 00 00 00                          @.......

~PointerInfo()
~LLP()
~LLPI()
deallocate called with p = 0x2109fd0, n = 1
REMOVE: comparing tracked pointer 0x2109fd0 with wanted pointer 0x2109fd0
~LLP()
deallocating 40 bytes of memory, total memory usage for 'std::mutex': 40 bytes. total memory usage: 57 bytes
logging contents
ptr: 0x000000: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
ptr: 0x000010: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
ptr: 0x000020: 00 00 00 00 00 00 00 00                          ........

~PointerInfo()
~LLP()
~LLPI()
~V()
deallocate called with p = 0x210a380, n = 1
REMOVE: comparing tracked pointer 0x210a380 with wanted pointer 0x210a380
~LLP()
deallocating 1 bytes of memory, total memory usage for 'V': 1 bytes. total memory usage: 17 bytes
logging contents
ptr: 0x000000: 00                                               .

~PointerInfo()
~LLP()
~LLPI()
~PointerInfo()
~LLP()
~LLP()
~LLP()
~LLP()
~LLP()
~LLP()
~LLP()
~LLP()
~LLP()
~LLPI()
~PointerInfo()
~LLP()
~LLP()
~LLP()
~LLP()
~LLP()
~LLP()
~LLPI()
deallocate called with p = 0x210a900, n = 1
REMOVE: comparing tracked pointer 0x210a900 with wanted pointer 0x210a900
~LLP()
deallocating 4 bytes of memory, total memory usage for 'int': 4 bytes. total memory usage: 16 bytes
logging contents
ptr: 0x000000: 04 00 00 00                                      ....

~PointerInfo()
~LLP()
~LLPI()
deallocate called with p = 0x210aae0, n = 1
REMOVE: comparing tracked pointer 0x210a9e0 with wanted pointer 0x210aae0
REMOVE: comparing tracked pointer 0x210aae0 with wanted pointer 0x210aae0
~LLP()
~LLP()
deallocating 1 bytes of memory, total memory usage for 'unsigned char': 8 bytes. total memory usage: 12 bytes
logging contents
ptr: 0x000000: 06                                               .

~PointerInfo()
~LLP()
~LLP()
~LLPI()
deallocate called with p = 0x210abc0, n = 1
REMOVE: comparing tracked pointer 0x210a9e0 with wanted pointer 0x210abc0
REMOVE: comparing tracked pointer 0x210ac50 with wanted pointer 0x210abc0
error: pointer 0x210abc0 could not be found in the list of allocated pointers, ignoring
~PointerInfo()
~LLP()
~LLP()
~LLP()
~LLP()
~LL()
~PER_TYPE<SA::TrackedAllocatorWithMemUsage>()
~LL()
~PER_TYPE<std::mutex>()
~LL()
~PER_TYPE<int>()
~LL()
~PER_TYPE<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >>()
~LL()
~PER_TYPE<A::A>()
~LL()
~PER_TYPE<A::A2>()
~LL()
~PER_TYPE<unsigned char>()
~LL()
~PER_TYPE<V>()
~LL()
~PER_TYPE<float>()
```