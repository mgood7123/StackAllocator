#include <SA.h>

int main () {
    SA::Logi("hi");
    SA::Allocator a;
    SA::DefaultAllocator * ap = a.alloc<SA::DefaultAllocator>();
    ap->alloc<int>()[0] = 567884;
    return 0;
}