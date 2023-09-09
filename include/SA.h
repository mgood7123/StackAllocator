#ifndef SA_STACK_ALLOCATOR
#define SA_STACK_ALLOCATOR

#include "log.h"
#include <vector>
#include <mutex>
#include <new>
#include <stdlib.h>
#include <limits>
#include "hexdump.h"
#include <cassert>

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
#define SA____STACK_ALLOCATOR__REF_ONLY_T(C, T) C() { if (log) { SA::SINGLETONS::PER_TYPE<T> t; Logeb(); printf("%s<%s>()\n", #C, t.demangled); Logr(); } }; C(const C<T> & other) = delete; C(C<T> && other) = delete; C<T> & operator=(const C<T> & other) = delete; C<T> & operator=(C<T> && other) = delete
#define SA____STACK_ALLOCATOR__REF_ONLY_NO_DEFAULT_CONSTRUCTOR(C, CT) C(const CT & other) = delete; C(CT && other) = delete; CT & operator=(const CT & other) = delete; CT & operator=(CT && other) = delete

namespace SA {

    struct AllocatorBase {};

    template <typename T> struct Mallocator;

    struct TrackedAllocator;

    extern bool log;

    struct SINGLETONS;
    extern SINGLETONS & GET_SINGLETONS();

    extern TrackedAllocator * GET_GLOBAL();
    extern bool IS_GLOBAL(AllocatorBase * allocator);

    struct SINGLETONS {
        class recursive_mutex {
            size_t lock_count_ = 0;
            std::recursive_mutex mutex;

            public:

            class Scoped {
                recursive_mutex * scope;
                
                public:

                Scoped(recursive_mutex * s) : scope(s) {
                    scope->lock();
                }
                
                Scoped(const Scoped & other) = delete;
                Scoped & operator=(const Scoped & other) = delete;

                Scoped(Scoped && other) = default;
                Scoped & operator=(Scoped && other) = default;

                ~Scoped() {
                    scope->unlock();
                }
            };

            Scoped scoped() { return {this}; }

            size_t lock_count() { return lock_count_; }

            void lock() { mutex.lock(); lock_count_++; }
            void unlock() { lock_count_--; mutex.unlock(); }
            bool try_lock () {
                if (mutex.try_lock()) {
                    lock_count_++;
                    return true;
                }
                return false;
            }
        };

        recursive_mutex mutex;

        size_t memory_usage = 0;
        Mallocator<uint8_t> mallocator;

        static void * inspect_calloc_return_value(void * return_value) {
            if (log) {
                Logib();
                printf("CALLOC(%p)\n", return_value);
                Logr();
            }
            return return_value;
        }

        static void * inspect_calloc(size_t memb, size_t size) {
            return inspect_calloc_return_value(calloc(memb, size));
        }

