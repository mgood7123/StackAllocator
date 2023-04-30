#include <SA.h>

int main () {
    SA::Logi("hi");
    SA::Allocator a;
    SA::DefaultAllocator * ap = a.alloc<SA::DefaultAllocator>();
    ap->alloc<int>()[0] = 567884;
    auto * str = ap->alloc<std::string>();
    str[0] = "hello";
    SA::Logi(str[0]);
    return 0;
}