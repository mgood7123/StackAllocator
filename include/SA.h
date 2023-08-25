#ifndef SA_STACK_ALLOCATOR
#define SA_STACK_ALLOCATOR

#include "log.h"
#include <vector>
#include <mutex>
#include <new>
#include <stdlib.h>
#include <limits>
#include "hexdump.h"

#ifndef RTTI_ENABLED
    #if defined(__clang__)
        #if __has_feature(cxx_rtti)
            #define RTTI_ENABLED
        #endif
    #elif defined(__GNUG__)
        #if defined(__GXX_RTTI)
            #define RTTI_ENABLED
        #endif
    #elif defined(_MSC_VER)
        #if defined(_CPPRTTI)
            #define RTTI_ENABLED
        #endif
    #endif
#endif

#ifdef RTTI_ENABLED
    #include <cstring>
    #if defined(__clang__)
        #include <cxxabi.h>
    #elif defined(__GNUC__)
        #include <cxxabi.h>
    #elif defined(_MSC_VER)
    #else
        #error Unsupported compiler
    #endif
#endif

#define SA____STACK_ALLOCATOR__REF_ONLY(C, CT) C() { if (log) { Logeb(); printf("%s()\n", #C); Logr(); } }; C(const CT & other) = delete; C(CT && other) = delete; CT & operator=(const CT & other) = delete; CT & operator=(CT && other) = delete
#define SA____STACK_ALLOCATOR__REF_ONLY_NO_DEFAULT_CONSTRUCTOR(C, CT) C(const CT & other) = delete; C(CT && other) = delete; CT & operator=(const CT & other) = delete; CT & operator=(CT && other) = delete

namespace SA {
    bool log = false;

    struct SINGLETONS {
        std::recursive_mutex mutex;
        size_t memory_usage = 0;

        template <typename T>
        struct PER_TYPE {
            char * demangled = nullptr;
            bool is_demangled = false;
            size_t memory_usage = 0;

            SA____STACK_ALLOCATOR__REF_ONLY_NO_DEFAULT_CONSTRUCTOR(PER_TYPE, PER_TYPE<T>);

            PER_TYPE() {
#ifdef RTTI_ENABLED
            if (!is_demangled) {
                    auto & ti = typeid(T);
#if defined(__clang__)
                    int status = -1;
                    auto tmp = abi::__cxa_demangle(ti.name(), NULL, NULL, &status);
                    demangled = strdup(tmp);
                    std::free(tmp);
#elif defined(__GNUC__)
                    int status = -1;
                    auto tmp = abi::__cxa_demangle(ti.name(), NULL, NULL, &status);
                    demangled = strdup(tmp);
                    std::free(tmp);
#elif defined(_MSC_VER)
                    demangled = dup(it.name());
#else
                    #error Unsupported compiler
#endif
                    is_demangled = true;
                }
#else
                demangled = "unknown type (RTTI NOT AVAILABLE)";
#endif
                if (log) {
                    Logeb();
                    printf("PER_TYPE<%s>()\n", demangled);
                    Logr();
                }
            }
            
            ~PER_TYPE() {
                if (log) {
                    Logeb();
                    printf("~PER_TYPE<%s>()\n", demangled);
                    Logr();
                }
                std::free(demangled);
            }
        };

        struct LL {
            SA____STACK_ALLOCATOR__REF_ONLY(LL, LL);
            struct ID {
                const std::type_info & info;
                void * data = nullptr;
                std::function<void(void*)> destructor;
                ID(const std::type_info & info, void * data, std::function<void(void*)> destructor) : info(info), data(data), destructor(destructor) {}
            };

            ID * value = nullptr;
            LL* next = nullptr;
            
