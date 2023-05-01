#include "log.h"
#include "hexdump.h"
#include <forward_list>

namespace SA {
    template<typename T = uint8_t>
    class Page {
        mutable T * ptr;
        mutable size_t size_;
        mutable std::function<void(void*)> destructor;
        void moveData(const Page<T> * from, Page<T> * to) {
            Logib();
            printf("moving ownership of page: %p with size %zu\n", from->ptr, from->size_);
            Logr();
            to->ptr = from->ptr;
            to->size_ = from->size_;
            to->destructor = from->destructor;
            from->ptr = nullptr;
            from->size_ = 0;
            from->destructor = [](void*ptr){};
        }
        public:
        Page() : Page(0) {}
        Page(size_t size, std::function<void(void*)> destructor = [](void*ptr){}) : size_(size) {
            ptr = size == 0 ? nullptr : new T[size];
            this->destructor = destructor;
            Logib();
            printf("allocating page: %p with size %zu\n", ptr, size);
            Logr();
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
            Logw(CustomHexdump<8, true, uint8_t>("ptr: ", ptr, length));
        }
        virtual ~Page() {
            Logib();
            printf("deallocating page: %p with size %zu\n", ptr, size_);
            Logr();
            print(size_);
            destructor(ptr);
            delete[] ptr;
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
        void* add(std::function<void(void*)> destructor = [](void*ptr){}) {
            return add(page_size, destructor);
        }
        void* add(size_t size, std::function<void(void*)> destructor = [](void*ptr){}) {
            pages.emplace_front(size, destructor);
            page_count++;
            return pages.front().data();
        }
    };
    
    using DefaultPageList = PageList<4096>;
    
    template <typename PageList = DefaultPageList>
    class Allocator {
        PageList list;
        public:
        
        template <typename T, typename ... Args>
        T* alloc(Args && ... args) {
            size_t s = sizeof(T);
            T * p = reinterpret_cast<T*>(list.add(s, [](void*ptr){
                ((T*)ptr)->~T();
            }));
            new(p) T(args...);
            return p;
        }
    };
    using DefaultAllocator = Allocator<DefaultPageList>;
}