# StackAllocator

this was going to be a stack based allocator but instead this turned into an allocation tracker instead

use namespace SA 

includes `Log` i, w, e, and a for logging

`Loga` logs an error then calls `std::terminate`

`SA::Allocator` is used to allocate objects

`SA::DefaultAllocator` is a type alias for `SA::Allocator<SA::PageList<4096>>`

`alloc<T>()` allocates an object large enough to hold that type, and returns a pointer to it as `T*`

the destructor `~Allocator` deallocates all allocated memory via the `alloc<T>()` function

multiple instances of `SA::Allocator` can co-exist and hold their own allocations

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
    SA::Allocator a;
    SA::DefaultAllocator * ap = a.alloc<SA::DefaultAllocator>();
    ap->alloc<int>()[0] = 567884;
    auto * str = ap->alloc<std::string>();
    str[0] = "hello";
    SA::Logi(str[0]);
    a.alloc<A::A>(a.alloc<A::A>());
    return 0;
}
```