            template <typename T>
            PER_TYPE<T> & get_per_type() {
                LL * t = this;
                LOOP:
                if (t->value == nullptr) {
                    t->value = static_cast<ID*>(calloc(sizeof(ID), 1));
                    new(t->value) ID(typeid(T), calloc(sizeof(PER_TYPE<T>), 1), [](void*data){static_cast<PER_TYPE<T>*>(data)->~PER_TYPE<T>(); free(data);});
                    new(t->value->data) PER_TYPE<T>();
                    return *static_cast<PER_TYPE<T>*>(t->value->data);
                } else if (t->value->info == typeid(T)) {
                    return *static_cast<PER_TYPE<T>*>(t->value->data);
                } else {
                    if (t->next == nullptr) {
                        t->next = static_cast<LL*>(calloc(sizeof(LL), 1));
                        new(t->next) LL();
                    }
                    t = t->next;
                    goto LOOP;
                }
            }
            ~LL() {
                if (log) {
                    Logeb();
                    printf("~LL()\n");
                    Logr();
                }
                LL * t = this;
                LOOP:
                if (t->value != nullptr) {
                    t->value->destructor(t->value->data);
                    t->value->~ID();
                    free(t->value);
                    t->value = nullptr;
                }
                if (t->next != nullptr) {
                    LL * current = t;
                    LL * next = t->next;
                    if (current != this) {
                        current->~LL();
                        free(current);
                        t = next;
                    } else {
                        t->value = next->value;
                        t->next = next->next;
                        next->value = nullptr;
                        next->next = nullptr;
                        next->~LL();
                        free(next);
                    }
                    goto LOOP;
                }
            }
        };

        LL per_type_linked_list;

        template <typename T>
        PER_TYPE<T> & per_type() {
            return per_type_linked_list.get_per_type<T>();
        }

        struct LLP {
            SA____STACK_ALLOCATOR__REF_ONLY(LLP, LLP);
            void * value = nullptr;
            LLP* next = nullptr;
            size_t size = 0;
            
            void add_pointer(void * p) {
                LLP * t = this;
                LOOP:
                if (t->value == nullptr) {
                    t->value = p;
                    size++;
                } else {
                    if (t->next == nullptr) {
                        t->next = static_cast<LLP*>(calloc(sizeof(LLP), 1));
                        new(t->next) LLP();
                    }
                    t = t->next;
                    goto LOOP;
                }
            }

            bool remove_pointer(void * p) {
                LLP * t = this;
                LOOP:
                if (log) {
                    Logeb();
                    printf("REMOVE: comparing tracked pointer %p with wanted pointer %p\n", t->value, p);
                    Logr();
                }
                if (t->value == p) {
                    t->value = nullptr;
                    if (t->next != nullptr) {
                        LLP * current = t;
                        LLP * next = t->next;
                        if (current != this) {
                            current->~LLP();
                            free(current);
                            t = next;
                        } else {
                            t->value = next->value;
                            t->next = next->next;
                            next->value = nullptr;
                            next->next = nullptr;
                            next->~LLP();
                            free(next);
                        }
                    }
                    size--;
                    return true;
                } else {
                    if (t->next == nullptr) {
                        return false;
                    }
                    t = t->next;
                    goto LOOP;
                }
            }

            void ** find_pointer(void * p) {
                LLP * t = this;
                LOOP:
                if (log) {
                    Logeb();
                    printf("FIND: comparing tracked pointer %p with wanted pointer %p\n", t->value, p);
                    Logr();
                }
                if (t->value == p) {
                    return &t->value;
                } else {
                    if (t->next == nullptr) {
                        return nullptr;
                    }
                    t = t->next;
                    goto LOOP;
                }
            }

            ~LLP() {
                if (log) {
                    Logeb();
                    printf("~LLP()\n");
                    Logr();
                }
                LLP * t = this;
                LOOP:
                if (t->value != nullptr) {
                    t->value = nullptr;
                }
                if (t->next != nullptr) {
                    LLP * current = t;
                    LLP * next = t->next;
                    if (current != this) {
                        current->~LLP();
                        free(current);
                        t = next;
                    } else {
                        t->value = next->value;
                        t->next = next->next;
                        next->value = nullptr;
                        next->next = nullptr;
                        next->~LLP();
                        free(next);
                    }
                    goto LOOP;
                }
                size = 0;
            }
        };

        LLP pointers;

        struct PointerInfo {
            void * pointer = nullptr;
            bool adopted = false;
            std::size_t count = 0;
            std::function<void(void*)> t_destructor;
            std::function<void(PointerInfo&)> destructor;
            LLP refs;
            SA____STACK_ALLOCATOR__REF_ONLY(PointerInfo, PointerInfo);
            ~PointerInfo() {
                if (log) {
                    Logeb();
                    printf("~PointerInfo()\n");
                    Logr();
                }
            }
        };

        struct LLPI {
            SA____STACK_ALLOCATOR__REF_ONLY(LLPI, LLPI);
            PointerInfo * value = nullptr;
            LLPI* next = nullptr;
            size_t size = 0;
            
