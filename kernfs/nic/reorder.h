#ifndef _NIC_REORDER_H_
#define _NIC_REORDER_H_

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <atomic>
#include <unordered_map>
#include <vector>

#define DPP_SCHEDULER_CONCURRENT_POST
#define DPP_SCHEDULER_CONCURRENT_POLL

// #define DPP_SCHEDULER_DEBUG

#define DPP_SCHEDULER_FAST_SWAP

class DppSchedulerCore {
protected:
    DppSchedulerCore(uint64_t hot_cap, uint64_t warm_cap,
                     uint64_t cold_cap, int64_t init_credit)
            : hot_cap_(hot_cap), warm_cap_(warm_cap),
              init_credit_(init_credit * kCreditFactor) {
        hot_.reserve(hot_cap);
        warm_.reserve(warm_cap);
        cold_.reserve(cold_cap);
        available_ = hot_cap_;
    }
    ~DppSchedulerCore() {;}

    inline void Submit(uint64_t key, void *context, void (*callback)(void *)) {
        bool is_new = hash_.find(key) == hash_.end();
        hash_[key].job.emplace_back(context, callback);
        if (!is_new) return;
        if (hot_.size() < hot_cap_) {
            hot_.push_back(key);
            hash_[key].Reset(init_credit_);
        } else if (warm_.size() < warm_cap_) {
            warm_.push_back(key);
            hash_[key].Reset(init_credit_);
        } else {
            cold_.push_back(key);
        }
    }

    inline void Commit(uint64_t key) {
        if (!hash_[key].working) {
            printf("CRITICAL: Commit to a non-working task!\n");
        }
        hash_[key].job.erase(hash_[key].job.begin());
        hash_[key].credit -= kCreditFactor;
        hash_[key].working = false;
        available_ += 1;
        aid_cnt_ += kCreditFactor * 2;
    }

    inline int Schedule(uint64_t *key, void **context, void (**callback)(void *)) {
        if (available_ == 0) return 0;
        if (aid_cnt_ >= init_credit_) {
            aid_cnt_ = 0;
            DemoteWarm();
        }
        const uint64_t range = hot_.size();
        for (uint64_t i = 0; i < range; i++) {
            uint64_t slot = clock_++ % range;
            uint64_t hot_key = hot_[slot];
            // Avoid concurrent jobs
            if (!hash_[hot_key].working) {
                if (hash_[hot_key].credit <= 0) {
                    // Try evicting the item to cold set if credit exhausted
                    EvictHot(slot);
                    return 0;
                } else if (hash_[hot_key].job.empty()) {
                    // Try demoting the item to warm set if no more job to do
                    DemoteHot(slot);
                    return 0;
                }
                // Got the job to execute
                hash_[hot_key].working = true;
                *key = hot_key;
                *context = hash_[hot_key].job[0].context;
                *callback = hash_[hot_key].job[0].callback;
                available_ -= 1;
                return 1;
            }
        }
        return 0;
    }

private:
    struct JobContext {
        struct JobItem {
            void *context;
            void (*callback)(void *);
            JobItem(void *context, void (*callback)(void *)) {
                this->context = context;
                this->callback = callback;
            }
        };
        std::vector<JobItem> job{};
        int64_t credit = 0;
        int64_t charge = 0;
        bool working = false;
        JobContext() {;}
        void Recover() {
            credit += charge;
            charge = (charge > 1) ? (charge >> 1) : 1;
        }
        void Reset(uint64_t init_credit) {
            credit += init_credit;
            charge = init_credit >> 1;
        }
    };
    std::unordered_map<uint64_t, JobContext> hash_{};

    std::vector<uint64_t> hot_{};
    std::vector<uint64_t> warm_{};
    std::vector<uint64_t> cold_{};
    const uint64_t hot_cap_;
    const uint64_t warm_cap_;
    const int64_t init_credit_;

#ifdef DPP_SCHEDULER_FAST_SWAP
    uint64_t warm_ptr_ = 0;
    uint64_t cold_ptr_ = 0;
#endif

    uint64_t available_ = 0;
    uint64_t clock_ = 0;
    int64_t aid_cnt_ = 0;
    static constexpr uint64_t kCreditFactor = 2;