        static void inspect_free(void * ptr) {
            if (log) {
                Logib();
                printf("FREE(%p)\n", ptr);
                Logr();
            }
            free(ptr);
        }

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
                    inspect_free(tmp);
#elif defined(__GNUC__)
                    int status = -1;
                    auto tmp = abi::__cxa_demangle(ti.name(), NULL, NULL, &status);
                    demangled = strdup(tmp);
                    inspect_free(tmp);
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
                inspect_free(demangled);
            }
        };

        template <typename T, typename ... Args>
        [[nodiscard]] static T* alloc(Args && ... args) {
            T * ptr;
            ptr = static_cast<T*>(inspect_calloc(1, sizeof(T)));
            new (ptr) T(std::forward<Args>(args)...);
            return ptr;
        }

        template <typename T>
        static void dealloc(T ** ptr) {
            if (*ptr != nullptr) {
                (*ptr)->~T();
                inspect_free(*ptr);
                *ptr = nullptr;
            }
        }

        template <typename T>
        struct SA__LinkedList {

            SA____STACK_ALLOCATOR__REF_ONLY_T(SA__LinkedList, T);

            T * node = nullptr;
            SA__LinkedList * next = nullptr;
            SA__LinkedList * tail = nullptr;
            size_t size = 0;

            void verify_tail() {
                if (size == 0) {
                    assert(tail == nullptr);
                } else if (size == 1) {
                    assert(tail == this);
                } else {
                    assert(tail != nullptr);
                    assert(tail != this);
                    assert(tail->next == nullptr);
                }
            }

            std::function<T*()> onNodeAlloc = []() {
                return alloc<T>();
            };

            T* append_node() {
                verify_tail();
                if (tail == nullptr) {
                    tail = this;
                }
                if(tail->node != nullptr) {
                    tail->next = alloc<SA__LinkedList<T>>();
                    tail = tail->next;
                }
                tail->node = onNodeAlloc();
                size++;
                verify_tail();
                return tail->node;
            }

            T * find_or_add(std::function<bool(T*node)> pred) {
                if (size == 0) {
                    return append_node();
                } else {
                    verify_tail();
                    if (size == 1) {
                        if (pred(node)) {
                            return node;
                        } else {
                            assert(next == nullptr);
                            tail->next = alloc<SA__LinkedList<T>>();
                            tail = tail->next;
                            tail->node = onNodeAlloc();
                            size++;
                            verify_tail();
                            return tail->node;
                        }
                    }
                    auto current = this;
                    if (size > 1) {
                        Logwb();
                        printf("searching %zu nodes...\n", size);
                        Logr();
                    }
                    while(current->node != nullptr) {
                        if (pred(current->node)) {
                            verify_tail();
                            return current->node;
                        } else {
                            if (current->next == nullptr) {
                                assert(current == tail);
                                tail->next = alloc<SA__LinkedList<T>>();
                                tail = tail->next;
                            }
                            current = current->next;
                        }
                    }
                    current->node = onNodeAlloc();
                    size++;
                    verify_tail();
                    return current->node;
                }
            }

            void remove_node_from_start() {
                verify_tail();
                dealloc(&node);
                if (next != nullptr) {
                    SA__LinkedList<T> * old = next;
                    node = next->node;
                    next = next->next;
                    old->node = nullptr;
                    old->next = nullptr;
                    if (tail == old) tail = this;
                    dealloc(&old);
                } else {
                    // we have removed this node, this->node == nullptr && this->next == nullptr, tail is now nullptr
                    tail = nullptr;
                }
                size--;
                verify_tail();
            }

            struct FoundNode {
                SA__LinkedList<T> * before_found;
                SA__LinkedList<T> * found;
            };

            void remove_node(FoundNode f) {
                if (f.found == nullptr) {
                    Logeb();
                    printf("f.found is nullptr, this is an error\n");
                    Logr();
                } else {
                    if (f.found == this) {
                        remove_node_from_start();
                    } else {
                        verify_tail();
                        if (tail == f.found) tail = f.before_found;
                        SA__LinkedList<T> * node_to_link = f.found->next;
                        dealloc(&f.found->node);
                        dealloc(&f.found);
                        f.before_found->next = node_to_link;
                        size--;
                        verify_tail();
                    }
                }
            }


            FoundNode find_node(std::function<bool(T*node)> pred) {
                verify_tail();
                if (size == 0) return {nullptr, nullptr};
                if (size == 1) if (pred(node)) return {this, this};
                auto current = this;
                auto prev = current;
                if (size > 1) {
                    Logwb();
                    printf("searching %zu nodes...\n", size);
                    Logr();
                }
                while(current->node != nullptr) {
                    if (pred(current->node)) {
                        verify_tail();
                        return {prev, current};
                    } else {
                        if (current->next == nullptr) {
                            verify_tail();
                            return {nullptr, nullptr};
                        }
                        prev = current;
                        current = current->next;
                    }
                }
                verify_tail();
                return {nullptr, nullptr};
            }

            void warn_ptr(const char * tag, void * ptr) {
                verify_tail();
                if (ptr != nullptr) {
                    Logeb();
                    printf("%s: COULD NOT FIND TRACKED POINTER %p\n", tag, ptr);
                    Logr();
                    // abort();
                }
            }

            void remove_all() {
                verify_tail();
                while (size != 0) {
                    remove_node_from_start();
                }
            }

            virtual ~SA__LinkedList() {
                if (log) {
                    SA::SINGLETONS::PER_TYPE<T> t;
                    Logeb();
                    printf("~SA__LinkedList<%s>()\n", t.demangled);
                    Logr();
                }
                remove_all();
            }
        };

        struct ID {
            SA____STACK_ALLOCATOR__REF_ONLY(ID, ID);
            const std::type_info * info = nullptr;
            void * data = nullptr;
            std::function<void(void*)> destructor;
            ID(const std::type_info * info, void * data, std::function<void(void*)> destructor) : info(info), data(data), destructor(destructor) {
                if (log) {
                    Logeb();
                    printf("ID()\n");
                    Logr();
                }
            }
            ~ID() {
                if (log) {
                    Logeb();
                    printf("~ID()\n");
                    Logr();
                }
                destructor(data);
            }
        };

        struct ID_LL : private SA__LinkedList<ID> {
            using SA__LinkedList<ID>::size;
            SA____STACK_ALLOCATOR__REF_ONLY(ID_LL, ID_LL);

            template <typename T>
            PER_TYPE<T> & get_per_type() {
                onNodeAlloc = []() {
                    auto & type = typeid(T);
                    return alloc<ID>(&type, alloc<PER_TYPE<T>>(), [](void*data) {
                        PER_TYPE<T>* p = static_cast<PER_TYPE<T>*>(data);
                        dealloc(&p);
                    });
                };
                return *static_cast<PER_TYPE<T>*>(find_or_add([](ID*node){return *node->info == typeid(T);})->data);
            }

            ~ID_LL() {
                if (log) {
                    Logeb();
                    printf("~ID_LL()\n");
                    Logr();
                }
            }

            protected:
        };

        ID_LL per_type_linked_list;

        template <typename T>
        PER_TYPE<T> & per_type() {
            return per_type_linked_list.get_per_type<T>();
        }

        struct PTR_LL : private SA__LinkedList<void*> {
            using SA__LinkedList<void*>::size;
            SA____STACK_ALLOCATOR__REF_ONLY(PTR_LL, PTR_LL);

            public:

            void add_pointer(void * p) {
                append_node()[0] = p;
            }

            void ** find_or_add_pointer(std::function<bool(void**node)> pred) {
                bool f = false;
                void ** ptr = find_or_add([&](void**ptr) {
                    if (pred(ptr)) {
                        f = true;
                    }
                    return f;
                });
                return ptr;
            }

            void ** find_pointer(void * p) {
                return find_pointer(p, true);
            }

            void ** find_pointer(void * p, bool warn_not_found) {
                if (size == 0) {
                    if (warn_not_found) {
                        warn_ptr("FIND", p);
                    }
                    return nullptr;
                }
                auto l = find_node([&](void**ptr) {
                    return *ptr == p;
                });
                if (l.found != nullptr) {
                    if (log) {
                        Logeb();
                        printf("FIND: found tracked pointer %p with wanted pointer %p\n", *l.found->node, p);
                        Logr();
                    }
                    return l.found->node;
                } else {
                    if (warn_not_found) {
                        warn_ptr("FIND", p);
                    }
                }
                return nullptr;
            }

            bool remove_all_pointers_except(void * p) {
                if (size == 0) {
                    warn_ptr("REMOVE ALL EXCEPT", p);
                    return false;
                }
                bool f = false;
                LOOP:
                auto l = find_node([&](void**ptr) {
                    return *ptr != p;
                });
                if (l.found != nullptr) {
                    f = true;
                    if (log) {
                        Logeb();
                        printf("REMOVE ALL EXCEPT: found tracked pointer %p\n", *l.found->node);
                        Logr();
                    }
                    remove_node(l);
                    goto LOOP;
                }
                return f;
            }

            bool remove_pointer(void * p) {
                if (size == 0) {
                    warn_ptr("REMOVE", p);
                    return false;
                }
                auto l = find_node([&](void**ptr) {
                    return *ptr == p;
                });
                if (l.found != nullptr) {
                    if (log) {
                        Logeb();
                        printf("REMOVE: found tracked pointer %p with wanted pointer %p\n", *l.found->node, p);
                        Logr();
                    }
                    remove_node(l);
                    return true;
                }
                warn_ptr("REMOVE", p);
                return false;
            }
            
            ~PTR_LL() {
                auto s = 0;
                if (s != 0) {
                    if (s != 1) {
                        Logeb();
                        printf("~PTR_LL(), FREEING %zu TrackedMallocator/OwnerReference POINTERS\n", s);
                        Logr();
                    }
                    remove_all();
                    if (s != 1) {
                        Logeb();
                        printf("~PTR_LL(), ALL TrackedMallocator/OwnerReference POINTERS HAVE BEEN FREED\n");
                        Logr();
                    }
                }
            }
        };

        // these are used by TrackedMallocator and by the TrackedAllocator for storing PointerInfo owner references
        PTR_LL pointers;

        struct PointerInfo {
            SA____STACK_ALLOCATOR__REF_ONLY(PointerInfo, PointerInfo);
            void * pointer = nullptr;
            bool adopted = false;
            std::size_t count = 0;
            std::function<void(void*)> t_destructor;
            std::function<void(PointerInfo&)> destructor;
            PTR_LL refs;

            void release() {
                pointer = nullptr;
                adopted = false;
                count = 0;
            }

            virtual ~PointerInfo() {
                if (log) {
                    Logeb();
                    printf("~PointerInfo()\n");
                    Logr();
                }
                destructor(*this);
            }
        };

        struct PTRINFO_LL : private SA__LinkedList<PointerInfo> {
            using SA__LinkedList<PointerInfo>::size;
            SA____STACK_ALLOCATOR__REF_ONLY(PTRINFO_LL, PTRINFO_LL);

            PointerInfo & ref(void * ptr, void * owner) {
                bool f = false;
                PointerInfo * p = find_or_add([&](PointerInfo*p) {
                    if (p->pointer == ptr) {
                        f = true;
                    }
                    return f;
                });
                if (f) {
                    if (log) {
                        Logeb();
                        printf("REF: found tracked pointer %p with wanted pointer %p\n", p->pointer, ptr);
                        Logr();
                    }
                } else {
                    p->pointer = ptr;
                    if (log) {
                        Logeb();
                        printf("REF: added tracked pointer %p with wanted pointer %p\n", p->pointer, ptr);
                        Logr();
                    }
                }
                bool pf = false;
                void ** o = p->refs.find_or_add_pointer([&](void**p) {
                    if (*p == owner) {
                        pf = true;
                    }
                    return pf;
                });
                if (pf) {
                    if (log) {
                        Logeb();
                        printf("REF: found tracked owner pointer %p with wanted pointer %p\n", *o, owner);
                        Logr();
                    }
                } else {
                    *o = owner;
                    if (log) {
                        Logeb();
                        printf("REF: added tracked owner pointer %p with wanted pointer %p\n", *o, owner);
                        Logr();
                    }
                }
                return *p;
            }
            
            bool release(void * ptr) {
                if (size == 0) {
                    warn_ptr("RELEASE", ptr);
                    return false;
                }
                auto l = find_node([&](PointerInfo*p) {
                    return p->pointer == ptr;
                });
                if (l.found != nullptr) {
                    if (log) {
                        Logeb();
                        printf("RELEASE: found tracked pointer %p with wanted pointer %p\n", l.found->node->pointer, ptr);
                        Logr();
                    }
                    // dont release if owned by global
                    auto global = GET_GLOBAL();
                    if (l.found->node->refs.find_pointer(global, false) != nullptr) {
                        if (l.found->node->refs.size != 1) {
                            l.found->node->refs.remove_all_pointers_except(global);
                            return true;
                        }
                        return false;
                    } else {
                        // we are not owned by global, it is safe to release
                        l.found->node->release();
                        remove_node(l);
                        return true;
                    }
                }
                warn_ptr("RELEASE", ptr);
                return false;
            }

            void unref(void * ptr, void * owner, std::function<bool(void*,void*)> pred) {
                if (size == 0) {
                    warn_ptr("UNREF", ptr);
                    return;
                }
                auto l = find_node([&](PointerInfo*p) {
                    return pred(p->pointer, ptr);
                });
                if (l.found != nullptr) {
                    if (log) {
                        Logeb();
                        printf("UNREF: found tracked pointer %p with wanted pointer %p\n", l.found->node->pointer, ptr);
                        Logr();
                    }
                    if (l.found->node->refs.find_pointer(owner, false) != nullptr) {
                        if (l.found->node->refs.size == 1) {
                            remove_node(l);
                        } else {
                            l.found->node->refs.remove_pointer(owner);
                        }
                    }
                } else {
                    warn_ptr("UNREF", ptr);
                }
            }

            ~PTRINFO_LL() {
                auto s = 0;
                if (s != 0) {
                    if (s != 1) {
                        Logeb();
                        printf("~PTRINFO_LL(), FREEING %zu TrackedAllocator POINTERS\n", s);
                        Logr();
                    }
                    remove_all();
                    if (s != 1) {
                        Logeb();
                        printf("~PTRINFO_LL(), ALL TrackedAllocator POINTERS HAVE BEEN FREED\n");
                        Logr();
                    }
                }
            }
        };

        // these are used by the TrackedAllocator
        PTRINFO_LL tracked_pointers;

        SA____STACK_ALLOCATOR__REF_ONLY(SINGLETONS, SINGLETONS);

        ~SINGLETONS() {
            if (log) {
                Logeb();
                printf("~SINGLETONS()\n");
                Logr();
            }
        }
    };

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

            auto & singleton = GET_SINGLETONS();
            singleton.mutex.lock();
            void * ptr;
            while (true) {
                // calloc initializes memory and stops valgrind complaining about uninitialized memory use
                ptr = SINGLETONS::inspect_calloc(n, sizeof(T));
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
            singleton.memory_usage += sizeof(T)*n;
            singleton.per_type<T>().memory_usage += sizeof(T)*n;
            if (log) {
                Logib();
                printf("allocated %zu bytes of memory, total memory usage for '%s': %zu bytes. total memory usage: %zu bytes\n", sizeof(T)*n, singleton.per_type<T>().demangled, singleton.per_type<T>().memory_usage, singleton.memory_usage);
                Logr();
            }
            onAlloc(static_cast<T*>(ptr), sizeof(T)*n);
            singleton.mutex.unlock();
            return static_cast<T*>(ptr);
        }
    
        void secure_free(T* p, std::size_t n) noexcept
        {
            // the compiler is not allowed to optimize out functions that use volatile pointers
            volatile uint8_t* s = reinterpret_cast<uint8_t*>(p);
            volatile uint8_t* e = s + (sizeof(T)*n);
            std::fill(s, e, 0);
            SINGLETONS::inspect_free(p);
            auto & singleton = GET_SINGLETONS();
            singleton.memory_usage -= sizeof(T)*n;
            singleton.per_type<T>().memory_usage -= sizeof(T)*n;
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
            auto & singleton = GET_SINGLETONS();
            singleton.mutex.lock();
            if (onDealloc(p, sizeof(T)*n)) {
                if (log) {
                    Logib();
                    printf("deallocating %zu bytes of memory, total memory usage for '%s': %zu bytes. total memory usage: %zu bytes\n", sizeof(T)*n, singleton.per_type<T>().demangled, singleton.per_type<T>().memory_usage, singleton.memory_usage);
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
            singleton.mutex.unlock();
        }
    };

    template<typename T>
    struct TrackedMallocator : public Mallocator<T>
    {
        using Mallocator<T>::Mallocator;

        void onAlloc(T * p, std::size_t n) override {
            GET_SINGLETONS().pointers.add_pointer(p);
        }
    
        bool onDealloc(T * p, std::size_t n) override {
            return GET_SINGLETONS().pointers.remove_pointer(p);
        }
    };

    template <typename T>
    static TrackedMallocator<T> & GET_TRACKED_MALLOCATOR() {
        static TrackedMallocator<T> m;
        return m;
    }

    struct TrackedAllocator : AllocatorBase {

        TrackedAllocator() {}

        TrackedAllocator(const TrackedAllocator & other) = delete;
        TrackedAllocator & operator=(const TrackedAllocator & other) = delete;

        TrackedAllocator(TrackedAllocator && other) = default;
        TrackedAllocator & operator=(TrackedAllocator && other) = default;
        
        template <typename T>
        void adopt(T * ptr, std::function<void(void*)> destructor = [](void*p){ delete static_cast<T*>(p); }) {
            auto & singleton = GET_SINGLETONS();
            singleton.mutex.lock();
            auto & p = singleton.tracked_pointers.ref(ptr, this);
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
            singleton.mutex.unlock();
        }

        static void release(void * ptr) {
            auto & singleton = GET_SINGLETONS();
            singleton.mutex.lock();
            singleton.tracked_pointers.release(ptr);
            singleton.mutex.unlock();
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
            T * ptr = GET_TRACKED_MALLOCATOR<T>().allocate(count);
            auto & singleton = GET_SINGLETONS();
            singleton.mutex.lock();
            auto & p = singleton.tracked_pointers.ref(ptr, this);
            if (p.refs.size == 1) {
                onAlloc(p.pointer, sizeof(T)*p.count);
                p.count = 1;
                p.adopted = false;
                p.t_destructor = destructor;
                p.destructor = [this](auto & p) {
                    if (p.pointer != nullptr) {
                        //onDealloc(p.pointer, sizeof(T)*p.count);
                        p.t_destructor(p.pointer);
                        GET_TRACKED_MALLOCATOR<T>().deallocate(static_cast<T*>(p.pointer), p.count);
                        p.pointer = nullptr;
                        p.adopted = false;
                        p.count = 0;
                    }
                };
            }
            singleton.mutex.unlock();
            return ptr;
        }

        void internal_dealloc(void * ptr, std::function<bool(void*,void*)> pred) {
            auto & singleton = GET_SINGLETONS();
            singleton.mutex.lock();
            singleton.tracked_pointers.unref(ptr, this, [&](void* a, void* b) { return pred(a, b); });
            singleton.mutex.unlock();
        }
    };

    class TrackedAllocatorWithMemUsage : public TrackedAllocator {
        size_t memory_usage = 0;

        Mallocator<std::mutex> mutex_allocator;
        std::mutex * mutex = nullptr;

        public:

        using TrackedAllocator::TrackedAllocator;

        TrackedAllocatorWithMemUsage() {
            mutex = mutex_allocator.allocate(1);
        }

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

            mutex_allocator.deallocate(mutex, sizeof(std::mutex));
            mutex = nullptr;
        }

        protected:

        void onAlloc(void * p, std::size_t n) override {
            mutex->lock();
            memory_usage += n;
            mutex->unlock();
        }

        void onDealloc(void * p, std::size_t n) override {
            mutex->lock();
            memory_usage -= n;
            mutex->unlock();
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

#include <memory>
#include <mutex>
#include <stdlib.h>
#include <limits>
#include <string.h>

void *operator new(size_t size) {
    auto & singleton = SA::GET_SINGLETONS();

    auto s = singleton.mutex.scoped();

    if (singleton.mutex.lock_count() != 1) {
        return singleton.mallocator.alloc(size);
    }

    if (SA::log) {
        SA::Logib();
        printf("new(%zu)\n", size);
        SA::Logr();
    }
    return SA::GET_GLOBAL()->alloc(size);
}

void *operator new[](size_t size) {
    auto & singleton = SA::GET_SINGLETONS();

    auto s = singleton.mutex.scoped();

    if (singleton.mutex.lock_count() != 1) {
        return singleton.mallocator.alloc(size);
    }
    if (SA::log) {
        SA::Logib();
        printf("new[](%zu)\n", size);
        SA::Logr();
    }
    return SA::GET_GLOBAL()->alloc(size);
}

void operator delete(void *ptr) noexcept {
    auto & singleton = SA::GET_SINGLETONS();
    singleton.mutex.lock();
    if (SA::log) {
        SA::Logib();
        printf("delete(%p)\n", ptr);
        SA::Logr();
    }
    SA::GET_GLOBAL()->dealloc(ptr);
    singleton.mutex.unlock();
}

void operator delete[](void *ptr) noexcept {
    auto & singleton = SA::GET_SINGLETONS();
    singleton.mutex.lock();
    if (SA::log) {
        SA::Logib();
        printf("delete[](%p)\n", ptr);
        SA::Logr();
    }
    SA::GET_GLOBAL()->dealloc(ptr);
    singleton.mutex.unlock();
}

void operator delete(void *ptr, std::size_t sz) noexcept {
    auto & singleton = SA::GET_SINGLETONS();
    singleton.mutex.lock();
    if (SA::log) {
        SA::Logib();
        printf("delete(%p, %zu)\n", ptr, sz);
        SA::Logr();
    }
    SA::GET_GLOBAL()->dealloc(ptr);
    singleton.mutex.unlock();
}

void operator delete[](void *ptr, std::size_t sz) noexcept {
    auto & singleton = SA::GET_SINGLETONS();
    singleton.mutex.lock();
    if (SA::log) {
        SA::Logib();
        printf("delete[](%p, %zu)\n", ptr, sz);
        SA::Logr();
    }
    SA::GET_GLOBAL()->dealloc(ptr);
    singleton.mutex.unlock();
}

#ifdef __cpp_aligned_new
void *operator new(size_t size, std::align_val_t al) {
    auto & singleton = SA::GET_SINGLETONS();
    singleton.mutex.lock();
    if (SA::log) {
        SA::Logib();
        printf("aligned new(%zu, %zu)\n", size, al);
        SA::Logr();
    }
    void * p = SA::GET_GLOBAL()->alloc(size);
    singleton.mutex.unlock();
    return p;
}

void *operator new[](std::size_t size, std::align_val_t al) {
    auto & singleton = SA::GET_SINGLETONS();
    singleton.mutex.lock();
    if (SA::log) {
        SA::Logib();
        printf("aligned new[](%zu, %zu)\n", size, al);
        SA::Logr();
    }
    void * p = SA::GET_GLOBAL()->alloc(size);
    singleton.mutex.unlock();
    return p;
}

void operator delete(void *ptr, std::align_val_t al) noexcept {
    auto & singleton = SA::GET_SINGLETONS();
    singleton.mutex.lock();
    if (SA::log) {
        SA::Logib();
        printf("aligned delete(%p, %zu)\n", ptr, al);
        SA::Logr();
    }
    SA::GET_GLOBAL()->dealloc(ptr);
    singleton.mutex.unlock();
}

void operator delete[](void *ptr, std::align_val_t al) noexcept {
    auto & singleton = SA::GET_SINGLETONS();
    singleton.mutex.lock();
    if (SA::log) {
        SA::Logib();
        printf("aligned delete[](%p, %zu)\n", ptr, al);
        SA::Logr();
    }
    SA::GET_GLOBAL()->dealloc(ptr);
    singleton.mutex.unlock();
}

void operator delete(void *ptr, std::size_t sz, std::align_val_t al) noexcept {
    auto & singleton = SA::GET_SINGLETONS();
    singleton.mutex.lock();
    if (SA::log) {
        SA::Logib();
        printf("aligned delete(%p, %zu, %zu)\n", ptr, sz, al);
        SA::Logr();
    }
    SA::GET_GLOBAL()->dealloc(ptr);
    singleton.mutex.unlock();
}

void operator delete[](void *ptr, std::size_t sz, std::align_val_t al) noexcept {
    auto & singleton = SA::GET_SINGLETONS();
    singleton.mutex.lock();
    if (SA::log) {
        SA::Logib();
        printf("aligned delete[](%p, %zu, %zu)\n", ptr, sz, al);
        SA::Logr();
    }
    SA::GET_GLOBAL()->dealloc(ptr);
    singleton.mutex.unlock();
}
#endif


#endif

#endif