            PointerInfo & ref(void * ptr, void * owner) {
                LLPI * t = this;
                LOOP:
                if (t->value == nullptr) {
                    t->value = static_cast<PointerInfo*>(calloc(sizeof(PointerInfo), 1));
                    new(t->value) PointerInfo();
                    t->value->pointer = ptr;
                    t->value->refs.add_pointer(owner);
                    size++;
                    return *t->value;
                } else if (t->value->pointer == ptr) {
                    t->value->refs.add_pointer(owner);
                    return *t->value;
                } else {
                    if (t->next == nullptr) {
                        t->next = static_cast<LLPI*>(calloc(sizeof(LLPI), 1));
                        new(t->next) LLPI();
                    }
                    t = t->next;
                    goto LOOP;
                }
            }

            bool release(void * ptr) {
                LLPI * t = this;
                LOOP:
                if (t->value != nullptr && t->value->pointer == ptr) {
                    t->value->~PointerInfo();
                    free(t->value);
                    t->value = nullptr;
                    if (t->next != nullptr) {
                        LLPI * current = t;
                        LLPI * next = t->next;
                        if (current != this) {
                            current->~LLPI();
                            free(current);
                            t = next;
                        } else {
                            t->value = next->value;
                            t->next = next->next;
                            next->value = nullptr;
                            next->next = nullptr;
                            next->~LLPI();
                            free(next);
                        }
                    }
                    size--;
                    return true;
                } else {
                    if (t->next == nullptr) {
                        return false;
                    }
                    t = t->next;
                    goto LOOP;
                }
            }

            void unref(void * ptr, void * owner, std::function<bool(void*,void*)> pred) {
                LLPI * t = this;
                LOOP:
                if (t->value != nullptr && pred(t->value->pointer, ptr)) {
                    if (t->value->refs.find_pointer(owner) != nullptr) {
                        if (t->value->refs.size == 1) {
                            t->value->destructor(*t->value);
                            t->value->refs.remove_pointer(owner);
                            t->value->~PointerInfo();
                            free(t->value);
                            t->value = nullptr;
                            if (t->next != nullptr) {
                                LLPI * current = t;
                                LLPI * next = t->next;
                                if (current != this) {
                                    current->~LLPI();
                                    free(current);
                                    t = next;
                                } else {
                                    t->value = next->value;
                                    t->next = next->next;
                                    next->value = nullptr;
                                    next->next = nullptr;
                                    next->~LLPI();
                                    free(next);
                                }
                            }
                            size--;
                        } else {
                            t->value->refs.remove_pointer(owner);
                        }
                    }
                } else if (t->next != nullptr) {
                    t = t->next;
                    goto LOOP;
                }
            }

            ~LLPI() {
                if (log) {
                    Logeb();
                    printf("~LLPI()\n");
                    Logr();
                }
                LLPI * t = this;
                LOOP:
                if (t->value != nullptr) {
                    t->value->destructor(*t->value);
                    t->value->~PointerInfo();
                    free(t->value);
                    t->value = nullptr;
                }
                if (t->next != nullptr) {
                    LLPI * current = t;
                    LLPI * next = t->next;
                    if (current != this) {
                        current->~LLPI();
                        free(current);
                        t = next;
                    } else {
                        t->value = next->value;
                        t->next = next->next;
                        next->value = nullptr;
                        next->next = nullptr;
                        next->~LLPI();
                        free(next);
                    }
                    goto LOOP;
                }
                size = 0;
            }
        };

        LLPI tracked_pointers;

        SA____STACK_ALLOCATOR__REF_ONLY_NO_DEFAULT_CONSTRUCTOR(SINGLETONS, SINGLETONS);

        SINGLETONS() {
            if (log) {
                Logeb();
                printf("SINGLETONS()\n");
                Logr();
            }
        }
        ~SINGLETONS() {
            if (log) {
                Logeb();
                printf("~SINGLETONS()\n");
                Logr();
            }
        }
    } singletons;

    class save_cout {
        std::ostream & s;
        std::ios_base::fmtflags f;

        public:

        save_cout() : save_cout(std::cout) {}
        save_cout(std::ostream & o) : s(o), f(o.flags()) {}
        template <typename T> std::ostream & operator<<(const T & item) { return s << item; }
        std::ostream & operator*() { return s; }
        std::ostream * operator->() { return &s; }
        ~save_cout() { s.setf(f); }
    };

    template<typename T>
    struct Mallocator
    {
        typedef T value_type;
    
        Mallocator () = default;

        template<class U>
        constexpr Mallocator (const Mallocator <U>&) noexcept {};

        template<class U>
        constexpr Mallocator<T> & operator= (const Mallocator <U>&) noexcept {
            return *this;
        };

