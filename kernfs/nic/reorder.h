#ifndef _NIC_REORDER_H_
#define _NIC_REORDER_H_

#include <pthread.h>
#include <cstdint>

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

    struct GroupMeta {
        uint64_t *trace = nullptr;
        uint64_t *volatile queue = nullptr;
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

    uint64_t GetReorderKey(void *req);
    int Submit(void *req);
public:
    ReqManager(uint32_t ideal, uint32_t total, uint32_t thres);
    ~ReqManager();

    struct CustomReq {
        uint64_t type;
        uint64_t key;
        char *arg[0];
    };

    int Post(void *req, uint64_t group_id = ReqManager::kTypeMagic);
    int Poll(int ncomp, uint64_t group_id = ReqManager::kTypeMagic);
    uint64_t Register(int (*post_handler)(void *arg));
    int Deregister(uint64_t type);
    uint64_t AllocGroup(uint32_t weight = ReqManager::kDefaultWeight);
    int FreeGroup(uint64_t group_id);
};

#endif  // _NIC_REORDER_H_