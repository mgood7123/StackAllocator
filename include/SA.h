#ifdef SA_STACK_ALLOCATOR__SA_OVERRIDE_NEW
#define SA_STACK_ALLOCATOR

#include <new>
#include <memory>
#include <mutex>
#include <stdlib.h>
#include <limits>

std::recursive_mutex SA_STACK_ALLOCATOR__SA_OVERRIDE_NEW________________MUTEX;

struct {
    static SA::DefaultMallocAllocatorWithMemUsage& instance()
    {
        static SA::DefaultMallocAllocatorWithMemUsage SA_STACK_ALLOCATOR__SA_OVERRIDE_NEW________________ALLOCATOR;
        return SA_STACK_ALLOCATOR__SA_OVERRIDE_NEW________________ALLOCATOR;
    }
} SA_STACK_ALLOCATOR__SA_OVERRIDE_NEW________________ALLOCATOR_WRAPPER;

void* alloc(size_t size) {
    SA_STACK_ALLOCATOR__SA_OVERRIDE_NEW________________MUTEX.lock();
    void * ptr;
    while (true) {
        ptr = SA_STACK_ALLOCATOR__SA_OVERRIDE_NEW________________ALLOCATOR_WRAPPER.instance().allocWithSizeWithVerboseAllocationTracking(size);
        // ptr = std::malloc(size);
        if (ptr == nullptr) {
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
    SA_STACK_ALLOCATOR__SA_OVERRIDE_NEW________________MUTEX.unlock();
    return ptr;
}

void dealloc(void* ptr) {
    SA_STACK_ALLOCATOR__SA_OVERRIDE_NEW________________MUTEX.lock();
    SA_STACK_ALLOCATOR__SA_OVERRIDE_NEW________________ALLOCATOR_WRAPPER.instance().dealloc(ptr);
    // std::free(ptr);
    SA_STACK_ALLOCATOR__SA_OVERRIDE_NEW________________MUTEX.unlock();
}

void *operator new(size_t size) {
    return alloc(size);
}

#ifdef __cpp_aligned_new
void *operator new(size_t size, std::align_val_t al) {
    return alloc(size);
}
#endif

void *operator new[](size_t size) {
    return alloc(size);
}

#ifdef __cpp_aligned_new
void *operator new[](std::size_t size, std::align_val_t al) {
    return alloc(size);
}
#endif

void operator delete(void *ptr) {
    return dealloc(ptr);
}

void operator delete[](void *ptr) {
    return dealloc(ptr);
}

#ifdef __cpp_aligned_new
void operator delete(void *ptr, std::align_val_t al) {
    return dealloc(ptr);
}

void operator delete[](void *ptr, std::align_val_t al) {
    return dealloc(ptr);
}
#endif

#endif

#ifndef SA_STACK_ALLOCATOR
#define SA_STACK_ALLOCATOR

#include "log.h"
#include "hexdump.h"
#include <forward_list>
#include <mutex>
#include <new>
#include <stdlib.h>
#include <limits>

namespace SA {

    // https://stackoverflow.com/a/11417774
    template <class T>
    struct MallocAllocator {
        typedef size_t size_type;
        typedef ptrdiff_t difference_type;
        typedef T* pointer;
        typedef const T* const_pointer;
        typedef T& reference;
        typedef const T& const_reference;
        typedef T value_type;

        template <class U> struct rebind { typedef MallocAllocator<U> other; };
        MallocAllocator() throw() {}
        MallocAllocator(const MallocAllocator&) throw() {}

        template <class U> MallocAllocator(const MallocAllocator<U>&) throw(){}

        ~MallocAllocator() throw() {}

        pointer address(reference x) const { return &x; }
        const_pointer address(const_reference x) const { return &x; }

        pointer allocate(size_type s, void const * = 0) {
            if (0 == s)
                return NULL;
            pointer temp = (pointer)malloc(s * sizeof(T)); 
            if (temp == NULL)
                throw std::bad_alloc();
            return temp;
        }

        void deallocate(pointer p, size_type) {
            free(p);
        }

        size_type max_size() const throw() { 
            return std::numeric_limits<size_t>::max() / sizeof(T); 
        }

        void construct(pointer p, const T& val) {
            new((void *)p) T(val);
        }

        void destroy(pointer p) {
            p->~T();
        }
    };

    template<typename T = uint8_t, class Allocator = std::allocator<T>>
    struct Pointer {
        Allocator allocator;
        mutable T * ptr;
        mutable size_t size_;
        mutable std::function<void(void*, size_t)> destructor;
        bool log_memory = false;
        bool log_contents = false;
        Pointer() : Pointer(false, false, 0) {}
        Pointer(size_t size, std::function<void(void*, size_t)> destructor = [](void*ptr, size_t) {}) : Pointer(false, false, size, destructor) {}
        Pointer(bool log_contents, bool log_memory, size_t size, std::function<void(void*, size_t)> destructor = [](void*ptr, size_t) {}) : size_(size), log_memory(log_memory), log_contents(log_contents) {
            if (size_ != 0) {
                ptr = size == 0 ? nullptr : allocator.allocate(size);
                this->destructor = destructor;
                if (log_memory) {
                    Logib();
                    printf("allocating pointer: %p with size %zu\n", ptr, size);
                    Logr();
                }
            } else {
                ptr = nullptr;
            }
        }

        void moveData(const Pointer<T> * from, Pointer<T> * to) {
            Logib();
            printf("moveData\n");
            Logr();
            to->log_memory = from->log_memory;
            to->log_contents = from->log_contents;
            if (size_ != 0) {
                if (from->log_memory) {
                    Logib();
                    printf("moving ownership of pointer: %p with size %zu\n", from->ptr, from->size_);
                    Logr();
                }
                to->ptr = from->ptr;
                to->size_ = from->size_;
                to->destructor = from->destructor;
                from->ptr = nullptr;
                from->size_ = 0;
                from->destructor = [](void*ptr, size_t) {};
            }
        }

        Pointer(const Pointer&copy) {
            moveData(&copy, this);
        }
        Pointer(const Pointer&&move) {
            moveData(&move, this);
        }
        Pointer&operator=(const Pointer&copy) {
            moveData(&copy, this);
            return *this;
        }
        const Pointer&operator=(const Pointer&copy) const {
            moveData(&copy, this);
            return *this;
        }
        Pointer&operator=(const Pointer&&move) {
            moveData(&move, this);
            return *this;
        }
        const Pointer&operator=(const Pointer&&move) const {
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
                Logw(CustomHexdump<16, true, uint8_t>("ptr: ", ptr, length));
            }
        }
        virtual ~Pointer() {
            if (size_ != 0) {
                if (log_memory) {
                    Logib();
                    printf("deallocating pointer: %p with size %zu\n", ptr, size_);
                    Logr();
                    if (log_contents) {
                        Logib();
                        printf("logging contents\n");
                        Logr();
                        print(size_);
                    }
                }
                destructor(ptr, size_);
                allocator.deallocate(ptr, size_);
            }
            ptr = nullptr;
            size_ = 0;
        }
    };

    using DefaultPointer = Pointer<uint8_t, std::allocator<uint8_t>>;
    using DefaultMallocPointer = Pointer<uint8_t, MallocAllocator<uint8_t>>;
    
    template <typename POINTER_T = uint8_t, class POINTER_Allocator = std::allocator<uint8_t>, class Allocator = std::allocator<Pointer<POINTER_T, POINTER_Allocator>>>
    struct PointerList {
        std::forward_list<Pointer<POINTER_T, POINTER_Allocator>, Allocator> pointers;
        size_t pointer_count = 0;
        
        void moveData(const PointerList<POINTER_T, POINTER_Allocator, Allocator> * from, PointerList<POINTER_T, POINTER_Allocator, Allocator> * to) {
            Logib();
            printf("moveData\n");
            Logr();
            to->pointers = from->pointers;
            to->pointer_count = from->pointer_count;
        }

        PointerList() = default;
        
        PointerList(const PointerList&copy) {
            moveData(&copy, this);
        }
        PointerList(const PointerList&&move) {
            moveData(&move, this);
        }
        PointerList&operator=(const PointerList&copy) {
            moveData(&copy, this);
            return *this;
        }
        const PointerList&operator=(const PointerList&copy) const {
            moveData(&copy, this);
            return *this;
        }
        PointerList&operator=(const PointerList&&move) {
            moveData(&move, this);
            return *this;
        }
        const PointerList&operator=(const PointerList&&move) const {
            moveData(&move, this);
            return *this;
        }

        void* add(size_t size, std::function<void(void*, size_t)> destructor = [](void*ptr, size_t) {}) {
            return add(false, false, size, destructor);
        }
        void* add(bool log_contents, bool log_memory, size_t size, std::function<void(void*, size_t)> destructor = [](void*ptr, size_t) {}) {
            pointers.emplace_front(log_contents, log_memory, size, destructor);
            pointer_count++;
            return pointers.front().data();
        }

        // calling this more than once with the same ptr, or a pointer not obtained by this instance, can lead to UB due to address recycling
        // calling with nullptr does nothing
        void remove(void* ptr) {
            if (ptr == nullptr) {
                return;
            }

            if (pointer_count == 0) {
                Logwb();
                printf("cannot remove any more pointers, pointer count is zero\n");
                Logr();
            }

            size_t items_removed = 0;

            pointers.remove_if([&](Pointer<POINTER_T, POINTER_Allocator> & pointer) {
                if (pointer.data() == ptr) {
                    items_removed++;
                    return true;
                }
                return false;
            });

            if (items_removed == 0) {
                Logeb();
                printf("pointer could not be found: %p\n", ptr);
                Logr();
                return;
            }

            Logib();
            printf("removing %zu pointers\n", items_removed);
            Logr();
            pointer_count -= items_removed;
        }

        ~PointerList() {
            Logib();
            printf("deallocating %zu pointers\n", pointer_count);
            Logr();
            pointer_count = 0;
        }
    };
    
    using DefaultPointerList = PointerList<uint8_t, std::allocator<uint8_t>, std::allocator<Pointer<uint8_t, std::allocator<uint8_t>>>>;
    using DefaultMallocPointerList = PointerList<uint8_t, MallocAllocator<uint8_t>, MallocAllocator<Pointer<uint8_t, MallocAllocator<uint8_t>>>>;

    class AllocatorBase {};
    
    template <typename PointerList = DefaultPointerList>
    struct Allocator : AllocatorBase {
        PointerList list;

        Allocator() = default;
        
        void moveData(const Allocator<PointerList> * from, Allocator<PointerList> * to) {
            Logib();
            printf("moveData\n");
            Logr();
            to->list = from->list;
        }
        
        Allocator(const Allocator&copy) {
            moveData(&copy, this);
        }
        Allocator(const Allocator&&move) {
            moveData(&move, this);
        }
        Allocator&operator=(const Allocator&copy) {
            moveData(&copy, this);
            return *this;
        }
        const Allocator&operator=(const Allocator&copy) const {
            moveData(&copy, this);
            return *this;
        }
        Allocator&operator=(const Allocator&&move) {
            moveData(&move, this);
            return *this;
        }
        const Allocator&operator=(const Allocator&&move) const {
            moveData(&move, this);
            return *this;
        }

        void dealloc(void* ptr) {
            list.remove(ptr);
        }

        template <typename T, typename ... Args>
        T* alloc(Args && ... args) {
            size_t s = sizeof(T);
            if (s != 0) {
                T * p = reinterpret_cast<T*>(list.add(false, false, s, [](void*ptr, size_t unused) {
                    ((T*)ptr)->~T();
                }));
                new(p) T(args...);
                return p;
            }
            return nullptr;
        }

        void* allocWithSize(size_t size) {
            if (size != 0) {
                return list.add(false, false, size);
            }
            return nullptr;
        }
    };

    template <typename PointerList = DefaultPointerList>
    struct AllocatorWithMemUsage : AllocatorBase {
        PointerList list;
        size_t memory_usage = 0;
        std::mutex m;

        AllocatorWithMemUsage() = default;

        ~AllocatorWithMemUsage() {
            Logib();
            printf("deallocating %zu bytes of memory\n", memory_usage);
            Logr();
        }

        void moveData(const AllocatorWithMemUsage<PointerList> * from, AllocatorWithMemUsage<PointerList> * to) {
            Logib();
            printf("moveData\n");
            Logr();
            to->list = from->list;
            to->memory_usage = from->memory_usage;
            to->m = from->m;
        }
        
        AllocatorWithMemUsage(const AllocatorWithMemUsage&copy) {
            moveData(&copy, this);
        }
        AllocatorWithMemUsage(const AllocatorWithMemUsage&&move) {
            moveData(&move, this);
        }
        AllocatorWithMemUsage&operator=(const AllocatorWithMemUsage&copy) {
            moveData(&copy, this);
            return *this;
        }
        const AllocatorWithMemUsage&operator=(const AllocatorWithMemUsage&copy) const {
            moveData(&copy, this);
            return *this;
        }
        AllocatorWithMemUsage&operator=(const AllocatorWithMemUsage&&move) {
            moveData(&move, this);
            return *this;
        }
        const AllocatorWithMemUsage&operator=(const AllocatorWithMemUsage&&move) const {
            moveData(&move, this);
            return *this;
        }

        void dealloc(void* ptr) {
            list.remove(ptr);
        }
        
        template <typename T, typename ... Args>
        T* alloc(Args && ... args) {
            size_t s = sizeof(T);
            if (s != 0) {
                T * p = reinterpret_cast<T*>(list.add(false, false, s, [&](void*ptr, size_t s) {
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
        T* allocWithVerboseAllocationTracking(Args && ... args) {
            size_t s = sizeof(T);
            if (s != 0) {
                T * p = reinterpret_cast<T*>(list.add(false, true, s, [&](void*ptr, size_t s) {
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
        T* allocWithVerboseAllocationTrackingAndVerboseContents(Args && ... args) {
            size_t s = sizeof(T);
            if (s != 0) {
                T * p = reinterpret_cast<T*>(list.add(true, true, s, [&](void*ptr, size_t s) {
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

        void* allocWithSize(size_t size) {
            if (size != 0) {
                m.lock();
                memory_usage += size;
                m.unlock();
                return list.add(false, false, size, [&](void*ptr, size_t s) {
                    m.lock();
                    memory_usage -= s;
                    m.unlock();
                });
            }
            return nullptr;
        }
        
        void* allocWithSizeWithVerboseAllocationTracking(size_t size) {
            if (size != 0) {
                m.lock();
                memory_usage += size;
                m.unlock();
                return list.add(false, true, size, [&](void*ptr, size_t s) {
                    m.lock();
                    memory_usage -= s;
                    m.unlock();
                });
            }
            return nullptr;
        }

        void* allocWithSizeWithVerboseAllocationTrackingAndVerboseContents(size_t size) {
            if (size != 0) {
                m.lock();
                memory_usage += size;
                m.unlock();
                return list.add(true, true, size, [&](void*ptr, size_t s) {
                    m.lock();
                    memory_usage -= s;
                    m.unlock();
                });
            }
            return nullptr;
        }
    };

    using DefaultAllocator = Allocator<DefaultPointerList>;
    using DefaultAllocatorWithMemUsage = AllocatorWithMemUsage<DefaultPointerList>;

    using DefaultMallocAllocator = Allocator<DefaultMallocPointerList>;
    using DefaultMallocAllocatorWithMemUsage = AllocatorWithMemUsage<DefaultMallocPointerList>;
}

#endif