        template<class U>
        constexpr bool operator== (const Mallocator <U>&) noexcept { return true; }

        template<class U>
        constexpr bool operator!= (const Mallocator <U>&) noexcept { return false; }

        virtual ~Mallocator() {}

        virtual void onAlloc(T * p, size_t n) {}
        virtual bool onDealloc(T * p, size_t n) { return true; }

        [[nodiscard]] T* allocate(std::size_t n)
        {
            if (n > std::numeric_limits<std::size_t>::max() / sizeof(T))
                throw std::bad_array_new_length();
            
            singletons.mutex.lock();
            void * ptr;
            while (true) {
                // calloc initializes memory and stops valgrind complaining about uninitialized memory use
                ptr = std::calloc(n, sizeof(T));
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
            singletons.memory_usage += sizeof(T)*n;
            singletons.per_type<T>().memory_usage += sizeof(T)*n;
            if (log) {
                Logib();
                printf("calloc returned %p\n", ptr);
                Logr();
                Logib();
                printf("allocated %zu bytes of memory, total memory usage for '%s': %zu bytes. total memory usage: %zu bytes\n", sizeof(T)*n, singletons.per_type<T>().demangled, singletons.per_type<T>().memory_usage, singletons.memory_usage);
                Logr();
            }
            onAlloc(static_cast<T*>(ptr), sizeof(T)*n);
            singletons.mutex.unlock();
            return static_cast<T*>(ptr);
        }
    
        void secure_free(T* p, std::size_t n) noexcept
        {
            // the compiler is not allowed to optimize out functions that use volatile pointers
            volatile uint8_t* s = reinterpret_cast<uint8_t*>(p);
            volatile uint8_t* e = s + (sizeof(T)*n);
            std::fill(s, e, 0);
            std::free(p);
            singletons.memory_usage -= sizeof(T)*n;
            singletons.per_type<T>().memory_usage -= sizeof(T)*n;
        }

        void deallocate(T* p, std::size_t n) noexcept
        {
            if (log) {
                Logib();
                printf("deallocate called with p = %p, n = %zu\n", p, n);
                Logr();
            }
            if (p == nullptr) {
                return;
            }
            singletons.mutex.lock();
            if (onDealloc(p, sizeof(T)*n)) {
                if (log) {
                    Logib();
                    printf("deallocating %zu bytes of memory, total memory usage for '%s': %zu bytes. total memory usage: %zu bytes\n", sizeof(T)*n, singletons.per_type<T>().demangled, singletons.per_type<T>().memory_usage, singletons.memory_usage);
                    Logr();
                    Logib();
                    printf("logging contents\n");
                    Logr();
                    Logw(CustomHexdump<16, true, uint8_t>("ptr: ", p, sizeof(T)*n));
                }
                secure_free(p, n);
            } else {
                Logeb();
                printf("error: pointer %p could not be found in the list of allocated pointers, ignoring\n", p);
                Logr();
            }
            singletons.mutex.unlock();
        }
    };

    struct PTR {
        std::vector<void*, Mallocator<void*>> pointers;
        static std::vector<void*, Mallocator<void*>> & instance()
        {
            static PTR ptr;
            Logeb();
            printf("PTR obtaining tracked pointers\n");
            Logr();
            return ptr.pointers;
        }
        PTR() {
            Logeb();
            printf("PTR() init tracked pointers\n");
            Logr();
        }
        ~PTR() {
            Logeb();
            printf("~PTR, removing %zu tracked pointers\n", pointers.size());
            Logr();
        }
    };

    template<typename T>
    struct TrackedMallocator : public Mallocator<T>
    {
        using Mallocator<T>::Mallocator;

        void onAlloc(T * p, std::size_t n) override {
            singletons.pointers.add_pointer(p);
        }
    
        bool onDealloc(T * p, std::size_t n) override {
            return singletons.pointers.remove_pointer(p);
        }
    };

    struct AllocatorBase {};

    class TrackedAllocator : AllocatorBase {

        public:

        TrackedAllocator() {}

        TrackedAllocator(const TrackedAllocator & other) = delete;
        TrackedAllocator & operator=(const TrackedAllocator & other) = delete;

        TrackedAllocator(TrackedAllocator && other) = default;
        TrackedAllocator & operator=(TrackedAllocator && other) = default;
        
