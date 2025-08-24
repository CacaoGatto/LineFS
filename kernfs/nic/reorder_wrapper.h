#ifndef _NIC_REORDER_WRAPPER_H_
#define _NIC_REORDER_WRAPPER_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

typedef struct rm_req {
    uint64_t type;
    char *arg[0];
} rm_req_t;

extern uint64_t rm_invalid_key;

#define RM_PF_REQ 0
#define RM_BD_REQ 1
#define RM_CP_REQ 2

int initialize_req_manager(uint32_t ideal, uint32_t total, uint32_t thres, int *rm_handle);
int destroy_req_manager(int rm_handle);
int post_rm_req(int rm_handle, void *req, uint64_t key, int type);
int poll_rm_req(int rm_handle, uint64_t key);
uint64_t register_rm_func(int rm_handle, int (*post_handler)(void *arg), int type);
int deregister_rm_func(int rm_handle, uint64_t type);
uint64_t alloc_rm_group(int rm_handle, uint32_t weight);
int free_rm_group(int rm_handle, uint64_t group_id);

#ifdef __cplusplus
};
#endif

#endif  // _NIC_REORDER_WRAPPER_H_