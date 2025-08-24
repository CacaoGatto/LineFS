#ifndef _NIC_REORDER_H_
#define _NIC_REORDER_H_

#include <pthread.h>
#include <cstdint>

#define LINEFS_REORDER

class ReqManager {
private:
    static const uint64_t kTypeMagic = 8;
    static const uint32_t kDefaultWeight = 8;
    static const uint64_t kRegisterCap = kTypeMagic;
    static const uint64_t kGroupCap = kTypeMagic;

    inline uint64_t type_u2m(uint64_t utype) { return (utype - kTypeMagic); }
    inline uint64_t type_m2u(uint64_t mtype) { return (mtype + kTypeMagic); }
    bool check_type(uint64_t mtype) { return (mtype < kRegisterCap); }

    uint32_t total_num_;
    uint32_t trace_num_;
    uint32_t thres_num_;
    volatile uint32_t weight_ = kDefaultWeight;

    struct WaitingBatch {
        volatile uint64_t key;
#ifdef LINEFS_REORDER
        void *volatile req[3];
#endif
    };
    bool enqueue_waiting(uint64_t group_id, void *req, uint64_t key);
    bool dequeue_waiting(uint64_t group_id, void **req, uint64_t *key);
    bool inc_trace(uint64_t group_id, uint64_t key, bool force);
    bool dec_trace(uint64_t group_id, uint64_t key);

    struct GroupMeta {
        struct WaitingBatch *queue = nullptr;
        volatile uint64_t *trace = nullptr;
        volatile uint32_t queue_head = 0;
        volatile uint32_t queue_tail = 0;
        volatile uint32_t trace_ptr = 0;
        volatile uint32_t flow_inflight = 0;
        uint32_t weight = kDefaultWeight;
        pthread_rwlock_t rwlock;
    };

    struct GroupMeta **group_meta_;
    int (**post_handler_)(void *);
    pthread_mutex_t reg_lock_;
    pthread_mutex_t grp_lock_;

#ifndef LINEFS_REORDER
    uint64_t GetReorderKey(void *req);
#endif
    int Submit(void *req);
public:
    ReqManager(uint32_t ideal, uint32_t total, uint32_t thres);
    ~ReqManager();

    struct CustomReq {
        uint64_t type;
#ifndef LINEFS_REORDER
        uint64_t key;
#endif
        char *arg[0];
    };
    static const uint64_t kInvalidKey = 0xfffffffffffffffc;

#ifndef LINEFS_REORDER
    int Post(void *req, uint64_t group_id = ReqManager::kTypeMagic);
    int Poll(int ncomp, uint64_t group_id = ReqManager::kTypeMagic);
#else
    int Post(void *req, uint64_t key, uint64_t group_id = ReqManager::kTypeMagic);
    int Poll(uint64_t key, uint64_t group_id = ReqManager::kTypeMagic);
#endif
    uint64_t Register(int (*post_handler)(void *arg));
    int Deregister(uint64_t type);
    uint64_t AllocGroup(uint32_t weight = ReqManager::kDefaultWeight);
    int FreeGroup(uint64_t group_id);

    // For testing
    uint32_t hit = 0;
    uint32_t all = 0;
    double GetHitRate() { return (double)hit / all; }
};

#endif  // _NIC_REORDER_H_