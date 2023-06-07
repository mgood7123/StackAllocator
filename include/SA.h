#ifndef SA_STACK_ALLOCATOR
#define SA_STACK_ALLOCATOR

#include <new>
#include <limits>
#include <cstdlib>
#include <iostream>
#include <functional>
#include <forward_list>
#include <cstdint>
#include <algorithm>
#include "log.h"

namespace SA {

    template<class T>
    struct Mallocator
    {
        typedef T value_type;
    
        Mallocator () = default;
    
        template<class U>
        constexpr Mallocator (const Mallocator <U>&) noexcept {}
    
        [[nodiscard]] T* allocate(std::size_t n)
        {
            if (n > std::numeric_limits<std::size_t>::max() / sizeof(T))
                throw std::bad_array_new_length();
            
            // calloc initializes memory and stops valgrind complaining about uninitialized memory use
            if (auto p = static_cast<T*>(std::calloc(n, sizeof(T))))
            {
                std::cout << "Alloc: allocated and zeroed " << sizeof(T) * n
                        << " bytes at " << std::hex << std::showbase
                        << reinterpret_cast<void*>(p) << std::dec << '\n';
                return p;
            }
    
            throw std::bad_alloc();
        }
    
        void secure_free(T* p, std::size_t n) noexcept
        {
            // the compiler is not allowed to optimize out functions that use volatile pointers
            volatile uint8_t* s = reinterpret_cast<uint8_t*>(p);
            volatile uint8_t* e = s + (sizeof(T)*n);
            std::fill(s, e, 0);
            std::cout << "Dealloc: zeroed " << sizeof(T) * n
                    << " bytes at " << std::hex << std::showbase
                    << reinterpret_cast<void*>(p) << std::dec << '\n';
            std::free(p);
            std::cout << "Dealloc: freed " << sizeof(T) * n
                    << " bytes at " << std::hex << std::showbase
                    << reinterpret_cast<void*>(p) << std::dec << '\n';
        }

        void deallocate(T* p, std::size_t n) noexcept
        {
            // since we zero out mallocs, we mays as well zero out free's as well (for security reasons)
            secure_free(p, n);
        }

    private:
        void report(T* p, std::size_t n, bool alloc = true) const
        {
            std::cout << (alloc ? "Alloc: " : "Dealloc: ") << sizeof(T) * n
                    << " bytes at " << std::hex << std::showbase
                    << reinterpret_cast<void*>(p) << std::dec << '\n';
        }
    };

    using pair = std::pair<std::pair<void*, size_t>, std::function<void(std::pair<void*, size_t>)>>;

    struct Allocator {
        const char * tag;

        std::forward_list<pair, Mallocator<pair>> l;

        Allocator() : Allocator("NO TAG") {}
        Allocator(const char * tag) : tag(tag) {}
        
        pair* find(void* ptr) {
            for (pair & p : l) {
                if (p.first.first == ptr) {
                    return &p;
                }
            }
            return nullptr;
        }

        void * internal_alloc(size_t size) {
            void * p = nullptr;
            while (p == nullptr) {
                p = Mallocator<uint8_t>().allocate(size);
                if (p == nullptr) {
                    auto handler = std::get_new_handler();
                    if (handler == nullptr) {
                        throw std::bad_alloc();
                    } else {
                        handler();
                        continue;
                    }
                } else {
                    break;
                }
            }
            return p;
        }

        template <typename T, typename ... Args>
        T* alloc(Args && ... args) {
            printf("[ Allocator instance: %p ] [ %s ] allocating pointer with size %zu\n", this, tag, sizeof(T));
            T* p = reinterpret_cast<T*>(internal_alloc(sizeof(T)));
            printf("[ Allocator instance: %p ] [ %s ] allocated pointer: %p with size %zu\n", this, tag, p, sizeof(T));
            new(p) T(args...);
            std::pair<void*, size_t> pa;
            pa.first = p;
            pa.second = sizeof(T);
            l.push_front(pair(pa, [] (std::pair<void*, size_t> pair) {
                if (pair.first != nullptr) {
                    ((T*)pair.first)->~T();
                }
            }));
            return p;
        }

