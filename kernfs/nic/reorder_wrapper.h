#ifndef _NIC_REORDER_WRAPPER_H_
#define _NIC_REORDER_WRAPPER_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

int initialize_req_manager(uint32_t ideal, uint32_t total, uint32_t thres, int *rm_handle);
int destroy_req_manager(int rm_handle);
int submit_rm_req(int rm_handle, uint64_t key, void *context, void (*callback)(void *));
int complete_rm_req(int rm_handle, uint64_t key);
int schedule_rm_req(int rm_handle, uint64_t max_submit);

#ifdef __cplusplus
};
#endif

#endif  // _NIC_REORDER_WRAPPER_H_