    // Move the warm item to cold and lift one cold item up
    // Return true if done and false for no move
    inline void DemoteWarm() {
#ifdef DPP_SCHEDULER_FAST_SWAP
        const uint64_t cold_range = cold_.size();
        for (uint64_t i = 0; i < cold_range; i++) {
            uint64_t cold_index = (cold_ptr_ + i) % cold_range;
            uint64_t cold_key = cold_[cold_index];
            if (hash_[cold_key].job.empty()) continue;
            // Get ready cold item
            const uint64_t warm_range = warm_.size();
            for (uint64_t j = 0; j < warm_range; j++) {
                uint64_t warm_index = (warm_ptr_ + j) % warm_range;
                uint64_t warm_key = warm_[warm_index];
                if (!hash_[warm_key].job.empty()) continue;
                // Get ready warm item
                cold_[cold_index] = warm_key;
                warm_[warm_index] = cold_key;
                // Charge for cold item
                hash_[cold_key].Reset(init_credit_);
                // Forward the set pointer
                cold_ptr_ = cold_index + 1;
                warm_ptr_ = warm_index + 1;
                break;
            }
            // No warm item. No need to go on
            return;
        }
#else
        // Get ready cold iterator
        auto icold = std::find_if(cold_.begin(), cold_.end(), [this](uint64_t key) {
            return !hash_[key].job.empty();
        });
        if (icold == cold_.end()) return;
        // Get idle warm iterator
        auto iwarm = std::find_if(warm_.begin(), warm_.end(), [this](uint64_t key) {
            return hash_[key].job.empty();
        });
        if (iwarm == warm_.end()) return;
        // Start demotion
        uint64_t warm_key = *iwarm;
        uint64_t cold_key = *icold;
        warm_.erase(iwarm);
        warm_.push_back(cold_key);
        cold_.erase(icold);
        cold_.push_back(warm_key);
        // Charge for cold item
        hash_[cold_key].Reset(init_credit_);
#endif
    }

    // Demote one hot item to warm.
    // Even if no warm item is available, credit will be consumed
    // Return true for successful demotion, and false for no available item
    inline void DemoteHot(uint64_t slot) {
        uint64_t hot_key = hot_[slot];
#ifdef DPP_SCHEDULER_FAST_SWAP
        const uint64_t warm_range = warm_.size();
        for (uint64_t j = 0; j < warm_range; j++) {
            uint64_t warm_index = (warm_ptr_ + j) % warm_range;
            uint64_t warm_key = warm_[warm_index];
            if (hash_[warm_key].job.empty()) continue;
            // Get ready warm item
            hot_[slot] = warm_key;
            warm_[warm_index] = hot_key;
            // Forward the set pointer
            warm_ptr_ = warm_index + 1;
            return;
        }
        // No warm item. Consume one credit
        hash_[hot_key].credit -= 1;
#else
        // Get ready warm iterator
        auto iwarm = std::find_if(warm_.begin(), warm_.end(), [this](uint64_t key) {
            return !hash_[key].job.empty();
        });
        if (iwarm == warm_.end()) {
            // No ready item. Consume hot credit instead
            hash_[hot_key].credit -= 1;
        } else {
            // Start demotion
            hot_[slot] = *iwarm;
            warm_.erase(iwarm);
            warm_.push_back(hot_key);
        }
#endif
    }

