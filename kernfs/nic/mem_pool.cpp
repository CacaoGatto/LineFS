#include "mem_pool.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <atomic>

#ifdef __cplusplus
extern "C" {
#endif

class MemPool {
private:
    class DppStack {
    public:
        DppStack(uint64_t capacity) : capacity_(capacity) {};

        ~DppStack() {};

        inline int Initialize() {
            if (capacity_ == 0) {
                return -1;
            }
            uint64_t entry_size = sizeof(uint64_t) * capacity_ * 8;  // padding to cache line
            entry_ = reinterpret_cast<uint64_t *volatile>(malloc(entry_size));
            if (!entry_) {
                return -1;
            }
            memset((void *)entry_, -1, entry_size);
            return 0;
        }

        inline void Uninitialize() {
            if (entry_) {
                free(entry_);
                entry_ = nullptr;
            }
            return;
        }

        inline int PushEntry(uint64_t value) {
            while (true) {
                uint64_t old_top = top_;
                if (__builtin_expect(!!(capacity_ == (old_top >> 3)), 0)) {
                    return -1;
                }
                if (__sync_bool_compare_and_swap(&top_, old_top, old_top + 8)) {
                    bool success = false;
                    do {
                        // try to find an empty slot in this cache line
                        for (int i = 0; i < 8; i++) {
                            success = __sync_bool_compare_and_swap(&entry_[old_top+i],
                                                                    kEmptySlot, value);
                            if (success) break;
                        }
                    } while (success == false);
                    return 0;
                }
            }
        }

        inline int PopEntry(uint64_t *value) {
            while (true) {
                uint64_t old_top = top_;
                if (__builtin_expect(!!(old_top == 0), 0)) {
                    return -1;
                }
                uint64_t new_top = old_top - 8;
                if (__sync_bool_compare_and_swap(&top_, old_top, new_top)) {
                    uint64_t tmp;
                    bool success = false;
                    while (success == false) {
                        // try to find a non-empty slot in this cache line
                        for (int i = 0; i < 8; i++) {
                            tmp = entry_[new_top + i];
                            if (tmp == kEmptySlot) continue;
                            success = __sync_bool_compare_and_swap(&entry_[new_top+i],
                                                                    tmp, kEmptySlot);
                            if (success) break;
                        }
                    }
                    *value = tmp;
                    return 0;
                }
            }
        }

    protected:
        uint64_t *volatile entry_ = nullptr;
        volatile uint64_t top_ = 0;
        const uint64_t capacity_;
        static const uint64_t kEmptySlot = 0xFFFFFFFFFFFFFFFF;
    };

    DppStack stack_;
    std::unordered_map<uint64_t, std::atomic<int>> map_;
public:
    MemPool(uint32_t total) :stack_(total) {
        stack_.Initialize();
        map_.clear();
        map_.reserve(total);
    }
    ~MemPool() {}
    void Add(void *addr) {
        uint64_t value = (uint64_t)addr;
        map_.emplace(value, 0);
        stack_.PushEntry(value);
    }
    int Allocate(void **addr, int ref_cnt) {
        uint64_t value = 0;
        if (stack_.PopEntry(&value)) {
            return -1;
        }
        map_[value].store(ref_cnt);
        *addr = (void *)value;
        return 0;
    }
    void Free(void *addr) {
        uint64_t value = (uint64_t)addr;
        if (map_[value].fetch_sub(1) == 1) {
            stack_.PushEntry(value);
        }
    }
};

static MemPool *g_mem_pool[8] = {nullptr};

static inline bool check_handle(int mp_handle) {
    if (mp_handle < 0 || mp_handle >= 8) {
        printf("Invalid handle %d\n", mp_handle);
        return false;
    }
    if (g_mem_pool[mp_handle] == nullptr) {
        printf("Handle %d is not initialized\n", mp_handle);
        return false;
    }
    return true;
}

int initialize_mem_pool(uint32_t total, int *mp_handle) {
    int index = 0;
    for (index = 0; index < 8; index++) {
        if (g_mem_pool[index] == nullptr) {
            break;
        }
    }
    if (index == 8) {
        return -1;
    }
    MemPool *pool = new MemPool(total);
    g_mem_pool[index] = pool;
    *mp_handle = index;
    return 0;
}

int destroy_mem_pool(int mp_handle) {
    if (!check_handle(mp_handle)) {
        return -1;
    }
    delete g_mem_pool[mp_handle];
    g_mem_pool[mp_handle] = nullptr;
    return 0;
}

void charge_mem_buf(int mp_handle, void *buf) {
    if (check_handle(mp_handle)) {
        g_mem_pool[mp_handle]->Add(buf);
    }
}

int allocate_mem_buf(int mp_handle, void **buf, int ref_cnt) {
    if (!check_handle(mp_handle)) {
        return -1;
    }
    return g_mem_pool[mp_handle]->Allocate(buf, ref_cnt);
}

void free_mem_buf(int mp_handle, void *buf) {
    if (check_handle(mp_handle)) {
        g_mem_pool[mp_handle]->Free(buf);
    }
}

#ifdef __cplusplus
};
#endif
