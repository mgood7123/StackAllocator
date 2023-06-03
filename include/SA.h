#pragma once

#include "log.h"
#include "hexdump.h"
#include <forward_list>
#include <mutex>

namespace SA {
    template<typename T = uint8_t>
    class Page {
        mutable T * ptr;
        mutable size_t size_;
        mutable std::function<void(void*, size_t)> destructor;
        bool log_memory = false;
        bool log_contents = false;
        void moveData(const Page<T> * from, Page<T> * to) {
            to->log_memory = from->log_memory;
            to->log_contents = from->log_contents;
            if (size_ != 0) {
                if (from->log_memory) {
                    Logib();
                    printf("moving ownership of page: %p with size %zu\n", from->ptr, from->size_);
                    Logr();
                }
                to->ptr = from->ptr;
                to->size_ = from->size_;
                to->destructor = from->destructor;
                from->ptr = nullptr;
                from->size_ = 0;
                from->destructor = [](void*ptr, size_t){};
            }
        }
        public:
        Page() : Page(false, false, 0) {}
        Page(size_t size, std::function<void(void*, size_t)> destructor = [](void*ptr, size_t){}) : Page(false, false, size, destructor) {}
        Page(bool log_contents, bool log_memory, size_t size, std::function<void(void*, size_t)> destructor = [](void*ptr, size_t){}) : size_(size), log_memory(log_memory), log_contents(log_contents) {
            if (size_ != 0) {
                ptr = size == 0 ? nullptr : new T[size];
                this->destructor = destructor;
                if (log_memory) {
                    Logib();
                    printf("allocating page: %p with size %zu\n", ptr, size);
                    Logr();
                }
            } else {
                ptr = nullptr;
            }
        }
        Page(const Page&copy) {
            moveData(&copy, this);
        }
        Page(const Page&&move) {
            moveData(&move, this);
        }
        Page&operator=(const Page&copy) {
            moveData(&copy, this);
            return *this;
        }
        const Page&operator=(const Page&copy) const {
            moveData(&copy, this);
            return *this;
        }
        Page&operator=(const Page&&move) {
            moveData(&move, this);
            return *this;
        }
        const Page&operator=(const Page&&move) const {
            moveData(&move, this);
            return *this;
        }
        T* data() {
            return ptr;
        }
        size_t size() {
            return size_;
        }
        void print(size_t length) {
            if (size_ != 0) {
                Logw(CustomHexdump<8, true, uint8_t>("ptr: ", ptr, length));
            }
        }
        virtual ~Page() {
            if (size_ != 0) {
                if (log_memory) {
                    Logib();
                    printf("deallocating page: %p with size %zu\n", ptr, size_);
                    Logr();
                    if (log_contents) {
                        print(size_);
                    }
                }
                destructor(ptr, size_);
                delete[] ptr;
            }
            ptr = nullptr;
            size_ = 0;
        }
    };
    
    using DefaultPage = Page<uint8_t>;
    
    template <size_t page_size = 4096>
    class PageList {
        std::forward_list<DefaultPage> pages;
        size_t page_count = 0;
        
        public:
        void* add(std::function<void(void*, size_t)> destructor = [](void*ptr, size_t){}) {
            return add(false, false, page_size, destructor);
        }
        void* add(bool log_contents, bool log_memory, std::function<void(void*, size_t)> destructor = [](void*ptr, size_t){}) {
            return add(log_contents, log_memory, page_size, destructor);
        }
        void* add(size_t size, std::function<void(void*, size_t)> destructor = [](void*ptr, size_t){}) {
            return add(false, false, page_size, destructor);
        }
        void* add(bool log_contents, bool log_memory, size_t size, std::function<void(void*, size_t)> destructor = [](void*ptr, size_t){}) {
            pages.emplace_front(log_contents, log_memory, size, destructor);
            page_count++;
            return pages.front().data();
        }
    };
    
    using DefaultPageList = PageList<4096>;

    class AllocatorBase {};
    
    template <typename PageList = DefaultPageList>
    class Allocator : AllocatorBase {
        PageList list;
        public:
        
        template <typename T, typename ... Args>
        T* alloc(Args && ... args) {
            size_t s = sizeof(T);
            if (s != 0) {
                T * p = reinterpret_cast<T*>(list.add(false, false, s, [](void*ptr, size_t unused){
                    ((T*)ptr)->~T();
                }));
                new(p) T(args...);
                return p;
            }
            return nullptr;
        }
    };

    template <typename PageList = DefaultPageList>
    class AllocatorWithMemUsage : AllocatorBase {
        PageList list;
        size_t memory_usage;
        std::mutex m;
        public:
        
        template <typename T, typename ... Args>
        T* alloc(Args && ... args) {
            size_t s = sizeof(T);
            if (s != 0) {
                T * p = reinterpret_cast<T*>(list.add(false, false, s, [&](void*ptr, size_t s){
                    ((T*)ptr)->~T();
                    m.lock();
                    memory_usage -= s;
                    m.unlock();
                }));
                new(p) T(args...);
                m.lock();
                memory_usage += s;
                m.unlock();
                return p;
            }
            return nullptr;
        }

        template <typename T, typename ... Args>
        T* allocWithAllocationTracking(Args && ... args) {
            size_t s = sizeof(T);
            if (s != 0) {
                T * p = reinterpret_cast<T*>(list.add(false, true, s, [&](void*ptr, size_t s){
                    ((T*)ptr)->~T();
                    m.lock();
                    memory_usage -= s;
                    m.unlock();
                }));
                new(p) T(args...);
                m.lock();
                memory_usage += s;
                m.unlock();
                return p;
            }
            return nullptr;
        }

        template <typename T, typename ... Args>
        T* allocWithVerboseContents(Args && ... args) {
            size_t s = sizeof(T);
            if (s != 0) {
                T * p = reinterpret_cast<T*>(list.add(true, false, s, [&](void*ptr, size_t s){
                    ((T*)ptr)->~T();
                    m.lock();
                    memory_usage -= s;
                    m.unlock();
                }));
                new(p) T(args...);
                m.lock();
                memory_usage += s;
                m.unlock();
                return p;
            }
            return nullptr;
        }

        template <typename T, typename ... Args>
        T* allocWithAllocationTrackingAndVerboseContents(Args && ... args) {
            size_t s = sizeof(T);
            if (s != 0) {
                T * p = reinterpret_cast<T*>(list.add(true, true, s, [&](void*ptr, size_t s){
                    ((T*)ptr)->~T();
                    m.lock();
                    memory_usage -= s;
                    m.unlock();
                }));
                new(p) T(args...);
                m.lock();
                memory_usage += s;
                m.unlock();
                return p;
            }
            return nullptr;
        }

        ~AllocatorWithMemUsage() {
            Logib();
            printf("deallocating %zu bytes of memory\n", memory_usage);
            Logr();
        }
    };

    using DefaultAllocator = Allocator<DefaultPageList>;
    using DefaultAllocatorWithMemUsage = AllocatorWithMemUsage<DefaultPageList>;
}