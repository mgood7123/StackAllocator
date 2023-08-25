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
            // float will be collected outer scoped allocator
            a.adopt(b.alloc<float>(5.7));
        }
    }
    
    return 0;
}