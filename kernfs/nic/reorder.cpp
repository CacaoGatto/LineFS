#include "reorder.h"

#include <cstdlib>
#include <iostream>
#include <cstring>

#ifndef LINEFS_REORDER
uint64_t ReqManager::GetReorderKey(void *req) {
    // For CustomReq, direct use the user-defined domain
    struct CustomReq *custom_req = (struct CustomReq *)req;
    return custom_req->key;
}
#endif

int ReqManager::Submit(void *req) {
    uint64_t type = *(uint64_t *)req;
    uint64_t mtype = type_u2m(type);
    struct CustomReq *custom_req = (struct CustomReq *)req;
    int ret;
    if (check_type(mtype) == false || post_handler_[mtype] == nullptr) {
        std::cerr << "Invalid Type" << std::endl;
        ret = -1;
    } else {
        ret = post_handler_[mtype](custom_req->arg);
    }
    free(req);
    return ret;
}

uint64_t ReqManager::Register(int (*post_handler)(void *arg)) {
    uint64_t mtype;
    pthread_mutex_lock(&reg_lock_);
    for (mtype = 0; mtype < kRegisterCap; mtype++) {
        if (post_handler_[mtype] == nullptr) {
            break;
        }
    }
    if (mtype == kRegisterCap) {
        pthread_mutex_unlock(&reg_lock_);
        std::cerr << "No Available Type" << std::endl;
        return 0;
    }
    post_handler_[mtype] = post_handler;
    pthread_mutex_unlock(&reg_lock_);
    return type_m2u(mtype);
}

int ReqManager::Deregister(uint64_t utype) {
    uint64_t mtype = type_u2m(utype);
    pthread_mutex_lock(&reg_lock_);
    if (mtype >= kRegisterCap || post_handler_[mtype] == nullptr) {
        pthread_mutex_unlock(&reg_lock_);
        std::cerr << "Invalid Type" << std::endl;
        return -1;
    }
    post_handler_[mtype] = nullptr;
    pthread_mutex_unlock(&reg_lock_);
    return 0;
}

uint64_t ReqManager::AllocGroup(uint32_t weight) {
    uint64_t group_id;
    pthread_mutex_lock(&grp_lock_);
    for (group_id = 0; group_id < kGroupCap; group_id++) {
        if (group_meta_[group_id] == nullptr) {
            break;
        }
    }
    if (group_id == kGroupCap) {
        pthread_mutex_unlock(&grp_lock_);
        std::cerr << "No Available Group" << std::endl;
        return 0;
    }
    struct GroupMeta *meta = new struct GroupMeta();
    meta->trace = (volatile uint64_t *)malloc(trace_num_ * sizeof(uint64_t));
    for (uint32_t i = 0; i < trace_num_; i++) {
        meta->trace[i] = kInvalidKey;
    }
    meta->queue = (struct WaitingBatch *)malloc(total_num_ * sizeof(struct WaitingBatch));
    for (uint32_t i = 0; i < total_num_; i++) {
        meta->queue[i].key = kInvalidKey;
#ifdef LINEFS_REORDER
        for (int j = 0; j < 3; j++) {
            meta->queue[i].req[j] = nullptr;
        }
#endif
    }
    pthread_rwlock_init(&meta->rwlock, nullptr);
    meta->weight = weight;
    group_meta_[group_id] = meta;
    pthread_mutex_unlock(&grp_lock_);
    weight_ += weight;
    return type_m2u(group_id);
}

int ReqManager::FreeGroup(uint64_t group_id) {
    uint64_t group_index = type_u2m(group_id);
    if (group_index >= kGroupCap || group_meta_[group_index] == nullptr) {
        std::cerr << "Invalid Group" << std::endl;
        return -1;
    }
    pthread_mutex_lock(&grp_lock_);
    struct GroupMeta *meta = group_meta_[group_index];
    if (meta->trace != nullptr) {
        free((void *)meta->trace);
    }
    if (meta->queue != nullptr) {
        free((void *)meta->queue);
    }
    pthread_rwlock_destroy(&meta->rwlock);
    delete meta;
    group_meta_[group_index] = nullptr;
    pthread_mutex_unlock(&grp_lock_);
    return 0;
}

