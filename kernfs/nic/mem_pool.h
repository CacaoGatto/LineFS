#ifndef _NIC_MEM_POOL_H_
#define _NIC_MEM_POOL_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

int initialize_mem_pool(uint32_t total, int *mp_handle);
int destroy_mem_pool(int mp_handle);
void charge_mem_buf(int mp_handle, void *buf);
int allocate_mem_buf(int mp_handle, void **buf, int ref_cnt);
void free_mem_buf(int mp_handle, void *buf);

#ifdef __cplusplus
};
#endif

#endif  // _NIC_MEM_POOL_H_