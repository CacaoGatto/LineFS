#include "reorder.h"

#include <cstdlib>
#include <iostream>
#include <cstring>

uint64_t ReqManager::GetReorderKey(void *req) {
    // For CustomReq, direct use the user-defined domain
    struct CustomReq *custom_req = (struct CustomReq *)req;
    return custom_req->key;
}

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
    meta->trace = (uint64_t *)malloc(trace_num_ * sizeof(uint64_t));
    meta->queue = (uint64_t *volatile)malloc(total_num_ * sizeof(uint64_t));
    memset(meta->trace, 0, trace_num_ * sizeof(uint64_t));
    memset(meta->queue, 0, total_num_ * sizeof(uint64_t));
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
        free(meta->trace);
    }
    if (meta->queue != nullptr) {
        free(meta->queue);
    }
    pthread_rwlock_destroy(&meta->rwlock);
    delete meta;
    group_meta_[group_index] = nullptr;
    pthread_mutex_unlock(&grp_lock_);
    return 0;
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