    // Evict one hot item to cold.
    // If no cold item available, return false directly
    // If valid warm item exists, make it hot and make the cold one warm
    // Otherwise, simply exchange the hot and the cold
    // Return true for successful eviction, and false for no available item
    inline void EvictHot(uint64_t slot) {
        uint64_t hot_key = hot_[slot];
#ifdef DPP_SCHEDULER_FAST_SWAP
        const uint64_t cold_range = cold_.size();
        const uint64_t warm_range = warm_.size();
        for (uint64_t i = 0; i < cold_range; i++) {
            uint64_t cold_index = (cold_ptr_ + i) % cold_range;
            uint64_t cold_key = cold_[cold_index];
            if (hash_[cold_key].job.empty()) continue;
            // Get ready cold item and forward the set pointer
            hash_[cold_key].Reset(init_credit_);
            cold_ptr_ = cold_index + 1;
            for (uint64_t j = 0; j < warm_range; j++) {
                uint64_t warm_index = (warm_ptr_ + j) % warm_range;
                uint64_t warm_key = warm_[warm_index];
                if (hash_[warm_key].job.empty()) continue;
                // Get ready warm item
                cold_[cold_index] = hot_key;
                warm_[warm_index] = cold_key;
                hot_[slot] = warm_key;
                // Forward the set pointer
                warm_ptr_ = warm_index + 1;
                return;
            }
            // No warm item. Use the first warm item (must be idle)
            uint64_t warm_index = warm_ptr_++ % warm_range;
            uint64_t warm_key = warm_[warm_index];
            hot_[slot] = cold_key;
            warm_[warm_index] = hot_key;
            cold_[cold_index] = warm_key;
            hash_[hot_key].Recover();
            return;
        }
        // No cold item. Recover the hot item
        hash_[hot_key].Recover();
        for (uint64_t j = 0; j < warm_range; j++) {
            uint64_t warm_index = (warm_ptr_ + j) % warm_range;
            uint64_t warm_key = warm_[warm_index];
            if (hash_[warm_key].job.empty()) continue;
            // Get ready warm item
            hot_[slot] = warm_key;
            warm_[warm_index] = hot_key;
            // Forward the set pointer
            warm_ptr_ = warm_index + 1;
            return;
        }
        // No warm item. Just go on
#else
        // Get ready cold iterator
        auto icold = std::find_if(cold_.begin(), cold_.end(), [this](uint64_t key) {
            return !hash_[key].job.empty();
        });
        // Get ready warm iterator
        auto iwarm = std::find_if(warm_.begin(), warm_.end(), [this](uint64_t key) {
            return !hash_[key].job.empty();
        });
        if (icold == cold_.end()) {
            // No cold item means the hot should be recovered
            hash_[hot_key].Recover();
            if (iwarm != warm_.end()) {
                // Demote the hot slot to warm
                hot_[slot] = *iwarm;
                warm_.erase(iwarm);
                warm_.push_back(hot_key);
            }
            // No warm item means we can just go on with this hot item
        } else {
            // Got an available cold item. Eviction must succeed
            hash_[*icold].Reset(init_credit_);
            if (iwarm != warm_.end()) {
                // Promote warm and cold. Invalidate hot
                hot_[slot] = *iwarm;
                warm_.erase(iwarm);
                warm_.push_back(*icold);
                cold_.erase(icold);
                cold_.push_back(hot_key);
            } else {
                if (warm_.empty()){
                    // Empty warm set is contradict to valid cold item
                    printf("Unexpected empty warm set !\n");
                    return;
                }
                // Now the first warm item must be idle
                iwarm = warm_.begin();
                hot_[slot] = *icold;
                cold_.erase(icold);
                cold_.push_back(*iwarm);
                warm_.erase(iwarm);
                warm_.push_back(hot_key);
                // Recover the original hot item
                hash_[hot_key].Recover();
            }
        }
#endif
    }
};

class DppScheduler : public DppSchedulerCore {
public:
    DppScheduler(uint64_t hot_size, uint64_t warm_size,
                 int64_t init_credit, uint64_t req_depth)
            : DppSchedulerCore(hot_size, warm_size, req_depth, init_credit),
              req_queue_(req_depth), req_stack_(req_depth), req_list_(req_depth),
              task_queue_(hot_size), task_stack_(hot_size), task_list_(hot_size),
              comp_queue_(hot_size) {
        req_stack_.Initialize();
        task_stack_.Initialize();
        for (uint64_t i = 0; i < req_depth; i++) {
            req_stack_.PushEntry(i);
        }
        for (uint64_t i = 0; i < hot_size; i++) {
            task_stack_.PushEntry(i);
        }
    }

    ~DppScheduler() {
        task_stack_.Uninitialize();
        req_stack_.Uninitialize();
    }

    // Thread-safe producer interface
    inline int Post(uint64_t key, void *context, void (*callback)(void *)) {
        uint64_t req_id = 0;
        if (req_stack_.PopEntry(&req_id)) {
            return 0;  // Request slot is empty
        }
        req_list_[req_id].key = key;
        req_list_[req_id].context = context;
        req_list_[req_id].callback = callback;
        if (req_queue_.Enqueue(req_id)) {
            return -1;  // Request queue is full
        }
        return 1;
    }

    // Thread-safe consumer interface
    inline int Poll(uint64_t *key) {
        uint64_t task_id = 0;
        if (task_queue_.Dequeue(&task_id)) return 0;    // Task queue is empty
        uint64_t task_key = task_list_[task_id].key;
        void *context = task_list_[task_id].context;
        void (*callback)(void *) = task_list_[task_id].callback;
        if (task_stack_.PushEntry(task_id)) return -1;  // Task stack is full
        callback(context);
        if (key) *key = task_key;
        return 1;
    }

    inline int Complete(uint64_t key) {
        return comp_queue_.Enqueue(key);
    }