#ifndef LINEFS_REORDER

ReqManager::ReqManager(uint32_t ideal, uint32_t total, uint32_t thres)
        : trace_num_(ideal), total_num_(total), thres_num_(thres) {
    /* Initialize global variables. */
    pthread_mutex_init(&reg_lock_, nullptr);
    pthread_mutex_init(&grp_lock_, nullptr);
    group_meta_ = (struct GroupMeta **)malloc(sizeof(struct GroupMeta *) * kGroupCap);
    memset(group_meta_, 0, sizeof(struct GroupMeta *) * kGroupCap);
    post_handler_ = (int (**)(void *))malloc(sizeof(int (*)(void *)) * kRegisterCap);
    memset(post_handler_, 0, sizeof(int (*)(void *)) * kRegisterCap);
    /* Initialize the default group. */
    struct GroupMeta *meta = new struct GroupMeta();
    meta->trace = (uint64_t *)malloc(trace_num_ * sizeof(uint64_t));
    meta->queue = (uint64_t *volatile)malloc(total_num_ * sizeof(uint64_t));
    memset(meta->trace, 0, trace_num_ * sizeof(uint64_t));
    memset(meta->queue, 0, total_num_ * sizeof(uint64_t));
    pthread_rwlock_init(&meta->rwlock, nullptr);
    group_meta_[0] = meta;
}

ReqManager::~ReqManager() {
    for (uint64_t i = 0; i < kGroupCap; i++) {
        if (group_meta_[i] != nullptr) {
            if (group_meta_[i]->trace != nullptr) {
                free(group_meta_[i]->trace);
            }
            if (group_meta_[i]->queue != nullptr) {
                free(group_meta_[i]->queue);
            }
            pthread_rwlock_destroy(&group_meta_[i]->rwlock);
            delete group_meta_[i];
        }
    }
    free(group_meta_);
    free(post_handler_);
    pthread_mutex_destroy(&reg_lock_);
    pthread_mutex_destroy(&grp_lock_);
}

int ReqManager::Post(void *req, uint64_t group_id) {
    uint64_t group_index = type_u2m(group_id);
    struct GroupMeta *group = group_meta_[group_index];
    uint32_t trace_num = trace_num_ * group->weight / weight_;
    uint64_t key = GetReorderKey(req);
    /* 1. Check whether the request hits history */
    bool hit = false;
    pthread_rwlock_rdlock(&group->rwlock);
    for (uint32_t i = 0; i < trace_num; i++) {
        if (group->trace[i] == key) {
            hit = true;
            break;
        }
    }
    pthread_rwlock_unlock(&group->rwlock);
    /* 2. If the request hits history, submit it directly. */
    if (hit) {
        __sync_fetch_and_add(&group->flow_inflight, 1);
        return Submit(req);
    }
    /* 3. Otherwise, append it to the queue. */
    uint32_t index = __sync_fetch_and_add(&group->queue_tail, 1) % total_num_;
    while (group->queue[index] != 0) {
        ;  // Wait for the slot to be empty.
    }
    group->queue[index] = (uint64_t)req;
    return 0;
}