        template <typename T>
        T* allocArray(size_t count) {
            printf("[ Allocator instance: %p ] [ %s ] allocating pointer with size %zu (array size: %zu, element size: %zu)\n", this, tag, sizeof(T)*count, count, sizeof(T));
            T* p = reinterpret_cast<T*>(internal_alloc(sizeof(T)*count));
            printf("[ Allocator instance: %p ] [ %s ] allocated pointer: %p with size %zu (array size: %zu, element size: %zu)\n", this, tag, p, sizeof(T)*count, count, sizeof(T));
            std::pair<void*, size_t> pa;
            pa.first = p;
            pa.second = sizeof(T)*count;
            l.push_front(pair(pa, [] (std::pair<void*, size_t> pair) {}));
            return p;
        }

        void * alloc(std::size_t s) {
            printf("[ Allocator instance: %p ] [ %s ] allocating pointer with size %zu\n", this, tag, s);
            void * p = internal_alloc(s);
            printf("[ Allocator instance: %p ] [ %s ] allocated pointer: %p with size %zu\n", this, tag, p, s);
            std::pair<void*, size_t> pa;
            pa.first = p;
            pa.second = s;
            l.push_front(pair(pa, [] (std::pair<void*, size_t> pair) {}));
            return p;
        }

        void dealloc(void* ptr) {
            if (ptr == nullptr) {
                return;
            }
            printf("[ Allocator instance: %p ] [ %s ] dealloc(void* ptr) deallocating pointer: %p ...\n", this, tag, ptr);
            
            auto * p = find(ptr);
            if (p != nullptr) {
                printf("[ Allocator instance: %p ] [ %s ] dealloc(void* ptr) deallocating pointer: %p with size %zu\n", this, tag, p->first.first, p->first.second);
                p->second(p->first);
                Mallocator<uint8_t>().deallocate((uint8_t*)p->first.first, p->first.second);
                p->first.first = nullptr;
                p->first.second = 0;
                printf("[ Allocator instance: %p ] [ %s ] dealloc(void* ptr) deallocated pointer: %p\n", this, tag, ptr);
            } else {
                printf("[ Allocator instance: %p ] [ %s ] dealloc(void* ptr) could not find pointer: %p\n", this, tag, ptr);
            }
            l.remove_if([&](pair & p) { return p.first.first == ptr; });
        }

        ~Allocator() {
            printf("[ Allocator instance: %p ] [ %s ] ~Allocator() BEGIN\n", this, tag);
            for (pair & p : l) {
                if (p.first.first != nullptr) {
                    printf("[ Allocator instance: %p ] [ %s ] ~Allocator() deallocating pointer: %p with size %zu\n", this, tag, p.first.first, p.first.second);
                    Logw(CustomHexdump<16, true, uint8_t>(tag, "ptr: ", reinterpret_cast<uint8_t*>(p.first.first), p.first.second));
                    p.second(p.first);
                    Mallocator<uint8_t>().deallocate((uint8_t*)p.first.first, p.first.second);
                    printf("[ Allocator instance: %p ] [ %s ] ~Allocator() deallocated pointer: %p\n", this, tag, p.first.first);
                    p.first.first = nullptr;
                    p.first.second = 0;
                }
            }
            printf("[ Allocator instance: %p ] [ %s ] ~Allocator() END\n", this, tag);
        }
    };
}

template<class T, class U>
bool operator==(const SA::Mallocator <T>&, const SA::Mallocator <U>&) { return true; }
 
template<class T, class U>
bool operator!=(const SA::Mallocator <T>&, const SA::Mallocator <U>&) { return false; }

#endif
