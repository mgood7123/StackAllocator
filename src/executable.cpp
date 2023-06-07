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
    SA::Allocator * ap = a.alloc<SA::Allocator>();
    ap->alloc<int>()[0] = 567884;
    auto * str = ap->alloc<std::string>();
    str[0] = "hello";
    SA::Logi(str[0]);
    A::A * ptr = a.alloc<A::A>();
    a.alloc<A::A>(ptr);
    return 0;
}