int ReqManager::Poll(int ncomp, uint64_t group_id) {
    uint64_t group_index = type_u2m(group_id);
    struct GroupMeta *group = group_meta_[group_index];
    uint32_t trace_num = trace_num_ * group->weight / weight_;
    uint32_t thres_num = thres_num_ * group->weight / weight_;
    if (ncomp > 0) {
        __sync_fetch_and_sub(&group->flow_inflight, ncomp);
        return ncomp;
    }
    /* If the inflight is less than the threshold, try to submit requests from the queue. */
    while (group->flow_inflight < thres_num) {
        uint32_t index = group->queue_head;
        // If the queue is empty, break the loop.
        if (index == group->queue_tail) {
            break;
        }
        // If concurrent threads are trying to fetch the same request, retry.
        if (!__sync_bool_compare_and_swap(&group->queue_head, index, index + 1)) {
            continue;
        }
        index %= total_num_;
        void *req = (void *)group->queue[index];
        while (req == nullptr) {
            req = (void *)group->queue[index];  // Wait for the request to be appended.
        }
        group->queue[index] = 0;
        /* 1. Submit the request. */
        __sync_fetch_and_add(&group->flow_inflight, 1);
        Submit(req);
        /* 2. Update the trace list. */
        uint64_t key = GetReorderKey(req);
        pthread_rwlock_wrlock(&group->rwlock);
        group->trace[group->trace_ptr++] = key;
        group->trace_ptr %= trace_num;
        pthread_rwlock_unlock(&group->rwlock);
    }
    return 0;
}

#else

bool ReqManager::enqueue_waiting(uint64_t group_id, void *req, uint64_t key) {
    struct GroupMeta *group = group_meta_[group_id];
    /* Note that this lock ensures that no concurrent dequeue operation is performed. */
    pthread_rwlock_rdlock(&group->rwlock);
    uint32_t index = group->queue_head;
    bool done = false;
    while (index < group->queue_tail) {
        uint32_t slot = index % total_num_;
        if (group->queue[slot].key != key) {
            index++;
            continue;
        }
        // Insert the request into the slot.
        for (int i = 0; i < 3; i++) {
            if (__sync_bool_compare_and_swap(&group->queue[slot].req[i], nullptr, req)) {
                done = true;
                break;
            }
        }
        if (!done) {
            std::cerr << "Unexpected slot full" << std::endl;
        }
        break;
    }
    if (done) {
        pthread_rwlock_unlock(&group->rwlock);
        return true;
    }
    // Insert the request at the tail.
    uint32_t slot = __sync_fetch_and_add(&group->queue_tail, 1) % total_num_;
    while (!__sync_bool_compare_and_swap(&group->queue[slot].key, kInvalidKey, key)) {
        ;  // Wait for the slot to be empty.
    }
    for (int i = 0; i < 3; i++) {
        if (__sync_bool_compare_and_swap(&group->queue[slot].req[i], nullptr, req)) {
            done = true;
            break;
        }
    }
    if (!done) {
        std::cerr << "Unexpected slot full" << std::endl;
    }
    pthread_rwlock_unlock(&group->rwlock);
    return true;
}

bool ReqManager::dequeue_waiting(uint64_t group_id, void **req, uint64_t *key) {
    struct GroupMeta *group = group_meta_[group_id];
    /* Note that this lock ensures that no concurrent enqueue operation is performed. */
    pthread_rwlock_wrlock(&group->rwlock);
    if (group->queue_head >= group->queue_tail) {
        // The queue is empty.
        pthread_rwlock_unlock(&group->rwlock);
        return false;
    }
    uint32_t slot = group->queue_head++ % total_num_;
    *key = group->queue[slot].key;
    group->queue[slot].key = kInvalidKey;
    for (int i = 0; i < 3; i++) {
        req[i] = (void *)group->queue[slot].req[i];
        group->queue[slot].req[i] = nullptr;
    }
    pthread_rwlock_unlock(&group->rwlock);
    return true;
}

