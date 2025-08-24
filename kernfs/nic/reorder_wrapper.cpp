#include "reorder.h"
#include "reorder_wrapper.h"

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>

static ReqManager *g_req_managers[8] = {nullptr};

uint64_t pf_func_type;
uint64_t bd_func_type;
uint64_t cp_func_type;

uint64_t rm_invalid_key = ReqManager::kInvalidKey;

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
#ifndef LINEFS_REORDER
    ReqManager *rm = new ReqManager(ideal, total, thres);
#else
    ReqManager *rm = new ReqManager(ideal * 2, total, thres);
#endif
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

#ifndef LINEFS_REORDER
int post_rm_req(int rm_handle, void *req, int type)
{
    if (!check_handle(rm_handle)) {
        return -1;
    }
    rm_req_t *rm_req = (rm_req_t *)req;
    if (type == RM_PF_REQ) {
        rm_req->type = pf_func_type;
    } else if (type == RM_BD_REQ) {
        rm_req->type = bd_func_type;
    } else if (type == RM_CP_REQ) {
        rm_req->type = cp_func_type;
    }
    return g_req_managers[rm_handle]->Post(req);
}
#else
int post_rm_req(int rm_handle, void *req, uint64_t key, int type)
{
    if (!check_handle(rm_handle)) {
        return -1;
    }
    rm_req_t *rm_req = (rm_req_t *)req;
    if (type == RM_PF_REQ) {
        rm_req->type = pf_func_type;
    } else if (type == RM_BD_REQ) {
        rm_req->type = bd_func_type;
    } else if (type == RM_CP_REQ) {
        rm_req->type = cp_func_type;
    }
    return g_req_managers[rm_handle]->Post(req, key);
}
#endif

#ifndef LINEFS_REORDER
int poll_rm_req(int rm_handle, int ncomp)
{
    if (!check_handle(rm_handle)) {
        return -1;
    }
    return g_req_managers[rm_handle]->Poll(ncomp);
}
#else
int poll_rm_req(int rm_handle, uint64_t key)
{
    if (!check_handle(rm_handle)) {
        return -1;
    }
    return g_req_managers[rm_handle]->Poll(key);
}
#endif

uint64_t register_rm_func(int rm_handle, int (*post_handler)(void *arg), int type)
{
    if (!check_handle(rm_handle)) {
        return -1;
    }
    uint64_t func_id = g_req_managers[rm_handle]->Register(post_handler);
    if (type == RM_PF_REQ) {
        pf_func_type = func_id;
    } else if (type == RM_BD_REQ) {
        bd_func_type = func_id;
    } else if (type == RM_CP_REQ) {
        cp_func_type = func_id;
    }
    return func_id;
}

int deregister_rm_func(int rm_handle, uint64_t type)
{
    if (!check_handle(rm_handle)) {
        return -1;
    }
    return g_req_managers[rm_handle]->Deregister(type);
}

uint64_t alloc_rm_group(int rm_handle, uint32_t weight)
{
    if (!check_handle(rm_handle)) {
        return -1;
    }
    return g_req_managers[rm_handle]->AllocGroup(weight);
}

int free_rm_group(int rm_handle, uint64_t group_id)
{
    if (!check_handle(rm_handle)) {
        return -1;
    }
    return g_req_managers[rm_handle]->FreeGroup(group_id);
}

#ifdef __cplusplus
};
#endif
