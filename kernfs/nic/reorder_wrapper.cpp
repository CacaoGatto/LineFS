#include "reorder.h"
#include "reorder_wrapper.h"

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>

#define SCHEDULER_STRIDE 128

static DppScheduler *g_req_managers[8] = {nullptr};

static inline bool check_handle(int rm_handle) {
    if (rm_handle < 0 || rm_handle >= 8) {
        printf("Invalid handle %d\n", rm_handle);
        return false;
    }
    if (g_req_managers[rm_handle] == nullptr) {
        printf("Handle %d is not initialized\n", rm_handle);
        return false;
    }
    return true;
}

int initialize_req_manager(uint32_t ideal, uint32_t total, uint32_t thres, int *rm_handle) {
    int index = 0;
    for (index = 0; index < 8; index++) {
        if (g_req_managers[index] == nullptr) {
            break;
        }
    }
    if (index == 8) {
        return -1;
    }
    DppScheduler *rm = new DppScheduler(ideal, thres, SCHEDULER_STRIDE, total);
    g_req_managers[index] = rm;
    *rm_handle = index;
    return 0;
}

int destroy_req_manager(int rm_handle) {
    if (!check_handle(rm_handle)) {
        return -1;
    }
    delete g_req_managers[rm_handle];
    g_req_managers[rm_handle] = nullptr;
    return 0;
}

int submit_rm_req(int rm_handle, uint64_t key, void *context, void (*callback)(void *)) {
    if (!check_handle(rm_handle)) {
        return -1;
    }
    return g_req_managers[rm_handle]->Post(key, context, callback);
}

int complete_rm_req(int rm_handle, uint64_t key) {
    if (!check_handle(rm_handle)) {
        return -1;
    }
    return g_req_managers[rm_handle]->Complete(key);
}

int schedule_rm_req(int rm_handle, uint64_t max_submit) {
    if (!check_handle(rm_handle)) {
        return -1;
    }
    g_req_managers[rm_handle]->Dispatch(max_submit);
    while (g_req_managers[rm_handle]->Poll(nullptr)) ;
    return 0;
}

#ifdef __cplusplus
};
#endif