bool ReqManager::inc_trace(uint64_t group_id, uint64_t key, bool force) {
    struct GroupMeta *group = group_meta_[group_id];
    uint32_t trace_num = trace_num_ * group->weight / weight_;
    for (uint32_t i = 0; i < trace_num; i++) {
        uint64_t old_val = group->trace[i];
        if ((old_val & kInvalidKey) != key) {
            // The key mismatches. Continue the search.
            continue;
        }
        // The key hits the trace. Try to increment the counter.
        bool done = true;
        while (!__sync_bool_compare_and_swap(&group->trace[i], old_val, old_val + 1)) {
            // Potential concurrent update. Retry.
            old_val = group->trace[i];
            if ((old_val & kInvalidKey) != key) {
                // The key is removed from the trace. Stop the increment.
                done = false;
                break;
            }
        }
        if (done) {
            if (!force && old_val == key) {
                // The key was temporarily invalidated. Increse the inflight counter.
                // Note that this operation may make the inflight counter larger than the threshold.
                // This is why we need to allocate a slightly larger trace array.
                __sync_fetch_and_add(&group->flow_inflight, 1);
            }
            return true;
        }
        // The key is removed from the trace. Continue the search.
    }
    // The key is not in the trace.
    if (!force) {
        // If the key is not forced to be inserted, return false.
        return false;
    }
    // Otherwise, try to insert it.
    // CRITICAL: This operation prevents repeated eviction on the first several slots.
    uint32_t start_slot = __sync_fetch_and_add(&group->trace_ptr, 1);
    for (uint32_t i = 0; i < trace_num; i++) {
        uint32_t slot = (start_slot + i) % trace_num;
        uint64_t old_val = group->trace[slot];
        uint64_t old_cnt = old_val & 0x3ull;
        if (old_cnt != 0) {  // Trick: Invalid key will always satisfy this condition.
            continue;
        }
        // The slot is empty. Try to insert the key.
        if (__sync_bool_compare_and_swap(&group->trace[slot], old_val, key + 1)) {
            return true;
        }
        // Potential concurrent update. Continue the search.
    }
    // No available slot. The key is not inserted.
    std::cerr << "No available slot" << std::endl;
    return false;
}

bool ReqManager::dec_trace(uint64_t group_id, uint64_t key) {
    struct GroupMeta *group = group_meta_[group_id];
    uint32_t trace_num = trace_num_ * group->weight / weight_;
    for (uint32_t i = 0; i < trace_num; i++) {
        uint64_t old_val = group->trace[i];
        if ((old_val & kInvalidKey) != key) {
            continue;
        }
        // The key hits the trace. Try to decrement the counter.
        if (old_val == key) {
            // Unexpected case. The key is not in the trace.
            std::cerr << "Trying to decrease 0 count key!" << std::endl;
            return false;
        }
        while (!__sync_bool_compare_and_swap(&group->trace[i], old_val, old_val - 1)) {
            // Potential concurrent update. Retry.
            old_val = group->trace[i];
            if ((old_val & kInvalidKey) != key || old_val == key) {
                // Unexpected case. The key is not in the trace.
                std::cerr << "Trying to decrease unexpected key or 0 count key!" << std::endl;
                return false;
            }
        }
        // Inform whether the key is ready to be removed.
        return (old_val == key + 1);
    }
    // The key is not in the trace. Unexpected case.
    std::cerr << "Do not find expected key!" << std::endl;
    return false;
}

ReqManager::ReqManager(uint32_t ideal, uint32_t total, uint32_t thres)
        : trace_num_(ideal), total_num_(total), thres_num_(thres) {
    /* Initialize global variables. */
    pthread_mutex_init(&reg_lock_, nullptr);
    pthread_mutex_init(&grp_lock_, nullptr);
    group_meta_ = (struct GroupMeta **)malloc(sizeof(struct GroupMeta *) * kGroupCap);
    memset(group_meta_, 0, sizeof(struct GroupMeta *) * kGroupCap);
    post_handler_ = (int (**)(void *))malloc(sizeof(int (*)(void *)) * kRegisterCap);
    memset(post_handler_, 0, sizeof(int (*)(void *)) * kRegisterCap);
    /* Initialize the default group. */
    struct GroupMeta *meta = new struct GroupMeta();
    meta->trace = (volatile uint64_t *)malloc(trace_num_ * sizeof(uint64_t));
    for (uint32_t i = 0; i < trace_num_; i++) {
        meta->trace[i] = kInvalidKey;
    }
    meta->queue = (struct WaitingBatch *)malloc(total_num_ * sizeof(struct WaitingBatch));
    for (uint32_t i = 0; i < total_num_; i++) {
        meta->queue[i].key = kInvalidKey;
        for (int j = 0; j < 3; j++) {
            meta->queue[i].req[j] = nullptr;
        }
    }
    pthread_rwlock_init(&meta->rwlock, nullptr);
    group_meta_[0] = meta;
}