    // Thread-unsafe specific interface
    inline int Dispatch(uint64_t max_submit) {
        // Commit
        while (true) {
            uint64_t key = 0;
            // Stop fetching completions if already empty
            if (comp_queue_.Dequeue(&key)) break;
            Commit(key);
        }
        // Submit
        for (uint64_t i = 0; i < max_submit; i++) {
            uint64_t req_id = 0;
            // Stop fetching requests if already empty
            if (req_queue_.Dequeue(&req_id)) break;
            Submit(req_list_[req_id].key,
                   req_list_[req_id].context,
                   req_list_[req_id].callback);
            req_stack_.PushEntry(req_id);
        }
        // Handle scheduling
        uint64_t key = 0;
        void *context = nullptr;
        void (*callback)(void *) = nullptr;
        if (Schedule(&key, &context, &callback) <= 0) return 0;
        uint64_t task_id = 0;
        if (task_stack_.PopEntry(&task_id)) return -1;  // Task stack is empty
        task_list_[task_id].key = key;
        task_list_[task_id].context = context;
        task_list_[task_id].callback = callback;
        if (task_queue_.Enqueue(task_id)) return -1;    // Task queue is full
        return 1;
    }

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

    class DppQueue {
    private:
        std::atomic<uint64_t> *queue_ = nullptr;    ///< The queue array
        std::atomic<uint32_t> head_{0};             ///< Index of the head of the queue
        std::atomic<uint32_t> tail_{0};             ///< Index of the tail of the queue
        const uint32_t capacity_;                   ///< Maximum capacity of the queue
        const uint64_t empty_slot_;                 ///< Value representing an empty slot in the queue

    public:
        DppQueue(uint32_t capacity, uint64_t empty_slot = 0xffffffffffffffff)
                : capacity_(capacity), empty_slot_(empty_slot) {
            queue_ = new std::atomic<uint64_t>[capacity_];
            if (queue_) {
                for (uint32_t i = 0; i < capacity_; ++i) {
                    queue_[i].store(empty_slot_, std::memory_order_relaxed);
                }
            }
        }

        ~DppQueue() {
            if (queue_) {
                delete[] queue_;
                queue_ = nullptr;
            }
        }

        DppQueue(const DppQueue&) = delete;
        DppQueue& operator=(const DppQueue&) = delete;

        int Enqueue(uint64_t val) {
            if (val == empty_slot_) {
                printf("Error: Cannot Enqueue reserved value\n");
                return -1;
            }
            while (true) {
                uint32_t tail_val = tail_.load(std::memory_order_acquire);
                uint32_t head_val = head_.load(std::memory_order_acquire);
                if (tail_val - head_val >= capacity_) {
                    return -1;
                }
                if (tail_.compare_exchange_weak(tail_val, tail_val + 1,
                                                std::memory_order_acq_rel)) {
                    uint32_t index = tail_val % capacity_;
                    uint64_t expected = empty_slot_;
                    while (!queue_[index].compare_exchange_weak(expected, val,
                                                                std::memory_order_release,
                                                                std::memory_order_acquire)) {
                        expected = empty_slot_;
                    }
                    return 0;
                }
            }
        }

        int Dequeue(uint64_t* val) {
            while (true) {
                uint32_t head_val = head_.load(std::memory_order_acquire);
                uint32_t tail_val = tail_.load(std::memory_order_acquire);
                if (head_val == tail_val) {
                    return -1;
                }
                uint32_t index = head_val % capacity_;
                uint64_t item = queue_[index].load(std::memory_order_acquire);
                if (item == empty_slot_) {
                    continue;
                }
                if (queue_[index].compare_exchange_strong(item, empty_slot_,
                                                        std::memory_order_acq_rel)) {
                    head_.store(head_val + 1, std::memory_order_release);
                    *val = item;
                    return 0;
                }
            }
        }

        uint32_t Size() const {
            uint32_t head_val = head_.load(std::memory_order_acquire);
            uint32_t tail_val = tail_.load(std::memory_order_acquire);
            return tail_val - head_val;
        }

        bool Empty() const {
            return Size() == 0;
        }

        bool Full() const {
            return Size() >= capacity_;
        }

        uint32_t Capacity() const {
            return capacity_;
        }
    };
    struct Parameters {
        uint64_t key;
        void *context;
        void (*callback)(void *);
    };
    DppQueue req_queue_;
    DppStack req_stack_;
    std::vector<Parameters> req_list_;
    DppQueue task_queue_;
    DppStack task_stack_;
    std::vector<Parameters> task_list_;
    DppQueue comp_queue_;
};

#endif  // _NIC_REORDER_H_