        template <typename T>
        void adopt(T * ptr, std::function<void(void*)> destructor = [](void*p){ delete static_cast<T*>(p); }) {
            singletons.mutex.lock();
            auto & p = singletons.tracked_pointers.ref(ptr, this);
            if (p.refs.size == 1) {
                p.count = 1;
                p.adopted = true;
                p.t_destructor = destructor;
                p.destructor = [this](auto & p) {
                    if (p.pointer != nullptr) {
                        p.t_destructor(p.pointer);
                        p.pointer = nullptr;
                        p.adopted = false;
                        p.count = 0;
                    }
                };
            }
            singletons.mutex.unlock();
        }

        static void release(void * ptr) {
            singletons.mutex.lock();
            singletons.tracked_pointers.release(ptr);
            singletons.mutex.unlock();
        }

        template <typename T, typename ... Args>
        [[nodiscard]] T* alloc(Args && ... args) {
            T * ptr = alloc_internal<T>(1, [] (void * ptr) { static_cast<T*>(ptr)->~T(); });
            new (ptr) T(std::forward<Args>(args)...);
            return ptr;
        }

        template <typename T>
        [[nodiscard]] T* allocArray(size_t count) {
            T * ptr = alloc_internal<T>(count, [] (void * ptr) {});
            new (ptr) T[count];
            return ptr;
        }

        [[nodiscard]] void * alloc(std::size_t s) {
            return alloc_internal<uint8_t>(s, [] (void * ptr) {});
        }

        void dealloc(void* ptr) {
            if (ptr == nullptr) {
                return;
            }
            internal_dealloc(ptr, [] (void * a, void * b) { return a == b; } );
        }

        void dealloc_all() {
            internal_dealloc(nullptr, [] (void * a, void * b) { return a != nullptr; } );
        }

        virtual ~TrackedAllocator() {
            dealloc_all();
        }

        protected:

        virtual void onAlloc(void * p, std::size_t n) {}
        virtual void onDealloc(void * p, std::size_t n) {}

        private:

        template <typename T>
        [[nodiscard]] T * alloc_internal(std::size_t count, std::function<void(void*)> destructor) {
            T * ptr = TrackedMallocator<T>().allocate(count);
            singletons.mutex.lock();
            auto & p = singletons.tracked_pointers.ref(ptr, this);
            if (p.refs.size == 1) {
                onAlloc(p.pointer, sizeof(T)*p.count);
                p.count = 1;
                p.adopted = false;
                p.t_destructor = destructor;
                p.destructor = [this](auto & p) {
                    if (p.pointer != nullptr) {
                        //onDealloc(p.pointer, sizeof(T)*p.count);
                        p.t_destructor(p.pointer);
                        TrackedMallocator<T>().deallocate(static_cast<T*>(p.pointer), p.count);
                        p.pointer = nullptr;
                        p.adopted = false;
                        p.count = 0;
                    }
                };
            }
            singletons.mutex.unlock();
            return ptr;
        }

        void internal_dealloc(void * ptr, std::function<bool(void*,void*)> pred) {
            singletons.mutex.lock();
            singletons.tracked_pointers.unref(ptr, this, [&](void* a, void* b) { return pred(a, b); });
            singletons.mutex.unlock();
        }
    };

    class TrackedMutex {
        TrackedAllocator mutex_allocator;

        public:

        std::mutex* mutex = nullptr;

        TrackedMutex() {
            mutex = mutex_allocator.alloc<std::mutex>();
        }

        TrackedMutex(const TrackedMutex & other) = delete;
        TrackedMutex & operator=(const TrackedMutex & other) = delete;

        TrackedMutex(TrackedMutex && other) = default;
        TrackedMutex & operator=(TrackedMutex && other) = default;

        virtual ~TrackedMutex() = default;
    };

    class TrackedAllocatorWithMemUsage : public TrackedAllocator {
        size_t memory_usage = 0;
        TrackedMutex mutex;

        public:

        using TrackedAllocator::TrackedAllocator;

        TrackedAllocatorWithMemUsage(const TrackedAllocatorWithMemUsage & other) = delete;
        TrackedAllocatorWithMemUsage & operator=(const TrackedAllocatorWithMemUsage & other) = delete;

        TrackedAllocatorWithMemUsage(TrackedAllocatorWithMemUsage && other) = default;
        TrackedAllocatorWithMemUsage & operator=(TrackedAllocatorWithMemUsage && other) = default;

        ~TrackedAllocatorWithMemUsage() {
            auto n = memory_usage;
            Logib();
            printf("deallocating %zu bytes of memory\n", n);
            Logr();

            dealloc_all();

            Logib();
            printf("deallocated %zu bytes of memory\n", n);
            Logr();
        }