ReqManager::~ReqManager() {
    for (uint64_t i = 0; i < kGroupCap; i++) {
        if (group_meta_[i] != nullptr) {
            if (group_meta_[i]->trace != nullptr) {
                free((void *)group_meta_[i]->trace);
            }
            if (group_meta_[i]->queue != nullptr) {
                free((void *)group_meta_[i]->queue);
            }
            pthread_rwlock_destroy(&group_meta_[i]->rwlock);
            delete group_meta_[i];
        }
    }
    free(group_meta_);
    free(post_handler_);
    pthread_mutex_destroy(&reg_lock_);
    pthread_mutex_destroy(&grp_lock_);
}

int ReqManager::Post(void *req, uint64_t key, uint64_t group_id) {
    __sync_fetch_and_add(&all, 1);
    uint64_t group_index = type_u2m(group_id);
    uint64_t trace_key = key << 2;  // Reserve the last two bits for the counter.
    /* 1. Check whether the request hits history */
    bool done = inc_trace(group_index, trace_key, false);
    if (done) {
        __sync_fetch_and_add(&hit, 1);
        //std::cout << key << std::endl;
        return Submit(req);
    }
    /* 2. If not, append it to the queue. */
    done = enqueue_waiting(group_index, req, trace_key);
    return 0;
}

int ReqManager::Poll(uint64_t key, uint64_t group_id) {
    uint64_t group_index = type_u2m(group_id);
    struct GroupMeta *group = group_meta_[group_index];
    uint64_t trace_key = kInvalidKey;
    /* 1. If one request is completed, update the trace. */
    if (key != kInvalidKey) {
        trace_key = key << 2;  // Reserve the last two bits for the counter.
        bool ready = dec_trace(group_index, trace_key);
#if 1
        if (ready) {
            __sync_fetch_and_sub(&group->flow_inflight, 1);
        }
#else  // Submit immediately after request completion can severely degrade hit rate.
        // If the key is ready to be removed, try submitting the next request batch.
        if (ready) {
            void *reqs[3];
            bool done = dequeue_waiting(group_index, reqs, &trace_key);
            if (done) {
                // Submit all requests in the batch.
                for (int i = 0; i < 3; i++) {
                    if (reqs[i] != nullptr) {
                        inc_trace(group_index, trace_key, true);
                        Submit(reqs[i]);
                    }
                }
                // No need to update the inflight counter since it is simply a replacement.
            } else {
                // No more requests in the queue. Stop the polling.
                __sync_fetch_and_sub(&group->flow_inflight, 1);
            }
        }
#endif
        return 0;
    }
    /* 2. Check whether there are available in-flight slots. */
    uint32_t thres_num = thres_num_ * group->weight / weight_;
    while (true) {
        uint32_t inflight = group->flow_inflight;
        if (inflight >= thres_num) {
            // The inflight is full. Stop the polling.
            break;
        }
        // Try to occupy an in-flight slot.
        if (!__sync_bool_compare_and_swap(&group->flow_inflight, inflight, inflight + 1)) {
            // Potential concurrent update. Retry.
            continue;
        }
        // Try to submit the next request batch.
        void *reqs[3];
        bool done = dequeue_waiting(group_index, reqs, &trace_key);
        if (!done) {
            // No more requests in the queue. Release the inflight slot.
            __sync_fetch_and_sub(&group->flow_inflight, 1);
        } else {
            // Submit all requests in the batch.
            for (int i = 0; i < 3; i++) {
                if (reqs[i] != nullptr) {
                    inc_trace(group_index, trace_key, true);
                    Submit(reqs[i]);
                }
            }
        }
        // Submissions are done. Finish the polling.
        break;
    }
    return 0;
}

#endif