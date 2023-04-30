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