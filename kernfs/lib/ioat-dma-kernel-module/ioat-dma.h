#ifndef _IOAT_DMA_H_
#define _IOAT_DMA_H_

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/types.h>
#include <linux/dax.h>
#include <linux/list.h>
#include <linux/dmaengine.h>
#include <linux/spinlock.h>
#include <linux/slab.h>
#include "dax-private.h"

/* DMA stuffs */
struct ioat_dma_completion_list_item {
  struct list_head list;
  struct completion comp;
  dma_cookie_t cookie;
  dma_addr_t src;
  dma_addr_t dst;
  u64 size;
};

struct ioat_dma_device {
  struct list_head list;
  u64 device_id;
  pid_t owner;
  struct dma_chan *chan;
  struct list_head comp_list;
  spinlock_t comp_list_lock;
};

int create_dma_devices(void);

struct ioat_dma_device *find_ioat_dma_device(u64 device_id);
struct ioat_dma_device *get_available_ioat_dma_device(void);
void release_ioat_dma_device(struct ioat_dma_device *dma_device);

extern struct list_head dma_devices;
extern u32 n_dma_devices;

/* ioctl stuffs */
struct ioctl_dma_args {
  u64 device_id;
  char device_name[32];
  u64 src_offset;
  u64 dst_offset;
  u64 size;
} __attribute__ ((packed));

struct ioctl_dma_wait_args {
  u64 device_id;
  u64 completed_dma_num;
} __attribute__ ((packed));
#define IOCTL_MAGIC 0xad
#define IOCTL_IOAT_DMA_SUBMIT _IOW(IOCTL_MAGIC, 0, struct ioctl_dma_args)
#define IOCTL_IOAT_DMA_GET_DEVICE_NUM _IOR(IOCTL_MAGIC, 0, u64)
#define IOCTL_IOAT_DMA_GET_DEVICE _IOR(IOCTL_MAGIC, 1, u64)
#define IOCTL_IOAT_DMA_WAIT_ALL _IOWR(IOCTL_MAGIC, 0, struct ioctl_dma_wait_args)

int ioat_dma_ioctl_dma_submit(struct ioctl_dma_args *args, struct dev_dax *dev_dax, struct ioat_dma_device *dma_device);
int ioat_dma_ioctl_get_device_num(void __user *arg);
int ioat_dma_ioctl_get_device(void __user *arg);
int ioat_dma_ioctl_dma_wait_all(struct ioat_dma_device *dma_device, u64 *result);

/* device driver stuffs */
extern struct device *dev;

#endif