        protected:

        void onAlloc(void * p, std::size_t n) override {
            mutex.mutex->lock();
            memory_usage += n;
            mutex.mutex->unlock();
        }

        void onDealloc(void * p, std::size_t n) override {
            mutex.mutex->lock();
            memory_usage -= n;
            mutex.mutex->unlock();
        }
    };

    using Allocator = TrackedAllocator;
    using AllocatorWithMemUsage = TrackedAllocatorWithMemUsage;

    using DefaultAllocator = Allocator;
    using DefaultAllocatorWithMemUsage = AllocatorWithMemUsage;
}

#endif

#ifdef SA_STACK_ALLOCATOR__SA_OVERRIDE_NEW
#ifndef SA____STACK_ALLOCATOR__SA_OVERRIDE_NEW___
#define SA____STACK_ALLOCATOR__SA_OVERRIDE_NEW___

#include <new>

void *operator new(size_t size);
void *operator new[](size_t size);
void operator delete(void *ptr) noexcept;
void operator delete[](void *ptr) noexcept;
void operator delete(void *ptr, std::size_t sz) noexcept;
void operator delete[](void *ptr, std::size_t sz) noexcept;

#ifdef __cpp_aligned_new
extern void *operator new(size_t size, std::align_val_t al);
extern void *operator new[](std::size_t size, std::align_val_t al);
extern void operator delete(void *ptr, std::align_val_t al) noexcept;
extern void operator delete[](void *ptr, std::align_val_t al) noexcept;
extern void operator delete(void *ptr, std::size_t sz, std::align_val_t al) noexcept;
extern void operator delete[](void *ptr, std::size_t sz, std::align_val_t al) noexcept;
#endif

#include <memory>
#include <mutex>
#include <stdlib.h>
#include <limits>
#include <string.h>

struct {
    static SA::Allocator & instance()
    {
        static SA::Allocator SA_STACK_ALLOCATOR_ALLOCATOR;
        return SA_STACK_ALLOCATOR_ALLOCATOR;
    }

    static void* alloc(size_t size) {
        return instance().alloc(size);
    }

    static void dealloc(void* ptr) {
        instance().dealloc(ptr);
    }
} SA_STACK_ALLOCATOR_ALLOCATOR_WRAPPER;

void *operator new(size_t size) {
    return SA_STACK_ALLOCATOR_ALLOCATOR_WRAPPER.alloc(size);
}

void *operator new[](size_t size) {
    return SA_STACK_ALLOCATOR_ALLOCATOR_WRAPPER.alloc(size);
}

void operator delete(void *ptr) noexcept {
    return SA_STACK_ALLOCATOR_ALLOCATOR_WRAPPER.dealloc(ptr);
}

void operator delete[](void *ptr) noexcept {
    return SA_STACK_ALLOCATOR_ALLOCATOR_WRAPPER.dealloc(ptr);
}

void operator delete(void *ptr, std::size_t sz) noexcept {
    return SA_STACK_ALLOCATOR_ALLOCATOR_WRAPPER.dealloc(ptr);
}

void operator delete[](void *ptr, std::size_t sz) noexcept {
    return SA_STACK_ALLOCATOR_ALLOCATOR_WRAPPER.dealloc(ptr);
}

#ifdef __cpp_aligned_new
void *operator new(size_t size, std::align_val_t al) {
    return SA_STACK_ALLOCATOR_ALLOCATOR_WRAPPER.alloc(size);
}

void *operator new[](std::size_t size, std::align_val_t al) {
    return SA_STACK_ALLOCATOR_ALLOCATOR_WRAPPER.alloc(size);
}

void operator delete(void *ptr, std::align_val_t al) noexcept {
    return SA_STACK_ALLOCATOR_ALLOCATOR_WRAPPER.dealloc(ptr);
}

void operator delete[](void *ptr, std::align_val_t al) noexcept {
    return SA_STACK_ALLOCATOR_ALLOCATOR_WRAPPER.dealloc(ptr);
}

void operator delete(void *ptr, std::size_t sz, std::align_val_t al) noexcept {
    return SA_STACK_ALLOCATOR_ALLOCATOR_WRAPPER.dealloc(ptr);
}

void operator delete[](void *ptr, std::size_t sz, std::align_val_t al) noexcept {
    return SA_STACK_ALLOCATOR_ALLOCATOR_WRAPPER.dealloc(ptr);
}
#endif


#endif

#endif